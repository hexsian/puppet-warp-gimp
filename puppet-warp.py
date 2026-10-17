#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Puppet Warp: ARAP-based non-destructive deform filter for GIMP 3."""

import ctypes
import json
import math
import os
import platform
import shutil
import subprocess
import sys
import time
import traceback

sys.dont_write_bytecode = True

import gi

gi.require_version("Gimp", "3.0")
gi.require_version("GimpUi", "3.0")
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Gegl", "0.4")

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


PLUGIN_DIR = (
    os.path.dirname(os.path.abspath(__file__))
    if "__file__" in globals()
    else _default_plugin_dir()
)


def _gegl_user_dir():
    return os.path.join(GLib.get_user_data_dir(), "gegl-0.4", "plug-ins")


GEGL_USER_DIR = _gegl_user_dir()

MW_SO = os.path.join(PLUGIN_DIR, _LIB_NAME)
MW_C = os.path.join(PLUGIN_DIR, "puppet-warp.c")
AGRID_C = os.path.join(PLUGIN_DIR, "arap_grid.c")
DIALOG_PID = os.path.join(GLib.get_tmp_dir(), "puppet-warp-dialog.pid")

MAX_PINS = 256

_op_ready = False
_op_freshly_installed = False
_LIB = None


class ArapVec2(ctypes.Structure):
    _fields_ = [("x", ctypes.c_float), ("y", ctypes.c_float)]


def _load_arap_lib():
    global _LIB

    if _LIB is not None:
        return _LIB

    candidates = []
    for prio, d in enumerate((PLUGIN_DIR, GEGL_USER_DIR)):
        for name in (_LIB_NAME, "puppet-warp.so", "puppet-warp.dll"):
            p = os.path.join(d, name)
            if p in candidates:
                continue
            mtime = os.path.getmtime(p) if os.path.exists(p) else 0.0
            candidates.append((prio, -mtime, p))
    candidates.sort()
    candidates = [p for _, _, p in candidates]

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
                ctypes.c_void_p,
                ctypes.c_int,
                ctypes.c_int,
                ctypes.c_float,
                ctypes.c_int,
            ]
            lib.arap_grid_solve.restype = ctypes.c_int

            lib.arap_grid_verts.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
            lib.arap_grid_verts.restype = ctypes.POINTER(ArapVec2)

            lib.arap_grid_free.argtypes = [ctypes.c_void_p]
            lib.arap_grid_free.restype = None

            if hasattr(lib, "arap_grid_set_cell_weights"):
                lib.arap_grid_set_cell_weights.argtypes = [
                    ctypes.c_void_p,
                    ctypes.POINTER(ctypes.c_float),
                    ctypes.c_int,
                ]
                lib.arap_grid_set_cell_weights.restype = None

            if hasattr(lib, "arap_grid_set_pin_edge"):
                lib.arap_grid_set_pin_edge.argtypes = [ctypes.c_void_p, ctypes.c_int]
                lib.arap_grid_set_pin_edge.restype = None

            if hasattr(lib, "arap_grid_add_pin_sim"):
                lib.arap_grid_add_pin_sim.argtypes = [ctypes.c_void_p] + [ctypes.c_float] * 6
                lib.arap_grid_add_pin_sim.restype = None

            has_reset = hasattr(lib, "arap_grid_reset_pins")
            if has_reset:
                lib.arap_grid_reset_pins.argtypes = [ctypes.c_void_p]
                lib.arap_grid_reset_pins.restype = None

            has_preview = hasattr(lib, "pw_render_preview")
            if has_preview:
                lib.pw_render_preview.argtypes = [
                    ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                    ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                    ctypes.c_int, ctypes.c_int,
                    ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                    ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float,
                ]
                lib.pw_render_preview.restype = ctypes.c_int

            view_argtypes = [
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int,
                ctypes.c_int, ctypes.c_int,
                ctypes.POINTER(ctypes.c_float), ctypes.c_int,
                ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float,
                ctypes.c_float, ctypes.c_float, ctypes.c_float,
                ctypes.c_int,
            ]

            has_view = hasattr(lib, "pw_render_preview_view")
            if has_view:
                lib.pw_render_preview_view.argtypes = view_argtypes
                lib.pw_render_preview_view.restype = ctypes.c_int

            has_depth_view = hasattr(lib, "pw_render_preview_view_depth")
            if has_depth_view:
                lib.pw_render_preview_view_depth.argtypes = view_argtypes + [
                    ctypes.c_int, ctypes.POINTER(ctypes.c_int)
                ]
                lib.pw_render_preview_view_depth.restype = ctypes.c_int

            if hasattr(lib, "arap_grid_add_pin_taper"):
                lib.arap_grid_add_pin_taper.argtypes = [
                    ctypes.c_void_p,
                    ctypes.c_float, ctypes.c_float, ctypes.c_float, ctypes.c_float,
                    ctypes.c_float, ctypes.c_float, ctypes.c_float,
                ]
                lib.arap_grid_add_pin_taper.restype = None

            _LIB = lib

            print(
                f"[puppet-warp] loaded {path} "
                f"(c-preview={'yes' if has_preview else 'no'} "
                f"view={'yes' if has_view else 'no'} "
                f"depth={'yes' if has_depth_view else 'no'} "
                f"reset={'yes' if has_reset else 'no'})"
            )

            return lib

        except Exception as e:
            print(f"[puppet-warp] ctypes failed {path}: {e}")
            continue

    return None


def _build_so():
    srcs = (MW_C, AGRID_C)

    if (
        os.path.exists(MW_SO)
        and all(os.path.exists(s) for s in srcs)
        and os.path.getmtime(MW_SO) >= max(os.path.getmtime(s) for s in srcs)
    ):
        return True

    if not all(os.path.exists(s) for s in srcs):
        return False

    try:
        pc = subprocess.run(
            ["pkg-config", "--cflags", "--libs", "gegl-0.4", "glib-2.0"],
            capture_output=True,
            text=True,
            timeout=30,
        )

        if pc.returncode != 0:
            return False

        omp_ok = False
        try:
            pr = subprocess.run(
                ["gcc", "-fopenmp", "-x", "c", "-c", "/dev/null", "-o", os.devnull],
                capture_output=True,
                timeout=30,
            )
            omp_ok = (pr.returncode == 0)
        except Exception:
            omp_ok = False

        flags = [
            "gcc",
            "-O3",
            "-ftree-vectorize",
            "-shared",
        ]

        if not _IS_WIN:
            flags.append("-fPIC")

        if omp_ok:
            flags.append("-fopenmp")

        flags += [
            MW_C,
            AGRID_C,
            "-o", MW_SO + ".tmp",
            "-lm",
        ]

        cmd = flags + pc.stdout.split()

        r = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=180,
            cwd=PLUGIN_DIR,
        )

        if r.returncode == 0 and os.path.exists(MW_SO + ".tmp"):
            os.replace(MW_SO + ".tmp", MW_SO)
            return True

        if os.environ.get("PW_DEBUG"):
            print("[puppet-warp] build failed:", r.stderr[-800:] if r.stderr else "")

        return False

    except Exception as e:
        if os.environ.get("PW_DEBUG"):
            print(f"[puppet-warp] build exception: {e}")
        return False


def _install_lib():
    """Copy built/shipped binary into GEGL user plug-in dir."""
    global _op_freshly_installed

    dst = os.path.join(GEGL_USER_DIR, _LIB_NAME)

    if not os.path.exists(MW_SO):
        return

    try:
        if (
            os.path.exists(dst)
            and os.path.getsize(dst) == os.path.getsize(MW_SO)
            and os.path.getmtime(dst) >= os.path.getmtime(MW_SO)
        ):
            return

        os.makedirs(GEGL_USER_DIR, exist_ok=True)
        fresh = not os.path.exists(dst)
        shutil.copy2(MW_SO, dst)
        _op_freshly_installed = fresh

    except Exception as e:
        print(
            f"[puppet-warp] could not install {dst}: {e} "
            "(close GIMP, reopen it and try again to update the operation)"
        )


def _load_op():
    for d in (PLUGIN_DIR, GEGL_USER_DIR):
        try:
            if os.path.isdir(d) and hasattr(Gegl, "load_directory"):
                Gegl.load_directory(d)
        except Exception:
            pass


def _pid_alive(pid):
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
            return True

    try:
        return os.path.exists("/proc/%d" % pid)
    except Exception:
        return True


def _dialog_lock_acquire():
    """Single-instance guard. Returns path, "" if unusable, None if busy."""
    try:
        if os.path.exists(DIALOG_PID):
            with open(DIALOG_PID) as fh:
                old = int(fh.read().strip() or "0")
            if old not in (0, os.getpid()) and _pid_alive(old):
                return None

        with open(DIALOG_PID, "w") as fh:
            fh.write(str(os.getpid()))

        return DIALOG_PID

    except Exception:
        return ""


def _dialog_lock_release(path):
    if not path:
        return

    try:
        with open(path) as fh:
            cur = int(fh.read().strip() or "0")
        if cur == os.getpid():
            os.unlink(path)
    except Exception:
        pass


def ensure_op():
    global _op_ready

    if _op_ready:
        return True

    _build_so()
    _install_lib()
    _load_op()
    _load_arap_lib()

    _op_ready = True
    return True


def _hug_default_size(cw, ch):
    asp = float(cw) / float(ch) if ch else 1.0
    th = 560
    tw = int(round(th * asp))

    try:
        disp = Gdk.Display.get_default()
        mon = disp.get_primary_monitor() or disp.get_monitor(0)
        geo = mon.get_geometry()
        mw = int(geo.width * 0.85)
        mh = int(geo.height * 0.80)
    except Exception:
        mw, mh = 1400, 1000

    if tw > mw:
        tw = mw
        th = int(round(tw / asp)) if asp else th

    if th > mh:
        th = mh
        tw = int(round(th * asp))

    tw = max(240, tw)
    th = max(200, th)

    return tw + 12, th + 150


def _unpack_offsets(t):
    if isinstance(t, (list, tuple)) and len(t) >= 3 and isinstance(t[0], bool):
        return int(t[1]), int(t[2])
    if isinstance(t, (list, tuple)) and len(t) >= 2:
        return int(t[0]), int(t[1])
    raise ValueError(f"unexpected offsets tuple: {t!r}")


def _layer_index(layers, target):
    for i, l in enumerate(layers):
        if l == target or l is target:
            return i
    raise ValueError("layer not found in get_layers()")


def _as_list(x):
    return x if isinstance(x, list) else list(x)


class Pin:
    __slots__ = ("sx", "sy", "dx", "dy", "theta", "scale", "qx", "qy", "taper", "depth")

    def __init__(self, sx, sy):
        self.sx = float(sx)
        self.sy = float(sy)
        self.dx = float(sx)
        self.dy = float(sy)
        self.theta = 0.0
        self.scale = 1.0
        self.qx = float(sx)
        self.qy = float(sy)
        self.taper = 0.0
        self.depth = 0


def load_existing_params(drawable):
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


def _pw_cw_floats(payload):
    obj = payload[0] if isinstance(payload, tuple) and payload else payload

    if isinstance(obj, (list, tuple)) and obj:
        first = obj[0]
        if (
            not isinstance(first, (int, float, complex))
            and hasattr(first, "__len__")
        ):
            obj = first

    try:
        return [float(getattr(v, "value", v)) for v in obj]
    except Exception:
        return []


def _pw_cw_json(payload):
    return [round(x, 4) for x in _pw_cw_floats(payload)]


class PuppetWarpDialog(Gtk.Dialog):
    _dead = False
    extend_on = False
    _extend_supported = True
    pad_left = 0
    pad_top = 0
    pad_right = 0
    pad_bottom = 0
    preview_pad_left = 0
    preview_pad_top = 0
    preview_pad_right = 0
    preview_pad_bottom = 0
    _open_pad_left = 0
    _open_pad_top = 0
    _open_pad_right = 0
    _open_pad_bottom = 0
    max_extend = 8192

    def __init__(self, image, drawable):
        super().__init__(
            title="Puppet Warp (ARAP)",
            modal=False,
            destroy_with_parent=True,
        )

        self._dead = False
        self.image = image
        self.drawable = drawable

        cw = drawable.get_width()
        ch = drawable.get_height()

        ww, wh = _hug_default_size(cw, ch)
        self.set_default_size(ww, wh)

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

        rect = Gegl.Rectangle.new(0, 0, cw, ch)
        raw = bytes(
            drawable.get_buffer().get(
                rect,
                1.0,
                "cairo-ARGB32",
                Gegl.AbyssPolicy.NONE,
            )
        )

        self.src_surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, cw, ch)
        self.src_surface.get_data()[:] = raw
        self.src_surface.mark_dirty()

        self._bg_surface = None
        self._bg_off_x = 0
        self._bg_off_y = 0
        self._bg_alpha = 1.0
        self._bg_bounds = None
        self.bg_mode = "below"
        self._bg_keys = ("off", "below", "composite")
        self._mesh_keys = ("off", "cells", "triangles", "both")
        self._bg_layers = None
        self._bg_pos = None
        self._bg_ox = 0
        self._bg_oy = 0

        try:
            self._bg_layers = _as_list(self.image.get_layers())
            self._bg_pos = _layer_index(self._bg_layers, drawable)
            self._bg_ox, self._bg_oy = _unpack_offsets(drawable.get_offsets())
        except Exception as e:
            print(f"[puppet-warp] bg: layer lookup failed ({e})")

        self._alpha = None
        if HAS_NUMPY:
            self._alpha = (
                np.frombuffer(raw, dtype=np.uint8)
                .reshape(ch, cw, 4)[:, :, 3]
                .astype(np.float32)
                / 255.0
            )

        self._cw_list_cache = {}
        self._cw_payload_cache = {}

        self.pins = []
        self.active_pin_index = None
        self.hovered_pin_index = None

        self.is_dragging_pin = False
        self.is_rotating = False
        self.is_scaling = False
        self.is_moving_pivot = False
        self.rot_grab = 0.0
        self.scale_grab = 1.0
        self.scale_grab_dist = 1.0
        self.scale_pin0 = (0.0, 0.0)

        self.selected = set()
        self.multi_orig = None
        self.multi_anchor = None
        self.mesh_style = "cells"
        self.mesh_color = (0.2, 0.9, 1.0)

        self.drag_offset = (0.0, 0.0)
        self.puppet_radius = 400.0

        self.pan_x = 25.0
        self.pan_y = 25.0
        self.zoom = 1.0

        self.is_panning = False
        self.pan_start = (0.0, 0.0)

        self._update_timer = None
        self._rz_timer = None
        self._motion_timer = None
        self._motion_pending = False
        self._last_drag_world = None
        self._drag_min_step = 0.5

        self.deformed_verts = None
        self._last_good_verts = None

        self._tri_stat = {
            "draw": 0,
            "det": 0,
            "scale": 0,
            "mdet": 0,
            "cairo": 0,
        }

        self._checker = None
        self._checker_w = 0
        self._checker_h = 0

        self._dst_surface = None
        self._dst_scaled = None
        self._raster_key = None
        self._raster_zoom = None

        self._solve_seq = 0

        self._grid = None
        self._grid_sig = None
        self._grid_cw_sig = None

        self._last_solve_sig = None
        self._tick_ms = 40.0
        self._render_ms = 0.0
        self._idle_iters = 30

        self._verts_raw = None
        self._verts_raw_n = 0
        self._verts_raw_np = None
        self._verts_raw_seq = -1

        self._verts_out = None
        self._verts_out_n = 0
        self._verts_out_np = None

        self._undo_stack = []
        self._redo_stack = []
        self._drag_pushed = False

        self.pin_edge = False
        self.rotate_on = True
        self.taper = 0.0
        self.use_stiffness = True
        self._updating_panel = False
        self._edit_burst_pending = False

        self.restored_pins = 0

        prev = load_existing_params(drawable)
        if prev:
            try:
                self.cols = max(2, min(64, int(prev.get("cols", self.cols))))
                self.rows = max(2, min(64, int(prev.get("rows", self.rows))))
                self.pin_edge = bool(prev.get("pin_edge", False))
                self.rotate_on = bool(prev.get("rotate_on", True))
                self.use_stiffness = bool(prev.get("use_stiffness", True))

                bm = str(prev.get("bg_mode", "below")).strip().lower()
                if bm not in self._bg_keys:
                    bm = "below"
                self.bg_mode = bm

                ms = str(prev.get("mesh_style", self.mesh_style)).strip().lower()
                if ms in self._mesh_keys:
                    self.mesh_style = ms

                mc = prev.get("mesh_color")
                if isinstance(mc, (list, tuple)) and len(mc) >= 3:
                    try:
                        self.mesh_color = tuple(
                            max(0.0, min(1.0, float(x))) for x in mc[:3]
                        )
                    except Exception:
                        pass
            except Exception:
                pass

            pf = prev.get("pins_flat") or []
            truncated = False
            for i in range(0, len(pf) - 3, 4):
                if len(self.pins) >= MAX_PINS:
                    truncated = True
                    break
                p = Pin(pf[i], pf[i + 1])
                p.dx = pf[i] + pf[i + 2]
                p.dy = pf[i + 1] + pf[i + 3]
                p.qx = p.dx
                p.qy = p.dy
                self.pins.append(p)

            if truncated:
                print(f"[puppet-warp] loaded pins truncated to {MAX_PINS}")

            pr = prev.get("pin_rot") or []
            ps = prev.get("pin_scale") or []
            pt = prev.get("pin_taper") or []
            pd = prev.get("pin_depth") or []

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
                if i < len(pt):
                    try:
                        p.taper = max(0.0, min(1.0, float(pt[i])))
                    except Exception:
                        pass
                if i < len(pd):
                    try:
                        nd = int(pd[i])
                        p.depth = -999 if nd < -999 else (999 if nd > 999 else nd)
                    except Exception:
                        pass

            if pt:
                try:
                    self.taper = max(0.0, min(1.0, float(pt[0])))
                except Exception:
                    self.taper = 0.0

            self.restored_pins = len(self.pins)

        self._refresh_bg_preview()
        # Extend attrs safety, in case older __init__ survived.
        if not hasattr(self, "extend_on"):
            self.extend_on = False
        if not hasattr(self, "_extend_supported"):
            self._extend_supported = True

        for _attr, _val in (
            ("pad_left", 0),
            ("pad_top", 0),
            ("pad_right", 0),
            ("pad_bottom", 0),
            ("preview_pad_left", 0),
            ("preview_pad_top", 0),
            ("preview_pad_right", 0),
            ("preview_pad_bottom", 0),
            ("_open_pad_left", 0),
            ("_open_pad_top", 0),
            ("_open_pad_right", 0),
            ("_open_pad_bottom", 0),
            ("max_extend", 8192),
        ):
            if not hasattr(self, _attr):
                setattr(self, _attr, _val)

        self._heal_reopen_pad()
        self._build_ui()
        self._sync_pin_panel()

        self.connect("key-press-event", self._on_dialog_key)
        self.connect("destroy", self._on_destroy)

        if self.restored_pins:
            self.status_label.set_label(
                f"Restored {self.restored_pins} pins from the active Puppet Warp filter. "
                "Use Clear Pins to start a new session."
            )

        self.fit_to_view()
        self._solve_preview()

    def _on_dialog_key(self, w, e):
        kv = e.keyval

        if kv == Gdk.KEY_Escape:
            if e.state & (Gdk.ModifierType.MOD1_MASK
                          | Gdk.ModifierType.CONTROL_MASK
                          | Gdk.ModifierType.SHIFT_MASK):
                return False
            self.response(Gtk.ResponseType.CANCEL)
            return True

        if kv in (Gdk.KEY_Return, Gdk.KEY_KP_Enter):
            self.response(Gtk.ResponseType.OK)
            return True

        return False

    def _on_destroy(self, w):
        self._dead = True

        for attr in ("_update_timer", "_rz_timer", "_motion_timer"):
            tid = getattr(self, attr, None)
            if tid:
                try:
                    GLib.source_remove(tid)
                except Exception:
                    pass
                setattr(self, attr, None)

        g = getattr(self, "_grid", None)
        if g is not None:
            try:
                lib = _load_arap_lib()
                if lib is not None:
                    lib.arap_grid_free(g)
            except Exception:
                pass

            self._grid = None
            self._grid_sig = None
            self._grid_cw_sig = None

    def cell_weights(self, cols, rows):
        key = (cols, rows)
        hit = self._cw_list_cache.get(key)
        if hit is not None:
            return hit

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

        self._cw_list_cache[key] = w
        return w

    def _build_ui(self):
        ca = self.get_content_area()
        ca.set_spacing(2)
        ca.set_border_width(3)

        # Compact styling, scoped by a class so it does not touch all GIMP UI.
        try:
            self.get_style_context().add_class("pw-compact")

            provider = Gtk.CssProvider()
            provider.load_from_data(b"""
.pw-compact {
    padding: 0;
}

.pw-compact box {
    spacing: 3px;
}

.pw-compact label {
    padding: 0 1px;
}

.pw-compact button {
    min-height: 0;
    padding: 1px 5px;
}

.pw-compact spinbutton {
    min-height: 0;
    padding: 0;
}

.pw-compact spinbutton entry {
    min-height: 0;
    min-width: 0;
    padding: 0 2px;
}

.pw-compact spinbutton button {
    min-height: 0;
    min-width: 13px;
    padding: 0;
    margin: 0;
    border-width: 0;
}

.pw-compact combobox {
    min-height: 0;
    padding: 0;
}

.pw-compact checkbutton {
    padding: 0 2px;
}
""")

            screen = Gdk.Screen.get_default()
            if screen is not None:
                Gtk.StyleContext.add_provider_for_screen(
                    screen,
                    provider,
                    Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION,
                )

            self._css_provider = provider
        except Exception:
            pass

        def sep():
            s = Gtk.Separator(orientation=Gtk.Orientation.VERTICAL)
            s.set_margin_top(2)
            s.set_margin_bottom(2)
            return s

        def small_spin(lo, hi, step, digits, chars):
            sp = Gtk.SpinButton.new_with_range(lo, hi, step)
            sp.set_digits(digits)
            sp.set_width_chars(chars)
            try:
                sp.set_alignment(0.5)
            except Exception:
                pass
            try:
                sp.set_numeric(True)
            except Exception:
                pass
            try:
                sp.set_max_width_chars(chars)
            except Exception:
                pass

            return sp

        # Row 1: grid + quick actions + pin counter.
        row1 = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)

        row1.pack_start(Gtk.Label(label="Grid"), False, False, 0)

        self.cols_spin = small_spin(2, 64, 1, 0, 3)
        self.cols_spin.set_value(self.cols)
        self.cols_spin.set_tooltip_text("Mesh columns.")
        self._cols_id = self.cols_spin.connect("value-changed", self._on_grid_changed)
        row1.pack_start(self.cols_spin, False, False, 0)

        row1.pack_start(Gtk.Label(label="×"), False, False, 0)

        self.rows_spin = small_spin(2, 64, 1, 0, 3)
        self.rows_spin.set_value(self.rows)
        self.rows_spin.set_tooltip_text("Mesh rows.")
        self._rows_id = self.rows_spin.connect("value-changed", self._on_grid_changed)
        row1.pack_start(self.rows_spin, False, False, 0)

        row1.pack_start(sep(), False, False, 0)

        clear = Gtk.Button(label="Clear")
        clear.set_tooltip_text("Remove all pins.")
        clear.connect("clicked", lambda b: self._clear_pins())
        row1.pack_start(clear, False, False, 0)

        fit = Gtk.Button(label="Fit")
        fit.set_tooltip_text("Zoom view to fit the layer.")
        fit.connect("clicked", lambda b: self.fit_to_view())
        row1.pack_start(fit, False, False, 0)

        reset = Gtk.Button(label="Reset")
        reset.set_tooltip_text("Reset deformation but keep pins.")
        reset.connect("clicked", lambda b: self._reset_warp())
        row1.pack_start(reset, False, False, 0)

        row1.pack_start(sep(), False, False, 0)

        self.pin_count_label = Gtk.Label(label=f"0/{MAX_PINS}")
        self.pin_count_label.set_tooltip_text(f"Pin count / hard limit ({MAX_PINS}).")
        row1.pack_end(self.pin_count_label, False, False, 0)

        ca.pack_start(row1, False, False, 0)

        # Row 2: mesh display + options popover.
        row2 = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)

        row2.pack_start(Gtk.Label(label="Mesh"), False, False, 0)

        self.mesh_combo = Gtk.ComboBoxText()
        for _lab in ("Off", "Cells", "Triangles", "Both"):
            self.mesh_combo.append_text(_lab)
        self.mesh_combo.set_active(self._mesh_keys.index(self.mesh_style))
        self.mesh_combo.set_size_request(86, -1)
        self.mesh_combo.set_tooltip_text(
            "Mesh display only. Soft cells are red when Alpha stiffness is on."
        )
        self._mesh_id = self.mesh_combo.connect("changed", self._on_mesh_changed)
        row2.pack_start(self.mesh_combo, False, False, 0)

        self.mesh_color_btn = Gtk.ColorButton()
        _mc = Gdk.RGBA()
        _mc.red, _mc.green, _mc.blue, _mc.alpha = (
            self.mesh_color[0],
            self.mesh_color[1],
            self.mesh_color[2],
            1.0,
        )
        self.mesh_color_btn.set_rgba(_mc)
        self.mesh_color_btn.set_size_request(28, -1)
        self.mesh_color_btn.set_tooltip_text("Mesh line color.")
        self._mesh_color_id = self.mesh_color_btn.connect("color-set", self._on_mesh_color)
        row2.pack_start(self.mesh_color_btn, False, False, 0)

        row2.pack_start(sep(), False, False, 0)

        options_btn = Gtk.MenuButton()
        options_btn.set_label("Options")
        options_btn.set_tooltip_text("Solver, preview background, presets, extend.")

        options_pop = Gtk.Popover.new(options_btn)
        options_pop.set_position(Gtk.PositionType.BOTTOM)
        options_pop.set_modal(True)

        try:
            options_pop.get_style_context().add_class("pw-compact")
        except Exception:
            pass

        options_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        options_box.set_border_width(6)

        try:
            options_box.get_style_context().add_class("pw-compact")
        except Exception:
            pass

        self.stiff_check = Gtk.CheckButton(label="Alpha stiffness")
        self.stiff_check.set_active(self.use_stiffness)
        self.stiff_check.set_tooltip_text(
            "Use layer alpha as solver stiffness. Off makes the mesh uniform."
        )
        self._stiff_id = self.stiff_check.connect("toggled", self._on_stiffness_toggled)
        options_box.pack_start(self.stiff_check, False, False, 0)

        self.pin_edge_check = Gtk.CheckButton(label="Pin edge")
        self.pin_edge_check.set_active(self.pin_edge)
        self.pin_edge_check.set_tooltip_text("Keep the outer mesh border fixed.")
        self._pin_edge_id = self.pin_edge_check.connect("toggled", self._on_pin_edge_toggled)
        options_box.pack_start(self.pin_edge_check, False, False, 0)

        self.rotate_check = Gtk.CheckButton(label="Rotate/scale")
        self.rotate_check.set_active(self.rotate_on)
        self.rotate_check.set_tooltip_text(
            "Allow pins to rotate/scale. Values are kept when off."
        )
        self._rotate_id = self.rotate_check.connect("toggled", self._on_rotate_toggled)
        options_box.pack_start(self.rotate_check, False, False, 0)

        self.extend_check = Gtk.CheckButton(label="Extend image")
        self.extend_check.set_active(bool(getattr(self, "extend_on", False)))
        if getattr(self, "_extend_supported", True):
            self.extend_check.set_tooltip_text(
                "Grow the layer when pins pull pixels outside bounds."
            )
        else:
            self.extend_check.set_sensitive(False)
            self.extend_check.set_active(False)
            self.extend_check.set_tooltip_text(
                "Mask is padded along with the layer."
            )
        self._extend_id = self.extend_check.connect("toggled", self._on_extend_toggled)
        options_box.pack_start(self.extend_check, False, False, 0)

        options_box.pack_start(
            Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL),
            False,
            False,
            2,
        )

        bg_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)
        bg_row.pack_start(Gtk.Label(label="BG"), False, False, 0)

        self.bg_combo = Gtk.ComboBoxText()
        for _lab in ("Off", "Below", "Composite"):
            self.bg_combo.append_text(_lab)
        self.bg_combo.set_active(self._bg_index())
        self.bg_combo.set_size_request(92, -1)
        self.bg_combo.set_tooltip_text(
            "Preview background: Below = first visible layer below, "
            "Composite = stack visible layers below, Off = none."
        )
        self._bg_id = self.bg_combo.connect("changed", self._on_bg_mode_changed)
        bg_row.pack_start(self.bg_combo, False, False, 0)
        options_box.pack_start(bg_row, False, False, 0)

        preset_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)
        preset_row.pack_start(Gtk.Label(label="Presets"), False, False, 0)

        save_btn = Gtk.Button(label="Save")
        save_btn.set_tooltip_text("Save pins and settings as a reusable JSON preset.")
        save_btn.connect("clicked", self._on_preset_save)
        preset_row.pack_start(save_btn, False, False, 0)

        load_btn = Gtk.Button(label="Load")
        load_btn.set_tooltip_text("Load pins and settings from a JSON preset.")
        load_btn.connect("clicked", self._on_preset_load)
        preset_row.pack_start(load_btn, False, False, 0)

        options_box.pack_start(preset_row, False, False, 0)

        options_box.show_all()
        options_pop.add(options_box)
        options_btn.set_popover(options_pop)

        self.options_popover = options_pop

        row2.pack_start(options_btn, False, False, 0)

        ca.pack_start(row2, False, False, 0)

        # Row 3: selected pin inspector, hidden until a pin is active.
        row3 = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)
        self.pin_row = row3

        row3.pack_start(Gtk.Label(label="Pin"), False, False, 0)
        row3.pack_start(sep(), False, False, 0)

        row3.pack_start(Gtk.Label(label="R°"), False, False, 0)
        self.pin_rot_spin = small_spin(-180.0, 180.0, 1.0, 0, 4)
        self.pin_rot_spin.set_value(0.0)
        self.pin_rot_spin.set_tooltip_text("Selected pin rotation.")
        self.pin_rot_spin.connect("value-changed", self._on_pin_rot_changed)
        row3.pack_start(self.pin_rot_spin, False, False, 0)

        row3.pack_start(Gtk.Label(label="S%"), False, False, 0)
        self.pin_scale_spin = small_spin(25.0, 400.0, 1.0, 0, 4)
        self.pin_scale_spin.set_value(100.0)
        self.pin_scale_spin.set_tooltip_text("Selected pin scale.")
        self.pin_scale_spin.connect("value-changed", self._on_pin_scale_changed)
        row3.pack_start(self.pin_scale_spin, False, False, 0)

        row3.pack_start(Gtk.Label(label="T"), False, False, 0)
        self.pin_taper_spin = small_spin(0.0, 1.0, 0.05, 2, 4)
        self.pin_taper_spin.set_value(0.0)
        self.pin_taper_spin.set_tooltip_text("Selected pin taper.")
        self.pin_taper_spin.connect("value-changed", self._on_pin_taper_changed)
        row3.pack_start(self.pin_taper_spin, False, False, 0)

        row3.pack_start(sep(), False, False, 0)

        row3.pack_start(Gtk.Label(label="D"), False, False, 0)
        self.pin_depth_spin = small_spin(-999.0, 999.0, 1.0, 0, 4)
        self.pin_depth_spin.set_value(0.0)
        self.pin_depth_spin.set_tooltip_text(
            "Selected pin depth. Higher draws in front. Keys [ and ]"
        )
        self.pin_depth_spin.connect("value-changed", self._on_pin_depth_changed)
        row3.pack_start(self.pin_depth_spin, False, False, 0)

        row3.pack_start(sep(), False, False, 0)

        row3.pack_start(Gtk.Label(label="Pivot"), False, False, 0)

        row3.pack_start(Gtk.Label(label="X"), False, False, 0)
        self.pin_pivot_x_spin = small_spin(-1000000.0, 1000000.0, 1.0, 0, 6)
        self.pin_pivot_x_spin.set_value(0.0)
        self.pin_pivot_x_spin.set_tooltip_text("Pivot X of selected pin. Runtime only.")
        self.pin_pivot_x_spin.connect("value-changed", self._on_pin_pivot_x_changed)
        row3.pack_start(self.pin_pivot_x_spin, False, False, 0)

        row3.pack_start(Gtk.Label(label="Y"), False, False, 0)
        self.pin_pivot_y_spin = small_spin(-1000000.0, 1000000.0, 1.0, 0, 6)
        self.pin_pivot_y_spin.set_value(0.0)
        self.pin_pivot_y_spin.set_tooltip_text("Pivot Y of selected pin. Runtime only.")
        self.pin_pivot_y_spin.connect("value-changed", self._on_pin_pivot_y_changed)
        row3.pack_start(self.pin_pivot_y_spin, False, False, 0)

        self.pin_reset_pivot_btn = Gtk.Button(label="Reset")
        self.pin_reset_pivot_btn.set_tooltip_text(
            "Move the pivot back onto the selected pin."
        )
        self.pin_reset_pivot_btn.connect("clicked", self._on_reset_pivot_clicked)
        row3.pack_start(self.pin_reset_pivot_btn, False, False, 0)

        ca.pack_start(row3, False, False, 0)

        # Show children once, then keep the row itself hidden by default.
        row3.show_all()
        row3.set_no_show_all(True)
        row3.hide()

        self.drawing_area = Gtk.DrawingArea()
        self.drawing_area.set_can_focus(True)
        self.drawing_area.add_events(
            Gdk.EventMask.BUTTON_PRESS_MASK
            | Gdk.EventMask.BUTTON_RELEASE_MASK
            | Gdk.EventMask.POINTER_MOTION_MASK
            | Gdk.EventMask.SCROLL_MASK
            | Gdk.EventMask.KEY_PRESS_MASK
            | Gdk.EventMask.LEAVE_NOTIFY_MASK
        )

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

        self.status_label = Gtk.Label(label="""Click add · drag move · Shift axis · right/Alt delete · double-click reset.
Ring: dot rotate, body scale · Ctrl pivot · wheel: Alt rot, Ctrl scale, Shift taper.
Shift click multi · Ctrl+A all · Del remove · arrows nudge · Alt+Esc clear · Enter apply · Esc cancel.
Options > Extend image grows the layer (and its mask) when needed.
Pin depth: select pin, [ back / ] front (overlap order).""")

        self.status_label.set_line_wrap(True)
        self.status_label.set_max_width_chars(112)
        self.status_label.set_xalign(0.0)
        self.status_label.set_hexpand(False)

        ca.pack_start(self.status_label, False, False, 0)

        self.add_button("_Cancel", Gtk.ResponseType.CANCEL)
        self.add_button("_OK", Gtk.ResponseType.OK)

        self.show_all()

    def _cw_payload(self):
        sig = (self.cols, self.rows, bool(self.use_stiffness))

        hit = self._cw_payload_cache.get(sig)
        if hit is not None:
            return hit

        if self.use_stiffness:
            cw = [round(x, 4) for x in self.cell_weights(self.cols, self.rows)]
        else:
            cw = [1.0] * (self.cols * self.rows)

        arr = (ctypes.c_float * len(cw))(*cw)

        hit = (arr, len(cw), sig)
        self._cw_payload_cache[sig] = hit

        return hit

    def get_params_json(self):
        pins_flat = []

        for p in self.pins:
            sx = round(p.sx * 2.0) / 2.0
            sy = round(p.sy * 2.0) / 2.0
            ddx = round((p.dx - p.sx) * 2.0) / 2.0
            ddy = round((p.dy - p.sy) * 2.0) / 2.0
            pins_flat.extend([sx, sy, ddx, ddy])

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
            "use_stiffness": bool(self.use_stiffness),
            "extend": bool(getattr(self, "extend_on", False)),
            "pad_left": int(getattr(self, "pad_left", 0)),
            "pad_top": int(getattr(self, "pad_top", 0)),
            "pad_right": int(getattr(self, "pad_right", 0)),
            "pad_bottom": int(getattr(self, "pad_bottom", 0)),
            "bg_mode": str(self.bg_mode),
            "mesh_style": str(self.mesh_style),
            "mesh_color": [
                round(float(self.mesh_color[0]), 4),
                round(float(self.mesh_color[1]), 4),
                round(float(self.mesh_color[2]), 4),
            ],
            "pin_rot": [round(p.theta, 4) for p in self.pins],
            "pin_scale": [round(p.scale, 4) for p in self.pins],
            "pin_taper": [round(p.taper, 3) for p in self.pins],
            "pin_depth": [int(p.depth) for p in self.pins],
            "region_x": 0,
            "region_y": 0,
            "orig_width": int(self.canvas_w),
            "orig_height": int(self.canvas_h),
            "pins_flat": pins_flat,
            "mask_w": int(self.cols),
            "mask_h": int(self.rows),
            "mask_flat": _pw_cw_json(self._cw_payload()),
            "verts_flat": verts_flat,
        }

        return json.dumps(params, separators=(",", ":"), sort_keys=True)

    def _snap(self):
        return [(p.sx, p.sy, p.dx, p.dy, p.theta, p.scale, p.taper, p.depth) for p in self.pins]

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
            p.taper = t[6] if len(t) > 6 else 0.0
            p.depth = t[7] if len(t) > 7 else 0
            p.qx = p.dx
            p.qy = p.dy
            self.pins.append(p)

        self.active_pin_index = None
        self.hovered_pin_index = None
        self.is_dragging_pin = False
        self.is_rotating = False
        self.is_scaling = False
        self.is_moving_pivot = False
        self._drag_pushed = False
        self.selected = set()
        self.multi_orig = None
        self.multi_anchor = None

        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self._update_pin_count()
        self._sync_pin_panel()
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
        st = e.state
        ctrl = bool(st & Gdk.ModifierType.CONTROL_MASK)
        alt = bool(st & Gdk.ModifierType.MOD1_MASK)
        shift = bool(st & Gdk.ModifierType.SHIFT_MASK)
        kv = e.keyval

        if kv == Gdk.KEY_Escape:
            if alt and self.selected:
                self.selected = set()
                self._sync_pin_panel()
                self.drawing_area.queue_draw()
                return True
            return False

        if kv == Gdk.KEY_Delete:
            if not self.selected and self.active_pin_index is not None:
                if 0 <= self.active_pin_index < len(self.pins):
                    self.selected = {self.active_pin_index}

            if self.selected:
                self._delete_selected_pins()
                return True

            return False

        arrows = (
            Gdk.KEY_Left,
            Gdk.KEY_Right,
            Gdk.KEY_Up,
            Gdk.KEY_Down,
            Gdk.KEY_KP_Left,
            Gdk.KEY_KP_Right,
            Gdk.KEY_KP_Up,
            Gdk.KEY_KP_Down,
        )

        if kv in arrows:
            if not self.selected and self.active_pin_index is not None:
                if 0 <= self.active_pin_index < len(self.pins):
                    self.selected = {self.active_pin_index}

            if not self.selected:
                return False

            step = 10.0 if shift else 1.0
            dx = 0.0
            dy = 0.0

            if kv in (Gdk.KEY_Left, Gdk.KEY_KP_Left):
                dx = -step
            elif kv in (Gdk.KEY_Right, Gdk.KEY_KP_Right):
                dx = step
            elif kv in (Gdk.KEY_Up, Gdk.KEY_KP_Up):
                dy = -step
            elif kv in (Gdk.KEY_Down, Gdk.KEY_KP_Down):
                dy = step

            self._nudge_selected(dx, dy)
            return True

        if kv == Gdk.KEY_bracketright:
            self._change_depth_selected(1)
            return True

        if kv == Gdk.KEY_bracketleft:
            self._change_depth_selected(-1)
            return True

        if not ctrl:
            return False

        if kv in (Gdk.KEY_a, Gdk.KEY_A):
            self.selected = set(range(len(self.pins)))
            if self.pins and self.active_pin_index is None:
                self.active_pin_index = 0
            self._sync_pin_panel()
            self.drawing_area.queue_draw()
            return True

        if kv in (Gdk.KEY_z, Gdk.KEY_Z):
            if shift:
                return self._do_redo()
            return self._do_undo()

        if kv in (Gdk.KEY_y, Gdk.KEY_Y):
            return self._do_redo()

        return False

    def _rot_handle_world(self, p):
        r = 28.0 / self.zoom
        return (p.qx + r * math.cos(p.theta), p.qy + r * math.sin(p.theta))

    def _find_handle_at(self, sx, sy, rad=12.0):
        if not self.rotate_on:
            return None

        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return None

        hx, hy = self._rot_handle_world(self.pins[i])
        px, py = self._world_to_screen(hx, hy)

        return i if (sx - px) ** 2 + (sy - py) ** 2 <= rad * rad else None

    def _ring_hit(self, sx, sy):
        """Active pin index if cursor is on the ring body (not the dot/pin).
        Ring radius on screen is constant 28 px (world 28/zoom * zoom)."""
        i = self.active_pin_index
        if i is None or not (0 <= i < len(self.pins)):
            return None
        p = self.pins[i]
        qx, qy = self._world_to_screen(p.qx, p.qy)
        d = math.hypot(sx - qx, sy - qy)
        if abs(d - 28.0) <= 7.0 and d > 12.0:
            return i
        return None

    def _reset_warp(self):
        self._push_undo()

        for p in self.pins:
            p.dx = p.sx
            p.dy = p.sy
            p.theta = 0.0
            p.scale = 1.0
            p.qx = p.sx
            p.qy = p.sy

        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self._sync_pin_panel()
        self.drawing_area.queue_draw()

    def _update_pin_count(self):
        lab = getattr(self, "pin_count_label", None)
        if lab is None:
            return

        n = len(self.pins)

        if n >= MAX_PINS:
            lab.set_markup(f'<span foreground="#ff6666">{n}/{MAX_PINS}</span>')
        else:
            lab.set_text(f"{n}/{MAX_PINS}")

    def _sync_pin_panel(self):
        if getattr(self, "_updating_panel", False):
            return

        i = self.active_pin_index
        valid = i is not None and 0 <= i < len(self.pins)

        row = getattr(self, "pin_row", None)
        if row is not None:
            if valid:
                row.show()
            else:
                row.hide()

        rot = getattr(self, "pin_rot_spin", None)
        scale = getattr(self, "pin_scale_spin", None)
        taper = getattr(self, "pin_taper_spin", None)
        depth = getattr(self, "pin_depth_spin", None)
        px = getattr(self, "pin_pivot_x_spin", None)
        py = getattr(self, "pin_pivot_y_spin", None)
        rbtn = getattr(self, "pin_reset_pivot_btn", None)

        self._updating_panel = True
        try:
            for wdg in (rot, scale, taper, depth, px, py):
                if wdg is not None:
                    wdg.set_sensitive(valid)

            if rbtn is not None:
                rbtn.set_sensitive(valid)

            if valid:
                p = self.pins[i]

                if rot is not None:
                    rot.set_value(math.degrees(p.theta))
                if scale is not None:
                    scale.set_value(p.scale * 100.0)
                if taper is not None:
                    taper.set_value(p.taper)
                if depth is not None:
                    depth.set_value(float(p.depth))
                if px is not None:
                    px.set_value(p.qx)
                if py is not None:
                    py.set_value(p.qy)
            else:
                # Neutral values when no pin is selected.
                if rot is not None:
                    rot.set_value(0.0)
                if scale is not None:
                    scale.set_value(100.0)
                if taper is not None:
                    taper.set_value(0.0)
                if depth is not None:
                    depth.set_value(0.0)
                if px is not None:
                    px.set_value(0.0)
                if py is not None:
                    py.set_value(0.0)
        finally:
            self._updating_panel = False

        self._update_pin_count()

    def _begin_edit_burst(self):
        if getattr(self, "_dead", False):
            return

        if not getattr(self, "_edit_burst_pending", False):
            self._push_undo()
            self._edit_burst_pending = True
            GLib.timeout_add(350, self._end_edit_burst)

    def _end_edit_burst(self):
        self._edit_burst_pending = False
        return False

    def _on_pin_rot_changed(self, spin):
        if getattr(self, "_updating_panel", False):
            return

        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return

        self._begin_edit_burst()

        p = self.pins[i]
        rad = math.radians(spin.get_value())

        while rad > math.pi:
            rad -= 2 * math.pi
        while rad < -math.pi:
            rad += 2 * math.pi

        delta = rad - p.theta
        c, s = math.cos(delta), math.sin(delta)
        rx, ry = p.dx - p.qx, p.dy - p.qy

        p.dx = p.qx + c * rx - s * ry
        p.dy = p.qy + s * rx + c * ry
        p.theta = rad

        self._schedule_update()
        self.drawing_area.queue_draw()

    def _on_pin_scale_changed(self, spin):
        if getattr(self, "_updating_panel", False):
            return

        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return

        self._begin_edit_burst()

        p = self.pins[i]
        ns = max(0.25, min(4.0, spin.get_value() / 100.0))
        k = ns / p.scale if p.scale else 1.0

        p.dx = p.qx + (p.dx - p.qx) * k
        p.dy = p.qy + (p.dy - p.qy) * k
        p.scale = ns

        self._schedule_update()
        self.drawing_area.queue_draw()

    def _on_pin_taper_changed(self, spin):
        if getattr(self, "_updating_panel", False):
            return

        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return

        self._begin_edit_burst()

        p = self.pins[i]
        p.taper = max(0.0, min(1.0, spin.get_value()))

        self._schedule_update()
        self.drawing_area.queue_draw()

    def _on_pin_pivot_x_changed(self, spin):
        if getattr(self, "_updating_panel", False):
            return

        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return

        p = self.pins[i]
        p.qx = spin.get_value()
        self.drawing_area.queue_draw()

    def _on_pin_pivot_y_changed(self, spin):
        if getattr(self, "_updating_panel", False):
            return

        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return

        p = self.pins[i]
        p.qy = spin.get_value()
        self.drawing_area.queue_draw()

    def _on_reset_pivot_clicked(self, b):
        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return

        self._begin_edit_burst()

        p = self.pins[i]
        p.qx = p.dx
        p.qy = p.dy

        self._sync_pin_panel()
        self.drawing_area.queue_draw()

    def _delete_selected_pins(self):
        sel = {i for i in self.selected if 0 <= i < len(self.pins)}
        if not sel:
            return

        self._push_undo()

        old_active = self.active_pin_index
        new_pins = []

        for i, p in enumerate(self.pins):
            if i not in sel:
                new_pins.append(p)

        self.pins = new_pins

        if old_active is not None and old_active not in sel:
            self.active_pin_index = old_active - sum(1 for d in sel if d < old_active)
        else:
            self.active_pin_index = None

        self.selected = set()

        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self._update_pin_count()
        self._sync_pin_panel()
        self.drawing_area.queue_draw()

    def _nudge_selected(self, dx, dy):
        sel = {i for i in self.selected if 0 <= i < len(self.pins)}
        if not sel:
            return

        self._begin_edit_burst()

        for i in sel:
            p = self.pins[i]
            p.dx += dx
            p.dy += dy
            p.qx += dx
            p.qy += dy

        self._schedule_update()
        self.drawing_area.queue_draw()

    def _bg_index(self):
        keys = getattr(self, "_bg_keys", ("off", "below", "composite"))
        mode = getattr(self, "bg_mode", "below")
        try:
            return keys.index(mode)
        except Exception:
            return 1

    def _refresh_bg_preview(self):
        self._bg_surface = None
        self._bg_off_x = 0
        self._bg_off_y = 0
        self._bg_alpha = 1.0
        self._bg_bounds = None

        keys = getattr(self, "_bg_keys", ("off", "below", "composite"))
        mode = getattr(self, "bg_mode", "below")
        if mode not in keys:
            mode = "below"
            self.bg_mode = mode

        if os.environ.get("PW_NO_BG") == "1" or mode == "off":
            return

        layers = getattr(self, "_bg_layers", None)
        pos = getattr(self, "_bg_pos", None)
        if layers is None or pos is None:
            return

        ox = getattr(self, "_bg_ox", 0)
        oy = getattr(self, "_bg_oy", 0)

        if mode == "composite":
            self._build_bg_composite(layers, pos, ox, oy)
        else:
            self._build_bg_below(layers, pos, ox, oy)

        da = getattr(self, "drawing_area", None)
        if da is not None:
            da.queue_draw()

    def _build_bg_below(self, layers, pos, ox, oy):
        below_idx = None

        for i in range(pos + 1, len(layers)):
            try:
                if layers[i].get_visible():
                    below_idx = i
                    break
            except Exception:
                continue

        if below_idx is None:
            return

        try:
            bl = layers[below_idx]
            lx, ly = _unpack_offsets(bl.get_offsets())
            lw, lh = bl.get_width(), bl.get_height()
            op = max(0.0, min(1.0, bl.get_opacity() / 100.0))

            if lw <= 0 or lh <= 0 or op <= 0.0:
                return

            braw = bytes(
                bl.get_buffer().get(
                    Gegl.Rectangle.new(0, 0, lw, lh),
                    1.0,
                    "cairo-ARGB32",
                    Gegl.AbyssPolicy.NONE,
                )
            )

            bs = cairo.ImageSurface(cairo.FORMAT_ARGB32, lw, lh)
            bs.get_data()[:] = braw
            bs.mark_dirty()

            self._bg_surface = bs
            self._bg_off_x = lx - ox
            self._bg_off_y = ly - oy
            self._bg_alpha = op
            self._bg_bounds = (
                float(self._bg_off_x),
                float(self._bg_off_y),
                float(lw),
                float(lh),
            )

        except Exception as e:
            print(f"[puppet-warp] bg below unavailable ({e})")
            self._bg_surface = None

    def _build_bg_composite(self, layers, pos, ox, oy):
        items = []
        minx = 1e18
        miny = 1e18
        maxx = -1e18
        maxy = -1e18

        for i in range(pos + 1, len(layers)):
            try:
                l = layers[i]
                if not l.get_visible():
                    continue

                lx, ly = _unpack_offsets(l.get_offsets())
                lw, lh = l.get_width(), l.get_height()
                op = max(0.0, min(1.0, l.get_opacity() / 100.0))
            except Exception:
                continue

            if lw <= 0 or lh <= 0 or op <= 0.0:
                continue

            x = float(lx - ox)
            y = float(ly - oy)

            items.append((l, x, y, lw, lh, op))

            minx = min(minx, x)
            miny = min(miny, y)
            maxx = max(maxx, x + float(lw))
            maxy = max(maxy, y + float(lh))

        if not items:
            return

        w = int(math.ceil(maxx - minx))
        h = int(math.ceil(maxy - miny))

        if w <= 0 or h <= 0:
            return

        MAX_BG_DIM = 8192
        MAX_BG_PIXELS = 64_000_000

        if w > MAX_BG_DIM or h > MAX_BG_DIM or w * h > MAX_BG_PIXELS:
            self._build_bg_below(layers, pos, ox, oy)
            return

        try:
            surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
            ctx = cairo.Context(surf)

            # layers list is topmost-first, so draw from bottom to top.
            for l, x, y, lw, lh, op in reversed(items):
                try:
                    raw = bytes(
                        l.get_buffer().get(
                            Gegl.Rectangle.new(0, 0, lw, lh),
                            1.0,
                            "cairo-ARGB32",
                            Gegl.AbyssPolicy.NONE,
                        )
                    )

                    ls = cairo.ImageSurface(cairo.FORMAT_ARGB32, lw, lh)
                    ls.get_data()[:] = raw
                    ls.mark_dirty()

                    ctx.save()
                    ctx.translate(x - minx, y - miny)
                    ctx.set_source_surface(ls, 0, 0)
                    ctx.paint_with_alpha(op)
                    ctx.restore()

                except Exception:
                    continue

            surf.flush()

            self._bg_surface = surf
            self._bg_off_x = minx
            self._bg_off_y = miny
            self._bg_alpha = 1.0
            self._bg_bounds = (float(minx), float(miny), float(w), float(h))

        except Exception as e:
            print(f"[puppet-warp] bg composite unavailable ({e})")
            self._bg_surface = None

    def _on_bg_mode_changed(self, combo):
        keys = getattr(self, "_bg_keys", ("off", "below", "composite"))
        idx = combo.get_active()

        if 0 <= idx < len(keys):
            self.bg_mode = keys[idx]

        self._refresh_bg_preview()

    def _set_blocked(self, widget, handler_id, setter, value):
        if widget is None:
            return

        if handler_id:
            try:
                widget.handler_block(handler_id)
            except Exception:
                pass

        try:
            setter(value)
        finally:
            if handler_id:
                try:
                    widget.handler_unblock(handler_id)
                except Exception:
                    pass

    def _preset_dir(self):
        base = os.path.join(
            GLib.get_user_config_dir(),
            "GIMP",
            "3.2",
            "puppet-warp",
            "presets",
        )

        try:
            os.makedirs(base, exist_ok=True)
            return base
        except Exception:
            fallback = os.path.join(PLUGIN_DIR, "presets")
            os.makedirs(fallback, exist_ok=True)
            return fallback

    def _preset_json(self):
        pins_flat = []

        for p in self.pins:
            sx = round(p.sx * 2.0) / 2.0
            sy = round(p.sy * 2.0) / 2.0
            ddx = round((p.dx - p.sx) * 2.0) / 2.0
            ddy = round((p.dy - p.sy) * 2.0) / 2.0
            pins_flat.extend([sx, sy, ddx, ddy])

        return json.dumps(
            {
                "preset": "puppet-warp",
                "version": 1,
                "cols": int(self.cols),
                "rows": int(self.rows),
                "pin_edge": bool(self.pin_edge),
                "rotate_on": bool(self.rotate_on),
                "extend": bool(self.extend_on),
                "use_stiffness": bool(self.use_stiffness),
                "bg_mode": str(self.bg_mode),
                "mesh_style": str(self.mesh_style),
                "mesh_color": [
                    round(float(self.mesh_color[0]), 4),
                    round(float(self.mesh_color[1]), 4),
                    round(float(self.mesh_color[2]), 4),
                ],
                "orig_width": int(self.canvas_w),
                "orig_height": int(self.canvas_h),
                "pins_flat": pins_flat,
                "pin_rot": [round(p.theta, 4) for p in self.pins],
                "pin_scale": [round(p.scale, 4) for p in self.pins],
                "pin_taper": [round(p.taper, 3) for p in self.pins],
                "pin_depth": [int(p.depth) for p in self.pins],
            },
            indent=2,
            sort_keys=True,
        )

    def _on_preset_save(self, b):
        try:
            d = self._preset_dir()
        except Exception as e:
            self.status_label.set_label(f"Preset save failed: {e}")
            return

        chooser = Gtk.FileChooserDialog(
            title="Save Puppet Warp Preset",
            parent=self,
            action=Gtk.FileChooserAction.SAVE,
        )
        chooser.add_buttons(
            "Cancel",
            Gtk.ResponseType.CANCEL,
            "Save",
            Gtk.ResponseType.ACCEPT,
        )
        chooser.set_current_name(time.strftime("puppet-warp-%Y%m%d-%H%M%S.json"))
        chooser.set_do_overwrite_confirmation(True)

        try:
            chooser.set_current_folder(d)
        except Exception:
            pass

        flt = Gtk.FileFilter()
        flt.set_name("JSON presets")
        flt.add_pattern("*.json")
        chooser.add_filter(flt)

        resp = chooser.run()
        fname = chooser.get_filename() if resp == Gtk.ResponseType.ACCEPT else None
        chooser.destroy()

        if not fname:
            return

        if not fname.lower().endswith(".json"):
            fname += ".json"

        try:
            with open(fname, "w", encoding="utf-8") as fh:
                fh.write(self._preset_json())

            self.status_label.set_label(f"Saved preset: {os.path.basename(fname)}")

        except Exception as e:
            self.status_label.set_label(f"Preset save failed: {e}")

    def _on_preset_load(self, b):
        try:
            d = self._preset_dir()
        except Exception:
            d = None

        chooser = Gtk.FileChooserDialog(
            title="Load Puppet Warp Preset",
            parent=self,
            action=Gtk.FileChooserAction.OPEN,
        )
        chooser.add_buttons(
            "Cancel",
            Gtk.ResponseType.CANCEL,
            "Load",
            Gtk.ResponseType.ACCEPT,
        )

        if d:
            try:
                chooser.set_current_folder(d)
            except Exception:
                pass

        flt = Gtk.FileFilter()
        flt.set_name("JSON presets")
        flt.add_pattern("*.json")
        chooser.add_filter(flt)

        resp = chooser.run()
        fname = chooser.get_filename() if resp == Gtk.ResponseType.ACCEPT else None
        chooser.destroy()

        if not fname:
            return

        try:
            with open(fname, "r", encoding="utf-8") as fh:
                pkg = json.load(fh)

            if self._apply_preset_dict(pkg):
                self.status_label.set_label(f"Loaded preset: {os.path.basename(fname)}")
            else:
                self.status_label.set_label("Preset file is not valid.")

        except Exception as e:
            self.status_label.set_label(f"Preset load failed: {e}")

    def _apply_preset_dict(self, pkg):
        if not isinstance(pkg, dict):
            return False

        self._push_undo()

        keys = getattr(self, "_bg_keys", ("off", "below", "composite"))
        mesh_keys = getattr(self, "_mesh_keys", ("off", "cells", "triangles", "both"))

        try:
            nc = max(2, min(64, int(pkg.get("cols", self.cols))))
        except Exception:
            nc = self.cols

        try:
            nr = max(2, min(64, int(pkg.get("rows", self.rows))))
        except Exception:
            nr = self.rows

        self.cols = nc
        self.rows = nr

        self._set_blocked(
            getattr(self, "cols_spin", None),
            getattr(self, "_cols_id", None),
            lambda v: self.cols_spin.set_value(v),
            float(nc),
        )
        self._set_blocked(
            getattr(self, "rows_spin", None),
            getattr(self, "_rows_id", None),
            lambda v: self.rows_spin.set_value(v),
            float(nr),
        )

        self.use_stiffness = bool(pkg.get("use_stiffness", self.use_stiffness))
        self._set_blocked(
            getattr(self, "stiff_check", None),
            getattr(self, "_stiff_id", None),
            lambda v: self.stiff_check.set_active(v),
            self.use_stiffness,
        )

        self.pin_edge = bool(pkg.get("pin_edge", self.pin_edge))
        self._set_blocked(
            getattr(self, "pin_edge_check", None),
            getattr(self, "_pin_edge_id", None),
            lambda v: self.pin_edge_check.set_active(v),
            self.pin_edge,
        )

        self.rotate_on = bool(pkg.get("rotate_on", self.rotate_on))
        self._set_blocked(
            getattr(self, "rotate_check", None),
            getattr(self, "_rotate_id", None),
            lambda v: self.rotate_check.set_active(v),
            self.rotate_on,
        )

        ex = bool(pkg.get("extend", self.extend_on))
        if not getattr(self, "_extend_supported", True):
            ex = False
        self.extend_on = ex
        self._set_blocked(
            getattr(self, "extend_check", None),
            getattr(self, "_extend_id", None),
            lambda v: self.extend_check.set_active(v),
            self.extend_on,
        )

        bm = str(pkg.get("bg_mode", self.bg_mode)).strip().lower()
        if bm not in keys:
            bm = "below"
        self.bg_mode = bm
        self._set_blocked(
            getattr(self, "bg_combo", None),
            getattr(self, "_bg_id", None),
            lambda v: self.bg_combo.set_active(v),
            keys.index(bm),
        )

        ms = str(pkg.get("mesh_style", self.mesh_style)).strip().lower()
        if ms in mesh_keys:
            self.mesh_style = ms
            self._set_blocked(
                getattr(self, "mesh_combo", None),
                getattr(self, "_mesh_id", None),
                lambda v: self.mesh_combo.set_active(v),
                mesh_keys.index(ms),
            )

        mc = pkg.get("mesh_color")
        if isinstance(mc, (list, tuple)) and len(mc) >= 3:
            try:
                r, g, b = (
                    max(0.0, min(1.0, float(mc[0]))),
                    max(0.0, min(1.0, float(mc[1]))),
                    max(0.0, min(1.0, float(mc[2]))),
                )
                self.mesh_color = (r, g, b)

                rgba = Gdk.RGBA()
                rgba.red = r
                rgba.green = g
                rgba.blue = b
                rgba.alpha = 1.0

                self._set_blocked(
                    getattr(self, "mesh_color_btn", None),
                    getattr(self, "_mesh_color_id", None),
                    lambda v: self.mesh_color_btn.set_rgba(v),
                    rgba,
                )
            except Exception:
                pass

        pf = pkg.get("pins_flat") or []

        try:
            ow = float(pkg.get("orig_width", self.canvas_w) or self.canvas_w)
        except Exception:
            ow = float(self.canvas_w)

        try:
            oh = float(pkg.get("orig_height", self.canvas_h) or self.canvas_h)
        except Exception:
            oh = float(self.canvas_h)

        sx_scale = float(self.canvas_w) / ow if ow > 0.0 else 1.0
        sy_scale = float(self.canvas_h) / oh if oh > 0.0 else 1.0

        new_pins = []

        for i in range(0, len(pf) - 3, 4):
            if len(new_pins) >= MAX_PINS:
                break

            try:
                sx0 = float(pf[i])
                sy0 = float(pf[i + 1])
                ddx = float(pf[i + 2])
                ddy = float(pf[i + 3])
            except Exception:
                continue

            p = Pin(sx0 * sx_scale, sy0 * sy_scale)
            p.dx = p.sx + ddx * sx_scale
            p.dy = p.sy + ddy * sy_scale
            p.qx = p.dx
            p.qy = p.dy
            new_pins.append(p)

        pr = pkg.get("pin_rot") or []
        ps = pkg.get("pin_scale") or []
        pt = pkg.get("pin_taper") or []
        pd = pkg.get("pin_depth") or []

        for i, p in enumerate(new_pins):
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

            if i < len(pt):
                try:
                    p.taper = max(0.0, min(1.0, float(pt[i])))
                except Exception:
                    pass
            if i < len(pd):
                try:
                    nd = int(pd[i])
                    p.depth = -999 if nd < -999 else (999 if nd > 999 else nd)
                except Exception:
                    pass

        self.pins = new_pins
        self.active_pin_index = None
        self.hovered_pin_index = None
        self.selected = set()
        self.multi_orig = None
        self.multi_anchor = None

        self.is_dragging_pin = False
        self.is_rotating = False
        self.is_scaling = False
        self.is_moving_pivot = False
        self._drag_pushed = False

        self._refresh_bg_preview()
        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self._sync_pin_panel()

        da = getattr(self, "drawing_area", None)
        if da is not None:
            da.queue_draw()

        return True

    def _on_extend_toggled(self, b):
        self.extend_on = b.get_active()
        self._update_preview_pad()
        da = getattr(self, "drawing_area", None)
        if da is not None:
            da.queue_draw()

    def _depth_on(self):
        return any(p.depth for p in self.pins)

    def _change_depth_selected(self, delta):
        sel = {i for i in self.selected if 0 <= i < len(self.pins)}
        if not sel and self.active_pin_index is not None:
            if 0 <= self.active_pin_index < len(self.pins):
                sel = {self.active_pin_index}
        if not sel:
            return
        self._begin_edit_burst()
        for i in sel:
            p = self.pins[i]
            nd = p.depth + delta
            if nd > 999:
                nd = 999
            elif nd < -999:
                nd = -999
            p.depth = nd
        # depth is render-layer z-order, not a solver input: no re-solve.
        self.drawing_area.queue_draw()
        self._sync_pin_panel()

    def _compute_vertex_depths(self):
        cols, rows = self.cols, self.rows
        nverts = (cols + 1) * (rows + 1)
        sig = (
            cols, rows,
            tuple((round(p.sx, 1), round(p.sy, 1), p.depth) for p in self.pins),
        )
        cache = getattr(self, "_vdepth_cache", None)
        if cache is not None and cache[0] == sig:
            return cache[1]

        vd = [0] * nverts
        pins = self.pins
        if pins:
            pw = self.puppet_w / max(1, cols)
            ph = self.puppet_h / max(1, rows)
            px0 = self.puppet_x
            py0 = self.puppet_y
            for r in range(rows + 1):
                vy = py0 + r * ph
                base = r * (cols + 1)
                for c in range(cols + 1):
                    vx = px0 + c * pw
                    best_d = None
                    best_dep = 0
                    for i, p in enumerate(pins):
                        ddx = vx - p.sx
                        ddy = vy - p.sy
                        d2 = ddx * ddx + ddy * ddy
                        if best_d is None or d2 < best_d:
                            best_d = d2
                            best_dep = p.depth
                    vd[base + c] = best_dep
        self._vdepth_cache = (sig, vd)
        return vd

    def _on_pin_depth_changed(self, spin):
        if getattr(self, "_updating_panel", False):
            return
        i = self.active_pin_index
        if i is None or i < 0 or i >= len(self.pins):
            return
        self._begin_edit_burst()
        try:
            nd = int(spin.get_value())
        except Exception:
            nd = 0
        if nd > 999:
            nd = 999
        elif nd < -999:
            nd = -999
        self.pins[i].depth = nd
        self.drawing_area.queue_draw()

    def _pad_from_verts(self, verts):
        if not getattr(self, "extend_on", False) or not verts:
            return (0, 0, 0, 0)

        minx = miny = float("inf")
        maxx = maxy = float("-inf")

        for vx, vy in verts:
            try:
                fx = float(vx)
                fy = float(vy)
            except Exception:
                continue

            if not math.isfinite(fx) or not math.isfinite(fy):
                continue

            if fx < minx:
                minx = fx
            if fy < miny:
                miny = fy
            if fx > maxx:
                maxx = fx
            if fy > maxy:
                maxy = fy

        if minx == float("inf"):
            return (0, 0, 0, 0)

        margin = 1.0
        cap = max(0, int(getattr(self, "max_extend", 8192)))

        def side(v):
            if v <= 0.0:
                return 0
            n = int(math.ceil(v + margin))
            if n < 0:
                n = 0
            if n > cap:
                n = cap
            return n

        pl = side(-minx)
        pt = side(-miny)
        pr = side(maxx - float(self.canvas_w))
        pb = side(maxy - float(self.canvas_h))

        return (pl, pt, pr, pb)

    def _update_preview_pad(self):
        if not getattr(self, "_extend_supported", True):
            self.preview_pad_left = getattr(self, "_open_pad_left", 0)
            self.preview_pad_top = getattr(self, "_open_pad_top", 0)
            self.preview_pad_right = getattr(self, "_open_pad_right", 0)
            self.preview_pad_bottom = getattr(self, "_open_pad_bottom", 0)
            return

        pl, pt, pr, pb = self._pad_from_verts(getattr(self, "deformed_verts", None))
        self.preview_pad_left = pl
        self.preview_pad_top = pt
        self.preview_pad_right = pr
        self.preview_pad_bottom = pb

    def finalize_extend_pad(self):
        # Mask-aware now: pad is computed even on masked layers.
        if not getattr(self, "_extend_supported", True):
            self.pad_left = getattr(self, "_open_pad_left", 0)
            self.pad_top = getattr(self, "_open_pad_top", 0)
            self.pad_right = getattr(self, "_open_pad_right", 0)
            self.pad_bottom = getattr(self, "_open_pad_bottom", 0)
            self._update_preview_pad()
            return

        pl, pt, pr, pb = self._pad_from_verts(getattr(self, "deformed_verts", None))
        self.pad_left = pl
        self.pad_top = pt
        self.pad_right = pr
        self.pad_bottom = pb
        self._update_preview_pad()

    def get_resize_plan(self):
        old = (
            getattr(self, "_open_pad_left", 0),
            getattr(self, "_open_pad_top", 0),
            getattr(self, "_open_pad_right", 0),
            getattr(self, "_open_pad_bottom", 0),
        )
        new = (
            int(self.pad_left),
            int(self.pad_top),
            int(self.pad_right),
            int(self.pad_bottom),
        )

        if old == new:
            return {"needs_resize": False}

        try:
            ox, oy = _unpack_offsets(self.drawable.get_offsets())
        except Exception:
            ox = oy = 0

        try:
            cur_w = self.drawable.get_width()
            cur_h = self.drawable.get_height()
        except Exception:
            cur_w = self.canvas_w + old[0] + old[2]
            cur_h = self.canvas_h + old[1] + old[3]

        # Original content sits at old pad inside the current layer.
        content_x = ox + old[0]
        content_y = oy + old[1]

        nw = self.canvas_w + new[0] + new[2]
        nh = self.canvas_h + new[1] + new[3]
        nox = content_x - new[0]
        noy = content_y - new[1]

        # Resize anchor = where the OLD layer's (0,0) lands in the NEW layer.
        # Old (0,0) is old_pad px left of the content; content must land at new_pad.
        ax = new[0] - old[0]
        ay = new[1] - old[1]

        needs = (nw != cur_w or nh != cur_h or nox != ox or noy != oy)

        return {
            "needs_resize": bool(needs),
            "new_w": int(nw),
            "new_h": int(nh),
            "new_off_x": int(nox),
            "new_off_y": int(noy),
            "anchor_x": int(ax),
            "anchor_y": int(ay),
        }

    def _heal_reopen_pad(self):
        # Independent re-detect, run after __init__ may have mis-read pad.
        # Override everything coordinate-related so a wrong __init__ cannot
        # leak a padded buffer into the mesh as if it were the base layer.
        draw_w = self.drawable.get_width()
        draw_h = self.drawable.get_height()
        try:
            dox, doy = _unpack_offsets(self.drawable.get_offsets())
        except Exception:
            dox = doy = 0

        prev = load_existing_params(self.drawable)
        dbg = bool(os.environ.get("PW_DEBUG"))

        if prev is None:
            if dbg:
                print("[pw-reopen] prev=None: cannot read filter params "
                      "(layer heals as unpadded)")
            return

        try:
            ow = int(prev.get("orig_width", 0) or 0)
            oh = int(prev.get("orig_height", 0) or 0)
            pl = max(0, int(prev.get("pad_left", 0) or 0))
            pt = max(0, int(prev.get("pad_top", 0) or 0))
            pr = max(0, int(prev.get("pad_right", 0) or 0))
            pb = max(0, int(prev.get("pad_bottom", 0) or 0))
            ext_flag = bool(prev.get("extend", False))
        except Exception:
            if dbg:
                print("[pw-reopen] parse pad fail")
            return

        if ow <= 0 or oh <= 0:
            if dbg:
                print("[pw-reopen] orig missing in params (ow=%d oh=%d)" % (ow, oh))
            return

        # Flexible: only require the crop rect to fit the buffer.
        if not (ow <= draw_w and oh <= draw_h and pl + ow <= draw_w and pt + oh <= draw_h):
            if dbg:
                print("[pw-reopen] WARN crop not fit: draw=%dx%d ow=%d oh=%d pad=(%d,%d,%d,%d)"
                      % (draw_w, draw_h, ow, oh, pl, pt, pr, pb))
            return

        pr = min(pr, max(0, draw_w - pl - ow))
        pb = min(pb, max(0, draw_h - pt - oh))

        # Already correct? nothing to heal.
        if (self.canvas_w == ow and self.canvas_h == oh and self.pad_left == pl
                and self.pad_top == pt and self.pad_right == pr and self.pad_bottom == pb):
            if dbg:
                print("[pw-reopen] ok (no heal needed) canvas=%dx%d pad=(%d,%d,%d,%d) extend=%d"
                      % (ow, oh, pl, pt, pr, pb, int(self.extend_on)))
            return

        # Override geometry.
        self.canvas_w = ow
        self.canvas_h = oh
        self.pad_left = pl
        self.pad_top = pt
        self.pad_right = pr
        self.pad_bottom = pb
        self.puppet_x = 0.0
        self.puppet_y = 0.0
        self.puppet_w = float(ow)
        self.puppet_h = float(oh)
        self.extend_on = bool(ext_flag or pl or pt or pr or pb)

        # Re-read only the original pixels out of the padded buffer.
        rect = Gegl.Rectangle.new(pl, pt, ow, oh)
        raw = bytes(
            self.drawable.get_buffer().get(
                rect, 1.0, "cairo-ARGB32", Gegl.AbyssPolicy.NONE,
            )
        )
        self.src_surface = cairo.ImageSurface(cairo.FORMAT_ARGB32, ow, oh)
        self.src_surface.get_data()[:] = raw
        self.src_surface.mark_dirty()

        self._alpha = None
        if HAS_NUMPY:
            try:
                self._alpha = (
                    np.frombuffer(raw, dtype=np.uint8)
                    .reshape(oh, ow, 4)[:, :, 3]
                    .astype(np.float32) / 255.0
                )
            except Exception:
                self._alpha = None

        # alpha/size changed -> stiffness + raster caches are stale.
        self._cw_list_cache = {}
        self._cw_payload_cache = {}
        self._raster_key = None

        # Background must align to the original content, not the padded corner.
        self._bg_ox = dox + pl
        self._bg_oy = doy + pt
        self._open_pad_left = pl
        self._open_pad_top = pt
        self._open_pad_right = pr
        self._open_pad_bottom = pb
        self.preview_pad_left = pl
        self.preview_pad_top = pt
        self.preview_pad_right = pr
        self.preview_pad_bottom = pb

        self._refresh_bg_preview()

        if dbg:
            print("[pw-reopen] HEALED draw=%dx%d -> canvas=%dx%d pad=(%d,%d,%d,%d) extend=%d"
                  % (draw_w, draw_h, ow, oh, pl, pt, pr, pb, int(self.extend_on)))

    def _raster_zoom_now(self):
        rz = getattr(self, "_raster_zoom", None)
        return self.zoom if rz is None else rz

    def _schedule_raster_zoom(self):
        if getattr(self, "_dead", False):
            return

        tid = getattr(self, "_rz_timer", None)
        if tid:
            try:
                GLib.source_remove(tid)
            except Exception:
                pass

        self._rz_timer = GLib.timeout_add(150, self._raster_zoom_settle)

    def _raster_zoom_settle(self):
        self._rz_timer = None

        if getattr(self, "_dead", False):
            return False

        try:
            if abs(self._raster_zoom_now() - self.zoom) > 1e-4:
                self._raster_zoom = self.zoom
                da = getattr(self, "drawing_area", None)
                if da is not None:
                    da.queue_draw()
        except Exception:
            pass

        return False

    def _preview_scale(self):
        try:
            env_s = float(os.environ.get("PW_PREVIEW_SCALE", ""))
        except Exception:
            env_s = 0.0

        if env_s > 0:
            return min(1.0, env_s)

        return min(1.0, max(0.25, self._raster_zoom_now()))

    def _view_rect(self):
        a = self.drawing_area.get_allocation()
        dw = a.width if a.width > 50 else 800
        dh = a.height if a.height > 50 else 600

        z = self.zoom if self.zoom > 1e-3 else 1.0
        m = 24.0

        vx0 = -self.pan_x / z - m
        vy0 = -self.pan_y / z - m
        vx1 = vx0 + dw / z + 2.0 * m
        vy1 = vy0 + dh / z + 2.0 * m

        # extend: allow the view to cover warped pixels outside bounds
        if bool(getattr(self, "extend_on", False)):
            verts = getattr(self, "deformed_verts", None)
            if verts:
                minx = miny = float("inf")
                maxx = maxy = float("-inf")
                for vx, vy in verts:
                    try:
                        fx = float(vx); fy = float(vy)
                    except Exception:
                        continue
                    if not math.isfinite(fx) or not math.isfinite(fy):
                        continue
                    if fx < minx: minx = fx
                    if fy < miny: miny = fy
                    if fx > maxx: maxx = fx
                    if fy > maxy: maxy = fy
                if minx != float("inf"):
                    pad = 2.0
                    vx0 = min(vx0, minx - pad)
                    vy0 = min(vy0, miny - pad)
                    vx1 = max(vx1, maxx + pad)
                    vy1 = max(vy1, maxy + pad)
        else:
            vx0 = max(0.0, min(vx0, float(self.canvas_w - 1)))
            vy0 = max(0.0, min(vy0, float(self.canvas_h - 1)))
            vx1 = max(vx0 + 1.0, min(vx1, float(self.canvas_w)))
            vy1 = max(vy0 + 1.0, min(vy1, float(self.canvas_h)))

        s = max(0.25, min(1.0, z))

        return (vx0, vy0, vx1, vy1, s)

    def _taper_base_radius(self):
        cw = self.puppet_w / max(1, self.cols)
        ch = self.puppet_h / max(1, self.rows)
        r = 3.0 * math.sqrt(cw * ch)
        if r < 24.0:
            r = 24.0
        return r

    def _on_mesh_changed(self, combo):
        idx = combo.get_active()
        if 0 <= idx < len(self._mesh_keys):
            self.mesh_style = self._mesh_keys[idx]
        self.drawing_area.queue_draw()

    def _on_mesh_color(self, btn):
        c = btn.get_rgba()
        self.mesh_color = (c.red, c.green, c.blue)
        self.drawing_area.queue_draw()

    def _on_stiffness_toggled(self, b):
        self.use_stiffness = b.get_active()
        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self.drawing_area.queue_draw()

    def _on_pin_edge_toggled(self, b):
        self.pin_edge = b.get_active()
        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self.drawing_area.queue_draw()

    def _on_rotate_toggled(self, b):
        self.rotate_on = b.get_active()
        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self.drawing_area.queue_draw()

    def _solve_preview(self, iters=None):
        lib = _load_arap_lib()
        if lib is None:
            self.deformed_verts = None
            return

        if iters is None:
            iters = self._idle_iters

        can_reuse = hasattr(lib, "arap_grid_reset_pins")
        grid = None

        try:
            cw_arr, ncw, cw_sig = self._cw_payload()
            grid_sig = (self.cols, self.rows)

            if can_reuse:
                grid = self._grid

                if grid is not None and self._grid_sig != grid_sig:
                    try:
                        lib.arap_grid_free(grid)
                    except Exception:
                        pass

                    grid = None
                    self._grid = None
                    self._grid_sig = None
                    self._grid_cw_sig = None

                if grid is None:
                    grid = lib.arap_grid_new(
                        ctypes.c_float(self.puppet_x),
                        ctypes.c_float(self.puppet_y),
                        ctypes.c_float(self.puppet_w),
                        ctypes.c_float(self.puppet_h),
                        ctypes.c_int(self.cols),
                        ctypes.c_int(self.rows),
                    )

                    if not grid:
                        self.deformed_verts = None
                        return

                    self._grid = grid
                    self._grid_sig = grid_sig
                    self._grid_cw_sig = None
            else:
                grid = lib.arap_grid_new(
                    ctypes.c_float(self.puppet_x),
                    ctypes.c_float(self.puppet_y),
                    ctypes.c_float(self.puppet_w),
                    ctypes.c_float(self.puppet_h),
                    ctypes.c_int(self.cols),
                    ctypes.c_int(self.rows),
                )

                if not grid:
                    self.deformed_verts = None
                    return

            if hasattr(lib, "arap_grid_set_cell_weights"):
                if (not can_reuse) or self._grid_cw_sig != cw_sig:
                    lib.arap_grid_set_cell_weights(grid, cw_arr, ncw)

                if can_reuse:
                    self._grid_cw_sig = cw_sig

            if hasattr(lib, "arap_grid_set_pin_edge"):
                lib.arap_grid_set_pin_edge(grid, 1 if self.pin_edge else 0)

            if can_reuse:
                lib.arap_grid_reset_pins(grid)

            has_taper = hasattr(lib, "arap_grid_add_pin_taper")
            for p in self.pins:
                th = p.theta if self.rotate_on else 0.0
                sc = p.scale if self.rotate_on else 1.0
                tp = p.taper
                if has_taper:
                    lib.arap_grid_add_pin_taper(
                        grid,
                        ctypes.c_float(p.sx),
                        ctypes.c_float(p.sy),
                        ctypes.c_float(p.dx - p.sx),
                        ctypes.c_float(p.dy - p.sy),
                        ctypes.c_float(th),
                        ctypes.c_float(sc),
                        ctypes.c_float(tp),
                    )
                elif self.rotate_on and (p.theta != 0.0 or p.scale != 1.0) \
                        and hasattr(lib, "arap_grid_add_pin_sim"):
                    lib.arap_grid_add_pin_sim(
                        grid,
                        ctypes.c_float(p.sx),
                        ctypes.c_float(p.sy),
                        ctypes.c_float(p.dx - p.sx),
                        ctypes.c_float(p.dy - p.sy),
                        ctypes.c_float(p.theta),
                        ctypes.c_float(p.scale),
                    )
                else:
                    lib.arap_grid_add_pin(
                        grid,
                        ctypes.c_float(p.sx),
                        ctypes.c_float(p.sy),
                        ctypes.c_float(p.dx - p.sx),
                        ctypes.c_float(p.dy - p.sy),
                    )

            lib.arap_grid_solve(
                grid,
                ctypes.c_int(iters),
                150,
                ctypes.c_float(0.05),
                ctypes.c_int(0),
            )

            n = ctypes.c_int(0)
            verts_ptr = lib.arap_grid_verts(grid, ctypes.byref(n))

            if verts_ptr and n.value > 0:
                n_val = n.value

                if self._verts_raw is None or self._verts_raw_n != n_val:
                    self._verts_raw = (ctypes.c_float * (2 * n_val))()
                    self._verts_raw_n = n_val

                    if HAS_NUMPY:
                        self._verts_raw_np = np.frombuffer(
                            self._verts_raw, dtype=np.float32
                        ).reshape(n_val, 2)
                    else:
                        self._verts_raw_np = None

                ctypes.memmove(
                    self._verts_raw,
                    verts_ptr,
                    2 * n_val * ctypes.sizeof(ctypes.c_float),
                )

                self._solve_seq += 1
                self._verts_raw_seq = self._solve_seq

                vv = [
                    (self._verts_raw[2 * i], self._verts_raw[2 * i + 1])
                    for i in range(n_val)
                ]

                self.deformed_verts = vv

                if len(vv) == (self.cols + 1) * (self.rows + 1):
                    self._last_good_verts = vv
            else:
                self.deformed_verts = None
                self._verts_raw = None
                self._verts_raw_n = 0
                self._verts_raw_np = None
                self._verts_raw_seq = -1

            # preview pad follows the solved mesh
            self._update_preview_pad()

        except Exception:
            traceback.print_exc()
            self.deformed_verts = None
            self._update_preview_pad()

        finally:
            if not can_reuse and grid is not None:
                try:
                    lib.arap_grid_free(grid)
                except Exception:
                    pass

    def _solve_sig(self):
        return (
            self.cols,
            self.rows,
            bool(self.pin_edge),
            bool(self.rotate_on),
            bool(self.use_stiffness),
            tuple(
                (
                    round(p.dx, 1),
                    round(p.dy, 1),
                    round(p.theta, 3),
                    round(p.scale, 3),
                    round(p.taper, 3),
                )
                for p in self.pins
            ),
        )

    def _cancel_update_timer(self):
        tid = getattr(self, "_update_timer", None)
        if tid:
            try:
                GLib.source_remove(tid)
            except Exception:
                pass
            self._update_timer = None

    def _deferred_update(self):
        self._update_timer = None

        if self._dead:
            return False

        sig = self._solve_sig()
        if sig == self._last_solve_sig:
            return False

        dbg = os.environ.get("PW_DEBUG")
        t0 = time.monotonic()

        try:
            self._solve_preview(self._idle_iters)

            dt = (time.monotonic() - t0) * 1000.0
            loop_ms = dt + getattr(self, "_render_ms", 0.0)

            old = self._tick_ms
            self._tick_ms = 0.65 * old + 0.35 * max(1.0, loop_ms)

            if dbg:
                print(
                    "[pw-perf] solve=%.1fms render~%.1fms tick_next=%dms"
                    % (
                        dt,
                        self._render_ms,
                        int(max(30, min(100, self._tick_ms * 1.25))),
                    )
                )

            self.drawing_area.queue_draw()
            self._sync_pin_panel()

        except Exception:
            traceback.print_exc()

        finally:
            self._last_solve_sig = self._solve_sig()

        return False

    def _schedule_update(self):
        if self._dead:
            return

        if self._update_timer is None:
            interval = int(max(30, min(100, self._tick_ms * 1.25)))
            self._update_timer = GLib.timeout_add(interval, self._deferred_update)

    def _screen_to_world(self, sx, sy):
        return ((sx - self.pan_x) / self.zoom, (sy - self.pan_y) / self.zoom)

    def _world_to_screen(self, wx, wy):
        return (wx * self.zoom + self.pan_x, wy * self.zoom + self.pan_y)

    def _find_pin_at(self, sx, sy, rad=14.0):
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
        if h is None or h < 0 or h >= len(self.pins):
            return

        self._push_undo()
        del self.pins[h]

        if self.active_pin_index == h:
            self.active_pin_index = None
        elif self.active_pin_index is not None and self.active_pin_index > h:
            self.active_pin_index -= 1

        ns = set()
        for i in self.selected:
            if i == h:
                continue
            ns.add(i - 1 if i > h else i)
        self.selected = ns

        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self._update_pin_count()
        self._sync_pin_panel()
        self.drawing_area.queue_draw()

    def _on_button_press(self, w, e):
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

        ctrl = bool(e.state & Gdk.ModifierType.CONTROL_MASK)
        alt = bool(e.state & Gdk.ModifierType.MOD1_MASK)
        shift = bool(e.state & Gdk.ModifierType.SHIFT_MASK)
        wx, wy = self._screen_to_world(e.x, e.y)

        if e.button == 1 and e.type == Gdk.EventType.DOUBLE_BUTTON_PRESS:
            hh = self._find_handle_at(e.x, e.y)
            if hh is not None:
                self._push_undo()
                p = self.pins[hh]
                p.theta = 0.0
                p.scale = 1.0
                self.active_pin_index = hh
                self.selected = {hh}
                self._solve_preview()
                self._last_solve_sig = self._solve_sig()
                self._sync_pin_panel()
                self.drawing_area.queue_draw()
                return True

            h = self._find_pin_at(e.x, e.y)
            if h is not None:
                p = self.pins[h]
                self._push_undo()
                p.dx, p.dy = p.sx, p.sy
                p.theta = 0.0
                p.scale = 1.0
                p.qx, p.qy = p.sx, p.sy
                self.active_pin_index = h
                self.selected = {h}
                self.is_dragging_pin = False
                self._solve_preview()
                self._last_solve_sig = self._solve_sig()
                self._sync_pin_panel()
                self.drawing_area.queue_draw()
                return True

            return True

        if e.button == 1:
            if (
                ctrl
                and self.rotate_on
                and self.active_pin_index is not None
                and 0 <= self.active_pin_index < len(self.pins)
            ):
                self._push_undo()
                p = self.pins[self.active_pin_index]
                p.qx, p.qy = wx, wy
                self.is_moving_pivot = True
                self.selected = {self.active_pin_index}
                self.drawing_area.queue_draw()
                return True

            hh = self._find_handle_at(e.x, e.y)
            if hh is not None and not alt:
                p = self.pins[hh]
                self._push_undo()
                self.is_rotating = True
                self.rot_grab = math.atan2(wy - p.qy, wx - p.qx) - p.theta
                self.active_pin_index = hh
                self.selected = {hh}
                self.drawing_area.queue_draw()
                return True

            if self.rotate_on and not alt:
                si = self._ring_hit(e.x, e.y)
                if si is not None:
                    p = self.pins[si]
                    self._push_undo()
                    self.is_scaling = True
                    self.scale_grab_dist = max(1e-3, math.hypot(wx - p.qx, wy - p.qy))
                    self.scale_grab = p.scale
                    self.scale_pin0 = (p.dx, p.dy)
                    self.active_pin_index = si
                    self.selected = {si}
                    self.drawing_area.queue_draw()
                    return True

            h = self._find_pin_at(e.x, e.y)

            if alt and h is not None:
                self._delete_pin_at(h)
                return True

            if shift and h is not None:
                if h in self.selected:
                    self.selected.discard(h)
                else:
                    self.selected.add(h)
                self.active_pin_index = h
                self._sync_pin_panel()
                self.drawing_area.queue_draw()
                return True

            if h is None:
                if len(self.pins) >= MAX_PINS:
                    self._update_pin_count()
                    return True

                self._push_undo()
                sx = round(wx * 2.0) / 2.0
                sy = round(wy * 2.0) / 2.0
                self.pins.append(Pin(sx, sy))
                self.active_pin_index = len(self.pins) - 1
                self.selected = {self.active_pin_index}
                self.drag_offset = (0.0, 0.0)
                self.is_dragging_pin = True
                self._drag_pushed = True
                self.multi_orig = None
                self.multi_anchor = None
                self._update_pin_count()
                self._sync_pin_panel()
                self.drawing_area.queue_draw()
                return True

            p = self.pins[h]
            self.active_pin_index = h

            if h not in self.selected:
                self.selected = {h}

            self.drag_offset = (p.dx - wx, p.dy - wy)
            self.is_dragging_pin = True
            self._drag_pushed = False

            if len(self.selected) > 1:
                self.multi_orig = {
                    i: (
                        self.pins[i].dx,
                        self.pins[i].dy,
                        self.pins[i].qx - self.pins[i].dx,
                        self.pins[i].qy - self.pins[i].dy,
                    )
                    for i in self.selected
                }
                self.multi_anchor = (wx, wy)
            else:
                self.multi_orig = None
                self.multi_anchor = None

            self._sync_pin_panel()
            self.drawing_area.queue_draw()
            return True

        return False

    def _on_button_release(self, w, e):
        tid = getattr(self, "_motion_timer", None)
        if tid is not None:
            try:
                GLib.source_remove(tid)
            except Exception:
                pass
            self._motion_timer = None

        self._motion_pending = False
        self._last_drag_world = None
        self.multi_orig = None
        self.multi_anchor = None

        if e.button == 2:
            self.is_panning = False
            return True

        if e.button == 1 and self.is_moving_pivot:
            self.is_moving_pivot = False
            self._sync_pin_panel()
            self.drawing_area.queue_draw()
            return True

        if e.button == 1 and self.is_scaling:
            self._cancel_update_timer()
            self.is_scaling = False
            self._solve_preview()
            self._last_solve_sig = self._solve_sig()
            self._sync_pin_panel()
            self.drawing_area.queue_draw()
            return True

        if e.button == 1 and self.is_rotating:
            self._cancel_update_timer()
            self.is_rotating = False
            self._solve_preview()
            self._last_solve_sig = self._solve_sig()
            self._sync_pin_panel()
            self.drawing_area.queue_draw()
            return True

        if e.button == 1 and self.is_dragging_pin:
            self._cancel_update_timer()
            self.is_dragging_pin = False
            self._drag_pushed = False
            self._solve_preview()
            self._last_solve_sig = self._solve_sig()
            self._sync_pin_panel()
            self.drawing_area.queue_draw()
            return True

        return False

    def _on_motion(self, w, e):
        if self.is_panning:
            self.pan_x = e.x - self.pan_start[0]
            self.pan_y = e.y - self.pan_start[1]
            self.drawing_area.queue_draw()
            return True

        wx, wy = self._screen_to_world(e.x, e.y)

        if self.is_moving_pivot and self.active_pin_index is not None:
            p = self.pins[self.active_pin_index]
            p.qx, p.qy = wx, wy
            self.drawing_area.queue_draw()
            return True

        if self.is_scaling and self.active_pin_index is not None:
            p = self.pins[self.active_pin_index]
            r = math.hypot(wx - p.qx, wy - p.qy) / self.scale_grab_dist
            ns = max(0.25, min(4.0, self.scale_grab * r))

            if e.state & Gdk.ModifierType.SHIFT_MASK:
                ns = math.floor(ns / 0.05 + 0.5) * 0.05
                ns = max(0.25, min(4.0, ns))

            k = ns / self.scale_grab if self.scale_grab else 1.0
            p.dx = p.qx + (self.scale_pin0[0] - p.qx) * k
            p.dy = p.qy + (self.scale_pin0[1] - p.qy) * k
            p.scale = ns

            self.drawing_area.queue_draw()
            self._queue_drag_solve()
            return True

        if self.is_rotating and self.active_pin_index is not None:
            p = self.pins[self.active_pin_index]
            ang = math.atan2(wy - p.qy, wx - p.qx) - self.rot_grab

            while ang > math.pi:
                ang -= 2 * math.pi
            while ang < -math.pi:
                ang += 2 * math.pi

            if e.state & Gdk.ModifierType.SHIFT_MASK:
                step = math.pi / 12.0
                ang = math.floor(ang / step + 0.5) * step

                while ang > math.pi:
                    ang -= 2 * math.pi
                while ang < -math.pi:
                    ang += 2 * math.pi

            delta = ang - p.theta
            c, s = math.cos(delta), math.sin(delta)
            rx, ry = p.dx - p.qx, p.dy - p.qy
            p.dx = p.qx + c * rx - s * ry
            p.dy = p.qy + s * rx + c * ry
            p.theta = ang

            self.drawing_area.queue_draw()
            self._queue_drag_solve()
            return True

        if self.is_dragging_pin and self.active_pin_index is not None:
            if not self._drag_pushed:
                self._push_undo()
                self._drag_pushed = True

            if self.multi_orig is not None and len(self.selected) > 1:
                ax, ay = self.multi_anchor
                ddx = wx - ax
                ddy = wy - ay

                for i, (ox, oy, oqx, oqy) in self.multi_orig.items():
                    if 0 <= i < len(self.pins):
                        p = self.pins[i]
                        p.dx = ox + ddx
                        p.dy = oy + ddy
                        p.qx = p.dx + oqx
                        p.qy = p.dy + oqy

                self.drawing_area.queue_draw()
                self._queue_drag_solve()
                return True

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
            rr = math.hypot(ddx, ddy)

            if rr > maxd:
                ndx = p.sx + ddx / rr * maxd
                ndy = p.sy + ddy / rr * maxd

            if self._last_drag_world is not None:
                lx, ly = self._last_drag_world
                if abs(ndx - lx) < self._drag_min_step and abs(ndy - ly) < self._drag_min_step:
                    return True

            self._last_drag_world = (ndx, ndy)
            ddx = ndx - p.dx
            ddy = ndy - p.dy
            p.dx = ndx
            p.dy = ndy
            p.qx += ddx
            p.qy += ddy

            self.drawing_area.queue_draw()
            self._queue_drag_solve()
            return True

        if (
            not self.is_panning
            and not self.is_dragging_pin
            and not self.is_rotating
            and not self.is_scaling
            and not self.is_moving_pivot
        ):
            h = self._find_pin_at(e.x, e.y)
            if h != self.hovered_pin_index:
                self.hovered_pin_index = h
                self.drawing_area.queue_draw()

        return False

    def _on_scroll(self, w, e):
        st = e.state
        ctrl = bool(st & Gdk.ModifierType.CONTROL_MASK)
        alt = bool(st & Gdk.ModifierType.MOD1_MASK)
        shift = bool(st & Gdk.ModifierType.SHIFT_MASK)

        # Shift wheel: taper pin.
        if shift and not ctrl and not alt:
            h = self._find_pin_at(e.x, e.y)
            if h is None:
                h = self.active_pin_index
            if h is not None and 0 <= h < len(self.pins):
                if e.direction == Gdk.ScrollDirection.UP:
                    tstep = 0.05
                elif e.direction == Gdk.ScrollDirection.DOWN:
                    tstep = -0.05
                else:
                    return True
                self._push_undo()
                p = self.pins[h]
                p.taper = max(0.0, min(1.0, p.taper + tstep))
                self.active_pin_index = h
                self.selected = {h}
                self._schedule_update()
                self.drawing_area.queue_draw()
                return True

        if self.rotate_on and ctrl:
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
                p = self.pins[h]
                ns = max(0.25, min(4.0, p.scale + step))
                k = ns / p.scale if p.scale else 1.0
                p.dx = p.qx + (p.dx - p.qx) * k
                p.dy = p.qy + (p.dy - p.qy) * k
                p.scale = ns
                self.active_pin_index = h
                self._schedule_update()
                self.drawing_area.queue_draw()
                return True

        if self.rotate_on and alt:
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
                p = self.pins[h]
                c, s = math.cos(step), math.sin(step)
                rx, ry = p.dx - p.qx, p.dy - p.qy
                p.dx = p.qx + c * rx - s * ry
                p.dy = p.qy + s * rx + c * ry
                a = p.theta + step
                while a > math.pi:
                    a -= 2 * math.pi
                while a < -math.pi:
                    a += 2 * math.pi
                p.theta = a
                self.active_pin_index = h

                self._schedule_update()
                self.drawing_area.queue_draw()
                return True

        s = 1.15 if e.direction == Gdk.ScrollDirection.UP else 1.0 / 1.15
        nz = max(0.05, min(10.0, self.zoom * s))

        self.pan_x = e.x - (e.x - self.pan_x) * (nz / self.zoom)
        self.pan_y = e.y - (e.y - self.pan_y) * (nz / self.zoom)
        self.zoom = nz

        try:
            self._schedule_raster_zoom()
        except Exception:
            pass

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
        self._last_solve_sig = self._solve_sig()
        self.drawing_area.queue_draw()

    def _clear_pins(self):
        self.pins = []
        self.active_pin_index = None
        self.selected = set()

        self._solve_preview()
        self._last_solve_sig = self._solve_sig()
        self._update_pin_count()
        self._sync_pin_panel()
        self.drawing_area.queue_draw()

    def fit_to_view(self):
        a = self.drawing_area.get_allocation()
        dw = a.width if a.width > 50 else 800
        dh = a.height if a.height > 50 else 600

        # Fit active layer, background preview, and extend padding if visible.
        x0, y0 = 0.0, 0.0
        x1, y1 = float(self.canvas_w), float(self.canvas_h)

        b = getattr(self, "_bg_bounds", None)
        if b is not None:
            bx, by, bw, bh = b
            x0 = min(x0, float(bx))
            y0 = min(y0, float(by))
            x1 = max(x1, float(bx) + float(bw))
            y1 = max(y1, float(by) + float(bh))

        if getattr(self, "extend_on", False):
            x0 = min(x0, -float(getattr(self, "preview_pad_left", 0)))
            y0 = min(y0, -float(getattr(self, "preview_pad_top", 0)))
            x1 = max(x1, float(self.canvas_w) + float(getattr(self, "preview_pad_right", 0)))
            y1 = max(y1, float(self.canvas_h) + float(getattr(self, "preview_pad_bottom", 0)))

        bw = max(1.0, x1 - x0)
        bh = max(1.0, y1 - y0)

        self.zoom = min(
            (dw - 20) / bw,
            (dh - 20) / bh,
            2.0,
        )

        self.pan_x = (dw - bw * self.zoom) / 2.0 - x0 * self.zoom
        self.pan_y = (dh - bh * self.zoom) / 2.0 - y0 * self.zoom
        self._raster_zoom = self.zoom

        self.drawing_area.queue_draw()

    def _queue_drag_solve(self):
        if getattr(self, "_dead", False):
            return

        if getattr(self, "_motion_timer", None) is not None:
            return

        self._motion_pending = True
        self._motion_timer = GLib.timeout_add(8, self._flush_drag_solve)

    def _flush_drag_solve(self):
        self._motion_timer = None
        self._motion_pending = False

        if getattr(self, "_dead", False):
            return False

        self._schedule_update()
        return False

    def _ensure_verts_raw(self, verts, nverts):
        seq = getattr(self, "_solve_seq", 0)

        if (
            getattr(self, "_verts_raw", None) is not None
            and getattr(self, "_verts_raw_n", 0) == nverts
            and getattr(self, "_verts_raw_seq", -1) == seq
        ):
            return self._verts_raw

        if verts is None or nverts <= 0 or len(verts) != nverts:
            return None

        arr = (ctypes.c_float * (2 * nverts))()

        if HAS_NUMPY:
            nparr = np.asarray(verts, dtype=np.float32).reshape(nverts, 2)
            ctypes.memmove(
                arr,
                ctypes.c_void_p(nparr.ctypes.data),
                2 * nverts * ctypes.sizeof(ctypes.c_float),
            )
            self._verts_raw_np = np.frombuffer(arr, dtype=np.float32).reshape(nverts, 2)
        else:
            for i, pt in enumerate(verts):
                x, y = pt
                arr[2 * i] = float(x)
                arr[2 * i + 1] = float(y)
            self._verts_raw_np = None

        self._verts_raw = arr
        self._verts_raw_n = nverts
        self._verts_raw_seq = seq

        return arr

    def _weight_bucket(self, wgt):
        try:
            w = float(getattr(wgt, "value", wgt))
        except Exception:
            w = 0.0

        if w < 0.0:
            w = 0.0
        elif w > 1.0:
            w = 1.0

        return 0 if w <= 0.02 else 1 + min(3, int(w * 4))

    def _on_draw(self, w, cr):
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
            cr.save()
            cr.translate(
                getattr(self, "_bg_off_x", 0.0),
                getattr(self, "_bg_off_y", 0.0),
            )
            cr.set_source_surface(self._bg_surface, 0, 0)

            a = getattr(self, "_bg_alpha", 1.0)
            if a >= 0.999:
                cr.paint()
            else:
                cr.paint_with_alpha(a)

            cr.restore()

        self._warp_preview_cairo(cr)

        style = self.mesh_style
        if (
            not self.is_dragging_pin
            and not self.is_rotating
            and not self.is_scaling
            and style != "off"
        ):
            ws = self.cell_weights(self.cols, self.rows) if self.use_stiffness else None
            verts = self.deformed_verts

            if verts is None:
                verts = [
                    (
                        self.puppet_x + cc * self.puppet_w / self.cols,
                        self.puppet_y + rr * self.puppet_h / self.rows,
                    )
                    for rr in range(self.rows + 1)
                    for cc in range(self.cols + 1)
                ]

            mr, mg, mb = self.mesh_color
            cr.set_line_width(max(0.75, 1.0 / self.zoom))

            for rr in range(self.rows):
                for cc in range(self.cols):
                    i00 = rr * (self.cols + 1) + cc
                    i10 = i00 + 1
                    i01 = i00 + (self.cols + 1)
                    i11 = i01 + 1
                    if ws is None:
                        cr.set_source_rgba(mr, mg, mb, 0.25)
                    else:
                        b = self._weight_bucket(ws[rr * self.cols + cc])

                        if b == 0:
                            cr.set_source_rgba(1.0, 0.25, 0.20, 0.12)
                        else:
                            cr.set_source_rgba(mr, mg, mb, 0.12 + 0.28 * ((b - 0.5) / 4.0))

                    if style in ("cells", "both"):
                        cr.new_path()
                        cr.move_to(verts[i00][0], verts[i00][1])
                        cr.line_to(verts[i10][0], verts[i10][1])
                        cr.line_to(verts[i11][0], verts[i11][1])
                        cr.line_to(verts[i01][0], verts[i01][1])
                        cr.close_path()
                        cr.stroke()

                    if style in ("triangles", "both"):
                        cr.new_path()
                        cr.move_to(verts[i10][0], verts[i10][1])
                        cr.line_to(verts[i01][0], verts[i01][1])
                        cr.stroke()

            cr.set_source_rgba(mr, mg, mb, 0.5)
            cr.set_line_width(max(1.0, 1.5 / self.zoom))
            cr.rectangle(self.puppet_x, self.puppet_y, self.puppet_w, self.puppet_h)
            cr.stroke()

        pr = max(5.0, 8.0 / self.zoom)

        for i, p in enumerate(self.pins):
            if (
                not self.is_dragging_pin
                and not self.is_rotating
                and (abs(p.dx - p.sx) > 0.5 or abs(p.dy - p.sy) > 0.5)
            ):
                cr.save()
                cr.set_source_rgba(1, 1, 1, 0.45)
                cr.set_line_width(max(1.0, 1.5 / self.zoom))
                cr.set_dash([5 / self.zoom, 5 / self.zoom])
                cr.move_to(p.sx, p.sy)
                cr.line_to(p.dx, p.dy)
                cr.stroke()
                cr.restore()

            if p.taper > 0.01 and (i == self.active_pin_index or i == self.hovered_pin_index):
                rt = self._taper_base_radius() * (1.0 + p.taper * 3.0)
                cr.save()
                cr.set_source_rgba(0.75, 0.45, 1.0, 0.28)
                cr.set_line_width(max(1.0, 1.5 / self.zoom))
                cr.arc(p.dx, p.dy, rt, 0, 6.2832)
                cr.stroke()
                cr.restore()

            cr.set_source_rgba(0, 0, 0, 0.85)
            cr.arc(p.dx, p.dy, pr + 1.5 / self.zoom, 0, 6.2832)
            cr.fill()

            if i == self.active_pin_index:
                cr.set_source_rgb(0.2, 1.0, 0.3)
            elif i in self.selected:
                cr.set_source_rgb(0.2, 0.9, 1.0)
            elif i == self.hovered_pin_index:
                cr.set_source_rgb(0.6, 0.95, 1.0)
            else:
                cr.set_source_rgb(1.0, 0.9, 0.2)

            cr.arc(p.dx, p.dy, pr, 0, 6.2832)
            cr.fill()

            if i in self.selected and i != self.active_pin_index:
                cr.save()
                cr.set_source_rgba(1.0, 1.0, 1.0, 0.85)
                cr.set_line_width(max(1.0, 1.5 / self.zoom))
                cr.arc(p.dx, p.dy, pr + 2.5 / self.zoom, 0, 6.2832)
                cr.stroke()
                cr.restore()

            if i == self.hovered_pin_index and i != self.active_pin_index and (p.taper > 0.005 or p.depth):
                cr.save()
                z = self.zoom
                cr.set_font_size(max(10.0, 12.0 / z))
                cr.set_source_rgba(0.75, 0.45, 1.0, 0.95)
                lbl = ""
                if p.taper > 0.005:
                    lbl += f"t={p.taper:.2f}"
                if p.depth:
                    lbl += (f"  d={p.depth:+d}" if lbl else f"d={p.depth:+d}")
                cr.move_to(p.dx + pr + 4.0 / z, p.dy - pr - 2.0 / z)
                cr.show_text(lbl)
                cr.restore()

        i = self.active_pin_index

        if (
            self.rotate_on
            and i is not None
            and 0 <= i < len(self.pins)
            and not self.is_dragging_pin
        ):
            p = self.pins[i]
            rr = 28.0 / self.zoom

            if abs(p.qx - p.dx) > 0.5 or abs(p.qy - p.dy) > 0.5:
                cr.save()
                cr.set_source_rgba(0.7, 0.4, 1.0, 0.5)
                cr.set_line_width(max(0.8, 1.0 / self.zoom))
                cr.set_dash([4 / self.zoom, 4 / self.zoom])
                cr.move_to(p.dx, p.dy)
                cr.line_to(p.qx, p.qy)
                cr.stroke()
                cr.restore()

                cr.set_source_rgba(0.0, 0.0, 0.0, 0.7)
                cr.arc(p.qx, p.qy, max(3.0, 4.5 / self.zoom) + 1.0 / self.zoom, 0, 6.2832)
                cr.fill()
                cr.set_source_rgb(0.75, 0.45, 1.0)
                cr.arc(p.qx, p.qy, max(3.0, 4.5 / self.zoom), 0, 6.2832)
                cr.fill()
                arm = 7.0 / self.zoom
                cr.set_source_rgba(1.0, 1.0, 1.0, 0.9)
                cr.set_line_width(max(1.0, 1.2 / self.zoom))
                cr.move_to(p.qx - arm, p.qy)
                cr.line_to(p.qx + arm, p.qy)
                cr.move_to(p.qx, p.qy - arm)
                cr.line_to(p.qx, p.qy + arm)
                cr.stroke()

            cr.set_line_width(max(1.0, 1.5 / self.zoom))
            cr.set_source_rgba(1.0, 0.6, 0.1, 0.9)
            cr.arc(p.qx, p.qy, rr, 0, 6.2832)
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

            txt = f"θ={math.degrees(p.theta):.0f}°"
            if abs(p.scale - 1.0) > 0.005:
                txt += f"  s={p.scale:.2f}"
            if p.taper > 0.005:
                txt += f"  t={p.taper:.2f}"
            if p.depth:
                txt += f"  d={p.depth:+d}"

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

    def _warp_preview_cairo(self, cr):
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
        depth_on = self._depth_on()
        use_depth_preview = bool(
            depth_on and lib is not None and hasattr(lib, "pw_render_preview_view_depth")
        )

        # C preview supports depth via z-buffer; old builds fall back to Python.
        if (
            lib is not None
            and hasattr(lib, "pw_render_preview_view")
            and os.environ.get("PW_NO_C_PREVIEW") != "1"
            and (not depth_on or use_depth_preview)
        ):
            dragging = self.is_dragging_pin or self.is_rotating

            if dragging or bool(getattr(self, "extend_on", False)):
                vx0, vy0, vx1, vy1, scale_now = self._view_rect()
                vkey = (
                    round(vx0, 1),
                    round(vy0, 1),
                    round(vx1, 1),
                    round(vy1, 1),
                )
            else:
                vx0 = vy0 = 0.0
                scale_now = self._preview_scale()
                vkey = ()

            dkey = tuple(int(p.depth) for p in self.pins) if depth_on else ()
            key = (
                self._solve_seq,
                self.cols,
                self.rows,
                self.canvas_w,
                self.canvas_h,
                round(scale_now, 4),
                vkey,
                dkey,
            )

            dst = self._dst_surface

            if self._raster_key == key and dst is not None and dst.get_width() > 0:
                cr.save()
                cr.translate(vx0, vy0)
                cr.scale(1.0 / scale_now, 1.0 / scale_now)
                cr.set_source_surface(dst, 0, 0)
                cr.paint()
                cr.restore()
                return

            if self._c_render(lib, cr, verts, depth_on, use_depth_preview):
                self._raster_key = key
                return

        cols, rows = self.cols, self.rows

        pattern = cairo.SurfacePattern(self.src_surface)
        pattern.set_filter(cairo.FILTER_BILINEAR)
        pattern.set_extend(cairo.EXTEND_PAD if getattr(self, "extend_on", False) else cairo.EXTEND_NONE)

        try:
            if self._depth_on():
                vd = self._compute_vertex_depths()
                tris = []
                for r in range(rows):
                    base = r * (cols + 1)
                    for c in range(cols):
                        i00 = base + c
                        i10 = i00 + 1
                        i01 = i00 + (cols + 1)
                        i11 = i01 + 1
                        a0, a1, a2 = vd[i00], vd[i10], vd[i01]
                        b0, b1, b2 = vd[i10], vd[i11], vd[i01]
                        td0 = a0 if a0 >= a1 and a0 >= a2 else (a1 if a1 >= a2 else a2)
                        td1 = b0 if b0 >= b1 and b0 >= b2 else (b1 if b1 >= b2 else b2)
                        tris.append((td0, r, c, 0))
                        tris.append((td1, r, c, 1))
                tris.sort()
                for td, r, c, which in tris:
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
                    if which == 0:
                        self._warp_tri(cr, pattern, s00, s10, s01,
                                       verts[i00], verts[i10], verts[i01])
                    else:
                        self._warp_tri(cr, pattern, s10, s11, s01,
                                       verts[i10], verts[i11], verts[i01])
            else:
                for r in range(rows):
                    for c in range(cols):
                        i00 = r * (cols + 1) + c
                        i10 = i00 + 1
                        i01 = i00 + (cols + 1)
                        i11 = i01 + 1

                        s00 = (
                            self.puppet_x + c * self.puppet_w / cols,
                            self.puppet_y + r * self.puppet_h / rows,
                        )
                        s10 = (
                            self.puppet_x + (c + 1) * self.puppet_w / cols,
                            self.puppet_y + r * self.puppet_h / rows,
                        )
                        s01 = (
                            self.puppet_x + c * self.puppet_w / cols,
                            self.puppet_y + (r + 1) * self.puppet_h / rows,
                        )
                        s11 = (
                            self.puppet_x + (c + 1) * self.puppet_w / cols,
                            self.puppet_y + (r + 1) * self.puppet_h / rows,
                        )

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
        u0, v0 = d0
        u1, v1 = d1
        u2, v2 = d2

        x0, y0 = s0
        x1, y1 = s1
        x2, y2 = s2

        det = (u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0)

        if not math.isfinite(det) or det < 0.5:
            self._tri_stat["det"] += 1
            return

        dsq01 = (u1 - u0) ** 2 + (v1 - v0) ** 2
        dsq12 = (u2 - u1) ** 2 + (v2 - v1) ** 2
        dsq20 = (u0 - u2) ** 2 + (v0 - v2) ** 2

        ssq01 = (x1 - x0) ** 2 + (y1 - y0) ** 2
        ssq12 = (x2 - x1) ** 2 + (y2 - y1) ** 2
        ssq20 = (x0 - x2) ** 2 + (y0 - y2) ** 2

        if dsq01 > 64.0 * ssq01 or dsq12 > 64.0 * ssq12 or dsq20 > 64.0 * ssq20:
            self._tri_stat["scale"] += 1
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

        pattern.set_matrix(
            cairo.Matrix(xx=a, yx=b, xy=c, yy=d, x0=e, y0=f)
        )

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

    def _ensure_checker(self):
        if (
            self._checker is not None
            and self._checker_w == self.canvas_w
            and self._checker_h == self.canvas_h
        ):
            return self._checker

        surf = cairo.ImageSurface(
            cairo.FORMAT_ARGB32,
            self.canvas_w,
            self.canvas_h,
        )

        crc = cairo.Context(surf)
        size = 16

        for rr in range(int(math.ceil(self.canvas_h / size))):
            for cc in range(int(math.ceil(self.canvas_w / size))):
                if (rr + cc) % 2 == 0:
                    crc.set_source_rgb(0.85, 0.85, 0.85)
                else:
                    crc.set_source_rgb(0.65, 0.65, 0.65)

                crc.rectangle(cc * size, rr * size, size, size)
                crc.fill()

        surf.flush()

        self._checker = surf
        self._checker_w = self.canvas_w
        self._checker_h = self.canvas_h

        return surf

    def _dst_has_content(self):
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

    def _c_render(self, lib, cr, verts, depth_on=False, use_depth_preview=False):
        try:
            nverts = (self.cols + 1) * (self.rows + 1)

            if verts is None or len(verts) != nverts:
                return False

            raw = self._ensure_verts_raw(verts, nverts)
            if raw is None:
                return False

            dragging = bool(
                getattr(self, "is_dragging_pin", False)
                or getattr(self, "is_rotating", False)
            )

            # Extended preview needs the widened view rect even when idle.
            use_view = dragging or bool(getattr(self, "extend_on", False))

            if use_view:
                vx0, vy0, vx1, vy1, scale = self._view_rect()
                sw = max(1, int((vx1 - vx0) * scale))
                sh = max(1, int((vy1 - vy0) * scale))
            else:
                vx0 = vy0 = 0.0
                scale = self._preview_scale()
                sw = max(1, int(self.canvas_w * scale))
                sh = max(1, int(self.canvas_h * scale))

            if scale <= 0.0:
                return False

            dst = getattr(self, "_dst_scaled", None)

            if dst is None or dst.get_width() != sw or dst.get_height() != sh:
                dst = cairo.ImageSurface(cairo.FORMAT_ARGB32, sw, sh)
                self._dst_scaled = dst

            self._dst_surface = dst

            src_mv = self.src_surface.get_data()
            dst_mv = dst.get_data()

            src_ptr = ctypes.addressof(ctypes.c_char.from_buffer(src_mv))
            dst_ptr = ctypes.addressof(ctypes.c_char.from_buffer(dst_mv))

            t0 = time.monotonic()

            if not hasattr(lib, "pw_render_preview_view"):
                return False

            common_args = (
                ctypes.c_void_p(src_ptr),
                ctypes.c_int(self.canvas_w),
                ctypes.c_int(self.canvas_h),
                ctypes.c_int(self.src_surface.get_stride()),
                ctypes.c_void_p(dst_ptr),
                ctypes.c_int(sw),
                ctypes.c_int(sh),
                ctypes.c_int(dst.get_stride()),
                ctypes.c_int(self.cols),
                ctypes.c_int(self.rows),
                ctypes.cast(raw, ctypes.POINTER(ctypes.c_float)),
                ctypes.c_int(nverts),
                ctypes.c_float(self.puppet_x),
                ctypes.c_float(self.puppet_y),
                ctypes.c_float(self.puppet_w),
                ctypes.c_float(self.puppet_h),
                ctypes.c_float(vx0),
                ctypes.c_float(vy0),
                ctypes.c_float(scale),
                ctypes.c_int(1 if bool(getattr(self, "extend_on", False)) else 0),
            )

            if use_depth_preview:
                vd = self._compute_vertex_depths()
                if len(vd) != nverts:
                    return False

                varr = (ctypes.c_int * nverts)(*vd)
                ret = lib.pw_render_preview_view_depth(
                    *common_args,
                    ctypes.c_int(1),
                    ctypes.cast(varr, ctypes.POINTER(ctypes.c_int)),
                )
            else:
                ret = lib.pw_render_preview_view(*common_args)

            rt = (time.monotonic() - t0) * 1000.0
            self._render_ms = 0.65 * getattr(self, "_render_ms", 0.0) + 0.35 * max(1.0, rt)

            if ret <= 0:
                if os.environ.get("PW_DEBUG"):
                    print("[pw-c-prev] ret<=0 -> fallback python")
                return False

            dst.mark_dirty()

            if not dragging and not self._dst_has_content():
                if os.environ.get("PW_DEBUG"):
                    print("[pw-c-prev] dst empty -> fallback python (draw=%d)" % ret)
                return False

            cr.save()
            cr.translate(vx0, vy0)
            cr.scale(1.0 / scale, 1.0 / scale)
            cr.set_source_surface(dst, 0, 0)
            cr.paint()
            cr.restore()

            stat = getattr(self, "_tri_stat", None)
            if stat is None:
                stat = {"draw": 0, "det": 0, "scale": 0, "mdet": 0, "cairo": 0}
                self._tri_stat = stat

            stat["draw"] = ret

            if os.environ.get("PW_DEBUG"):
                print(
                    "[pw-c-prev] draw=", ret,
                    "scale=", round(scale, 3),
                    "dst=", sw, "x", sh,
                    "drag=", int(dragging),
                    "view_fn=", 1,
                    "depth=", int(use_depth_preview),
                )

            return True

        except Exception as e:
            print(f"[puppet-warp] c-preview failed, fallback python: {e}")
            return False

def _pw_try(fn, *args):
    try:
        fn(*args)
        return True
    except Exception:
        return False


def _pw_has_resize_api(item):
    if getattr(item, "resize", None):
        return True

    for cls_name in ("Item", "Layer", "Drawable"):
        cls = getattr(Gimp, cls_name, None)
        if cls is not None and getattr(cls, "resize", None):
            return True

    if getattr(Gimp, "item_resize", None):
        return True

    pdb = getattr(Gimp, "pdb", None)
    if pdb is not None and getattr(pdb, "gimp_item_resize", None):
        return True

    return False


def _pw_has_offset_api(item):
    for name in ("set_offset", "set_offsets"):
        if getattr(item, name, None):
            return True

    for cls_name in ("Item", "Layer", "Drawable"):
        cls = getattr(Gimp, cls_name, None)
        if cls is None:
            continue

        for name in ("set_offset", "set_offsets"):
            if getattr(cls, name, None):
                return True

    for name in ("item_set_offset", "item_set_offsets"):
        if getattr(Gimp, name, None):
            return True

    pdb = getattr(Gimp, "pdb", None)
    if pdb is not None:
        for name in ("gimp_item_set_offset", "gimp_item_set_offsets"):
            if getattr(pdb, name, None):
                return True

    return False


def _pw_resize_item(item, w, h, ax, ay):
    # Bound method first: item.resize(...)
    fn = getattr(item, "resize", None)
    if fn and _pw_try(fn, w, h, ax, ay):
        return True

    # Unbound GIMP 3 class methods: Gimp.Item.resize(item, ...)
    for cls_name in ("Item", "Layer", "Drawable"):
        cls = getattr(Gimp, cls_name, None)
        fn = getattr(cls, "resize", None) if cls is not None else None
        if fn and _pw_try(fn, item, w, h, ax, ay):
            return True

    # Older namespace-style fallbacks, if present.
    fn = getattr(Gimp, "item_resize", None)
    if fn and _pw_try(fn, item, w, h, ax, ay):
        return True

    # GIMP 2 PDB compatibility, only if this build actually exposes it.
    pdb = getattr(Gimp, "pdb", None)
    fn = getattr(pdb, "gimp_item_resize", None) if pdb is not None else None
    if fn and _pw_try(fn, item, w, h, ax, ay):
        return True

    return False


def _pw_set_item_offset(item, x, y):
    # Bound method first: item.set_offset(x, y)
    for name in ("set_offset", "set_offsets"):
        fn = getattr(item, name, None)
        if fn and _pw_try(fn, x, y):
            return True

    # Unbound GIMP 3 class methods: Gimp.Item.set_offset(item, x, y)
    for cls_name in ("Item", "Layer", "Drawable"):
        cls = getattr(Gimp, cls_name, None)
        if cls is None:
            continue

        for name in ("set_offset", "set_offsets"):
            fn = getattr(cls, name, None)
            if fn and _pw_try(fn, item, x, y):
                return True

    # Namespace-style fallbacks.
    for name in ("item_set_offset", "item_set_offsets"):
        fn = getattr(Gimp, name, None)
        if fn and _pw_try(fn, item, x, y):
            return True

    # GIMP 2 PDB compatibility, guarded.
    pdb = getattr(Gimp, "pdb", None)
    if pdb is not None:
        for name in ("gimp_item_set_offset", "gimp_item_set_offsets"):
            fn = getattr(pdb, name, None)
            if fn and _pw_try(fn, item, x, y):
                return True

    return False


def _pw_image_undo(image):
    # Roll back the measurement group if GIMP did not resize the mask.
    if image is None:
        return False
    fn = getattr(image, "undo", None)
    if fn and _pw_try(fn):
        return True
    cls = getattr(Gimp, "Image", None)
    f = getattr(cls, "undo", None) if cls is not None else None
    if f and _pw_try(f, image):
        return True
    f = getattr(Gimp, "image_undo", None)
    if f and _pw_try(f, image):
        return True
    return False


def _resize_drawable_plan(drawable, plan):
    # mask-aware extend: measure GIMP's own mask resize, never hand-pad it.
    if not plan or not plan.get("needs_resize"):
        return True

    try:
        nw = int(plan["new_w"])
        nh = int(plan["new_h"])
        nox = int(plan["new_off_x"])
        noy = int(plan["new_off_y"])
        ax = int(plan.get("anchor_x", 0))
        ay = int(plan.get("anchor_y", 0))
    except Exception:
        return False

    if nw <= 0 or nh <= 0:
        return False

    if not _pw_has_resize_api(drawable):
        print("[puppet-warp] resize aborted: no usable item resize API")
        return False

    if not _pw_has_offset_api(drawable):
        print("[puppet-warp] resize aborted: no usable item offset API")
        return False

    if os.environ.get("PW_DEBUG"):
        print(
            "[pw-extend] resize w=%d h=%d anchor=(%d,%d) off=(%d,%d)"
            % (nw, nh, ax, ay, nox, noy)
        )

    image = None
    try:
        image = drawable.get_image()
    except Exception:
        image = None

    has_mask = False
    try:
        has_mask = drawable.get_mask() is not None
    except Exception:
        has_mask = False

    if has_mask and os.environ.get("PW_NO_MASK_EXTEND") == "1":
        if os.environ.get("PW_DEBUG"):
            print("[pw-mask] PW_NO_MASK_EXTEND=1: padding layer, mask untouched")
        has_mask = False

    started = False
    try:
        if image is not None and hasattr(image, "undo_group_start"):
            image.undo_group_start()
            started = True
    except Exception:
        image = None

    try:
        if not _pw_resize_item(drawable, nw, nh, ax, ay):
            raise RuntimeError("item resize failed")
        if not _pw_set_item_offset(drawable, nox, noy):
            raise RuntimeError("item offset failed")

        if has_mask:
            mw = mh = -1
            mox = moy = 0
            try:
                m = drawable.get_mask()
                if m is not None:
                    mw = m.get_width()
                    mh = m.get_height()
                    mox, moy = _unpack_offsets(m.get_offsets())
            except Exception:
                pass

            auto = (mw == nw and mh == nh)
            if os.environ.get("PW_DEBUG"):
                print(
                    "[pw-mask] measure: layer %dx%d, mask now %dx%d off=(%d,%d) auto=%d"
                    % (nw, nh, mw, mh, mox, moy, int(auto))
                )

            if not auto:
                if _pw_image_undo(image):
                    try:
                        Gimp.displays_flush()
                    except Exception:
                        pass
                    print(
                        "[pw-mask] layer resize did NOT auto-pad mask; "
                        "rolled back; extend skipped for this masked layer"
                    )
                else:
                    print(
                        "[pw-mask] did NOT auto-pad mask and undo unavailable; "
                        "layer left padded with drifted mask -- press Ctrl+Z"
                    )
                return False

        return True

    except Exception as e:
        print(f"[puppet-warp] resize plan failed: {e}")
        return False

    finally:
        if started and image is not None:
            try:
                image.undo_group_end()
            except Exception:
                pass

def apply_filter_to_drawable(drawable, params_json):
    stale = []

    try:
        for f in list(drawable.get_filters() or []):
            try:
                f.get_config().get_property("params-json")
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

    if hasattr(drawable, "append_filter"):
        drawable.append_filter(filt)
    elif hasattr(drawable, "add_filter"):
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
        return True, None, None

    def do_query_procedures(self):
        return [PLUG_IN_PROC]

    def do_create_procedure(self, name):
        proc = Gimp.ImageProcedure.new(
            self,
            name,
            Gimp.PDBProcType.PLUGIN,
            self.run,
            None,
        )

        proc.set_image_types("RGB*, GRAY*, INDEXED*")
        proc.set_sensitivity_mask(
            Gimp.ProcedureSensitivityMask.DRAWABLE
            | Gimp.ProcedureSensitivityMask.DRAWABLES
        )
        proc.set_menu_label("_Puppet Warp (ARAP)...")
        proc.add_menu_path("<Image>/Filters/Deform")
        proc.add_menu_path("<Image>/Tools/Transform Tools")
        proc.set_attribution("Hexsian", "Hexsian", "2026")
        proc.set_documentation(
            "ARAP-based puppet warp. Non-destructive and re-editable. "
            "Isolate your selection to its own layer before warping.",
            "Click to add pins, drag to move, right-click to delete, "
            "double-click to reset a pin, drag the orange ring to rotate, "
            "Ctrl+wheel to scale. Enter applies, Esc cancels.",
            name,
        )

        return proc

    def run(self, procedure, run_mode, image, drawables, config, data):
        if len(drawables) != 1 or not isinstance(drawables[0], Gimp.Drawable):
            return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

        if run_mode == Gimp.RunMode.NONINTERACTIVE:
            return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

        try:
            ensure_op()
            GimpUi.init(PLUG_IN_BINARY)
        except Exception as e:
            traceback.print_exc()
            print(f"[puppet-warp] init failed: {e}")
            return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

        lock = _dialog_lock_acquire()

        if lock is None:
            Gimp.message(
                "Puppet Warp: the dialog is already open in another GIMP instance."
            )
            return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

        try:
            try:
                dlg = PuppetWarpDialog(image, drawables[0])
            except Exception as e:
                traceback.print_exc()
                Gimp.message("Puppet Warp: " + str(e))
                return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

            GimpUi.window_set_transient(dlg)

            resp = dlg.run()

            # Freeze final pad before reading JSON/plan.
            dlg.finalize_extend_pad()

            params_json = dlg.get_params_json()
            pin_count = len(dlg.pins)
            plan = dlg.get_resize_plan()
            drawable = drawables[0]

            dlg.destroy()

            if resp != Gtk.ResponseType.OK:
                return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

            if pin_count == 0:
                remove_filter_from_drawable(drawable)
                if plan and plan.get("needs_resize"):
                    if not _resize_drawable_plan(drawable, plan):
                        Gimp.message("Puppet Warp: could not restore the original layer size.")
                try:
                    Gimp.displays_flush()
                except Exception:
                    pass
                return procedure.new_return_values(Gimp.PDBStatusType.SUCCESS, None)

            # Resize before attaching the filter so GEGL sees the padded layer.
            if plan and plan.get("needs_resize"):
                remove_filter_from_drawable(drawable)
                if not _resize_drawable_plan(drawable, plan):
                    Gimp.message("Puppet Warp: could not resize the layer for Extend Image.")
                    return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

            # Verts ride in the JSON itself; no temp sidecar file.
            apply_filter_to_drawable(drawables[0], params_json)

            try:
                Gimp.displays_flush()
            except Exception:
                pass

            print(f"[puppet-warp] applied filter, pins={pin_count}")

            if _op_freshly_installed:
                Gimp.message(
                    "Puppet Warp: the GEGL operation was installed into "
                    f"{GEGL_USER_DIR}. Restart GIMP once so the filter can render."
                )

            return procedure.new_return_values(Gimp.PDBStatusType.SUCCESS, None)

        except Exception as e:
            traceback.print_exc()
            print(f"[puppet-warp] run failed: {e}")
            return procedure.new_return_values(Gimp.PDBStatusType.CANCEL, None)

        finally:
            _dialog_lock_release(lock)

if __name__ == "__main__":
    Gimp.main(PuppetWarpPlugin.__gtype__, sys.argv)
