#!/usr/bin/env python3
"""Build a relocatable macOS application from an existing CMake build."""
import argparse
import json
import platform
import plistlib
from pathlib import Path
import shutil
import subprocess
import tempfile


def run(*args):
    subprocess.run([str(arg) for arg in args], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", default="build/release")
    parser.add_argument("--output", default="dist/FluentPLC Simulator.app")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    build = (root / args.build_dir).resolve()
    destination = (root / args.output).resolve()
    if platform.system() != "Darwin":
        parser.error("This packager runs on macOS")
    if destination.exists():
        parser.error("Output exists; choose a new --output path")
    cache = (build / "CMakeCache.txt").read_text()
    qt_dir = next(line.split("=", 1)[1] for line in cache.splitlines() if line.startswith("Qt6_DIR:PATH="))
    deploy = Path(qt_dir).parents[2] / "bin/macdeployqt"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".fplc-package-", dir=destination.parent) as temporary:
        app = Path(temporary) / destination.name
        shutil.copytree(build / "FluentPLCSimulator.app", app, symlinks=True)
        contents = app / "Contents"
        frameworks = contents / "Frameworks"
        frameworks.mkdir(exist_ok=True)
        native = frameworks / "libsnap7.1.dylib"
        shutil.copy2((build / "lib/libsnap7.1.dylib").resolve(), native)
        run("install_name_tool", "-id", "@rpath/libsnap7.1.dylib", native)
        server = contents / "MacOS/FluentPLCServer"
        shutil.copy2(build / "FluentPLCServer", server)
        run(deploy, app, "-always-overwrite", "-verbose=1", f"-libpath={build / 'lib'}", f"-executable={server}")
        resources = contents / "Resources"
        resources.mkdir(exist_ok=True)
        for name in ("README.md", "LICENSE", "THIRD_PARTY_NOTICES.md"):
            shutil.copy2(root / name, resources / name)
        for name in ("licenses", "examples", "docs"):
            shutil.copytree(root / name, resources / name, dirs_exist_ok=True)
        shutil.copytree(root / "patches", resources / "licenses/snap7-patches", dirs_exist_ok=True)
        # Qt's source and licensing links accompany the redistributed frameworks.
        (resources / "licenses/Qt.txt").write_text(
            "Qt 6.12: https://www.qt.io/licensing/\n"
            "Source: https://download.qt.io/archive/qt/6.12/6.12.0/single/\n"
            "Qt is redistributed under the applicable open-source license terms.\n"
            "See LGPL-3.0.txt and GPL-3.0.txt in this directory.\n"
        )
        revision = subprocess.check_output(["git", "-C", str(root / "third_party/snap7"), "rev-parse", "HEAD"], text=True).strip()
        (resources / "build-info.json").write_text(json.dumps({
            "snap7Revision": revision, "architecture": platform.machine(), "qtVersion": "6.12.0"
        }, indent=2) + "\n")
        plist_file = contents / "Info.plist"
        with plist_file.open("rb") as file:
            info = plistlib.load(file)
        info["CFBundleDisplayName"] = "FluentPLC Simulator"
        info["NSHighResolutionCapable"] = True
        with plist_file.open("wb") as file:
            plistlib.dump(info, file)
        run("codesign", "--force", "--deep", "--sign", "-", app)
        run("codesign", "--verify", "--deep", "--strict", app)
        app.rename(destination)
    print(destination)


if __name__ == "__main__":
    main()
