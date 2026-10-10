import argparse
import subprocess
import sys
from pathlib import Path


def find_engine():
    root = Path(__file__).resolve().parents[1]
    executable = "symvoice-audio.exe" if sys.platform == "win32" else "symvoice-audio"
    candidates = (
        root / "build" / executable,
        root / "build" / "Release" / executable,
        root / "build" / "Debug" / executable,
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser(prog="symvoice")
    commands = parser.add_subparsers(dest="command", required=True)
    prepare = commands.add_parser("prepare", help="convert a voice recording to mono 16 kHz PCM WAV")
    prepare.add_argument("input", type=Path)
    prepare.add_argument("output", type=Path)
    prepare.add_argument("--engine", type=Path, help="path to the native symvoice-audio executable")
    args = parser.parse_args()

    engine = args.engine or find_engine()
    if engine is None or not engine.is_file():
        parser.error("native engine not found; build it with CMake or pass --engine")
    try:
        subprocess.run([str(engine), str(args.input), str(args.output)], check=True)
    except subprocess.CalledProcessError as error:
        return error.returncode
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
