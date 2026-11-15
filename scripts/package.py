#!/usr/bin/env python3
"""Package FluentPLC Simulator for OTA, with a platform package and SHA-256 sidecar.

Derived from FluentPLC Simulator's packager. The packages ship their own
Qt and Snap7 libraries.
"""
import argparse
import hashlib
import json
import os
import platform
import plistlib
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Variables that point Qt at the build machine's kit. A user's machine has none of them, and
# Qt searches QT_PLUGIN_PATH (which install-qt-action sets) before the paths in qt.conf. On
# Linux install-qt-action also puts the kit's lib directory on LD_LIBRARY_PATH, which the
# loader searches before a RUNPATH.
BUILD_ONLY_ENV = ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                  "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "LD_LIBRARY_PATH")


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def cache_value(build_dir, key):
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    match = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", cache, re.MULTILINE)
    return match.group(1).strip() if match else ""


def qt_bin_dir(build_dir):
    """The Qt kit the build used: <prefix>/lib/cmake/Qt6 -> <prefix>/bin."""
    qt_dir = cache_value(build_dir, "Qt6_DIR")
    if not qt_dir:
        sys.exit("Qt6_DIR not found in CMakeCache.txt; is this a configured build directory?")
    prefix = Path(qt_dir).resolve().parent.parent.parent
    for candidate in (prefix / "bin", prefix / "libexec"):
        if candidate.is_dir():
            return candidate
    sys.exit(f"no Qt bin directory under {prefix}")


def find_built(build_dir, name, config):
    """A target's output in a single- or multi-config build tree."""
    for candidate in (build_dir / name, build_dir / config / name):
        if candidate.exists():
            return candidate
    sys.exit(f"{name} not found in {build_dir}; build the {config} configuration first")


def user_env(**extra):
    """The environment to run packaged programs in: what they would see on a user's machine."""
    env = {key: value for key, value in os.environ.items() if key not in BUILD_ONLY_ENV}
    env.update(extra)
    return env


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    out = path.with_name(path.name + ".sha256")
    # LF even on Windows: `shasum -c` reads a CR as part of the file name.
    out.write_text(f"{digest.hexdigest()}  {path.name}\n", encoding="utf-8", newline="\n")
    return out


def macho_files(bundle):
    """Executables, libraries and plugins in a bundle.

    By content: framework binaries are neither executable nor named .dylib.
    """
    for path in bundle.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        with open(path, "rb") as stream:
            magic = stream.read(4)
        if magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xce\xfa\xed\xfe"):
            yield path


def otool(*args):
    return subprocess.run(["otool", *map(str, args)], capture_output=True, text=True).stdout


# otool lists a universal binary once per architecture, each under a "<file> (architecture
# x):" header, while one install_name_tool call changes every architecture. So the helpers
# drop the headers and return each name once; deleting an rpath twice is an error.

def macho_install_name(path):
    lines = [line.strip() for line in otool("-D", path).splitlines()[1:] if not line.endswith(":")]
    return lines[0] if lines else ""


def macho_dependencies(path):
    own = macho_install_name(path)
    deps = [line.strip().split(" (compatibility")[0] for line in otool("-L", path).splitlines()[1:]
            if not line.endswith(":")]
    return list(dict.fromkeys(dep for dep in deps if dep and dep != own))


def macho_rpaths(path):
    lines = otool("-l", path).splitlines()
    return list(dict.fromkeys(lines[i + 2].split()[1] for i, line in enumerate(lines)
                              if line.strip() == "cmd LC_RPATH" and i + 2 < len(lines)))


def fix_macos_load_paths(bundle):
    """Point the bundle at its own Frameworks only, and only through @rpath.

    CMake leaves the build machine's library directory (/opt/homebrew/lib for Homebrew's Qt)
    as the executables' only rpath, and macdeployqt keeps it. Libraries that load each other
    through @rpath then still resolve on the build machine and fail everywhere else.

    Both executables use the same frameworks through their own relative rpaths.
    """
    frameworks = "@executable_path/../Frameworks"
    for path in macho_files(bundle):
        for rpath in macho_rpaths(path):
            if not rpath.startswith("@"):
                run(["install_name_tool", "-delete_rpath", rpath, path])
        in_macos = path.parent.name == "MacOS"
        if in_macos and frameworks not in macho_rpaths(path):
            run(["install_name_tool", "-add_rpath", frameworks, path])
        own = macho_install_name(path)
        changes = []
        if own.startswith("/") and path.parent.name == "Frameworks":
            changes += ["-id", f"@rpath/{path.name}"]
        elif own.startswith(frameworks + "/"):
            changes += ["-id", "@rpath/" + own[len(frameworks) + 1:]]
        for dep in macho_dependencies(path):
            if dep.startswith(frameworks + "/"):
                changes += ["-change", dep, "@rpath/" + dep[len(frameworks) + 1:]]
        if changes:
            run(["install_name_tool", *changes, path])


def macos_linked_outside(bundle, forbidden_prefix):
    """Binaries that would still load something from the build machine's Qt prefix."""
    offenders = []
    for path in macho_files(bundle):
        outside = [dep for dep in macho_dependencies(path) if dep.startswith(forbidden_prefix)]
        outside += [rpath for rpath in macho_rpaths(path) if not rpath.startswith("@")]
        if outside:
            offenders.append(f"{path.relative_to(bundle)}: {', '.join(outside)}")
    return offenders


def vc_runtime_dlls():
    """The Visual C++ runtime DLLs to ship beside the executables.

    windeployqt only deploys them from a developer prompt (VCINSTALLDIR), and a zip has no
    installer to put the redistributable on a PC that lacks it.
    """
    roots = []
    if os.environ.get("VCToolsRedistDir"):
        roots.append(Path(os.environ["VCToolsRedistDir"]))
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                               capture_output=True, text=True).stdout.strip()
        if found:
            roots += sorted((Path(found) / "VC" / "Redist" / "MSVC").glob("*"), reverse=True)
    for root in roots:
        crt = sorted((root / "x64").glob("Microsoft.VC*.CRT"), reverse=True)
        if crt:
            dlls = sorted(crt[0].glob("*.dll"))
            if dlls:
                return dlls
    sys.exit("Visual C++ runtime not found: run from a Visual Studio developer prompt "
             "or install the Visual Studio C++ build tools")


DEB_PACKAGE = "fluentplc-simulator"
INSTALL_ROOT = "/opt/FluentPLCSimulator"  # where UpdateInstaller expects an installation it can update
APP_ID = "io.github.txp666.FluentPLCSimulator"

# Plugin folders an application may load on Linux. generic is left out: Qt loads its input plugins only when told
# to (-plugin, QT_QPA_GENERIC_PLUGINS), and eglfs and linuxfb read evdev and libinput themselves.
LINUX_PLUGIN_DIRS = ("platforms", "wayland-shell-integration", "wayland-graphics-integration-client",
                     "wayland-decoration-client", "xcbglintegrations", "egldeviceintegrations", "platformthemes",
                     "platforminputcontexts", "imageformats", "iconengines", "tls", "networkinformation")
# Native desktop plugins and offscreen rendering for the --screenshot option.
LINUX_PLATFORMS = ("libqxcb.so", "libqwayland", "libqoffscreen.so")
# Qt's xcb helpers. The matching libxkbcommon and -x11 libraries stay together.
LINUX_BUNDLED_SYSTEM_LIBS = ("libxcb-cursor.so.", "libxcb-icccm.so.", "libxcb-image.so.", "libxcb-keysyms.so.",
                             "libxcb-render-util.so.", "libxcb-util.so.", "libxkbcommon-x11.so.", "libxkbcommon.so.")
# Other libraries are installed by the system package manager.
LINUX_SYSTEM_PACKAGES = (
    "libgl1", "libopengl0", "libegl1", "libfontconfig1", "libfreetype6", "libglib2.0-0t64", "libdbus-1-3",
    "libudev1", "libssl3t64", "libgssapi-krb5-2", "libxkbcommon0", "libx11-6", "libx11-xcb1", "libxext6",
    "libxfixes3", "libxi6", "libxrender1", "libsm6", "libice6", "libxcb1", "libxcb-glx0", "libxcb-randr0",
    "libxcb-render0", "libxcb-shape0", "libxcb-shm0", "libxcb-sync1", "libxcb-xfixes0", "libxcb-xinerama0",
    "libxcb-xinput0", "libxcb-xkb1", "libwayland-client0", "libwayland-cursor0", "libwayland-egl1")
# apt and pkexec install updates; the binary is built against Ubuntu 24.04's glibc.
DEB_DEPENDS = ("apt", "pkexec", "libc6 (>= 2.39)", "libstdc++6", "libgcc-s1") + LINUX_SYSTEM_PACKAGES + (
    "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0", "libxcb-keysyms1", "libxcb-render-util0", "libxcb-util1",
    "libxkbcommon-x11-0")
# FluentQtWidgets' frameless title bar is proven on X11, which Wayland desktops run through XWayland.
LAUNCHER = f"""#!/bin/sh
if [ -z "${{QT_QPA_PLATFORM:-}}" ]; then
    QT_QPA_PLATFORM=xcb
    export QT_QPA_PLATFORM
fi
exec {INSTALL_ROOT}/FluentPLCSimulator "$@"
"""


def elf_files(folder):
    for path in sorted(folder.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        with open(path, "rb") as stream:
            if stream.read(4) == b"\x7fELF":
                yield path


def elf_runpath(path):
    return subprocess.run(["patchelf", "--print-rpath", str(path)], capture_output=True, text=True,
                          check=True).stdout.strip()


def ldd(path):
    """Every library a binary loads, its dependencies' included: soname -> path on this machine, or None."""
    out = subprocess.run(["ldd", str(path)], capture_output=True, text=True, env=user_env()).stdout
    found = re.finditer(r"^\s*(\S+) => (?:(/\S+) \(0x|not found)", out, re.MULTILINE)
    return {match.group(1): match.group(2) for match in found}


def copy_linux_plugins(qt_plugins, target):
    """Copies the plugins of LINUX_PLUGIN_DIRS that Qt has; returns their sources and paths in the package."""
    copied = []
    for folder in LINUX_PLUGIN_DIRS:
        for source in sorted((qt_plugins / folder).glob("*.so")):
            if folder == "platforms" and not source.name.startswith(LINUX_PLATFORMS):
                continue
            (target / folder).mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target / folder / source.name)
            copied.append((source, f"plugins/{folder}/{source.name}"))
    if not any(rel.startswith("plugins/platforms/libqxcb") for _, rel in copied):
        sys.exit(f"no xcb platform plugin in {qt_plugins}")
    return copied


def bundle_linux_libs(binaries, lib_dir, qt_prefix):
    """Copies into lib_dir, by soname, what the binaries load from Qt and the system libraries listed above.

    Returns those sonames. The binaries are the build's and Qt's own, whose RUNPATHs still find Qt in its prefix.
    """
    needed = {}
    for binary in binaries:
        needed.update(ldd(binary))
    names = []
    for soname, resolved in sorted(needed.items()):
        listed = soname.startswith(LINUX_BUNDLED_SYSTEM_LIBS)
        if resolved is None:
            if listed:
                sys.exit(f"{soname} is not installed here, and the package carries it")
            # Needed by a plugin whose system library is missing here (GTK for gtk3). The plugin still ships, and
            # Qt skips it wherever that library is missing too.
            print(f"not on this machine, left to the system: {soname}")
            continue
        if listed or soname.startswith("libsnap7.so") or Path(resolved).resolve().is_relative_to(qt_prefix):
            if not (lib_dir / soname).exists():
                lib_dir.mkdir(exist_ok=True)
                shutil.copy2(Path(resolved).resolve(), lib_dir / soname)
            names.append(soname)
    return names


def fix_linux_runpaths(stage):
    """Point each bundled library at its own folder and each plugin at lib/.

    Qt's libraries and plugins come with $ORIGIN and $ORIGIN/../../lib. The system libraries bundled with them
    come with none, and a RUNPATH only serves the object that carries it, not the libraries that object loads.
    """
    for path in elf_files(stage / "lib"):
        if elf_runpath(path) != "$ORIGIN":
            run(["patchelf", "--set-rpath", "$ORIGIN", path])
    for path in elf_files(stage / "plugins"):
        if "$ORIGIN/../../lib" not in elf_runpath(path).split(":"):
            run(["patchelf", "--set-rpath", "$ORIGIN/../../lib", path])


def linux_linked_outside(stage, qt_prefix):
    """Binaries with a RUNPATH outside the package, or that miss a library Qt has."""
    offenders = []
    for path in elf_files(stage):
        outside = [entry for entry in elf_runpath(path).split(":") if entry and not entry.startswith("$ORIGIN")]
        for soname, resolved in ldd(path).items():
            if resolved is None and (qt_prefix / "lib" / soname).exists():
                outside.append(f"{soname} not found")
            elif resolved and Path(resolved).resolve().is_relative_to(qt_prefix):
                outside.append(resolved)
        if outside:
            offenders.append(f"{path.relative_to(stage)}: {', '.join(outside)}")
    return offenders


def package_metadata(destination, version):
    """The manifest distinguishes a deployed app from a CMake build directory."""
    destination.mkdir(parents=True, exist_ok=True)
    for name in ("README.md", "README_zh.md", "LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(ROOT / name, destination / name)
    for name in ("licenses", "examples", "docs"):
        shutil.copytree(ROOT / name, destination / name, dirs_exist_ok=True)
    shutil.copytree(ROOT / "patches", destination / "licenses/snap7-patches", dirs_exist_ok=True)
    (destination / "licenses/Qt.txt").write_text(
        "Qt 6.12: https://www.qt.io/licensing/\n"
        "Source: https://download.qt.io/archive/qt/6.12/6.12.0/single/\n"
        "See LGPL-3.0.txt and GPL-3.0.txt in this directory.\n", encoding="utf-8")
    revision = subprocess.check_output(
        ["git", "-C", str(ROOT / "third_party/snap7"), "rev-parse", "HEAD"], text=True).strip()
    (destination / "fplc-install.json").write_text(json.dumps({
        "formatVersion": 1, "application": APP_ID, "version": version,
        "snap7Revision": revision, "qtVersion": "6.12.0"
    }, indent=2) + "\n", encoding="utf-8")


def build_macos_app(build_dir, destination, version, config="Release"):
    if destination.exists():
        sys.exit(f"output exists: {destination}; choose another output path")
    destination.parent.mkdir(parents=True, exist_ok=True)
    app = destination
    shutil.copytree(find_built(build_dir, "FluentPLCSimulator.app", config), app, symlinks=True)
    contents = app / "Contents"
    frameworks = contents / "Frameworks"
    frameworks.mkdir(exist_ok=True)
    native = frameworks / "libsnap7.1.dylib"
    shutil.copy2((build_dir / "lib/libsnap7.1.dylib").resolve(), native)
    run(["install_name_tool", "-id", "@rpath/libsnap7.1.dylib", native])
    server = contents / "MacOS/FluentPLCServer"
    shutil.copy2(find_built(build_dir, "FluentPLCServer", config), server)
    run([qt_bin_dir(build_dir) / "macdeployqt", app, "-always-overwrite", "-verbose=1",
         f"-libpath={build_dir / 'lib'}", f"-executable={server}"])
    fix_macos_load_paths(app)
    resources = contents / "Resources"
    package_metadata(resources, version)
    plist = contents / "Info.plist"
    with plist.open("rb") as stream:
        info = plistlib.load(stream)
    info.update(CFBundleDisplayName="FluentPLC Simulator", CFBundleDevelopmentRegion="en",
                CFBundleLocalizations=["en", "zh-Hans"], NSHighResolutionCapable=True)
    with plist.open("wb") as stream:
        plistlib.dump(info, stream)
    offenders = macos_linked_outside(app, str(qt_bin_dir(build_dir).parent))
    if offenders:
        sys.exit("not self-contained:\n  " + "\n  ".join(offenders))
    run(["codesign", "--force", "--deep", "--sign", "-", app])
    run(["codesign", "--verify", "--deep", "--strict", app])
    return app


def package_macos(build_dir, output_dir, version, config):
    stage = output_dir / "stage"
    stage.mkdir(parents=True)
    app = build_macos_app(build_dir, stage / "FluentPLC Simulator.app", version, config)
    architectures = cache_value(build_dir, "CMAKE_OSX_ARCHITECTURES").split(";")
    arch = "universal" if {"arm64", "x86_64"}.issubset(architectures) else platform.machine()
    archive = output_dir / f"FluentPLC-Simulator-v{version}-macos-{arch}.zip"
    run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", app, archive])
    return archive


def package_windows(build_dir, output_dir, version, config):
    name = f"FluentPLC-Simulator-v{version}-windows-x64"
    stage = output_dir / "stage" / name
    stage.mkdir(parents=True)
    for name in ("FluentPLCSimulator.exe", "FluentPLCServer.exe"):
        shutil.copy2(find_built(build_dir, name, config), stage)
    shutil.copy2(find_built(build_dir, "snap7.dll", config), stage)
    run([qt_bin_dir(build_dir) / "windeployqt.exe", "--release", "--no-translations",
         "--no-system-d3d-compiler", "--no-opengl-sw", "--no-compiler-runtime", stage / "FluentPLCSimulator.exe"])
    for dll in vc_runtime_dlls():
        shutil.copy2(dll, stage)
    package_metadata(stage, version)
    return Path(shutil.make_archive(str(output_dir / stage.name), "zip", stage.parent, stage.name))


def build_deb(stage, deb, version, architecture):
    root = deb.parent / "deb-root"
    shutil.copytree(stage, root / INSTALL_ROOT.lstrip("/"), symlinks=True)
    launcher = root / "usr/bin/fluentplc-simulator"
    launcher.parent.mkdir(parents=True)
    launcher.write_text(LAUNCHER, encoding="utf-8")
    launcher.chmod(0o755)
    applications = root / "usr/share/applications"
    applications.mkdir(parents=True)
    (applications / f"{APP_ID}.desktop").write_text(
        "[Desktop Entry]\nType=Application\nName=FluentPLC Simulator\n"
        f"Comment=S7 communication test bench\nExec=fluentplc-simulator %f\nIcon={APP_ID}\n"
        "Terminal=false\nCategories=Development;Electronics;\n", encoding="utf-8")
    icons = root / "usr/share/icons/hicolor/512x512/apps"
    icons.mkdir(parents=True)
    shutil.copy2(ROOT / "resources/logo.png", icons / f"{APP_ID}.png")
    installed = sum(p.stat().st_size for p in root.rglob("*") if p.is_file() and not p.is_symlink())
    (root / "DEBIAN").mkdir()
    (root / "DEBIAN/control").write_text(f"""Package: {DEB_PACKAGE}
Version: {version}
Architecture: {architecture}
Maintainer: txp <771454616@qq.com>
Installed-Size: {(installed + 1023) // 1024}
Section: electronics
Priority: optional
Homepage: https://github.com/txp666/FluentPLC-Simulator
Depends: {", ".join(DEB_DEPENDS)}
Description: S7 communication and memory simulator
 Test HMI and S7 clients with a Snap7 server, typed variables and simple rules.
""", encoding="utf-8")
    run(["dpkg-deb", "--root-owner-group", "--build", root, deb])
    shutil.rmtree(root)


def package_linux(build_dir, output_dir, version, config):
    qt_prefix = qt_bin_dir(build_dir).parent.resolve()
    arch, deb_arch = {"x86_64": ("x64", "amd64"), "aarch64": ("arm64", "arm64")}.get(platform.machine(), (None, None))
    if not arch:
        sys.exit(f"no Linux package for {platform.machine()}")
    stage = output_dir / "stage/FluentPLCSimulator"
    stage.mkdir(parents=True)
    binaries = [find_built(build_dir, name, config) for name in ("FluentPLCSimulator", "FluentPLCServer")]
    plugins = copy_linux_plugins(qt_prefix / "plugins", stage / "plugins")
    bundle_linux_libs(binaries + [source for source, _ in plugins], stage / "lib", qt_prefix)
    for program in binaries:
        shutil.copy2(program, stage / program.name)
        run(["patchelf", "--set-rpath", "$ORIGIN/lib", stage / program.name])
    fix_linux_runpaths(stage)
    (stage / "qt.conf").write_text("[Paths]\nPrefix = .\nLibraries = lib\nPlugins = plugins\n", encoding="utf-8")
    package_metadata(stage, version)
    offenders = linux_linked_outside(stage, qt_prefix)
    if offenders:
        sys.exit("not self-contained:\n  " + "\n  ".join(offenders))
    deb = output_dir / f"FluentPLC-Simulator-v{version}-linux-{arch}.deb"
    build_deb(stage, deb, version, deb_arch)
    return deb


def main():
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/release")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    parser.add_argument("--config", default="Release")
    args = parser.parse_args()
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    if (output / "stage").exists():
        sys.exit("the output directory contains a stage folder; choose a new --output-dir")
    version = (build / "FluentPLCSimulator-version.txt").read_text().strip()
    packager = {"darwin": package_macos, "win32": package_windows, "linux": package_linux}.get(sys.platform)
    if not packager:
        sys.exit(f"no packager for {sys.platform}")
    package = packager(build, output, version, args.config)
    checksum = sha256_file(package)
    shutil.rmtree(output / "stage")
    print(f"packaged {package}\nchecksum {checksum}")


if __name__ == "__main__":
    main()
