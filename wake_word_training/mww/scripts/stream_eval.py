#!/usr/bin/env python3
"""Evaluate a trained streaming wake-word model on PC features.

Runs the streaming model over pre-computed feature files (produced by the
compiled microfeatures frontend CLI) and reports the max probability per clip
so a sensible detection cutoff can be chosen.

Usage:
    conda run -n mww python stream_eval.py MODEL.tflite FEATURE_DIR [cutoff]

FEATURE_DIR contains .feat files; prefix positive ones with `pos_`/`hv_` and
negative ones with `neg_`. Prints a table plus a suggested cutoff.
"""
import sys
from pathlib import Path

import numpy as np
import tensorflow as tf

model_path = Path(sys.argv[1])
feat_dir = Path(sys.argv[2])
window_size = int(sys.argv[3]) if len(sys.argv) > 3 else 5

interp = tf.lite.Interpreter(model_path=str(model_path))
interp.allocate_tensors()
inp = interp.get_input_details()[0]
out = interp.get_output_details()[0]
stride = inp["shape"][1]

rows = []
for feat_file in sorted(feat_dir.glob("*.feat")):
    raw = np.fromfile(feat_file, dtype=np.int8)
    if raw.size == 0:
        continue
    feats = raw.reshape(-1, inp["shape"][2])
    buf = np.zeros((1, stride, inp["shape"][2]), dtype=np.int8)
    step = 0
    probs = []
    for f in feats:
        buf[0, step % stride] = f
        step += 1
        if step % stride == 0:
            interp.set_tensor(inp["index"], buf)
            interp.invoke()
            probs.append(interp.get_tensor(out["index"])[0][0] / 255.0)
    name = feat_file.name.replace(".feat", "")
    is_pos = name.startswith(("pos", "hv"))
    rows.append((name, is_pos, max(probs) if probs else 0.0, len(probs)))

print(f"{'clip':<20} {'type':<8} {'max_p':>7} {'infs':>5}")
for name, is_pos, mx, n in rows:
    print(f"{name:<20} {'POS' if is_pos else 'NEG':<8} {mx:7.3f} {n:5d}")

pos = [r[2] for r in rows if r[1]]
neg = [r[2] for r in rows if not r[1]]
if pos and neg:
    # A cutoff between the min positive and max negative works best; suggest
    # the midpoint of the gap.
    lo, hi = min(pos), max(neg)
    if hi < lo:
        suggestion = round((lo + hi) / 2, 2)
        print(f"\npos range [{min(pos):.3f}, {max(pos):.3f}], "
              f"neg max {max(neg):.3f} -> suggested cutoff {suggestion}")
    else:
        print(f"\nWARNING: negatives overlap positives (neg_max {max(neg):.3f} "
              f">= pos_min {min(pos):.3f})")
