# Puppet Warp (ARAP) for GIMP 3

A non-destructive **puppet warp** filter for GIMP 3. Drop pins on a layer, drag
them, and the image deforms *as rigidly as possible* (ARAP: as-rigid-as-possible
deformation). Pins can also rotate and scale. The warp is stored as a regular
GIMP layer filter, so you can reopen and edit it at any time.

<!-- Add a screenshot or GIF here once you have one, e.g.:
![Puppet Warp dialog](docs/screenshot.png)
-->

## Features

- Click to add pins, drag to deform; Shift snaps a drag to an axis
- Rotate and scale around any pin (orange ring / Alt+wheel / Ctrl+wheel)
- Stiffness mesh derived from the layer's alpha: transparent areas stay soft, so
  pins on empty background barely move the artwork
- **Pin Edge** option keeps the mesh border at its rest position
- Live preview that includes the layers below (shown static, not warped)
- Undo / redo inside the dialog (Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y)
- Non-destructive: parameters are saved with the layer in the `.xcf`; reopen
  the tool to continue editing the pins

## Requirements

- GIMP 3.2 (Python plug-ins enabled, which is the default)
- Windows x64, or Linux x86_64

## Demo


https://github.com/user-attachments/assets/7aa5b27d-8a5c-434b-b70f-2c4b4fb20dce



## Install

Grab the prebuilt binary for your system from `binaries/`:

| Platform | File | Requirements |
| --- | --- | --- |
| Linux x86_64 | [`binaries/linux-x86_64/puppet-warp.so`](binaries/linux-x86_64/puppet-warp.so) | glibc ≥ 2.38 (Ubuntu 24.04+, Fedora 39+, Arch, Debian 13+) |
| Windows x64 | [`binaries/windows-x64/puppet-warp.dll`](binaries/windows-x64/puppet-warp.dll) | Windows 10+ (UCRT); depends only on DLLs shipped with GIMP |

Or download a packaged zip from the [Releases](../../releases) page.

### Windows

1. Extract the zip. You get a folder named `puppet-warp`.
2. Copy that folder to `%APPDATA%\GIMP\3.2\plug-ins\`
   (final path: `%APPDATA%\GIMP\3.2\plug-ins\puppet-warp\puppet-warp.py`).
3. Start GIMP and run the filter once (**Filters ▸ Deform ▸ Puppet Warp (ARAP)…**).
   The plug-in copies `puppet-warp.dll` to `%LOCALAPPDATA%\gegl-0.4\plug-ins\`
   and tells you to restart GIMP.
4. Restart GIMP once. After that the filter works normally.

If the automatic copy fails, copy `puppet-warp.dll` to
`%LOCALAPPDATA%\gegl-0.4\plug-ins\` yourself (create the folders if missing)
and restart GIMP. To update later, close GIMP first: Windows will not overwrite a
DLL that GIMP has loaded.

### Linux

```sh
mkdir -p ~/.config/GIMP/3.2/plug-ins
cp -r puppet-warp ~/.config/GIMP/3.2/plug-ins/
chmod +x ~/.config/GIMP/3.2/plug-ins/puppet-warp/puppet-warp.py
```

Start GIMP, run the filter once, then restart GIMP so it picks up the GEGL
operation (it is copied to `~/.local/share/gegl-0.4/plug-ins/`).

The prebuilt `puppet-warp.so` needs **glibc 2.38 or newer** (Ubuntu 24.04,
Fedora 39+, Arch, Debian 13). On an older distribution, build it yourself
(next section); the plug-in also builds it automatically on first run when
`gcc` and the development packages are installed.

## Build from source

Sources: `puppet-warp.c` (GEGL operation and preview renderer) and
`arap_grid.c` (ARAP solver). The result is one shared library.

**Linux** (needs `gcc`, `pkg-config` and the GEGL, GLib and cairo dev packages):

```sh
gcc -std=gnu17 -O2 -shared -fPIC -Wall -I. puppet-warp.c arap_grid.c \
    -o puppet-warp.so -lm \
    $(pkg-config --cflags --libs gegl-0.4 glib-2.0 cairo)
```

`-std=gnu17` keeps the binary loadable on older glibc versions (newer GCC
defaults to C23, which pulls in glibc 2.38 symbols).

**Windows** (MSYS2, **UCRT64** shell):

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf \
    mingw-w64-ucrt-x86_64-gegl mingw-w64-ucrt-x86_64-cairo
gcc -O2 -shared -Wall -I. puppet-warp.c arap_grid.c -o puppet-warp.dll -lm \
    $(pkg-config --cflags --libs gegl-0.4 glib-2.0 cairo)
```

The DLL only depends on libraries that ship with GIMP (`libgegl`, `libbabl`,
`libglib`, `libgobject`, `libcairo`) plus the Windows UCRT.

Put the result next to `puppet-warp.py`.

## Usage

| Action | Control |
| --- | --- |
| Add a pin | Click empty space |
| Move a pin | Drag it |
| Delete a pin | Right-click, or Alt+click |
| Reset a pin | Double-click it |
| Rotate | Drag the dot on the orange ring, or Alt+wheel (Shift = bigger steps) |
| Scale | Ctrl+wheel (Shift = bigger steps) |
| Clear rotate/scale of a pin | Double-click the ring |
| Zoom / pan | Wheel / middle-drag |
| Undo / redo | Ctrl+Z / Ctrl+Shift+Z |

Tips:

- Put the artwork you want to warp on **its own layer** first.
- Tick **Stiffness mesh** to see which cells are stiff (cyan) or soft (red).
- More **Cols / Rows** gives finer control but is slower to solve.
- Unticking **Rotate/Scale** mutes angles and scales without losing them.

## How it works

`arap_grid.c` builds a regular vertex grid over the layer and solves ARAP with
a banded Cholesky factorization. Pins are hard constraints; rotate/scale pins
become a smooth similarity ring. `puppet-warp.c` registers the GEGL operation
`custom:puppet-warp`, which inverse-maps every grid triangle from the solved
mesh. The Python UI drives it and stores its parameters as JSON on the layer
filter.

## Troubleshooting

- Set `PW_DEBUG=1` before starting GIMP for logs.
- `PW_NO_C_PREVIEW=1` forces the slower Python preview renderer,
  `PW_NO_BG=1` hides the background layers in the preview.
- Filter missing or the layer looks unchanged after applying: restart GIMP once
  so it loads the GEGL operation, and check that `puppet-warp.dll` / `.so` is in
  the GEGL plug-in folder listed above.
