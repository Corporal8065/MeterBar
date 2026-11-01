#!/usr/bin/env python3
"""Download the two caption models into the local model cache.

Explicit user-invoked setup step (spec section 3.5): ordinary
caption operation NEVER downloads — the worker is handed these
local directories and reports "Setup needed" when they are absent.

Usage: download_models.py [models_dir]
Default models_dir: %LOCALAPPDATA%\\MeterBar\\Captions\\models
"""
import os
import sys

MODELS = {
    "small": "Systran/faster-whisper-small",
    "base": "Systran/faster-whisper-base",
}


def main():
    if len(sys.argv) > 1:
        models_dir = sys.argv[1]
    else:
        base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
        models_dir = os.path.join(base, "MeterBar", "Captions", "models")
    from huggingface_hub import snapshot_download
    for size, repo in MODELS.items():
        dest = os.path.join(models_dir, size)
        if os.path.isfile(os.path.join(dest, "model.bin")):
            print(f"{size}: already present at {dest}")
            continue
        print(f"{size}: downloading {repo} -> {dest} (one-time) ...")
        snapshot_download(repo, local_dir=dest)
        print(f"{size}: done")
    print("Caption models ready in", models_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
