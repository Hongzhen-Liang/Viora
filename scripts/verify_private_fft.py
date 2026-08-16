#!/usr/bin/env python3
"""Verify the private DIT radix-2 FFT used in wake_word.cpp against the
firmware golden data. Reimplements the KWS log-mel frontend twice:
  1) reference: numpy FFT (equivalent to the original esp-dsp path)
  2) private :  the exact C implementation (float32 DIT FFT + bit-reversal)
and compares both against the quantized golden input tensor."""
import re
import numpy as np

SRC = "src"

def parse_array(path, name, dtype):
    text = open(path).read()
    m = re.search(rf"\b{name}\[\]\s*=\s*\{{(.*?)\}};", text, re.S)
    vals = re.findall(r"-?\d+(?:\.\d+)?(?:[eE][+-]?\d+)?", m.group(1))
    return np.array([float(v) for v in vals], dtype=dtype)

pcm = parse_array(f"{SRC}/hi_vesper_golden_data.cpp", "g_hi_vesper_golden_pcm", np.float32)
golden_in = parse_array(f"{SRC}/hi_vesper_golden_data.cpp", "g_hi_vesper_golden_input", np.int8)
mel_w = parse_array(f"{SRC}/hi_vesper_frontend_data.cpp", "g_hi_vesper_mel_weights", np.float32)

N = 512
FRAME = 480
STEP = 160
MEL = 40
BINS = 257
NF = 148

assert pcm.size == 24000, pcm.size
assert golden_in.size == 5920, golden_in.size
assert mel_w.size == BINS * MEL, mel_w.size

hann = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(FRAME) / FRAME)).astype(np.float32)
mel_mat = mel_w.reshape(BINS, MEL)  # [bin, mel]

# ---- private FFT exactly as the C code (float32) ----
W = np.zeros(N, dtype=np.float32)
REV = np.zeros(N, dtype=np.int32)
for i in range(N // 2):
    a = np.float32(np.float32(2.0) * np.float32(np.pi) * np.float32(i) / np.float32(N))
    W[2 * i] = np.float32(np.cos(a))
    W[2 * i + 1] = np.float32(-np.sin(a))
for i in range(N):
    r = 0
    for b in range(9):
        r = (r << 1) | ((i >> b) & 1)
    REV[i] = r

def private_fft(data):
    x = data.astype(np.float32).copy()  # interleaved re/im, length N*2
    for i in range(N):
        j = REV[i]
        if j > i:
            x[i * 2], x[j * 2] = x[j * 2], x[i * 2]
            x[i * 2 + 1], x[j * 2 + 1] = x[j * 2 + 1], x[i * 2 + 1]
    length = 2
    while length <= N:
        half = length >> 1
        step = N // length
        for base in range(0, N, length):
            for j in range(half):
                wi_ = (j * step) * 2
                wr = np.float32(W[wi_])
                wim = np.float32(W[wi_ + 1])
                p = base + j
                q = p + half
                qr = np.float32(x[q * 2])
                qi = np.float32(x[q * 2 + 1])
                tr = np.float32(np.float32(qr * wr) - np.float32(qi * wim))
                ti = np.float32(np.float32(qr * wim) + np.float32(qi * wr))
                x[q * 2] = np.float32(x[p * 2] - tr)
                x[q * 2 + 1] = np.float32(x[p * 2 + 1] - ti)
                x[p * 2] = np.float32(x[p * 2] + tr)
                x[p * 2 + 1] = np.float32(x[p * 2 + 1] + ti)
        length <<= 1
    return x

def features(use_private):
    feats = np.zeros((NF, MEL), dtype=np.float32)
    for f in range(NF):
        frame = pcm[f * STEP : f * STEP + FRAME]
        windowed = (frame.astype(np.float32) / np.float32(32768.0)) * hann
        if use_private:
            spec = np.zeros(N * 2, dtype=np.float32)
            spec[0:2*FRAME:2] = windowed
            spec = private_fft(spec)
        else:
            c = np.fft.fft(windowed, n=N)
            spec = np.empty(N * 2, dtype=np.float32)
            spec[0::2] = c.real.astype(np.float32)
            spec[1::2] = c.imag.astype(np.float32)
        power = (spec[0::2] ** 2 + spec[1::2] ** 2)[:BINS]
        energy = power @ mel_mat
        feats[f] = np.log(energy + np.float32(1e-6))
    return feats

def quantize(feats):
    x = feats.reshape(-1)
    mean = x.mean(dtype=np.float32)
    var = (x.astype(np.float64) ** 2).mean() - float(mean) ** 2
    var = max(var, 0.0)
    inv_std = 1.0 / (np.sqrt(var) + 1e-6)
    scale = 0.03131344
    zp = -23
    out = np.rint((x.astype(np.float64) - mean) * inv_std / scale).astype(np.int32) + zp
    return np.clip(out, -128, 127).astype(np.int8)

for name, use_private in [("numpy-FFT", False), ("private-FFT", True)]:
    q = quantize(features(use_private))
    delta = np.abs(q.astype(np.int16) - golden_in.astype(np.int16))
    print(f"{name}: max_delta={delta.max()} mean_delta={delta.mean():.4f} "
          f"mismatch_count={(delta != 0).sum()}/5920")
    if delta.max() <= 8:
        print(f"{name}: PASS (<=8 tolerance)")
    else:
        print(f"{name}: FAIL")
