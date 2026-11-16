# Puppet Warp (ARAP) for GIMP 3

A non-destructive **puppet warp** filter for GIMP 3. Drop pins on a layer, drag them, and the image deforms *as rigidly as possible* (ARAP: as-rigid-as-possible deformation). Pins rotate, scale, taper, and carry a **depth** value so you can decide which part of the image overlaps in front when two regions cross. The warp lives as a regular GIMP layer filter, so you can close the dialog, keep editing, and reopen it later — nothing is baked in.

Equipped with an ultra-fast **C 1:1 Viewport Compositor**, hardware-accelerated **AVX2/FMA** routines, multi-level **Mipmap pyramids**, **Bicubic interpolation**, and alignment guides/snapping, the filter stays smooth and responsive (~60 FPS) even when working on large 4K/8K images with complex layer stacks.

## Features

### Warping & Rigidity
- Click to add pins, drag to deform; Shift snaps a drag to an axis
- Rotate and scale around any pin (orange ring: drag the dot to rotate, drag the body to scale; or Alt+wheel / Ctrl+wheel)
- Per-pin **taper** — how far a pin's influence reaches before it fades to the rest of the mesh (Shift+wheel, or the `T` field)
- Per-pin **depth** — when two warped regions overlap, higher depth draws in front (`[` / `]`, or the `D` field). See [Pin depth](#pin-depth)
- Custom pivot placement per pin (Ctrl+drag)
- **Pin Edge** keeps the outer mesh border at its rest position
- Adaptive stiffness mesh derived from the layer's alpha: transparent areas stay soft, while artwork silhouettes stay firm

### Canvas & Preview
- **1:1 C Viewport Compositor**: Renders background, active warp, and foreground directly at screen viewport resolution using OpenMP + AVX2, bypassing heavy CPU scaling
- **Mipmap Pyramid**: Automatically generates 1/2, 1/4, 1/8 downscaled textures for huge layers, eliminating zoom-out lag and pixel shimmering
- **Preview Opacity**: Real-time 0–100% opacity slider to align artwork against underlying layers without affecting final export
- **Layers Below & Layers Above**: Independently toggle and composite context layers:
  - *Off* (none)
  - *Nearest* (single visible layer immediately adjacent)
  - *Composite* (stack of all visible layers)
- **Mathematical Checkerboard**: Generated in zero-allocation RAM space
- **Mesh Overlay**: Display as Cells, Triangles, Both, or Off with custom line color, backed by cached Cairo paths
- **Extend Image**: Automatically grows the layer bounds (and its mask) if pixels are warped outside original edges

### Alignment & Guides
- **Snapping (`Snap`)**: Snaps pin movements to Guides, Alignment Grid, and smart axis alignments with other pins
- **Guides**: Automatically loads GIMP image guides (Cyan); allows creating, dragging, and deleting custom local guides (Magenta)
- **Alignment Grid**: Configurable pixel-grid overlay (`Step px`) independent of the ARAP deformation mesh

### Workflow, Comparison & Multi-Layer Export
- **Snapshots (`A`, `B`, `C`)**: Quick one-click state slots to save, restore, overwrite, or clear setup variations
- **`A⇄B⇄C` Compare & Cycle (`→` / `` ` ``)**: Non-destructively toggle or step sequentially through all saved variations (`Live → A → B → C → Live`) without losing unsaved work
- **`Export A-C` to New Layers**: Optionally export all saved snapshots (`A`, `B`, `C`) as separate, non-destructive layers above the active layer when applying (`OK`), each with its own editable Puppet Warp filter and `Extend Image` bounds
- **`Orig` Before/After (`\`)**: Instant visual comparison against the unwarped original layer
- Full-state undo / redo inside the dialog (Ctrl+Z, Ctrl+Shift+Z / Ctrl+Y)
- Multi-select pins (Shift+click, Ctrl+A), nudge with arrow keys (Shift = 10 px)
- Save / load setups (pins, depth, taper, guides, options) as JSON **presets**
- Non-destructive & Locale-safe: Full parameter persistence via GEGL filter config; unaffected by system comma/period decimal settings

## Requirements

- GIMP 3.2 (Python plug-ins enabled, default)
- Windows x64 or Linux x86_64
- **NumPy is optional but strongly recommended.** Without it, alpha stiffness and vector bounding box optimizations fall back to slower pure-Python loops.

## Demo

https://github.com/user-attachments/assets/5bd12069-d2a2-4147-b73a-235c6677b966

## How to use

The filter appears in two menus:
- Filters ▸ Deform ▸ Puppet Warp (ARAP)…
- Tools ▸ Transform Tools ▸ Puppet Warp (ARAP)…

Isolate the part you want to warp onto **its own layer** first — puppet warp deforms the entire layer, so a clean cutout gives the cleanest result.

## Install

### Windows
1. Download or extract the plugin files into `%APPDATA%\GIMP\3.2\plug-ins\puppet-warp\`
   (ensure `puppet-warp.py`, `puppet_warp_extras.py`, `puppet_warp_native.py`, `puppet_warp_resize.py`, and `puppet-warp.dll` are in this folder).
2. Start GIMP and run the filter once. The plug-in automatically loads or installs `puppet-warp.dll` into `%LOCALAPPDATA%\gegl-0.4\plug-ins\` and prompts for a restart if newly installed.
3. Restart GIMP.

### Linux
```sh
mkdir -p ~/.config/GIMP/3.2/plug-ins/puppet-warp
cp * ~/.config/GIMP/3.2/plug-ins/puppet-warp/
chmod +x ~/.config/GIMP/3.2/plug-ins/puppet-warp/puppet-warp.py
```
Start GIMP, run the filter once, then restart GIMP so GEGL registers `custom:puppet-warp` from `~/.local/share/gegl-0.4/plug-ins/`.

## Build from source

Sources: `puppet-warp.c` (GEGL op + viewport compositor + mipmap pyramid) and `arap_grid.c` (banded Cholesky ARAP solver).

**Linux** (`gcc`, `pkg-config`, `gegl-0.4`, `glib-2.0`):
```sh
gcc -O3 -ftree-vectorize -fno-math-errno -fno-trapping-math -shared -fPIC -fopenmp \
    puppet-warp.c arap_grid.c -o puppet-warp.so -lm \
    $(pkg-config --cflags --libs gegl-0.4 glib-2.0)
```
*(Add `-std=gnu17` if compiling on modern GCC targeting older glibc distributions).*

**Windows** (MSYS2 UCRT64 shell):
```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-pkgconf \
    mingw-w64-ucrt-x86_64-gegl mingw-w64-ucrt-x86_64-cairo
gcc -O3 -ftree-vectorize -fno-math-errno -fno-trapping-math -shared -fopenmp \
    puppet-warp.c arap_grid.c -o puppet-warp.dll -lm \
    $(pkg-config --cflags --libs gegl-0.4 glib-2.0)
```

## Controls

| Action | Control |
| --- | --- |
| **Add pin** | Click empty space |
| **Move pin** | Drag it |
| **Multi-select pins** | Shift+click to toggle selection, then drag any selected pin |
| **Select / Clear all pins** | Ctrl+A (all) / Alt+Esc (clear selection) |
| **Nudge pins** | Arrow keys (Shift = 10 px) |
| **Delete pin** | Right-click, Alt+click, or Delete |
| **Reset pin to rest** | Double-click the pin |
| **Rotate** | Drag dot on orange ring, or Alt+wheel (Shift = 15° steps) |
| **Scale** | Drag orange ring body, or Ctrl+wheel (Shift = 10% steps) |
| **Clear rotate / scale** | Double-click the orange ring |
| **Move pin pivot** | Ctrl+drag on canvas |
| **Taper (influence reach)** | Shift+wheel over pin, or `T` spinbox |
| **Depth (overlap order)** | `[` = send back, `]` = bring front, or `D` spinbox |
| **Cycle snapshots (`A → B → C → Live`)** | `` ` `` (backtick/grave), `→` button, or Right-click `A⇄B⇄C` |
| **Before / After toggle** | `\` (backslash) or `Orig` button |
| **Guides toggle** | Ctrl+; or `Guides` button |
| **Add / Move Guide** | Drag top / left edge tabs; right-click tab to remove |
| **Zoom / Pan** | Scroll wheel / Middle-drag |
| **Fit view** | `Fit` button |
| **Reset deformation** | `Reset` button (preserves pins) |
| **Remove all pins** | `Clear` button |
| **Undo / Redo** | Ctrl+Z / Ctrl+Shift+Z (or Ctrl+Y) |
| **Apply / Cancel** | Enter (`OK`) / Esc (`Cancel`) |

### Pin depth

When warped areas overlap, depth decides which region renders in front:
1. Select a pin in the target area.
2. Press **`]`** to bring forward or **`[`** to send backward (shows as `d=+1`, `d=-2`, etc.).
3. Triangle depth interpolates to the maximum depth of its vertices, matching Photoshop-style front-bleeding.
4. Depth is resolved via back-to-front sorted triangle blending inside the C viewport compositor and depth-aware compositing in the final GEGL filter.

### Snapshots, A-B-C Compare & Layer Export

- **Slots `A`, `B`, `C`**: Click to save/load. Shift+click (or Right-click) to overwrite. Alt+click (or Middle-click) to clear.
- **`A⇄B⇄C` & `→` (`` ` ``)**:
  - Click **`A⇄B⇄C`** to toggle a non-destructive preview against the active compare slot without erasing or altering your live work (Right-click **`A⇄B⇄C`** to jump to the next saved slot).
  - Click **`→`** (or press **`` ` ``**) to step sequentially through all saved snapshots and back to your live edit (`Live → A → B → C → Live`).
  - Clicking the canvas while peeking keeps the displayed snapshot for editing and pushes your previous live state onto the Undo stack (`Ctrl+Z` restores it).
- **`Export A-C`**:
  - Enable **`Export A-C`** before pressing **`OK`** to export every saved snapshot (`A`, `B`, `C`) as a separate layer right above the active layer (`<Layer> [Warp A]`, `[Warp B]`, `[Warp C]`).
  - Each exported layer remains fully non-destructive with its own editable Puppet Warp filter and independent `Extend Image` padding, grouped into a single GIMP undo step.

## Troubleshooting & Environment Variables

- `PW_DEBUG=1`: Prints solver iteration counters, viewport compositor timings (`[pw-c-vp]`), and GEGL chunk processing metrics.
- `PW_APPLY_BICUBIC=1`: Forces high-quality Catmull-Rom Bicubic filtering on final layer apply (default applies fast Bilinear).
- `PW_NO_C_PREVIEW=1`: Forces fallback to the slower Python/Cairo preview renderer.
- `PW_NO_BG=1`: Disables all background layer compositing.
- `PW_PREVIEW_SCALE=0.5`: Clamps viewport preview scale (0.25–1.0) on ultra-high-resolution canvases.
- `PW_NO_MASK_EXTEND=1`: Pads the layer bounds but ignores the layer mask on extend.
