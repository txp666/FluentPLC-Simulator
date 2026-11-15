#!/usr/bin/env python3
"""Build a relocatable macOS application (for OTA ZIPs use scripts/package.py)."""
import argparse
import platform
import tempfile
from pathlib import Path

from package import ROOT, build_macos_app


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=Path("build/release"))
    parser.add_argument("--output", type=Path, default=Path("dist/FluentPLC Simulator.app"))
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    if platform.system() != "Darwin":
        parser.error("This packager runs on macOS")
    build = (ROOT / args.build_dir).resolve()
    destination = (ROOT / args.output).resolve()
    if destination.exists():
        parser.error("Output exists; choose a new --output path")
    version = (build / "FluentPLCSimulator-version.txt").read_text().strip()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".fplc-package-", dir=destination.parent) as temporary:
        app = build_macos_app(build, Path(temporary) / destination.name, version, args.config)
        app.rename(destination)
    print(destination)


if __name__ == "__main__":
    main()
