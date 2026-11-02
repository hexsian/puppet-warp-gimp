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

MW_SO = os.path.join(GEGL_USER_DIR, _LIB_NAME)
MW_C = os.path.join(PLUGIN_DIR, "puppet-warp.c")
AGRID_C = os.path.join(PLUGIN_DIR, "arap_grid.c")
DIALOG_PID = os.path.join(GLib.get_tmp_dir(), "puppet-warp-dialog.pid")

MAX_PINS = 256

# Read once; debug prints in the draw path check this flag, not os.environ.
_PW_DEBUG = bool(os.environ.get("PW_DEBUG"))

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
    for prio, d in enumerate((GEGL_USER_DIR, PLUGIN_DIR)):
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
    
    try:
        os.makedirs(GEGL_USER_DIR, exist_ok=True)
    except Exception:
        return False
        
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

    for _name in (_LIB_NAME, "puppet-warp.so", "puppet-warp.dll"):
        _p = os.path.join(PLUGIN_DIR, _name)
        if os.path.exists(_p):
            try:
                os.unlink(_p)
            except Exception:
                pass

    _build_so()
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


def _pw_lib_handle():
    """Current ARAP library handle (module global lives here, not in callers)."""
    return _LIB


def _pw_op_fresh():
    return _op_freshly_installed
