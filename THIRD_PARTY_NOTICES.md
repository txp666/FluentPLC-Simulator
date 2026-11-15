# Third-party components

FluentPLC Simulator is licensed under GPL-3.0. The repository contains pinned
submodules; initialize them with `git submodule update --init --recursive`.

| Component | Source | Pinned revision | License |
| --- | --- | --- | --- |
| FluentQtWidgets | https://github.com/txp666/FluentQtWidgets | 4f0ec11508f1b04f28af268497416657e4557720 | GPL-3.0 |
| Snap7 | https://github.com/davenardella/snap7 | 30f37da3114024a71ba93f7fd855c680b97a406f | LGPL-3.0-or-later |
| Qt | https://www.qt.io/ | 6.12 | License terms of the installed Qt distribution |

Snap7 copyright: (C) 2013–2025 Davide Nardella. Its native shared library and
official C++ wrapper are built from the pinned sources. On modern macOS, the
documented `patches/snap7-macos.patch` fixes monotonic timing and socket SIGPIPE
handling in a separate build copy. The submodule remains unchanged. The portable
CMake build recipe is in this repository. The LGPL and GPL texts are distributed
in `licenses/`; complete corresponding Snap7 sources are available through the
pinned submodule. The project and its native dependencies are independent of
Siemens and are not certified or supported by Siemens.
