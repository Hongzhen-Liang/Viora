"""
PlatformIO extra_scripts —— ESP-SR(esp-sr 1.9.2 / esp-dsp) 集成。

功能：
  1. 编译 esp-sr 的 3 个源码文件 (model_path.c / esp_mn_speech_commands.c / esp_process_sdkconfig.c)
  2. 编译 esp-dsp 需要的模块 (common / fft / dotprod / math-sqrt)
  3. 链接 esp-sr 的 esp32s3 预编译静态库 (--start-group 保证互相解析)
  4. 上传前自动生成 srmodels.bin 并烧录到 'model' 数据分区
"""
import os
import sys

from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

# 工程根目录：优先取环境变量，否则用当前工作目录（pio run 都在工程根执行）
PROJECT_DIR = env.get("PROJECT_DIR") or os.getcwd()
ESP_SR = os.path.join(PROJECT_DIR, "components", "esp-sr")
ESP_DSP = os.path.join(PROJECT_DIR, "components", "esp-dsp")

# ---------------------------------------------------------------- include 路径
env.Append(
    CPPPATH=[
        os.path.join(ESP_SR, "include", "esp32s3"),
        os.path.join(ESP_SR, "src", "include"),
    ]
)

# esp-dsp 所有模块的 include 目录
for root, dirs, files in os.walk(os.path.join(ESP_DSP, "modules")):
    if os.path.basename(root) == "include":
        env.Append(CPPPATH=[root])

# ---------------------------------------------------------------- 编译源码
# esp-sr 3 个 glue 源文件
env.BuildSources(os.path.join("$BUILD_DIR", "esp_sr_src"), os.path.join(ESP_SR, "src"))

# esp-dsp 需要的模块源码（只编 float/common，跳过 fixed/test/examples）
DSP_MODULES = [
    ("common/misc", "esp_dsp_common"),
    ("fft/float", "esp_dsp_fft_float"),
    ("dotprod/float", "esp_dsp_dotprod"),
    ("math/sqrt/float", "esp_dsp_sqrt"),
]
for rel, tag in DSP_MODULES:
    src = os.path.join(ESP_DSP, "modules", rel)
    if os.path.isdir(src):
        env.BuildSources(os.path.join("$BUILD_DIR", tag), src)

# ---------------------------------------------------------------- 链接 esp-sr 预编译库
# 注意：所有 esp-sr 静态库已合并为单个 libespsr.a（见 esp32s3_merged/），
#       单个归档链接时 ld 会反复扫描，能解决库之间互相引用的问题；
#       且放入 LIBS 会排在所有目标文件之后，保证符号能被解析。
env.Append(LIBPATH=[os.path.join(ESP_SR, "lib", "esp32s3_merged")])
env.Append(LIBS=["espsr"])

def _find_upload_port(env):
    """优先用 PlatformIO 变量/配置，否则自动检测 USB 串口"""
    port = env.subst("$UPLOAD_PORT")
    if not port:
        port = env.GetProjectOption("upload_port")
    if not port:
        try:
            from platformio.util import get_serial_ports
            ports = get_serial_ports()
            for p in ports:
                name = p["port"].lower()
                if "usbmodem" in name or "wchusb" in name:
                    return p["port"]
            if ports:
                return ports[0]["port"]
        except Exception:
            pass
    return port


MODEL_FLASH_BAUD = "921600"  # 模型分区烧录波特率（原生 USB-CDC 支持高速）


def _esptool(env):
    esptool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    esptool_py = os.path.join(esptool_dir, "esptool.py")
    if not os.path.isfile(esptool_py):
        raise RuntimeError("未找到 esptool.py: %s" % esptool_py)
    return esptool_py


def _model_marker_path(offset):
    return os.path.join(PROJECT_DIR, ".pio", "model_flashed_%s.marker" % offset.replace("0x", ""))


def _model_already_flashed(env, offset, bin_path):
    """本地标记法：模型文件 md5 + 分区偏移 + 串口 都没变，则跳过模型烧录。
    完全不做 esptool 连接，因此不会卡在 Connecting。
    换模型 / 换串口 / 换板子 / 执行过 clean 都会导致重新烧录；
    也可用环境变量 FORCE_MODEL_FLASH=1 强制重烧。"""
    if os.environ.get("FORCE_MODEL_FLASH") == "1":
        return False
    try:
        import hashlib
        with open(bin_path, "rb") as f:
            digest = hashlib.md5(f.read()).hexdigest()
    except OSError:
        return False
    port = _find_upload_port(env) or "?"
    marker = _model_marker_path(offset)
    try:
        with open(marker) as f:
            saved = f.read().split("|")
        return len(saved) == 3 and saved[0] == digest and saved[1] == offset and saved[2] == port
    except (OSError, ValueError):
        return False


# ---------------------------------------------------------------- 上传时烧录模型分区
def _flash_models(source, target, env):
    # 1. 生成 srmodels.bin
    build_script = os.path.join(PROJECT_DIR, "scripts", "build_srmodels.py")
    if env.Execute(sys.executable + " " + build_script):
        raise RuntimeError("生成 srmodels.bin 失败")

    bin_path = os.path.join(PROJECT_DIR, "components", "srmodels.bin")
    if not os.path.isfile(bin_path):
        raise RuntimeError("未找到 srmodels.bin")

    # 2. 从分区表解析 model 分区偏移
    parts = os.path.join(PROJECT_DIR, "partitions.csv")
    model_offset = None
    with open(parts) as f:
        for line in f:
            line = line.split("#")[0].strip()
            if not line:
                continue
            cols = [c.strip() for c in line.split(",")]
            if len(cols) >= 4 and cols[0] == "model":
                model_offset = cols[3]
                break
    if model_offset is None:
        raise RuntimeError("partitions.csv 中未找到 model 分区")

    # 3. 端口
    port = _find_upload_port(env)
    if not port:
        raise RuntimeError("未检测到上传端口，请确认板子已连接")

    # 4. 模型已烧过相同版本则跳过（本地标记判断，不做串口连接，不会卡）
    if _model_already_flashed(env, model_offset, bin_path):
        print(">> model 分区已是最新模型，跳过烧录（如需强制重烧：FORCE_MODEL_FLASH=1）")
        return

    # 5. 烧录模型分区（失败自动重试一次，规避高波特率偶发同步失败）
    cmd = (
        "%s %s --chip esp32s3 --port %s --baud %s --connect-attempts 2 write_flash -z "
        "--flash_mode keep --flash_freq keep --flash_size keep %s %s"
        % (sys.executable, _esptool(env), port, MODEL_FLASH_BAUD, model_offset, bin_path)
    )
    print(">> 烧录模型分区 @%s (baud=%s) : %s" % (model_offset, MODEL_FLASH_BAUD, bin_path))
    for attempt in (1, 2):
        if not env.Execute(cmd):
            # 烧录成功 → 写本地标记（下次跳过）
            try:
                import hashlib
                with open(bin_path, "rb") as f:
                    digest = hashlib.md5(f.read()).hexdigest()
                with open(_model_marker_path(model_offset), "w") as f:
                    f.write("%s|%s|%s" % (digest, model_offset, port))
            except OSError:
                pass
            return
        print(">> 模型烧录第 %d 次失败，重试中..." % attempt)
    raise RuntimeError("烧录模型分区失败（已重试）")

env.AddPreAction("upload", _flash_models)
