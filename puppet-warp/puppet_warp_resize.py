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


from puppet_warp_native import (
    GEGL_OP_LABEL,
    GEGL_OP_NAME,
    _unpack_offsets,
)
def _pw_try(fn, *args):
    # GI methods report failure by returning False, not only by raising.
    try:
        res = fn(*args)
    except Exception:
        return False
    return res is not False


def _pw_has_resize_api(item):
    if getattr(item, "resize", None):
        return True

    for cls_name in ("Item", "Layer", "Drawable"):
        cls = getattr(Gimp, cls_name, None)
        if cls is not None and getattr(cls, "resize", None):
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
    # GIMP 3 exposes no programmatic "undo to group" call: Gimp.Image has
    # undo_group_start/end, undo_freeze/thaw, undo_enable/disable and nothing
    # that reverts. The resize is recorded inside one undo group, so the user
    # reverts it with a single Ctrl+Z. Nothing is rolled back in code.
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

    resized = False
    try:
        if not _pw_resize_item(drawable, nw, nh, ax, ay):
            raise RuntimeError("item resize failed")
        resized = True
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
                # No code-level rollback exists; the resize is one undo step.
                print(
                    "[pw-mask] layer resize did NOT auto-pad mask; "
                    "extend skipped. Press Ctrl+Z once to revert the resize."
                )
                return False

        return True

    except Exception as e:
        print(f"[puppet-warp] resize plan failed: {e}")
        if resized and image is not None:
            # Layer was resized but not repositioned; the group is one undo step.
            print("[puppet-warp] layer half-resized; press Ctrl+Z once to revert.")
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

    if hasattr(drawable, "append_filter"):
        drawable.append_filter(filt)
    elif hasattr(drawable, "add_filter"):
        drawable.add_filter(filt)

    try:
        drawable.update(0, 0, drawable.get_width(), drawable.get_height())
    except Exception:
        pass

    try:
        Gimp.displays_flush()
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
