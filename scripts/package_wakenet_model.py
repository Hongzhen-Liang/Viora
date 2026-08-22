#!/usr/bin/env python3
"""Package the selected ESP-SR WakeNet model for the Arduino model partition."""

import argparse
import shutil
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    # Reuse Espressif's v1 model container format.
    import sys
    sys.path.insert(0, str(args.source.parent.parent))
    from pack_model import pack_models

    with tempfile.TemporaryDirectory() as temp_dir:
        model_root = Path(temp_dir) / args.source.name
        shutil.copytree(args.source, model_root)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        pack_models(str(model_root.parent), str(args.output))


if __name__ == "__main__":
    main()
