# Puppet Warp (ARAP) for GIMP 3

A non-destructive **puppet warp** filter for GIMP 3. Drop pins on a layer, drag
them, and the image deforms *as rigidly as possible* (ARAP: as-rigid-as-possible
deformation). Pins rotate, scale, taper, and carry a **depth** value so you can
decide which part of the image overlaps in front when two regions cross. The
warp lives as a regular GIMP layer filter, so you can close the dialog, keep
editing, and reopen it later — nothing is baked in.

## Features

**Warping**

- Click to add pins, drag to deform; Shift snaps a drag to an axis
- Rotate and scale around any pin (orange ring: drag the dot to rotate, drag the
  body to scale; or Alt+wheel / Ctrl+wheel)
- Per-pin **taper** — how far a pin's influence reaches before it fades to the
  rest of the mesh (Shift+wheel, or the `T` field)
- Per-pin **depth** — when two warped regions overlap, higher depth draws in
  front (`[` / `]`, or the `D` field). See [Pin depth](#pin-depth)
- **Pin Edge** keeps the outer mesh border at its rest position
- Stiffness mesh derived from the layer's alpha: transparent areas stay soft, so
  a pin on empty background barely tugs the artwork

**Canvas & preview**

- **Extend image** grows the layer when pins pull pixels outside its bounds; the
  layer mask (if any) grows with it
- Live preview with three background modes: *Below* (first visible layer under),
  *Composite* (stack of all visible layers under), *Off*
- Mesh overlay in four styles (Cells / Triangles / Both / Off) with a custom
  line color, so you can see the quad grid while you work
- Pan, zoom, fit-to-view; the view widens automatically to show extended pixels

**Workflow**

- Undo / redo inside the dialog (Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y)
- Multi-select pins (Shift+click, Ctrl+A), nudge with arrow keys (Shift = 10 px)
- Save / load the whole setup (pins, depth, taper, grid, options) as a JSON
  **preset**
- Non-destructive: every parameter is stored as JSON on the layer filter and
  travels with the `.xcf`. Reopen the tool to continue editing — including after
  an extend, where the padding is re-detected so pins never drift

## Requirements

- GIMP 3.2 (Python plug-ins enabled, which is the default)
- Windows x64, or Linux x86_64
- **NumPy is optional but recommended.** Without it, alpha-derived stiffness and
  some reopen heuristics are skipped; the warp itself still works.

## Demo



https://github.com/user-attachments/assets/5bd12069-d2a2-4147-b73a-235c6677b966



## How to use

The filter appears in two menus:

- Filters ▸ Deform ▸ Puppet Warp (ARAP)…
- Tools ▸ Transform Tools ▸ Puppet Warp (ARAP)…

Isolate the part you want to warp onto **its own layer** first — puppet warp
deforms the whole layer, so a clean cutout gives the cleanest result.

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
3. Start GIMP and run the filter once. The plug-in copies `puppet-warp.dll` to
   `%LOCALAPPDATA%\gegl-0.4\plug-ins\` and tells you to restart GIMP.
4. Restart GIMP once. After that the filter works normally.

If the automatic copy fails, copy `puppet-warp.dll` to
`%LOCALAPPDATA%\gegl-0.4\plug-ins\` yourself (create the folders if missing) and
restart GIMP. To update later, close GIMP first: Windows will not overwrite a
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

Sources: `puppet-warp.c` (GEGL operation + preview renderer) and `arap_grid.c`
(ARAP solver). They compile into one shared library.

The plug-in auto-builds this on first run when the toolchain is present, using
the flags below (OpenMP is added automatically if the compiler supports it).

**Linux** (needs `gcc`, `pkg-config`, and the GEGL / GLib dev packages):

```sh
gcc -O3 -ftree-vectorize -shared -fPIC -fopenmp \
    puppet-warp.c arap_grid.c -o puppet-warp.so -lm \
    $(pkg-config --cflags --libs gegl-0.4 glib-2.0)
```

Drop `-fopenmp` if your compiler lacks OpenMP — the preview simply runs
single-threaded. To target an **older glibc**, add `-std=gnu17`: newer GCC
defaults to C23, which pulls in glibc 2.38 symbols that won't load on, say,
Debian 12.

**Windows** (MSYS2, **UCRT64** shell):

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf \
    mingw-w64-ucrt-x86_64-gegl mingw-w64-ucrt-x86_64-cairo
gcc -O3 -ftree-vectorize -shared \
    puppet-warp.c arap_grid.c -o puppet-warp.dll -lm \
    $(pkg-config --cflags --libs gegl-0.4 glib-2.0)
```

The DLL only depends on libraries that ship with GIMP (`libgegl`, `libbabl`,
`libglib`, `libgobject`) plus the Windows UCRT. Put the result next to
`puppet-warp.py`.

## Controls

Everything happens on the canvas. The pin inspector row (rotation, scale, taper,
depth, pivot) appears only when a pin is selected.

| Action | Control |
| --- | --- |
| Add a pin | Click empty space |
| Move a pin | Drag it |
| Move several pins | Shift+click to add to selection, then drag any selected pin |
| Select all pins | Ctrl+A |
| Clear selection | Alt+Esc |
| Nudge selected pins | Arrow keys (Shift = 10 px) |
| Delete a pin | Right-click, Alt+click, or Delete |
| Reset a pin to rest | Double-click the pin |
| Rotate | Drag the dot on the orange ring, or Alt+wheel (Shift = bigger steps) |
| Scale | Drag the ring body, or Ctrl+wheel (Shift = bigger steps) |
| Clear rotate/scale | Double-click the ring |
| Move a pin's pivot | Ctrl+drag on the canvas |
| Taper (influence reach) | Shift+wheel over a pin, or the `T` field |
| Depth (overlap order) | `[` = send back, `]` = bring front, or the `D` field |
| Zoom / pan | Wheel / middle-drag |
| Fit view | `Fit` button |
| Reset all deformation | `Reset` button (keeps pins) |
| Remove all pins | `Clear` button |
| Undo / redo | Ctrl+Z / Ctrl+Shift+Z (or Ctrl+Y) |
| Apply | Enter or `OK` |
| Cancel | Esc or `Cancel` |

### Options (popover, top row)

| Control | What it does |
| --- | --- |
| **Alpha stiffness** | Use the layer's alpha as solver stiffness. Off = uniform mesh. Soft (transparent) cells show red in the overlay. |
| **Pin edge** | Hold the outer mesh border at rest. |
| **Rotate/scale** | Allow pins to rotate/scale. Turning it off mutes the values without discarding them. |
| **Extend image** | Grow the layer (and its mask) when pins pull pixels outside bounds. |
| **BG** | Preview background: Off / Below / Composite. |
| **Presets** | Save / load pins + settings as JSON. |

### Pin depth

When you drag two parts of the image so they overlap, depth decides which one is
in front — the same idea as Photoshop's Pin Depth.

1. Click a pin in the region you want to control.
2. Press **`]`** to bring it forward, **`[`** to send it back. The number shows
   next to the pin (`d=+2`) and in the `D` field.
3. Depth is per-pin; a triangle's depth is the highest depth among its three
   corners, so a front region "bleeds" slightly past its border, like Photoshop.

Depth is a rendering order, not a solver input — changing it does not re-solve
the mesh, so it's instant. It is saved with the pin, travels in the preset, and
survives reopen. With a current build, the live preview uses the same z-buffer
rule as the final render, so what you see is what you get.

> Tip: depth matters most on cutouts with hard edges. On heavily feathered
> borders the overlap is a soft blend either way, and the exact ordering is less
> visible.

## How it works

`arap_grid.c` builds a regular vertex grid over the layer and solves ARAP with a
banded Cholesky factorization. Pins are hard constraints; a rotate/scale pin
becomes a smooth similarity ring; taper controls how fast a pin's influence
falls off.

`puppet-warp.c` registers the GEGL operation `custom:puppet-warp`. The **apply**
path inverse-maps every grid triangle from the solved mesh into the output
buffer. The **preview** path is a separate fast rasterizer (premultiplied ARGB32,
bilinear, OpenMP over horizontal bands) so dragging stays responsive; when pin
depth is active it uses a per-pixel z-buffer so the preview's overlap order
matches the final render.

The Python UI drives both and stores every parameter — pins, rotation, scale,
taper, depth, grid size, options, and the solved vertices — as JSON on the layer
filter. There is no sidecar file: the vertices ride in the filter params, so
reopen is exact and the `.xcf` stays self-contained.

**Extend** pads the layer to fit the deformed bounds. For the mask, the plug-in
does not try to rebuild it by hand (GIMP 3's Python bindings expose no reliable
way to create/attach a layer mask); instead it resizes the layer and lets GIMP's
own core cascade the mask to the new size, then verifies the mask actually grew.
If it didn't, the resize is rolled back and extend is skipped for that layer
rather than leaving the mask drifted. Set `PW_NO_MASK_EXTEND=1` to opt into the
old behavior (pad the layer, leave the mask alone) if you specifically want that.

## Troubleshooting

- Start GIMP with `PW_DEBUG=1` for verbose logs (solver timings, preview path,
  extend/measure decisions, depth preview mode).
- `PW_NO_C_PREVIEW=1` forces the slower Python preview renderer. Useful to
  compare, or if a GPU/driver quirk shows in the C path.
- `PW_NO_BG=1` hides background layers in the preview.
- `PW_PREVIEW_SCALE=0.5` caps the preview raster resolution (0.25–1.0). Lower it
  on very large layers if dragging feels heavy.
- `PW_NO_MASK_EXTEND=1` makes Extend pad the layer but ignore the mask (legacy;
  the mask will not follow the layer).
- Filter missing, or the layer looks unchanged after applying: restart GIMP once
  so it loads the GEGL operation, and confirm `puppet-warp.dll` / `.so` is in the
  GEGL plug-in folder listed above.
- Pin depth preview falls back to the Python renderer (slower) if your
  `puppet-warp.so` predates the depth z-buffer export. Rebuild it (see *Build
  from source*) to get the fast, apply-matching preview.
