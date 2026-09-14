#!/usr/bin/env python3
"""Build a browser preview from the firmware bitmaps and animation timings."""
import base64
import json
import re
from pathlib import Path
from gen_orchid_expressions import EXPRESSIONS, WIDTH, HEIGHT, pack_bitmap

ROOT = Path(__file__).resolve().parents[1]
schedule = (ROOT / "src/display/expression_animation.h").read_text()
timelines = {}
for name, body in re.findall(r"Frame k(\w+)\[\] = \{(.*?)\};", schedule, re.S):
    timelines[name] = [[int(a), int(b)] for a, b in re.findall(r"\{(\d+),\s*(\d+)\}", body)]
timelines["Sleep"] = [[12, 6000]]
data = {
    "width": WIDTH, "height": HEIGHT,
    "names": [name for _, name in EXPRESSIONS],
    "frames": [base64.b64encode(bytes(pack_bitmap(ROOT / "assets" / name))).decode()
               for _, name in EXPRESSIONS],
    "timelines": timelines,
    "regions": {name: [int(v) for v in values.split(",")]
                for name, values in re.findall(r"k(Face|Sleep)Region\[\] = \{(.*?)\}", schedule)},
}
html = '''<!doctype html><html lang="zh-CN"><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Viora · 表情与动画</title>
<style>
*{box-sizing:border-box}body{margin:0;background:#f3f4ee;color:#25372e;font:16px system-ui,sans-serif}
main{max-width:1100px;margin:48px auto;padding:0 24px}h1{font-size:36px;margin-bottom:12px}p{color:#63736a;line-height:1.7}
.stage{display:grid;grid-template-columns:1fr 1fr;gap:24px;margin:28px 0;background:white;border-radius:24px;padding:28px}
canvas{width:100%;max-width:460px;height:auto;image-rendering:pixelated;background:white}
button{padding:11px 17px;border:1px solid #d1d9d0;border-radius:24px;background:white;color:inherit;cursor:pointer;margin:4px}
button[aria-pressed=true]{background:#253f32;color:white}#frame{font-variant-numeric:tabular-nums;margin:24px 0}
#gallery{display:grid;grid-template-columns:repeat(3,1fr);gap:16px}figure{margin:0;background:white;border-radius:16px;overflow:hidden}
img{width:100%;display:block}figcaption{padding:12px 16px;font-size:14px}h2{margin-top:40px}small{color:#63736a}
@media(max-width:700px){.stage{grid-template-columns:1fr}#gallery{grid-template-columns:repeat(2,1fr)}}
</style><main><small>VIORA / ORCHID EXPRESSIONS</small><h1>同一朵兰花，自然回应。</h1>
<p>以 1 idle_normal.png 为统一参考：简洁笑嘴、轻盈花瓣纹理和克制的眼神变化。</p>
<section class="stage"><canvas id="screen" width="230" height="210"></canvas><div>
<h2 style="margin-top:0">动画预览</h2><p>这里播放的是固件使用的黑白点阵与时序；实机刷新延迟需在设备上确认。</p>
<div id="states"></div><p id="frame"></p><button id="pause">暂停</button><button id="restart">重新播放</button>
<p>眨眼闭合 160 毫秒。感知只播放一次后保持关注；夜间安静休息。</p></div></section>
<h2>完整表情 · 13 张</h2><p>第 1 张保留原图，其余 12 张沿用相同造型。</p><div id="gallery"></div></main>
<script>
const data = __DATA__;
const labels={Idle:'待机',Sensing:'感知',Listening:'聆听',Thinking:'思考',Speaking:'说话',Sleep:'睡眠'};
const frames=data.frames.map(s=>Uint8Array.from(atob(s),c=>c.charCodeAt(0)));
const canvas=document.querySelector('#screen'),ctx=canvas.getContext('2d');
let state='Idle',epoch=performance.now(),paused=false,elapsed=0,last=-1;
for(const name of Object.keys(labels)){const b=document.createElement('button');b.textContent=labels[name];b.dataset.state=name;b.onclick=()=>{state=name;epoch=performance.now();elapsed=0;last=-1;updateButtons()};document.querySelector('#states').append(b)}
function updateButtons(){document.querySelectorAll('[data-state]').forEach(b=>b.setAttribute('aria-pressed',String(b.dataset.state===state)))}updateButtons();
function draw(i){const pixels=ctx.createImageData(data.width,data.height),bytes=frames[i],stride=Math.ceil(data.width/8);
for(let y=0;y<data.height;y++)for(let x=0;x<data.width;x++){const inside=r=>x>=r[0]&&y>=r[1]&&x<r[2]&&y<r[3];const layer=inside(data.regions.Face)||(i===12&&inside(data.regions.Sleep))?bytes:frames[0];const v=layer[y*stride+(x>>3)]&(1<<(x%8))?0:255;const o=(y*data.width+x)*4;pixels.data[o]=pixels.data[o+1]=pixels.data[o+2]=v;pixels.data[o+3]=255}ctx.putImageData(pixels,0,0)}
function tick(now){if(!paused)elapsed=now-epoch;const seq=data.timelines[state],total=seq.reduce((a,f)=>a+f[1],0);let t=state==='Sensing'?Math.min(elapsed,total-1):elapsed%total,i=seq.at(-1)[0];for(const f of seq){if(t<f[1]){i=f[0];break}t-=f[1]}if(i!==last){draw(i);last=i}document.querySelector('#frame').textContent=labels[state]+' · '+data.names[i]+' · '+(elapsed/1000).toFixed(1)+' 秒';requestAnimationFrame(tick)}requestAnimationFrame(tick);
document.querySelector('#pause').onclick=()=>{paused=!paused;if(!paused)epoch=performance.now()-elapsed;document.querySelector('#pause').textContent=paused?'继续':'暂停'};
document.querySelector('#restart').onclick=()=>{epoch=performance.now();elapsed=0};
data.names.forEach(name=>{const f=document.createElement('figure'),img=document.createElement('img'),c=document.createElement('figcaption');img.src=encodeURIComponent(name);img.alt=name;img.loading='lazy';c.textContent=name;f.append(img,c);document.querySelector('#gallery').append(f)});
</script></html>'''
(ROOT / "assets/animation-preview.html").write_text(html.replace("__DATA__", json.dumps(data)), encoding="utf-8")
print("assets/animation-preview.html")
