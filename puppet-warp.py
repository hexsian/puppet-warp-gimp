#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Puppet Warp: ARAP-based non-destructive deform filter for GIMP 3.

Pins translate (single-vertex Dirichlet), optionally rotate/scale via a
similarity ring. Preview is rendered in C (cairo) and falls back to Python;
apply runs as a GEGL filter whose params live in the XCF. Background layers
are shown (static, not warped) so the preview matches the canvas.

Cross-platform: builds/loads .so on Linux/macOS and .dll on Windows (MSYS2 /
MinGW-w64 toolchain with gegl-0.4, glib-2.0, cairo dev packages required).

Runtime env switches: PW_DEBUG=1 (logs), PW_NO_C_PREVIEW=1 (Python raster),
PW_NO_BG=1 (no background layer).
"""
import math
import sys
sys.dont_write_bytecode = True
import json
import os
import shutil
import subprocess
import ctypes
import platform
import gi

gi.require_version('Gimp', '3.0')
gi.require_version('GimpUi', '3.0')
gi.require_version('Gtk', '3.0')
gi.require_version('Gdk', '3.0')
gi.require_version('Gegl', '0.4')
from gi.repository import Gimp, GimpUi, Gtk, Gdk, Gegl, GLib

try:
    import cairo
except ImportError:
    from gi.repository import cairo

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

PLUG_IN_PROC = "plug-in-puppet-warp"
PLUG_IN_BINARY = "puppet_warp"
GEGL_OP_NAME = "custom:puppet-warp"
GEGL_OP_LABEL = "Puppet Warp"

_IS_WIN = platform.system() == "Windows"
_LIB_EXT = ".dll" if _IS_WIN else ".so"
_LIB_NAME = "puppet-warp" + _LIB_EXT

def _default_plugin_dir():
    if _IS_WIN:
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "GIMP", "3.2", "plug-ins", "puppet-warp")
    return os.path.expanduser("~/.config/GIMP/3.2/plug-ins/puppet-warp")


PLUGIN_DIR = os.path.dirname(os.path.abspath(__file__)) if '__file__' in globals() \
    else _default_plugin_dir()


def _gegl_user_dir():
    """Per-user GEGL plug-in dir, exactly where GEGL scans for operations:
    $XDG_DATA_HOME/gegl-0.4/plug-ins (Linux), %LOCALAPPDATA%\\gegl-0.4\\plug-ins
    (Windows; NOT %APPDATA%, which is the roaming folder)."""
    return os.path.join(GLib.get_user_data_dir(), "gegl-0.4", "plug-ins")


GEGL_USER_DIR = _gegl_user_dir()

MW_SO = os.path.join(PLUGIN_DIR, _LIB_NAME)      # name kept for history; ext varies
MW_C = os.path.join(PLUGIN_DIR, "puppet-warp.c")
AGRID_C = os.path.join(PLUGIN_DIR, "arap_grid.c")

_op_ready = False
_op_freshly_installed = False
_LIB = None


class ArapVec2(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]


def _load_arap_lib():
    """Load the shared solver/renderer library and bind ctypes signatures."""
    global _LIB
    if _LIB is not None:
        return _LIB
    candidates = []
    for d in (GEGL_USER_DIR, PLUGIN_DIR):
        for name in (_LIB_NAME, "puppet-warp.so", "puppet-warp.dll"):
            p = os.path.join(d, name)
            if p not in candidates:
                candidates.append(p)
    for path in candidates:
        if not os.path.exists(path):
            continue
        try:
            lib = ctypes.CDLL(path)
            lib.arap_grid_new.argtypes = [ctypes.c_float] * 4 + [ctypes.c_int] * 2
            lib.arap_grid_new.restype = ctypes.c_void_p
            lib.arap_grid_add_pin.argtypes = [ctypes.c_void_p] + [ctypes.c_float] * 4
            lib.arap_grid_add_pin.restype = None
            lib.arap_grid_solve.argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                ctypes.c_float, ctypes.c_int,
            ]
            lib.arap_grid_solve.restype = ctypes.c_int
            lib.arap_grid_verts.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
            lib.arap_grid_verts.restype = ctypes.POINTER(ArapVec2)
            lib.arap_grid_free.argtypes = [ctypes.c_void_p]
            lib.arap_grid_free.restype = None
            if hasattr(lib, "arap_grid_set_cell_weights"):
                lib.arap_grid_set_cell_weights.argtypes = [
                    ctypes.c_void_p, ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                ]
                lib.arap_grid_set_cell_weights.restype = None
            if hasattr(lib, "arap_grid_set_pin_edge"):
                lib.arap_grid_set_pin_edge.argtypes = [ctypes.c_void_p, ctypes.c_int]
                lib.arap_grid_set_pin_edge.restype = None
            if hasattr(lib, "arap_grid_add_pin_sim"):
                lib.arap_grid_add_pin_sim.argtypes = [
                    ctypes.c_void_p] + [ctypes.c_float] * 6
                lib.arap_grid_add_pin_sim.restype = None
            if hasattr(lib, "pw_render_preview"):
                lib.pw_render_preview.argtypes = [
                    ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                    ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                    ctypes.c_int, ctypes.c_int,
                    ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                    ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float,
                ]
                lib.pw_render_preview.restype = ctypes.c_int
            _LIB = lib
            print(f"[puppet-warp] loaded {path} "
                  f"(c-preview={'yes' if hasattr(lib, 'pw_render_preview') else 'no'})")
            return lib
        except Exception as e:
            print(f"[puppet-warp] ctypes failed {path}: {e}")
    return None


def _build_so():
    """Compile the shared library if either source is newer than the binary."""
    srcs = (MW_C, AGRID_C)
    if os.path.exists(MW_SO) and all(os.path.exists(s) for s in srcs) \
            and os.path.getmtime(MW_SO) >= max(os.path.getmtime(s) for s in srcs):
        return True
    if not all(os.path.exists(s) for s in srcs):
        return False
    try:
        pc = subprocess.run(
            ["pkg-config", "--cflags", "--libs", "gegl-0.4", "glib-2.0", "cairo"],
            capture_output=True, text=True, timeout=30)
        if pc.returncode != 0:
            return False
        flags = ["gcc", "-O2", "-shared", "-I.", MW_C, AGRID_C,
                 "-o", MW_SO + ".tmp", "-lm"]
        if not _IS_WIN:
            flags.insert(flags.index("-shared") + 1, "-fPIC")
        cmd = flags + pc.stdout.split()
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=180, cwd=PLUGIN_DIR)
        if r.returncode == 0 and os.path.exists(MW_SO + ".tmp"):
            os.replace(MW_SO + ".tmp", MW_SO)
            try:
                os.makedirs(GEGL_USER_DIR, exist_ok=True)
                shutil.copy2(MW_SO, os.path.join(GEGL_USER_DIR, _LIB_NAME))
            except Exception:
                pass
            return True
        if os.environ.get("PW_DEBUG"):
            print("[puppet-warp] build failed:", r.stderr[-800:] if r.stderr else "")
        return False
    except Exception as e:
        if os.environ.get("PW_DEBUG"):
            print(f"[puppet-warp] build exception: {e}")
        return False


def _install_lib():
    """Copy the shipped/built binary into GEGL's user plug-in dir.

    Runs even when nothing was compiled, so a prebuilt .dll/.so from a release
    works without a compiler. GEGL inside GIMP only discovers a new operation
    at GIMP startup, so the first install needs one GIMP restart. On Windows a
    DLL that GIMP already loaded cannot be overwritten until GIMP is closed.
    """
    global _op_freshly_installed
    dst = os.path.join(GEGL_USER_DIR, _LIB_NAME)
    if not os.path.exists(MW_SO):
        return
    try:
        if os.path.exists(dst) \
                and os.path.getsize(dst) == os.path.getsize(MW_SO) \
                and os.path.getmtime(dst) >= os.path.getmtime(MW_SO):
            return
        os.makedirs(GEGL_USER_DIR, exist_ok=True)
        fresh = not os.path.exists(dst)
        shutil.copy2(MW_SO, dst)
        _op_freshly_installed = fresh
    except Exception as e:
        print(f"[puppet-warp] could not install {dst}: {e} "
              "(close GIMP, reopen it and try again to update the operation)")


def _load_op():
    """Register the GEGL operation directory with GEGGL."""
    for d in (PLUGIN_DIR, GEGL_USER_DIR):
        try:
            if os.path.isdir(d) and hasattr(Gegl, "load_directory"):
                Gegl.load_directory(d)
        except Exception:
            pass


def _pid_alive(pid):
    """True if process `pid` exists (OpenProcess on Windows, /proc elsewhere)."""
    if pid <= 0:
        return False
    if _IS_WIN:
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        STILL_ACTIVE = 259
        try:
            k32 = ctypes.windll.kernel32
            h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
            if not h:
                return False
            code = ctypes.c_ulong(0)
            ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
            k32.CloseHandle(h)
            return bool(ok) and code.value == STILL_ACTIVE
        except Exception:
            return True   # conservative: keep files when we cannot tell
    try:
        return os.path.exists("/proc/%d" % pid)
    except Exception:
        return True


def _cleanup_orphan_sidecars():
    """Delete leftover vert sidecars in temp dir whose owning PID is gone."""
    try:
        import re
        tmp = GLib.get_tmp_dir()
        pat = re.compile(r'^puppet-warp-verts-(\d+)')
        mypid = os.getpid()
        for fn in os.listdir(tmp):
            m = pat.match(fn)
            if not m:
                continue
            pid = int(m.group(1))
            if pid == mypid:
                continue
            if not _pid_alive(pid):
                try:
                    os.unlink(os.path.join(tmp, fn))
                except OSError:
                    pass
    except Exception:
        pass


def ensure_op():
    """One-time init: clean orphans, build binary, load op, load lib."""
    global _op_ready
    if _op_ready:
        return True
    _cleanup_orphan_sidecars()
    _build_so()
    _install_lib()
    _load_op()
    _load_arap_lib()
    _op_ready = True
    return True


def _unpack_offsets(t):
    """Return (x, y) from a get_offsets() tuple regardless of binding layout.

    Some GIMP 3 builds return (success, x, y) where success is a bool; others
    return (x, y) or (x, y, unknown). Detect by the type of element 0 (a pixel
    coordinate is never a bool) so no layout has to be guessed.
    """
    if isinstance(t, (list, tuple)) and len(t) >= 3 and isinstance(t[0], bool):
        return int(t[1]), int(t[2])
    if isinstance(t, (list, tuple)) and len(t) >= 2:
        return int(t[0]), int(t[1])
    raise ValueError(f"unexpected offsets tuple: {t!r}")


def _layer_index(layers, target):
    """Index of `target` in a topmost-first layer list, by object identity."""
    for i, l in enumerate(layers):
        if l == target or l is target:
            return i
    raise ValueError("layer not found in get_layers()")


def _as_list(x):
    return x if isinstance(x, list) else list(x)


class Pin:
    """A control point: rest (sx,sy), current (dx,dy), rotation, scale."""
    __slots__ = ('sx', 'sy', 'dx', 'dy', 'theta', 'scale')

    def __init__(self, sx, sy):
        self.sx = float(sx)
        self.sy = float(sy)
        self.dx = float(sx)
        self.dy = float(sy)
        self.theta = 0.0
        self.scale = 1.0


def load_existing_params(drawable):
    """Read params-json from an already-applied Puppet Warp filter, if any."""
    try:
        for f in list(drawable.get_filters() or []):
            try:
                if f.get_operation_name() == GEGL_OP_NAME:
                    cfg = f.get_config()
                    if cfg is not None:
                        s = cfg.get_property("params-json")
                        if s:
                            return json.loads(s)
            except Exception:
                pass
    except Exception:
        pass
    return None


class PuppetWarpDialog(Gtk.Dialog):
    def __init__(self, image, drawable):
        super().__init__(title="Puppet Warp (ARAP)", modal=True,
                         destroy_with_parent=True)
        self.set_default_size(1000, 780)
        self.image = image
        self.drawable = drawable
        cw = drawable.get_width()
        ch = drawable.get_height()
        self.pad_left = 0
        self.pad_top = 0
        self.canvas_w = cw
        self.canvas_h = ch
        self.cols = 15
        self.rows = 15
        self.puppet_x = 0.0
        self.puppet_y = 0.0
        self.puppet_w = float(cw)
        self.puppet_h = float(ch)

        # Source = the layer being warped (what apply will deform).
        rect = Gegl.Rectangle.new(0, 0, cw, ch)
        raw = bytes(drawable.get_buffer().get(rect, 1.0, "cairo-ARGB32",
                                              Gegl.AbyssPolicy.NONE))
        self.src_surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, cw, ch)
        self.src_surface.get_data()[:] = raw
        self.src_surface.mark_dirty()

        # Static background = composite of layers below, drawn under the
        # warped layer so the preview matches the canvas without warping it.
        self._bg_surface = None
        if os.environ.get("PW_NO_BG") != "1":
            try:
                layers = _as_list(self.image.get_layers())
                pos = _layer_index(layers, drawable)
            except Exception as e:
                layers = None
                print(f"[puppet-warp] bg: layer lookup failed ({e})")
            if layers is not None:
                ox, oy = _unpack_offsets(drawable.get_offsets())
                iw, ih = self.image.get_width(), self.image.get_height()
                if os.environ.get("PW_DEBUG"):
                    print(f"[bg] pos={pos} off=({ox},{oy}) img={iw}x{ih}")
                try:
                    dup = self.image.duplicate()
                    try:
                        dl = _as_list(dup.get_layers())
                        dl[pos].set_visible(False)
                        flat = dup.flatten()
                        if flat is None:
                            raise ValueError("flatten returned None")
                        fw, fh = flat.get_width(), flat.get_height()
                        fraw = bytes(flat.get_buffer().get(
                            Gegl.Rectangle.new(0, 0, fw, fh), 1.0,
                            "cairo-ARGB32", Gegl.AbyssPolicy.NONE))
                        fs = cairo.ImageSurface(cairo.FORMAT_ARGB32, fw, fh)
                        fs.get_data()[:] = fraw
                        fs.mark_dirty()
                        tmp = cairo.ImageSurface(cairo.FORMAT_ARGB32, iw, ih)
                        tc = cairo.Context(tmp)
                        tc.set_source_surface(fs, -ox, -oy)
                        tc.paint()
                        tmp.flush()
                        self._bg_surface = tmp
                    finally:
                        try:
                            dup.delete()
                        except Exception:
                            pass
                except Exception as e:
                    # Fallback: manual composite of layers below (ignores
                    # blend modes / masks of those layers).
                    try:
                        bs = cairo.ImageSurface(cairo.FORMAT_ARGB32, cw, ch)
                        bc = cairo.Context(bs)
                        for l in reversed(layers[pos + 1:]):
                            if not l.get_visible():
                                continue
                            lx, ly = _unpack_offsets(l.get_offsets())
                            lw, lh = l.get_width(), l.get_height()
                            if lw <= 0 or lh <= 0:
                                continue
                            lraw = bytes(l.get_buffer().get(
                                Gegl.Rectangle.new(0, 0, lw, lh), 1.0,
                                "cairo-ARGB32", Gegl.AbyssPolicy.NONE))
                            ls = cairo.ImageSurface(cairo.FORMAT_ARGB32, lw, lh)
                            ls.get_data()[:] = lraw
                            ls.mark_dirty()
                            op = max(0.0, min(1.0, l.get_opacity() / 100.0))
                            if op <= 0.0:
                                continue
                            bc.save()
                            bc.translate(lx - ox, ly - oy)
                            bc.set_source_surface(ls, 0, 0)
                            bc.paint_with_alpha(op)
                            bc.restore()
                        bs.flush()
                        self._bg_surface = bs
                    except Exception as e2:
                        print(f"[puppet-warp] bg unavailable ({e2})")
                        self._bg_surface = None

        self._alpha = None
        if HAS_NUMPY:
            self._alpha = np.frombuffer(raw, dtype=np.uint8).reshape(
                ch, cw, 4)[:, :, 3].astype(np.float32) / 255.0
        self._cw_cache = {}
        self.pins = []
        self.active_pin_index = None
        self.hovered_pin_index = None
        self.is_dragging_pin = False
        self.drag_offset = (0.0, 0.0)
        self.puppet_radius = 400.0
        self.pan_x = 25.0
        self.pan_y = 25.0
        self.zoom = 1.0
        self.is_panning = False
        self.pan_start = (0.0, 0.0)
        self._update_timer = None
        self.deformed_verts = None
        self._tri_stat = {"draw": 0, "det": 0, "scale": 0,
                          "mdet": 0, "bbox": 0, "cairo": 0}
        self._checker = None
        self._checker_w = 0
        self._checker_h = 0
        self._dst_surface = None
        self._verts_arr = None
        self._undo_stack = []
        self._redo_stack = []
        self._drag_pushed = False
        self.pin_edge = False
        self.rotate_on = True
        self._drag_iters = 30
        self._idle_iters = 30
        self._last_good_verts = None
        self.is_rotating = False
        self.rot_grab = 0.0
        self.restored_pins = 0

        prev = load_existing_params(drawable)
        if prev:
            try:
                self.cols = max(2, min(64, int(prev.get("cols", self.cols))))
                self.rows = max(2, min(64, int(prev.get("rows", self.rows))))
                self.pin_edge = bool(prev.get("pin_edge", False))
                self.rotate_on = bool(prev.get("rotate_on", True))
            except Exception:
                pass
            pf = prev.get("pins_flat") or []
            for i in range(0, len(pf) - 3, 4):
                p = Pin(pf[i], pf[i + 1])
                p.dx = pf[i] + pf[i + 2]
                p.dy = pf[i + 1] + pf[i + 3]
                self.pins.append(p)
            pr = prev.get("pin_rot") or []
            ps = prev.get("pin_scale") or []
            for i, p in enumerate(self.pins):
                if i < len(pr):
                    try:
                        p.theta = float(pr[i])
                    except Exception:
                        pass
                if i < len(ps):
                    try:
                        p.scale = float(ps[i])
                    except Exception:
                        pass
            self.restored_pins = len(self.pins)

        self._build_ui()
        if self.restored_pins:
            self.status_label.set_label(
                f"Restored {self.restored_pins} pins from the active Puppet "
                "Warp filter. Use Clear Pins to start a new session.")
        self.fit_to_view()
        self._solve_preview()

    def _ensure_checker(self):
        """Build/cache the transparency checkerboard surface."""
        if (self._checker is not None and self._checker_w == self.canvas_w
                and self._checker_h == self.canvas_h):
            return self._checker
        surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, self.canvas_w, self.canvas_h)
        cr = cairo.Context(surf)
        size = 16
        for rr in range(int(math.ceil(self.canvas_h / size))):
            for cc in range(int(math.ceil(self.canvas_w / size))):
                cr.set_source_rgb(0.85, 0.85, 0.85) if (rr + cc) % 2 == 0 \
                    else cr.set_source_rgb(0.65, 0.65, 0.65)
                cr.rectangle(cc * size, rr * size, size, size)
                cr.fill()
        surf.flush()
        self._checker = surf
        self._checker_w = self.canvas_w
        self._checker_h = self.canvas_h
        return surf

    def cell_weights(self, cols, rows):
        """Per-cell stiffness = mean alpha of the layer region (cached)."""
        key = (cols, rows)
        if key in self._cw_cache:
            return self._cw_cache[key]
        w = [1.0] * (cols * rows)
        if self._alpha is not None:
            h, wd = self._alpha.shape
            for r in range(rows):
                y0 = int(r * h / rows)
                y1 = max(y0 + 1, int((r + 1) * h / rows))
                for c in range(cols):
                    x0 = int(c * wd / cols)
                    x1 = max(x0 + 1, int((c + 1) * wd / cols))
                    m = float(self._alpha[y0:y1, x0:x1].mean())
                    w[r * cols + c] = m if m > 0.03 else 0.0
        self._cw_cache[key] = w
        return w

    def _build_ui(self):
        """Construct toolbar, drawing area and status hint."""
        ca = self.get_content_area()
        ca.set_spacing(6)
        ca.set_border_width(8)
        bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        bar.pack_start(Gtk.Label(label="<b>Puppet Warp (ARAP)</b>",
                                 use_markup=True), False, False, 0)
        bar.pack_start(Gtk.Label(label="Cols:"), False, False, 0)
        self.cols_spin = Gtk.SpinButton.new_with_range(2, 64, 1)
        self.cols_spin.set_value(self.cols)
        self.cols_spin.connect("value-changed", self._on_grid_changed)
        bar.pack_start(self.cols_spin, False, False, 0)
        bar.pack_start(Gtk.Label(label="Rows:"), False, False, 0)
        self.rows_spin = Gtk.SpinButton.new_with_range(2, 64, 1)
        self.rows_spin.set_value(self.rows)
        self.rows_spin.connect("value-changed", self._on_grid_changed)
        bar.pack_start(self.rows_spin, False, False, 0)
        clear = Gtk.Button(label="Clear Pins")
        clear.connect("clicked", lambda b: self._clear_pins())
        bar.pack_start(clear, False, False, 0)
        reset = Gtk.Button(label="Reset View")
        reset.connect("clicked", lambda b: self.fit_to_view())
        bar.pack_start(reset, False, False, 0)
        self.show_weights_check = Gtk.CheckButton(label="Stiffness mesh")
        self.show_weights_check.set_active(True)
        self.show_weights_check.set_tooltip_text(
            "Cyan = stiff cells (contain image), faded red = soft cells "
            "(background, weight 0). A pin on a red cell barely moves the "
            "image. The mesh hides while dragging a pin.")
        self.show_weights_check.connect(
            "toggled", lambda b: self.drawing_area.queue_draw())
        bar.pack_start(self.show_weights_check, False, False, 0)
        self.pin_edge_check = Gtk.CheckButton(label="Pin Edge")
        self.pin_edge_check.set_active(self.pin_edge)
        self.pin_edge_check.set_tooltip_text(
            "Pin the mesh border to its rest position so dragging pins does "
            "not pull the background/edges. Off by default.")
        self.pin_edge_check.connect("toggled", self._on_pin_edge_toggled)
        bar.pack_start(self.pin_edge_check, False, False, 0)
        self.rotate_check = Gtk.CheckButton(label="Rotate/Scale")
        self.rotate_check.set_active(self.rotate_on)
        self.rotate_check.set_tooltip_text(
            "Enable/disable rotate and scale effects. Off = translate-only, "
            "but angles/scales are remembered (re-enable to restore). Saving "
            "while off does not discard them.")
        self.rotate_check.connect("toggled", self._on_rotate_toggled)
        bar.pack_start(self.rotate_check, False, False, 0)
        ca.pack_start(bar, False, False, 0)
        self.drawing_area = Gtk.DrawingArea()
        self.drawing_area.set_can_focus(True)
        self.drawing_area.set_size_request(720, 520)
        self.drawing_area.add_events(
            Gdk.EventMask.BUTTON_PRESS_MASK | Gdk.EventMask.BUTTON_RELEASE_MASK |
            Gdk.EventMask.POINTER_MOTION_MASK | Gdk.EventMask.SCROLL_MASK |
            Gdk.EventMask.KEY_PRESS_MASK | Gdk.EventMask.LEAVE_NOTIFY_MASK)
        self.drawing_area.connect("draw", self._on_draw)
        self.drawing_area.connect("button-press-event", self._on_button_press)
        self.drawing_area.connect("button-release-event", self._on_button_release)
        self.drawing_area.connect("motion-notify-event", self._on_motion)
        self.drawing_area.connect("scroll-event", self._on_scroll)
        self.drawing_area.connect("leave-notify-event", self._on_leave)
        self.drawing_area.connect("key-press-event", self._on_key_press)
        fr = Gtk.Frame()
        fr.set_shadow_type(Gtk.ShadowType.IN)
        fr.add(self.drawing_area)
        ca.pack_start(fr, True, True, 0)
        self.status_label = Gtk.Label(
            label="Click: add pin | Drag: move (Shift snaps to axis) | "
                  "Right-click / Alt+click: delete | Double-click pin: reset | "
                  "Orange ring: drag dot = rotate | Alt+wheel: rotate | "
                  "Ctrl+wheel: scale | Double-click ring: clear sim | "
                  "Uncheck Rotate/Scale: disable (values kept) | "
                  "Ctrl+Z / Ctrl+Shift+Z: undo/redo")
        self.status_label.set_xalign(0.0)
        ca.pack_start(self.status_label, False, False, 0)
        self.add_button("_Cancel", Gtk.ResponseType.CANCEL)
        self.add_button("_OK", Gtk.ResponseType.OK)
        self.show_all()

    def get_params_json(self):
        """Serialize the full warp state for the GEGL filter / XCF."""
        pins_flat = []
        for p in self.pins:
            sx = round(p.sx * 2.0) / 2.0
            sy = round(p.sy * 2.0) / 2.0
            ddx = round((p.dx - p.sx) * 2.0) / 2.0
            ddy = round((p.dy - p.sy) * 2.0) / 2.0
            pins_flat.extend([sx, sy, ddx, ddy])
        cw = self.cell_weights(self.cols, self.rows)
        verts_flat = []
        n_expect = (self.cols + 1) * (self.rows + 1)
        if self.deformed_verts and len(self.deformed_verts) == n_expect:
            for vx, vy in self.deformed_verts:
                verts_flat.extend([round(vx, 4), round(vy, 4)])
        params = {
            "cols": int(self.cols),
            "rows": int(self.rows),
            "pin_edge": bool(self.pin_edge),
            "rotate_on": bool(self.rotate_on),
            "pin_rot": [round(p.theta, 4) for p in self.pins],
            "pin_scale": [round(p.scale, 4) for p in self.pins],
            "region_x": 0,
            "region_y": 0,
            "orig_width": int(self.canvas_w),
            "orig_height": int(self.canvas_h),
            "pad_left": int(self.pad_left),
            "pad_top": int(self.pad_top),
            "pins_flat": pins_flat,
            "mask_w": int(self.cols),
            "mask_h": int(self.rows),
            "mask_flat": [round(x, 4) for x in cw],
            "verts_flat": verts_flat,
        }
        return json.dumps(params, separators=(',', ':'), sort_keys=True)

    def _snap(self):
        """Current pin state as a list of 6-tuples for undo/redo."""
        return [(p.sx, p.sy, p.dx, p.dy, p.theta, p.scale) for p in self.pins]

    def _push_undo(self):
        self._undo_stack.append(self._snap())
        if len(self._undo_stack) > 64:
            self._undo_stack.pop(0)
        self._redo_stack.clear()

    def _restore_snapshot(self, snap):
        self.pins = []
        for t in snap:
            p = Pin(t[0], t[1])
            p.dx = t[2]
            p.dy = t[3]
            p.theta = t[4] if len(t) > 4 else 0.0
            p.scale = t[5] if len(t) > 5 else 1.0
            self.pins.append(p)
        self.active_pin_index = None
        self.hovered_pin_index = None
        self.is_dragging_pin = False
        self.is_rotating = False
        self._drag_pushed = False
        self._solve_preview()
        self.drawing_area.queue_draw()

    def _do_undo(self):
        if self._undo_stack:
            self._redo_stack.append(self._snap())
            self._restore_snapshot(self._undo_stack.pop())
        return True

    def _do_redo(self):
        if self._redo_stack:
            self._undo_stack.append(self._snap())
            self._restore_snapshot(self._redo_stack.pop())
        return True

    def _on_key_press(self, w, e):
        """Ctrl+Z undo, Ctrl+Shift+Z / Ctrl+Y redo."""
        if not (e.state & Gdk.ModifierType.CONTROL_MASK):
            return False
        kv = e.keyval
        if kv in (Gdk.KEY_z, Gdk.KEY_Z):
            if e.state & Gdk.ModifierType.SHIFT_MASK:
                return self._do_redo()
            return self._do_undo()
        if kv in (Gdk.KEY_y, Gdk.KEY_Y):
            return self._do_redo()
        return False

    def _rot_handle_world(self, p):
        """World position of the rotate handle on the selected pin's ring."""
        r = 28.0 / self.zoom
        return (p.dx + r * math.cos(p.theta), p.dy + r * math.sin(p.theta))

    def _find_handle_at(self, sx, sy, rad=12.0):
        """Index of the pin whose rotate handle is under screen (sx,sy)."""
        if not self.rotate_on:
            return None
        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return None
        hx, hy = self._rot_handle_world(self.pins[i])
        px, py = self._world_to_screen(hx, hy)
        return i if (sx - px) ** 2 + (sy - py) ** 2 <= rad * rad else None

    def _dst_has_content(self):
        """True if the C-rendered destination surface has any opaque pixels."""
        try:
            mv = self._dst_surface.get_data()
            stride = self._dst_surface.get_stride()
            w = self._dst_surface.get_width()
            h = self._dst_surface.get_height()
            cnt = 0
            y = 0
            while y < h:
                base = y * stride
                x = 0
                while x < w:
                    if mv[base + x * 4 + 3] != 0:
                        cnt += 1
                        if cnt >= 30:
                            return True
                    x += 16
                y += 16
            return False
        except Exception:
            return False

    def _c_render(self, lib, cr, verts):
        """Rasterize the preview via the C library; return False to fall back."""
        try:
            nverts = (self.cols + 1) * (self.rows + 1)
            if verts is None or len(verts) != nverts:
                return False
            if (self._dst_surface is None
                    or self._dst_surface.get_width() != self.canvas_w
                    or self._dst_surface.get_height() != self.canvas_h):
                self._dst_surface = cairo.ImageSurface(
                    cairo.FORMAT_ARGB32, self.canvas_w, self.canvas_h)
            arr = self._verts_arr
            if arr is None or len(arr) != 2 * nverts:
                arr = (ctypes.c_float * (2 * nverts))()
                self._verts_arr = arr
            for i in range(nverts):
                x, y = verts[i]
                arr[2 * i] = x
                arr[2 * i + 1] = y
            src_mv = self.src_surface.get_data()
            dst_mv = self._dst_surface.get_data()
            src_ptr = ctypes.addressof(ctypes.c_char.from_buffer(src_mv))
            dst_ptr = ctypes.addressof(ctypes.c_char.from_buffer(dst_mv))
            ret = lib.pw_render_preview(
                ctypes.c_void_p(src_ptr),
                ctypes.c_int(self.canvas_w), ctypes.c_int(self.canvas_h),
                ctypes.c_int(self.src_surface.get_stride()),
                ctypes.c_void_p(dst_ptr),
                ctypes.c_int(self.canvas_w), ctypes.c_int(self.canvas_h),
                ctypes.c_int(self._dst_surface.get_stride()),
                ctypes.c_int(self.cols), ctypes.c_int(self.rows),
                arr, ctypes.c_int(nverts),
                ctypes.c_float(self.puppet_x), ctypes.c_float(self.puppet_y),
                ctypes.c_float(self.puppet_w), ctypes.c_float(self.puppet_h))
            if ret <= 0:
                if os.environ.get("PW_DEBUG"):
                    print("[pw-c-prev] ret<=0 -> fallback python")
                return False
            self._dst_surface.mark_dirty()
            if not self._dst_has_content():
                if os.environ.get("PW_DEBUG"):
                    print("[pw-c-prev] dst empty -> fallback python (draw=%d)" % ret)
                return False
            cr.set_source_surface(self._dst_surface, 0, 0)
            cr.paint()
            self._tri_stat["draw"] = ret
            if os.environ.get("PW_DEBUG"):
                print("[pw-c-prev] draw=", ret)
            return True
        except Exception as e:
            print(f"[puppet-warp] c-preview failed, fallback python: {e}")
            return False

    def _on_pin_edge_toggled(self, b):
        self.pin_edge = b.get_active()
        self._solve_preview()
        self.drawing_area.queue_draw()

    def _on_rotate_toggled(self, b):
        """Mute rotate+scale without discarding theta/scale (re-enable restores)."""
        self.rotate_on = b.get_active()
        self._solve_preview()
        self.drawing_area.queue_draw()

    def _solve_preview(self, iters=None):
        """Solve the ARAP system from scratch (cold) and store deformed verts."""
        lib = _load_arap_lib()
        if lib is None:
            self.deformed_verts = None
            return
        if iters is None:
            iters = self._idle_iters
        try:
            grid = lib.arap_grid_new(
                ctypes.c_float(self.puppet_x),
                ctypes.c_float(self.puppet_y),
                ctypes.c_float(self.puppet_w),
                ctypes.c_float(self.puppet_h),
                ctypes.c_int(self.cols),
                ctypes.c_int(self.rows))
            if not grid:
                self.deformed_verts = None
                return
            if hasattr(lib, "arap_grid_set_cell_weights"):
                cw = self.cell_weights(self.cols, self.rows)
                arr = (ctypes.c_float * len(cw))(*cw)
                lib.arap_grid_set_cell_weights(grid, arr, len(cw))
            if hasattr(lib, "arap_grid_set_pin_edge"):
                lib.arap_grid_set_pin_edge(grid, 1 if self.pin_edge else 0)
            for p in self.pins:
                if (self.rotate_on
                        and (p.theta != 0.0 or p.scale != 1.0)
                        and hasattr(lib, "arap_grid_add_pin_sim")):
                    lib.arap_grid_add_pin_sim(grid,
                                              ctypes.c_float(p.sx),
                                              ctypes.c_float(p.sy),
                                              ctypes.c_float(p.dx - p.sx),
                                              ctypes.c_float(p.dy - p.sy),
                                              ctypes.c_float(p.theta),
                                              ctypes.c_float(p.scale))
                else:
                    lib.arap_grid_add_pin(grid,
                                          ctypes.c_float(p.sx),
                                          ctypes.c_float(p.sy),
                                          ctypes.c_float(p.dx - p.sx),
                                          ctypes.c_float(p.dy - p.sy))
            lib.arap_grid_solve(grid, ctypes.c_int(iters), 150,
                                ctypes.c_float(0.05), ctypes.c_int(0))
            n = ctypes.c_int(0)
            verts_ptr = lib.arap_grid_verts(grid, ctypes.byref(n))
            if verts_ptr:
                vv = [(verts_ptr[i].x, verts_ptr[i].y)
                      for i in range(n.value)]
                self.deformed_verts = vv
                if len(vv) == (self.cols + 1) * (self.rows + 1):
                    self._last_good_verts = vv
            else:
                self.deformed_verts = None
            lib.arap_grid_free(grid)
        except Exception as e:
            print(f"[puppet-warp] preview solve failed: {e}")
            self.deformed_verts = None

    def _deferred_update(self):
        self._update_timer = None
        dragging = self.is_dragging_pin or self.is_rotating
        self._solve_preview(self._drag_iters if dragging else self._idle_iters)
        self.drawing_area.queue_draw()
        return False

    def _schedule_update(self):
        if self._update_timer is None:
            self._update_timer = GLib.timeout_add(40, self._deferred_update)

    def _screen_to_world(self, sx, sy):
        return ((sx - self.pan_x) / self.zoom, (sy - self.pan_y) / self.zoom)

    def _world_to_screen(self, wx, wy):
        return (wx * self.zoom + self.pan_x, wy * self.zoom + self.pan_y)

    def _find_pin_at(self, sx, sy, rad=14.0):
        """Index of the pin whose body is under screen (sx,sy)."""
        best = None
        bd = rad * rad
        for i, p in enumerate(self.pins):
            px, py = self._world_to_screen(p.dx, p.dy)
            d = (sx - px) ** 2 + (sy - py) ** 2
            if d <= bd:
                bd = d
                best = i
        return best

    def _delete_pin_at(self, h):
        self._push_undo()
        del self.pins[h]
        if self.active_pin_index == h:
            self.active_pin_index = None
        elif self.active_pin_index is not None and self.active_pin_index > h:
            self.active_pin_index -= 1
        self._solve_preview()
        self.drawing_area.queue_draw()

    def _on_button_press(self, w, e):
        """Handle add / select / drag-start / rotate-start / delete / reset."""
        self.drawing_area.grab_focus()
        if e.button == 2:
            self.is_panning = True
            self.pan_start = (e.x - self.pan_x, e.y - self.pan_y)
            return True
        if e.button == 3:
            h = self._find_pin_at(e.x, e.y)
            if h is not None:
                self._delete_pin_at(h)
            return True
        if e.button == 1 and e.type == Gdk.EventType.DOUBLE_BUTTON_PRESS:
            if self._find_handle_at(e.x, e.y) is not None:
                i = self.active_pin_index
                self._push_undo()
                self.pins[i].theta = 0.0
                self.pins[i].scale = 1.0
                self._solve_preview()
                self.drawing_area.queue_draw()
                return True
            h = self._find_pin_at(e.x, e.y)
            if h is not None:
                p = self.pins[h]
                self._push_undo()
                p.dx, p.dy = p.sx, p.sy
                p.theta = 0.0
                p.scale = 1.0
                self.active_pin_index = h
                self.is_dragging_pin = False
                self._solve_preview()
                self.drawing_area.queue_draw()
                return True
        if e.button == 1:
            alt = bool(e.state & Gdk.ModifierType.MOD1_MASK)
            hh = self._find_handle_at(e.x, e.y)
            if hh is not None and not alt:
                p = self.pins[hh]
                wx, wy = self._screen_to_world(e.x, e.y)
                self._push_undo()
                self.is_rotating = True
                self.rot_grab = math.atan2(wy - p.dy, wx - p.dx) - p.theta
                self.active_pin_index = hh
                self.drawing_area.queue_draw()
                return True
            h = self._find_pin_at(e.x, e.y)
            wx, wy = self._screen_to_world(e.x, e.y)
            if alt and h is not None:
                self._delete_pin_at(h)
                return True
            if h is None:
                self._push_undo()
                self.pins.append(Pin(wx, wy))
                self.active_pin_index = len(self.pins) - 1
                self.drag_offset = (0.0, 0.0)
                self.is_dragging_pin = True
                self._drag_pushed = True
            else:
                self.active_pin_index = h
                self.drag_offset = (self.pins[h].dx - wx, self.pins[h].dy - wy)
                self.is_dragging_pin = True
                self._drag_pushed = False
            self.drawing_area.queue_draw()
            return True
        return False

    def _on_button_release(self, w, e):
        if e.button == 2:
            self.is_panning = False
        if e.button == 1 and self.is_rotating:
            self.is_rotating = False
            self._solve_preview()
            self.drawing_area.queue_draw()
            return True
        if e.button == 1:
            if self.is_dragging_pin:
                self.is_dragging_pin = False
                self._drag_pushed = False
                self._solve_preview()
                self.drawing_area.queue_draw()
            return True

    def _on_motion(self, w, e):
        """Pan / rotate-drag / translate-drag / hover highlight."""
        if self.is_panning:
            self.pan_x = e.x - self.pan_start[0]
            self.pan_y = e.y - self.pan_start[1]
            self.drawing_area.queue_draw()
            return True
        if self.is_rotating and self.active_pin_index is not None:
            wx, wy = self._screen_to_world(e.x, e.y)
            p = self.pins[self.active_pin_index]
            ang = math.atan2(wy - p.dy, wx - p.dx) - self.rot_grab
            while ang > math.pi:
                ang -= 2 * math.pi
            while ang < -math.pi:
                ang += 2 * math.pi
            p.theta = ang
            self._schedule_update()
            self.drawing_area.queue_draw()
            return True
        if self.is_dragging_pin and self.active_pin_index is not None:
            if not self._drag_pushed:
                self._push_undo()
                self._drag_pushed = True
            wx, wy = self._screen_to_world(e.x, e.y)
            p = self.pins[self.active_pin_index]
            ndx = wx + self.drag_offset[0]
            ndy = wy + self.drag_offset[1]
            if e.state & Gdk.ModifierType.SHIFT_MASK:
                ddx0 = ndx - p.sx
                ddy0 = ndy - p.sy
                if abs(ddx0) >= abs(ddy0):
                    ndy = p.sy
                else:
                    ndx = p.sx
            maxd = 3.0 * self.puppet_radius
            ddx = ndx - p.sx
            ddy = ndy - p.sy
            r = math.hypot(ddx, ddy)
            if r > maxd:
                ndx = p.sx + ddx / r * maxd
                ndy = p.sy + ddy / r * maxd
            p.dx = ndx
            p.dy = ndy
            self._schedule_update()
            self.drawing_area.queue_draw()
            return True
        h = self._find_pin_at(e.x, e.y)
        if h != self.hovered_pin_index:
            self.hovered_pin_index = h
            self.drawing_area.queue_draw()
        return False

    def _on_scroll(self, w, e):
        """Ctrl+wheel scale, Alt+wheel rotate, otherwise zoom the view."""
        if self.rotate_on and (e.state & Gdk.ModifierType.CONTROL_MASK):
            h = self._find_pin_at(e.x, e.y)
            if h is None:
                h = self.active_pin_index
            if h is not None and 0 <= h < len(self.pins):
                if e.direction == Gdk.ScrollDirection.UP:
                    step = 0.10 if (e.state & Gdk.ModifierType.SHIFT_MASK) else 0.02
                elif e.direction == Gdk.ScrollDirection.DOWN:
                    step = -(0.10 if (e.state & Gdk.ModifierType.SHIFT_MASK) else 0.02)
                else:
                    return True
                self._push_undo()
                self.pins[h].scale = max(0.25, min(4.0, self.pins[h].scale + step))
                self.active_pin_index = h
                self._schedule_update()
                self.drawing_area.queue_draw()
                return True
        if self.rotate_on and (e.state & Gdk.ModifierType.MOD1_MASK):
            h = self._find_pin_at(e.x, e.y)
            if h is None:
                h = self.active_pin_index
            if h is not None and 0 <= h < len(self.pins):
                deg = 15.0 if (e.state & Gdk.ModifierType.SHIFT_MASK) else 2.0
                if e.direction == Gdk.ScrollDirection.UP:
                    step = math.radians(deg)
                elif e.direction == Gdk.ScrollDirection.DOWN:
                    step = -math.radians(deg)
                else:
                    return True
                self._push_undo()
                a = self.pins[h].theta + step
                while a > math.pi:
                    a -= 2 * math.pi
                while a < -math.pi:
                    a += 2 * math.pi
                self.pins[h].theta = a
                self.active_pin_index = h
                self._schedule_update()
                self.drawing_area.queue_draw()
                return True
        s = 1.15 if e.direction == Gdk.ScrollDirection.UP else 1.0 / 1.15
        nz = max(0.05, min(10.0, self.zoom * s))
        self.pan_x = e.x - (e.x - self.pan_x) * (nz / self.zoom)
        self.pan_y = e.y - (e.y - self.pan_y) * (nz / self.zoom)
        self.zoom = nz
        self.drawing_area.queue_draw()
        return True

    def _on_leave(self, w, e):
        self.hovered_pin_index = None
        self.drawing_area.queue_draw()
        return False

    def _on_grid_changed(self, spin):
        nc = int(self.cols_spin.get_value())
        nr = int(self.rows_spin.get_value())
        if nc == self.cols and nr == self.rows:
            return
        self.cols = nc
        self.rows = nr
        self._solve_preview()
        self.drawing_area.queue_draw()

    def _clear_pins(self):
        self.pins = []
        self.active_pin_index = None
        self._solve_preview()
        self.drawing_area.queue_draw()

    def fit_to_view(self):
        """Zoom/pan so the whole canvas fits the drawing area."""
        a = self.drawing_area.get_allocation()
        dw = a.width if a.width > 50 else 720
        dh = a.height if a.height > 50 else 520
        self.zoom = min((dw - 80) / float(self.canvas_w),
                        (dh - 80) / float(self.canvas_h), 2.0)
        self.pan_x = (dw - self.canvas_w * self.zoom) / 2.0
        self.pan_y = (dh - self.canvas_h * self.zoom) / 2.0
        self.drawing_area.queue_draw()

    def _warp_preview_cairo(self, cr):
        """Draw the warped layer: C raster first, Python triangle loop as fallback."""
        for k in self._tri_stat:
            self._tri_stat[k] = 0
        n_expect = (self.cols + 1) * (self.rows + 1)
        verts = self.deformed_verts
        if verts is None or len(verts) != n_expect:
            verts = self._last_good_verts
        if verts is None or len(verts) != n_expect:
            cr.set_source_surface(self.src_surface, 0, 0)
            cr.paint_with_alpha(0.55)
            return
        lib = _LIB
        if (lib is not None and hasattr(lib, "pw_render_preview")
                and os.environ.get("PW_NO_C_PREVIEW") != "1"):
            if self._c_render(lib, cr, verts):
                return
        cols, rows = self.cols, self.rows
        pattern = cairo.SurfacePattern(self.src_surface)
        pattern.set_filter(cairo.FILTER_BILINEAR)
        pattern.set_extend(cairo.EXTEND_NONE)
        try:
            for r in range(rows):
                for c in range(cols):
                    i00 = r * (cols + 1) + c
                    i10 = i00 + 1
                    i01 = i00 + (cols + 1)
                    i11 = i01 + 1
                    s00 = (self.puppet_x + c * self.puppet_w / cols,
                           self.puppet_y + r * self.puppet_h / rows)
                    s10 = (self.puppet_x + (c + 1) * self.puppet_w / cols,
                           self.puppet_y + r * self.puppet_h / rows)
                    s01 = (self.puppet_x + c * self.puppet_w / cols,
                           self.puppet_y + (r + 1) * self.puppet_h / rows)
                    s11 = (self.puppet_x + (c + 1) * self.puppet_w / cols,
                           self.puppet_y + (r + 1) * self.puppet_h / rows)
                    d00 = verts[i00]
                    d10 = verts[i10]
                    d01 = verts[i01]
                    d11 = verts[i11]
                    self._warp_tri(cr, pattern, s00, s10, s01, d00, d10, d01)
                    self._warp_tri(cr, pattern, s10, s11, s01, d10, d11, d01)
        except Exception:
            pass
        if os.environ.get("PW_DEBUG"):
            print("[pw-py-tri]", self._tri_stat)

    def _warp_tri(self, cr, pattern, s0, s1, s2, d0, d1, d2):
        """Affine-map one source triangle onto its deformed destination."""
        u0, v0 = d0
        u1, v1 = d1
        u2, v2 = d2
        x0, y0 = s0
        x1, y1 = s1
        x2, y2 = s2
        det = (u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0)
        if abs(det) < 0.5:
            self._tri_stat["det"] += 1
            return
        inv = 1.0 / det
        a = inv * ((v2 - v0) * (x1 - x0) - (v1 - v0) * (x2 - x0))
        b = inv * ((v2 - v0) * (y1 - y0) - (v1 - v0) * (y2 - y0))
        c = inv * (-(u2 - u0) * (x1 - x0) + (u1 - u0) * (x2 - x0))
        d = inv * (-(u2 - u0) * (y1 - y0) + (u1 - u0) * (y2 - y0))
        e = x0 - a * u0 - c * v0
        f = y0 - b * u0 - d * v0
        sc = max(abs(a), abs(b), abs(c), abs(d))
        if sc > 1e4 or sc < 1e-6:
            self._tri_stat["scale"] += 1
            return
        m_det = a * d - b * c
        if not math.isfinite(m_det) or abs(m_det) < 1e-12:
            self._tri_stat["mdet"] += 1
            return
        if not all(math.isfinite(v) for v in (a, b, c, d, e, f)):
            self._tri_stat["mdet"] += 1
            return
        pattern.set_matrix(cairo.Matrix(xx=a, yx=b, xy=c, yy=d, x0=e, y0=f))
        self._tri_stat["draw"] += 1
        try:
            cr.set_source(pattern)
            cr.new_path()
            cr.move_to(d0[0], d0[1])
            cr.line_to(d1[0], d1[1])
            cr.line_to(d2[0], d2[1])
            cr.close_path()
            cr.fill()
        except cairo.Error:
            self._tri_stat["cairo"] += 1

    def _on_draw(self, w, cr):
        """Paint: bg fill, checker, static background, warped layer, mesh, pins, ring."""
        cr.set_source_rgb(0.13, 0.13, 0.14)
        cr.paint()
        cr.save()
        cr.translate(self.pan_x, self.pan_y)
        cr.scale(self.zoom, self.zoom)
        cr.save()
        cr.rectangle(0, 0, self.canvas_w, self.canvas_h)
        cr.clip()
        cr.set_source_surface(self._ensure_checker(), 0, 0)
        cr.paint()
        cr.restore()
        if self._bg_surface is not None:
            cr.set_source_surface(self._bg_surface, 0, 0)
            cr.paint()
        self._warp_preview_cairo(cr)
        if not self.is_dragging_pin and not self.is_rotating \
                and self.show_weights_check.get_active():
            ws = self.cell_weights(self.cols, self.rows)
            verts = self.deformed_verts
            if verts is None:
                verts = [(self.puppet_x + cc * self.puppet_w / self.cols,
                          self.puppet_y + rr * self.puppet_h / self.rows)
                         for rr in range(self.rows + 1)
                         for cc in range(self.cols + 1)]
            cr.set_line_width(max(0.75, 1.0 / self.zoom))
            for rr in range(self.rows):
                for cc in range(self.cols):
                    wgt = ws[rr * self.cols + cc]
                    i00 = rr * (self.cols + 1) + cc
                    i10 = i00 + 1
                    i01 = i00 + (self.cols + 1)
                    i11 = i01 + 1
                    if wgt > 0.02:
                        cr.set_source_rgba(0.2, 0.9, 1.0, 0.10 + 0.30 * wgt)
                    else:
                        cr.set_source_rgba(1.0, 0.25, 0.20, 0.10)
                    cr.new_path()
                    cr.move_to(verts[i00][0], verts[i00][1])
                    cr.line_to(verts[i10][0], verts[i10][1])
                    cr.line_to(verts[i11][0], verts[i11][1])
                    cr.line_to(verts[i01][0], verts[i01][1])
                    cr.close_path()
                    cr.stroke()
            cr.set_source_rgba(0.2, 0.9, 1.0, 0.5)
            cr.set_line_width(max(1.0, 1.5 / self.zoom))
            cr.rectangle(self.puppet_x, self.puppet_y,
                         self.puppet_w, self.puppet_h)
            cr.stroke()
        pr = max(5.0, 8.0 / self.zoom)
        for i, p in enumerate(self.pins):
            if (not self.is_dragging_pin and not self.is_rotating
                    and (abs(p.dx - p.sx) > 0.5 or abs(p.dy - p.sy) > 0.5)):
                cr.save()
                cr.set_source_rgba(1, 1, 1, 0.45)
                cr.set_line_width(max(1.0, 1.5 / self.zoom))
                cr.set_dash([5 / self.zoom, 5 / self.zoom])
                cr.move_to(p.sx, p.sy)
                cr.line_to(p.dx, p.dy)
                cr.stroke()
                cr.restore()
            cr.set_source_rgba(0, 0, 0, 0.85)
            cr.arc(p.dx, p.dy, pr + 1.5 / self.zoom, 0, 6.2832)
            cr.fill()
            if i == self.active_pin_index:
                cr.set_source_rgb(0.2, 1.0, 0.3)
            elif i == self.hovered_pin_index:
                cr.set_source_rgb(0.2, 0.9, 1.0)
            else:
                cr.set_source_rgb(1.0, 0.9, 0.2)
            cr.arc(p.dx, p.dy, pr, 0, 6.2832)
            cr.fill()
        i = self.active_pin_index
        if (self.rotate_on and i is not None and 0 <= i < len(self.pins)
                and not self.is_dragging_pin):
            p = self.pins[i]
            rr = 28.0 / self.zoom
            cr.set_line_width(max(1.0, 1.5 / self.zoom))
            cr.set_source_rgba(1.0, 0.6, 0.1, 0.9)
            cr.arc(p.dx, p.dy, rr, 0, 6.2832)
            cr.stroke()
            hx, hy = self._rot_handle_world(p)
            hr = max(4.0, 6.0 / self.zoom)
            cr.set_source_rgba(0.0, 0.0, 0.0, 0.85)
            cr.arc(hx, hy, hr + 1.2 / self.zoom, 0, 6.2832)
            cr.fill()
            cr.set_source_rgba(1.0, 0.6, 0.1, 1.0)
            cr.arc(hx, hy, hr, 0, 6.2832)
            cr.fill()
            z = self.zoom
            cr.set_font_size(max(12.0, 15.0 / z))
            txt = f"\u03b8={math.degrees(p.theta):.0f}\u00b0"
            if abs(p.scale - 1.0) > 0.005:
                txt += f"  s={p.scale:.2f}"
            tx = p.dx + rr + 6.0 / z
            ty = p.dy - rr - 4.0 / z
            ext = cr.text_extents(txt)
            padx = 4.0 / z
            pady = 2.0 / z
            crn = 3.0 / z
            bx = tx + ext.x_bearing - padx
            by = ty + ext.y_bearing - pady
            bw = ext.width + 2.0 * padx
            bh = ext.height + 2.0 * pady
            cr.save()
            cr.set_source_rgba(0.0, 0.0, 0.0, 0.60)
            cr.new_sub_path()
            cr.arc(bx + bw - crn, by + crn, crn, -math.pi / 2, 0)
            cr.arc(bx + bw - crn, by + bh - crn, crn, 0, math.pi / 2)
            cr.arc(bx + crn, by + bh - crn, crn, math.pi / 2, math.pi)
            cr.arc(bx + crn, by + crn, crn, math.pi, 3 * math.pi / 2)
            cr.close_path()
            cr.fill()
            cr.restore()
            cr.set_source_rgba(1.0, 0.6, 0.1, 1.0)
            cr.move_to(tx, ty)
            cr.show_text(txt)
        cr.restore()


def apply_filter_to_drawable(drawable, params_json):
    """Replace any stale Puppet Warp filter with a fresh one carrying params."""
    stale = []
    try:
        for f in list(drawable.get_filters() or []):
            try:
                if f.get_operation_name() == GEGL_OP_NAME:
                    stale.append(f)
            except Exception:
                continue
    except Exception:
        pass
    for f in stale:
        try:
            f.delete()
        except Exception:
            try:
                drawable.remove_filter(f)
            except Exception:
                pass
    filt = Gimp.DrawableFilter.new(drawable, GEGL_OP_NAME, GEGL_OP_LABEL)
    if filt is None:
        return None
    cfg = filt.get_config()
    cfg.set_property("params-json", params_json)
    if hasattr(drawable, 'append_filter'):
        drawable.append_filter(filt)
    elif hasattr(drawable, 'add_filter'):
        drawable.add_filter(filt)
    try:
        drawable.update(0, 0, drawable.get_width(), drawable.get_height())
    except Exception:
        pass
    try:
        filt.set_visible(False)
        filt.set_visible(True)
    except Exception:
        pass
    try:
        op = drawable.get_opacity()
        drawable.set_opacity(max(0.0, op - 1.0))
        drawable.set_opacity(op)
    except Exception:
        pass
    try:
        drawable.set_visible(False)
        Gimp.displays_flush()
        drawable.set_visible(True)
    except Exception:
        pass
    return filt


def remove_filter_from_drawable(drawable):
    """Delete all Puppet Warp filters from a drawable."""
    try:
        for f in list(drawable.get_filters() or []):
            try:
                if f.get_operation_name() == GEGL_OP_NAME:
                    f.delete()
            except Exception:
                continue
    except Exception:
        pass


class PuppetWarpPlugin(Gimp.PlugIn):
    def do_set_i18n(self, procname):
        return True, 'gimp30-python', None

    def do_query_procedures(self):
        return [PLUG_IN_PROC]

    def do_create_procedure(self, name):
        proc = Gimp.ImageProcedure.new(self, name, Gimp.PDBProcType.PLUGIN,
                                       self.run, None)
        proc.set_image_types("RGB*, GRAY*, INDEXED*")
        proc.set_sensitivity_mask(Gimp.ProcedureSensitivityMask.DRAWABLE |
                                  Gimp.ProcedureSensitivityMask.DRAWABLES)
        proc.set_menu_label("_Puppet Warp (ARAP)...")
        proc.add_menu_path("<Image>/Filters/Deform")
        proc.add_menu_path("<Image>/Tools/Transform Tools")
        proc.set_attribution("Hexsian", "Hexsian", "2026")
        proc.set_documentation(
            "ARAP-based puppet warp. Non-destructive and re-editable. "
            "Isolate your selection to its own layer before warping.",
            "Click to add pins, drag to move, right-click to delete, "
            "double-click to reset a pin, drag the orange ring to rotate, "
            "Ctrl+wheel to scale.", name)
        return proc

    def run(self, procedure, run_mode, image, drawables, config, data):
        if len(drawables) != 1 or not isinstance(drawables[0], Gimp.Drawable):
            return procedure.new_return_values(
                Gimp.PDBStatusType.CALLING_ERROR,
                GLib.Error("Need exactly one drawable."))
        if run_mode == Gimp.RunMode.NONINTERACTIVE:
            return procedure.new_return_values(
                Gimp.PDBStatusType.CALLING_ERROR,
                GLib.Error("Interactive only."))
        ensure_op()
        GimpUi.init(PLUG_IN_BINARY)
        try:
            dlg = PuppetWarpDialog(image, drawables[0])
        except RuntimeError as e:
            Gimp.message("Puppet Warp: " + str(e))
            return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)
        GimpUi.window_set_transient(dlg)
        resp = dlg.run()
        params_json = dlg.get_params_json()
        pin_count = len(dlg.pins)
        verts_snapshot = list(dlg.deformed_verts or [])
        cols_snap, rows_snap = dlg.cols, dlg.rows
        dlg.destroy()
        if resp != Gtk.ResponseType.OK:
            return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)
        if pin_count == 0:
            remove_filter_from_drawable(drawables[0])
            try:
                Gimp.displays_flush()
            except Exception:
                pass
            return procedure.new_return_values(Gimp.PDBStatusType.SUCCESS, None)
        token = ""
        path = ""
        if verts_snapshot and len(verts_snapshot) == (cols_snap + 1) * (rows_snap + 1):
            import struct
            import time
            token = f"{os.getpid()}-{time.time_ns()}"
            path = os.path.join(GLib.get_tmp_dir(),
                                f"puppet-warp-verts-{token}.bin")
            flat = [c for v in verts_snapshot for c in v]
            try:
                with open(path, "wb") as fh:
                    fh.write(token.encode().ljust(64, b"\0"))
                    fh.write(struct.pack("<III", cols_snap, rows_snap,
                                         len(verts_snapshot)))
                    fh.write(struct.pack(f"<{len(flat)}f", *flat))
            except Exception as e:
                print(f"[puppet-warp] sidecar write failed: {e}")
                token = ""
        pkg = json.loads(params_json)
        pkg["verts_token"] = token
        params_json = json.dumps(pkg, separators=(',', ':'), sort_keys=True)
        apply_filter_to_drawable(drawables[0], params_json)
        try:
            Gimp.displays_flush()
        except Exception:
            pass
        if token and path:
            try:
                os.unlink(path)
            except OSError:
                pass
        print(f"[puppet-warp] applied filter, pins={pin_count}, sidecar={bool(token)}")
        if _op_freshly_installed:
            Gimp.message("Puppet Warp: the GEGL operation was installed into "
                         f"{GEGL_USER_DIR}.\nRestart GIMP once so the filter "
                         "can render.")
        return procedure.new_return_values(Gimp.PDBStatusType.SUCCESS, None)


if __name__ == '__main__':
    Gimp.main(PuppetWarpPlugin.__gtype__, sys.argv)
