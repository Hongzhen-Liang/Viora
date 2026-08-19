#!/usr/bin/env python3
"""Evaluate a trained model using the firmware's actual detection rule:
5-frame sliding-window AVERAGE > cutoff (not single-frame max)."""
import sys
from pathlib import Path
import numpy as np, tensorflow as tf

model_path, feat_dir, window_size = sys.argv[1], Path(sys.argv[2]), int(sys.argv[3]) if len(sys.argv)>3 else 5
interp = tf.lite.Interpreter(model_path=str(model_path)); interp.allocate_tensors()
inp, out = interp.get_input_details()[0], interp.get_output_details()[0]
stride = inp["shape"][1]
rows = []
for feat_file in sorted(feat_dir.glob("*.feat")):
    raw = np.fromfile(feat_file, dtype=np.int8); feats = raw.reshape(-1, inp["shape"][2])
    buf = np.zeros((1, stride, inp["shape"][2]), dtype=np.int8); step = 0; probs = []
    for f in feats:
        buf[0, step % stride] = f; step += 1
        if step % stride == 0:
            interp.set_tensor(inp["index"], buf); interp.invoke()
            probs.append(interp.get_tensor(out["index"])[0][0] / 255.0)
    # sliding window average
    avgs = []
    for i in range(len(probs) - window_size + 1):
        avgs.append(np.mean(probs[i:i+window_size]))
    name = feat_file.name.replace(".feat", ""); is_pos = name.startswith(("pos","hv"))
    rows.append((name, is_pos, max(avgs) if avgs else 0.0, max(probs) if probs else 0.0))
print(f"{'clip':<18} {'type':<5} {'max_avg5':>9} {'max_single':>10}")
for name, is_pos, avg, mx in rows:
    print(f"{name:<18} {'POS' if is_pos else 'NEG':<5} {avg:9.3f} {mx:10.3f}")
pos = [r[2] for r in rows if r[1]]; neg = [r[2] for r in rows if not r[1]]
print(f"\nPOS avg5 range [{min(pos):.3f}, {max(pos):.3f}]   NEG avg5 max {max(neg):.3f}")
