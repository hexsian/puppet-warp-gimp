# Prebuilt binaries

This folder contains prebuilt shared libraries for the ARAP solver and
preview renderer used by `puppet-warp.py`. The Python plug-in loads the
appropriate library for the current platform automatically.

| Platform | File | Notes |
| --- | --- | --- |
| Linux x86_64 | `linux-x86_64/puppet-warp.so` | Needs glibc ≥ 2.38 (Ubuntu 24.04, Fedora 39+, Arch, Debian 13+) |
| Windows x64 | `windows-x64/puppet-warp.dll` | Needs Windows 10+ (UCRT). Depends only on DLLs shipped with GIMP |

Both binaries are built from the C sources in the repository root
(`puppet-warp.c` and `arap_grid.c`). See the main `README.md` for build
instructions if you need to rebuild them for your system.
