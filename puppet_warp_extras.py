#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Guides, Snapping, Preview Opacity, Layers Above/Below, Snapshots, and C Viewport."""

import ctypes
import math
import os
import time
import gi

gi.require_version("Gimp", "3.0")
gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
gi.require_version("Gegl", "0.4")

from gi.repository import Gimp, Gtk, Gdk, Gegl

try:
    import cairo
except ImportError:
    from gi.repository import cairo

from puppet_warp_native import _pw_lib_handle, _unpack_offsets


class PuppetWarpExtrasMixin:
    """Mixin adding Guides, Snapping, Opacity, Layers Above/Below, Snapshots, and C Viewport."""

    def _free_mip(self, attr):
        ptr = getattr(self, attr, None)
        if ptr:
            try:
                lib = _pw_lib_handle()
                if lib is not None and hasattr(lib, "pw_mip_free"):
                    lib.pw_mip_free(ctypes.c_void_p(ptr))
            except Exception:
                pass
            setattr(self, attr, None)

    def _build_mip_for_surface(self, surf):
        if surf is None:
            return None
        w = surf.get_width()
        h = surf.get_height()
        if w * h <= 250_000:
            return None
        lib = _pw_lib_handle()
        if lib is None or not hasattr(lib, "pw_mip_new"):
            return None
        try:
            surf.flush()
            mv = surf.get_data()
            ptr = ctypes.addressof(ctypes.c_char.from_buffer(mv))
            return lib.pw_mip_new(
                ctypes.c_void_p(ptr),
                ctypes.c_int(w),
                ctypes.c_int(h),
                ctypes.c_int(surf.get_stride()),
            )
        except Exception:
            return None

    def _init_extras(self, prev):
        self._updating_extras = False

        self._vp_surface = None
        self._vp_key = None
        self._mesh_path_cache = None

        self._free_mip("_src_mip")
        self._src_mip = self._build_mip_for_surface(getattr(self, "src_surface", None))

        if getattr(self, "_bg_surface", None) is not None and getattr(self, "_bg_mip", None) is None:
            self._bg_mip = self._build_mip_for_surface(self._bg_surface)

        self.preview_opacity = 1.0
        self._fg_keys = ("off", "above", "composite")
        self._fg_surface = None
        self._fg_mip = None
        self._fg_ver = 0
        self._fg_off_x = 0.0
        self._fg_off_y = 0.0
        self._fg_alpha = 1.0
        self._fg_bounds = None

        if prev:
            fm = str(prev.get("fg_mode", "")).strip().lower()
            if fm not in self._fg_keys:
                if (
                    str(prev.get("bg_mode", "")).strip().lower() == "full"
                    or bool(prev.get("fg_on", False))
                ):
                    fm = "composite"
                else:
                    fm = "off"
            self.fg_mode = fm
        else:
            self.fg_mode = "off"

        self.guides_on = bool(prev.get("guides_on", True)) if prev else True
        self.align_grid_on = bool(prev.get("align_grid_on", False)) if prev else False

        default_step = 50
        try:
            if self.image is not None and hasattr(self.image, "grid_get_spacing"):
                sp = self.image.grid_get_spacing()
                if isinstance(sp, (list, tuple)) and len(sp) >= 2:
                    val = float(sp[-2] if isinstance(sp[0], bool) else sp[0])
                    if 4.0 <= val <= 512.0:
                        default_step = int(round(val))
        except Exception:
            pass

        self.align_grid_step = (
            max(4, min(512, int(prev.get("align_grid_step", default_step))))
            if prev
            else default_step
        )

        self.snap_on = bool(prev.get("snap_on", False)) if prev else False
        self.snap_guides = True
        self.snap_grid = True
        self.snap_pins = True

        self._gimp_guides = []
        self._local_guides = []
        self._load_gimp_guides()

        if prev and isinstance(prev.get("guides_local"), list):
            for item in prev.get("guides_local"):
                if isinstance(item, (list, tuple)) and len(item) >= 2:
                    ori = str(item[0]).upper()
                    if ori in ("H", "V"):
                        try:
                            pos = float(item[1])
                            if math.isfinite(pos):
                                self._local_guides.append([ori, pos])
                        except Exception:
                            pass

        self._dragging_guide = None
        self._hovered_guide = None
        self._active_snap_guides = (None, None)

        self._snapshots = {"A": None, "B": None, "C": None}
        self._last_snap_slot = None
        self._compare_ab_on = False
        self._compare_orig_on = False
        self._compare_live_backup = None
        self._compare_peeking_slot = None
        self.export_snaps_on = False

        self._refresh_fg_preview()

    # ------------------------------------------------------------------
    # C 1:1 Viewport Compositor (Checker + Below + Warped/Orig + Above)
    # ------------------------------------------------------------------

    def _vp_render(self, cr, dw, dh):
        lib = _pw_lib_handle()
        if (
            lib is None
            or not hasattr(lib, "pw_render_viewport")
            or os.environ.get("PW_NO_C_PREVIEW") == "1"
        ):
            return False

        if dw <= 0 or dh <= 0 or not (self.zoom > 0.0):
            return False

        orig_mode = bool(getattr(self, "_compare_orig_on", False))
        nverts = (self.cols + 1) * (self.rows + 1)
        raw = None

        if not orig_mode:
            if (
                self._verts_raw is not None
                and self._verts_raw_n == nverts
                and self._verts_raw_seq == self._solve_seq
            ):
                raw = self._verts_raw
            else:
                raw = self._ensure_verts_raw(self._get_deformed_verts(), nverts)

            if raw is None:
                return False

        depth_on = (not orig_mode) and self._depth_on()
        dkey = tuple(int(p.depth) for p in self.pins) if depth_on else ()
        op = float(getattr(self, "preview_opacity", 1.0))

        vp_key = (
            dw,
            dh,
            round(self.pan_x, 3),
            round(self.pan_y, 3),
            round(self.zoom, 6),
            0 if orig_mode else self._solve_seq,
            self.cols,
            self.rows,
            self.canvas_w,
            self.canvas_h,
            bool(getattr(self, "extend_on", False)),
            round(op, 4),
            orig_mode,
            dkey,
            getattr(self, "_bg_ver", 0),
            getattr(self, "_fg_ver", 0),
        )

        vp_surf = getattr(self, "_vp_surface", None)
        if (
            getattr(self, "_vp_key", None) == vp_key
            and vp_surf is not None
            and vp_surf.get_width() == dw
            and vp_surf.get_height() == dh
        ):
            cr.set_source_surface(vp_surf, 0, 0)
            cr.paint()
            return True

        if vp_surf is None or vp_surf.get_width() != dw or vp_surf.get_height() != dh:
            if vp_surf is not None:
                try:
                    vp_surf.finish()
                except Exception:
                    pass
            vp_surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, dw, dh)
            self._vp_surface = vp_surf

        try:
            vp_surf.flush()
            vp_mv = vp_surf.get_data()
            vp_ptr = ctypes.addressof(ctypes.c_char.from_buffer(vp_mv))

            src_mv = self.src_surface.get_data()
            src_ptr = ctypes.addressof(ctypes.c_char.from_buffer(src_mv))

            bg_surf = getattr(self, "_bg_surface", None)
            if bg_surf is not None:
                bg_mv = bg_surf.get_data()
                bg_ptr = ctypes.addressof(ctypes.c_char.from_buffer(bg_mv))
                bg_w = bg_surf.get_width()
                bg_h = bg_surf.get_height()
                bg_stride = bg_surf.get_stride()
                bg_mip = getattr(self, "_bg_mip", None) or 0
                bg_ox = float(getattr(self, "_bg_off_x", 0.0))
                bg_oy = float(getattr(self, "_bg_off_y", 0.0))
                bg_alpha = float(getattr(self, "_bg_alpha", 1.0))
            else:
                bg_mv = None
                bg_ptr = 0
                bg_w = bg_h = bg_stride = 0
                bg_mip = 0
                bg_ox = bg_oy = bg_alpha = 0.0

            fg_surf = getattr(self, "_fg_surface", None)
            if fg_surf is not None:
                fg_mv = fg_surf.get_data()
                fg_ptr = ctypes.addressof(ctypes.c_char.from_buffer(fg_mv))
                fg_w = fg_surf.get_width()
                fg_h = fg_surf.get_height()
                fg_stride = fg_surf.get_stride()
                fg_mip = getattr(self, "_fg_mip", None) or 0
                fg_ox = float(getattr(self, "_fg_off_x", 0.0))
                fg_oy = float(getattr(self, "_fg_off_y", 0.0))
                fg_alpha = float(getattr(self, "_fg_alpha", 1.0))
            else:
                fg_mv = None
                fg_ptr = 0
                fg_w = fg_h = fg_stride = 0
                fg_mip = 0
                fg_ox = fg_oy = fg_alpha = 0.0

            if depth_on:
                vd = self._compute_vertex_depths()
                if len(vd) != nverts:
                    return False
                varr = (ctypes.c_int * nverts)(*vd)
                vdepth_ptr = ctypes.cast(varr, ctypes.POINTER(ctypes.c_int))
            else:
                varr = None
                vdepth_ptr = ctypes.POINTER(ctypes.c_int)()

            if not orig_mode:
                active_arr = self.cell_active_payload(self.cols, self.rows)
                active_ptr = ctypes.cast(active_arr, ctypes.POINTER(ctypes.c_uint8))
                verts_ptr = ctypes.cast(raw, ctypes.POINTER(ctypes.c_float))
                nverts_arg = nverts
            else:
                active_arr = None
                active_ptr = ctypes.POINTER(ctypes.c_uint8)()
                verts_ptr = ctypes.POINTER(ctypes.c_float)()
                nverts_arg = 0

            t0 = time.monotonic()
            ret = lib.pw_render_viewport(
                ctypes.c_void_p(vp_ptr),
                ctypes.c_int(dw),
                ctypes.c_int(dh),
                ctypes.c_int(vp_surf.get_stride()),
                ctypes.c_float(self.pan_x),
                ctypes.c_float(self.pan_y),
                ctypes.c_float(self.zoom),
                ctypes.c_void_p(src_ptr),
                ctypes.c_int(self.canvas_w),
                ctypes.c_int(self.canvas_h),
                ctypes.c_int(self.src_surface.get_stride()),
                ctypes.c_void_p(getattr(self, "_src_mip", None) or 0),
                ctypes.c_int(self.cols),
                ctypes.c_int(self.rows),
                verts_ptr,
                ctypes.c_int(nverts_arg),
                ctypes.c_float(self.puppet_x),
                ctypes.c_float(self.puppet_y),
                ctypes.c_float(self.puppet_w),
                ctypes.c_float(self.puppet_h),
                ctypes.c_int(1 if bool(getattr(self, "extend_on", False)) else 0),
                ctypes.c_float(op),
                ctypes.c_int(1 if orig_mode else 0),
                ctypes.c_int(1 if depth_on else 0),
                vdepth_ptr,
                active_ptr,
                ctypes.c_void_p(bg_ptr),
                ctypes.c_int(bg_w),
                ctypes.c_int(bg_h),
                ctypes.c_int(bg_stride),
                ctypes.c_void_p(bg_mip),
                ctypes.c_float(bg_ox),
                ctypes.c_float(bg_oy),
                ctypes.c_float(bg_alpha),
                ctypes.c_void_p(fg_ptr),
                ctypes.c_int(fg_w),
                ctypes.c_int(fg_h),
                ctypes.c_int(fg_stride),
                ctypes.c_void_p(fg_mip),
                ctypes.c_float(fg_ox),
                ctypes.c_float(fg_oy),
                ctypes.c_float(fg_alpha),
            )

            rt = (time.monotonic() - t0) * 1000.0
            self._render_ms = 0.65 * getattr(self, "_render_ms", 0.0) + 0.35 * max(1.0, rt)

            if ret <= 0:
                return False

            vp_surf.mark_dirty()
            self._vp_key = vp_key

            cr.set_source_surface(vp_surf, 0, 0)
            cr.paint()

            if os.environ.get("PW_DEBUG"):
                print(
                    "[pw-c-vp] draw=", ret,
                    "vp=", dw, "x", dh,
                    "zoom=", round(self.zoom, 3),
                    "ms=", round(rt, 2),
                )

            return True

        except Exception as e:
            print(f"[puppet-warp] c-viewport failed, fallback python: {e}")
            return False

    # ------------------------------------------------------------------
    # GIMP & Local Guides
    # ------------------------------------------------------------------

    def _load_gimp_guides(self):
        self._gimp_guides = []
        if self.image is None or not hasattr(self.image, "find_next_guide"):
            return

        ox = float(getattr(self, "_bg_ox", 0))
        oy = float(getattr(self, "_bg_oy", 0))
        h_enum = getattr(Gimp.OrientationType, "HORIZONTAL", None)
        v_enum = getattr(Gimp.OrientationType, "VERTICAL", None)

        try:
            gid = self.image.find_next_guide(0)
            seen = set()
            while gid and gid not in seen:
                seen.add(gid)
                ori = self.image.get_guide_orientation(gid)
                pos = float(self.image.get_guide_position(gid))
                if ori == h_enum:
                    self._gimp_guides.append(("H", pos - oy))
                elif ori == v_enum:
                    self._gimp_guides.append(("V", pos - ox))
                gid = self.image.find_next_guide(gid)
        except Exception as e:
            print(f"[puppet-warp] guides unavailable ({e})")

    def _add_local_guide(self, ori):
        a = self.drawing_area.get_allocation()
        dw = a.width if a.width > 50 else 800
        dh = a.height if a.height > 50 else 600
        cx, cy = self._screen_to_world(dw * 0.5, dh * 0.5)
        pos = round(cy if ori == "H" else cx)
        self._local_guides.append([ori, float(pos)])
        if not self.guides_on:
            self.guides_on = True
            self._sync_extras_ui()
        self.drawing_area.queue_draw()

    def _clear_local_guides(self):
        self._local_guides = []
        self._dragging_guide = None
        self._hovered_guide = None
        self.drawing_area.queue_draw()

    def _guide_handle_hit(self, sx, sy):
        if not getattr(self, "guides_on", True) or not self._local_guides:
            return None
        for idx, (ori, pos) in enumerate(self._local_guides):
            if ori == "V":
                gsx, _ = self._world_to_screen(pos, 0.0)
                if sy <= 18.0 and abs(sx - gsx) <= 9.0:
                    return idx
            else:
                _, gsy = self._world_to_screen(0.0, pos)
                if sx <= 18.0 and abs(sy - gsy) <= 9.0:
                    return idx
        return None

    def _guide_line_hit(self, sx, sy):
        if not getattr(self, "guides_on", True) or not self._local_guides:
            return None
        best = None
        bd = 8.0
        for idx, (ori, pos) in enumerate(self._local_guides):
            if ori == "V":
                gsx, _ = self._world_to_screen(pos, 0.0)
                d = abs(sx - gsx)
            else:
                _, gsy = self._world_to_screen(0.0, pos)
                d = abs(sy - gsy)
            if d <= bd:
                bd = d
                best = idx
        return best

    # ------------------------------------------------------------------
    # Snapping
    # ------------------------------------------------------------------

    def _snap_world(self, wx, wy, exclude=None, lock_x=False, lock_y=False):
        if not getattr(self, "snap_on", False):
            self._active_snap_guides = (None, None)
            return wx, wy

        thresh = 10.0 / max(0.05, self.zoom)
        exclude = exclude or set()

        best_dx = thresh
        best_x = None
        snap_vx = None

        best_dy = thresh
        best_y = None
        snap_hy = None

        if not lock_x:
            if self.snap_guides and self.guides_on:
                for ori, pos in self._gimp_guides:
                    if ori == "V":
                        d = abs(wx - pos)
                        if d <= best_dx:
                            best_dx = d
                            best_x = pos
                            snap_vx = pos
                for ori, pos in self._local_guides:
                    if ori == "V":
                        d = abs(wx - pos)
                        if d <= best_dx:
                            best_dx = d
                            best_x = pos
                            snap_vx = pos

            if self.snap_pins:
                for i, p in enumerate(self.pins):
                    if i in exclude:
                        d0 = abs(wx - p.sx)
                        if d0 < best_dx:
                            best_dx = d0
                            best_x = p.sx
                            snap_vx = p.sx
                        continue
                    d = abs(wx - p.dx)
                    if d < best_dx:
                        best_dx = d
                        best_x = p.dx
                        snap_vx = p.dx

            if self.snap_grid and self.align_grid_on:
                step = max(4.0, float(self.align_grid_step))
                gx = round(wx / step) * step
                dg = abs(wx - gx)
                if dg < best_dx:
                    best_dx = dg
                    best_x = gx
                    snap_vx = gx

        if not lock_y:
            if self.snap_guides and self.guides_on:
                for ori, pos in self._gimp_guides:
                    if ori == "H":
                        d = abs(wy - pos)
                        if d <= best_dy:
                            best_dy = d
                            best_y = pos
                            snap_hy = pos
                for ori, pos in self._local_guides:
                    if ori == "H":
                        d = abs(wy - pos)
                        if d <= best_dy:
                            best_dy = d
                            best_y = pos
                            snap_hy = pos

            if self.snap_pins:
                for i, p in enumerate(self.pins):
                    if i in exclude:
                        d0 = abs(wy - p.sy)
                        if d0 < best_dy:
                            best_dy = d0
                            best_y = p.sy
                            snap_hy = p.sy
                        continue
                    d = abs(wy - p.dy)
                    if d < best_dy:
                        best_dy = d
                        best_y = p.dy
                        snap_hy = p.dy

            if self.snap_grid and self.align_grid_on:
                step = max(4.0, float(self.align_grid_step))
                gy = round(wy / step) * step
                dg = abs(wy - gy)
                if dg < best_dy:
                    best_dy = dg
                    best_y = gy
                    snap_hy = gy

        out_x = best_x if best_x is not None else wx
        out_y = best_y if best_y is not None else wy
        self._active_snap_guides = (snap_vx, snap_hy)
        return out_x, out_y

    # ------------------------------------------------------------------
    # Layers Above Preview (Off / Nearest / Composite)
    # ------------------------------------------------------------------

    def _fg_index(self):
        keys = getattr(self, "_fg_keys", ("off", "above", "composite"))
        mode = getattr(self, "fg_mode", "off")
        try:
            return keys.index(mode)
        except Exception:
            return 0

    def _refresh_fg_preview(self):
        self._free_mip("_fg_mip")
        old_fg = getattr(self, "_fg_surface", None)
        if old_fg is not None:
            try:
                old_fg.finish()
            except Exception:
                pass
        self._fg_surface = None
        self._fg_off_x = 0.0
        self._fg_off_y = 0.0
        self._fg_alpha = 1.0
        self._fg_bounds = None
        self._fg_ver = getattr(self, "_fg_ver", 0) + 1

        keys = getattr(self, "_fg_keys", ("off", "above", "composite"))
        mode = getattr(self, "fg_mode", "off")
        if mode not in keys:
            mode = "off"
            self.fg_mode = mode

        if os.environ.get("PW_NO_BG") == "1" or mode == "off":
            da = getattr(self, "drawing_area", None)
            if da is not None:
                da.queue_draw()
            return

        layers = getattr(self, "_bg_layers", None)
        pos = getattr(self, "_bg_pos", None)
        if layers is None or pos is None or pos <= 0:
            da = getattr(self, "drawing_area", None)
            if da is not None:
                da.queue_draw()
            return

        ox = getattr(self, "_bg_ox", 0)
        oy = getattr(self, "_bg_oy", 0)

        if mode == "composite":
            self._build_fg_composite(layers, pos, ox, oy)
        else:
            self._build_fg_above(layers, pos, ox, oy)

        if self._fg_surface is not None:
            self._fg_mip = self._build_mip_for_surface(self._fg_surface)

        da = getattr(self, "drawing_area", None)
        if da is not None:
            da.queue_draw()

    def _build_fg_above(self, layers, pos, ox, oy):
        above_idx = None
        for i in range(pos - 1, -1, -1):
            try:
                if layers[i].get_visible():
                    above_idx = i
                    break
            except Exception:
                continue

        if above_idx is None:
            return

        try:
            al = layers[above_idx]
            lx, ly = _unpack_offsets(al.get_offsets())
            lw, lh = al.get_width(), al.get_height()
            op = max(0.0, min(1.0, al.get_opacity() / 100.0))

            if lw <= 0 or lh <= 0 or op <= 0.0:
                return

            araw = bytes(
                al.get_buffer().get(
                    Gegl.Rectangle.new(0, 0, lw, lh),
                    1.0,
                    "cairo-ARGB32",
                    Gegl.AbyssPolicy.NONE,
                )
            )

            surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, lw, lh)
            surf.get_data()[:] = araw
            del araw
            surf.mark_dirty()

            self._fg_surface = surf
            self._fg_off_x = float(lx - ox)
            self._fg_off_y = float(ly - oy)
            self._fg_alpha = op
            self._fg_bounds = (
                float(self._fg_off_x),
                float(self._fg_off_y),
                float(lw),
                float(lh),
            )
        except Exception as e:
            print(f"[puppet-warp] layers above (nearest) unavailable ({e})")
            self._fg_surface = None

    def _build_fg_composite(self, layers, pos, ox, oy):
        items = []
        minx = 1e18
        miny = 1e18
        maxx = -1e18
        maxy = -1e18

        for i in range(0, pos):
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

        MAX_DIM = 8192
        MAX_PIX = 64_000_000
        if w > MAX_DIM or h > MAX_DIM or w * h > MAX_PIX:
            self._build_fg_above(layers, pos, ox, oy)
            return

        try:
            surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
            ctx = cairo.Context(surf)
            for l, x, y, lw, lh, op in reversed(items):
                ls = None
                raw = None
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
                    del raw
                    raw = None
                    ls.mark_dirty()

                    ctx.save()
                    ctx.translate(x - minx, y - miny)
                    ctx.set_source_surface(ls, 0, 0)
                    ctx.paint_with_alpha(op)
                    ctx.restore()
                except Exception:
                    continue
                finally:
                    raw = None
                    if ls is not None:
                        try:
                            ls.finish()
                        except Exception:
                            pass

            surf.flush()
            self._fg_surface = surf
            self._fg_off_x = minx
            self._fg_off_y = miny
            self._fg_alpha = 1.0
            self._fg_bounds = (float(minx), float(miny), float(w), float(h))
        except Exception as e:
            print(f"[puppet-warp] layers above composite unavailable ({e})")
            self._fg_surface = None

    def _on_fg_mode_changed(self, combo):
        if getattr(self, "_updating_extras", False):
            return
        keys = getattr(self, "_fg_keys", ("off", "above", "composite"))
        idx = combo.get_active()
        if 0 <= idx < len(keys):
            self.fg_mode = keys[idx]
        self._refresh_fg_preview()

    # ------------------------------------------------------------------
    # Snapshots, A-B-C Compare & Export
    # ------------------------------------------------------------------

    def _snapshot_capture(self):
        return {
            "pins": self._snap(),
            "cols": int(self.cols),
            "rows": int(self.rows),
            "pin_edge": bool(self.pin_edge),
            "rotate_on": bool(self.rotate_on),
            "use_stiffness": bool(self.use_stiffness),
            "deform_mode": str(getattr(self, "deform_mode", "normal")),
        }

    def _snapshot_apply_state(self, st, push_undo=True):
        if not st:
            return
        if push_undo:
            self._push_undo()

        self.cols = max(2, min(64, int(st.get("cols", self.cols))))
        self.rows = max(2, min(64, int(st.get("rows", self.rows))))
        self.pin_edge = bool(st.get("pin_edge", self.pin_edge))
        self.rotate_on = bool(st.get("rotate_on", self.rotate_on))
        self.use_stiffness = bool(st.get("use_stiffness", self.use_stiffness))

        dm = str(st.get("deform_mode", getattr(self, "deform_mode", "normal"))).strip().lower()
        mode_keys = getattr(self, "_deform_keys", ("normal", "rigid", "distort", "organic", "hybrid"))
        if dm not in mode_keys:
            dm = "normal"
        self.deform_mode = dm

        self._set_blocked(
            getattr(self, "cols_spin", None),
            getattr(self, "_cols_id", None),
            lambda v: self.cols_spin.set_value(v),
            float(self.cols),
        )
        self._set_blocked(
            getattr(self, "rows_spin", None),
            getattr(self, "_rows_id", None),
            lambda v: self.rows_spin.set_value(v),
            float(self.rows),
        )
        self._set_blocked(
            getattr(self, "pin_edge_check", None),
            getattr(self, "_pin_edge_id", None),
            lambda v: self.pin_edge_check.set_active(v),
            self.pin_edge,
        )
        self._set_blocked(
            getattr(self, "rotate_check", None),
            getattr(self, "_rotate_id", None),
            lambda v: self.rotate_check.set_active(v),
            self.rotate_on,
        )
        self._set_blocked(
            getattr(self, "stiff_check", None),
            getattr(self, "_stiff_id", None),
            lambda v: self.stiff_check.set_active(v),
            self.use_stiffness,
        )
        self._set_blocked(
            getattr(self, "deform_combo", None),
            getattr(self, "_deform_id", None),
            lambda v: self.deform_combo.set_active(v),
            mode_keys.index(dm),
        )

        self._restore_snapshot(st["pins"])

    def _exit_compare_modes(self, keep_shown=False):
        if getattr(self, "_compare_orig_on", False):
            self._compare_orig_on = False
            btn = getattr(self, "compare_orig_btn", None)
            if btn is not None:
                self._set_blocked(
                    btn, getattr(self, "_compare_orig_id", None),
                    lambda v: btn.set_active(v), False
                )
        if getattr(self, "_compare_ab_on", False):
            if keep_shown and self._compare_live_backup is not None:
                self._push_undo(self._compare_live_backup)
            elif not keep_shown and self._compare_live_backup is not None:
                self._snapshot_apply_state(self._compare_live_backup, push_undo=False)
            self._compare_ab_on = False
            self._compare_live_backup = None
            self._compare_peeking_slot = None
            btn = getattr(self, "compare_ab_btn", None)
            if btn is not None:
                self._set_blocked(
                    btn, getattr(self, "_compare_ab_id", None),
                    lambda v: btn.set_active(v), False
                )

    def _sync_snapshot_buttons(self):
        btns = getattr(self, "_snap_btns", None)
        if not btns:
            return
        for k, btn in btns.items():
            has = self._snapshots.get(k) is not None
            btn.set_label(f"{k}●" if has else k)
        has_any = any(v is not None for v in self._snapshots.values())
        ab = getattr(self, "compare_ab_btn", None)
        if ab is not None:
            ab.set_sensitive(has_any)
        cyc = getattr(self, "compare_cycle_btn", None)
        if cyc is not None:
            cyc.set_sensitive(has_any)
        exp = getattr(self, "export_snaps_chk", None)
        if exp is not None:
            exp.set_sensitive(has_any)
            if not has_any and exp.get_active():
                exp.set_active(False)
                self.export_snaps_on = False

    def _on_snapshot_btn_press(self, btn, e, slot):
        st = e.state
        shift = bool(st & Gdk.ModifierType.SHIFT_MASK)
        alt = bool(st & Gdk.ModifierType.MOD1_MASK)

        if e.button == 2 or (e.button == 1 and alt):
            if self._compare_ab_on and self._compare_peeking_slot == slot:
                self._exit_compare_modes(keep_shown=False)
            self._snapshots[slot] = None
            if self._last_snap_slot == slot:
                self._last_snap_slot = None
            self._sync_snapshot_buttons()
            return True

        if e.button == 3 or (e.button == 1 and shift):
            if self._compare_orig_on:
                self._exit_compare_modes(keep_shown=False)
            self._snapshots[slot] = self._snapshot_capture()
            self._last_snap_slot = slot
            self._sync_snapshot_buttons()
            self.status_label.set_label(f"Snapshot {slot} updated.")
            return True

        if e.button == 1:
            if self._snapshots[slot] is None:
                if self._compare_orig_on:
                    self._exit_compare_modes(keep_shown=False)
                self._snapshots[slot] = self._snapshot_capture()
                self._last_snap_slot = slot
                self._sync_snapshot_buttons()
                self.status_label.set_label(f"Saved current state to Snapshot {slot}.")
            else:
                self._exit_compare_modes(keep_shown=False)
                self._snapshot_apply_state(self._snapshots[slot], push_undo=True)
                self._last_snap_slot = slot
                self.status_label.set_label(f"Loaded Snapshot {slot} (Ctrl+Z to undo).")
            return True

        return False

    def _on_compare_ab_btn_press(self, btn, e):
        if e.button == 3:
            self._on_compare_cycle()
            return True
        return False

    def _on_compare_cycle(self, widget=None):
        avail = [k for k in ("A", "B", "C") if self._snapshots.get(k) is not None]
        if not avail:
            return False

        if self._compare_orig_on:
            self._compare_orig_on = False
            ob = getattr(self, "compare_orig_btn", None)
            if ob is not None:
                self._set_blocked(
                    ob, getattr(self, "_compare_orig_id", None),
                    lambda v: ob.set_active(v), False
                )

        if not self._compare_ab_on:
            live = self._snapshot_capture()
            self._compare_live_backup = live

            target = avail[0]
            if len(avail) >= 2 and live == self._snapshots[target]:
                target = avail[1]

            self._compare_ab_on = True
            self._compare_peeking_slot = target
            ab = getattr(self, "compare_ab_btn", None)
            if ab is not None:
                self._set_blocked(
                    ab, getattr(self, "_compare_ab_id", None),
                    lambda v: ab.set_active(v), True
                )
            self._snapshot_apply_state(self._snapshots[target], push_undo=False)
            return True

        cur = getattr(self, "_compare_peeking_slot", None)
        if cur in avail:
            idx = avail.index(cur)
            if idx + 1 < len(avail):
                target = avail[idx + 1]
                self._compare_peeking_slot = target
                self._snapshot_apply_state(self._snapshots[target], push_undo=False)
                return True

        self._exit_compare_modes(keep_shown=False)
        return True

    def _on_compare_ab_toggled(self, btn):
        if getattr(self, "_updating_extras", False):
            return

        if btn.get_active():
            avail = [k for k in ("A", "B", "C") if self._snapshots.get(k) is not None]
            if not avail:
                self._set_blocked(
                    btn, getattr(self, "_compare_ab_id", None),
                    lambda v: btn.set_active(v), False
                )
                return

            if self._compare_orig_on:
                self._compare_orig_on = False
                ob = getattr(self, "compare_orig_btn", None)
                if ob is not None:
                    self._set_blocked(
                        ob, getattr(self, "_compare_orig_id", None),
                        lambda v: ob.set_active(v), False
                    )

            live = self._snapshot_capture()
            self._compare_live_backup = live

            target = self._last_snap_slot if self._last_snap_slot in avail else avail[0]
            if len(avail) >= 2 and live == self._snapshots[target]:
                others = [k for k in avail if k != target]
                target = others[0]

            self._compare_ab_on = True
            self._compare_peeking_slot = target
            self._snapshot_apply_state(self._snapshots[target], push_undo=False)
        else:
            self._compare_ab_on = False
            self._compare_peeking_slot = None
            if self._compare_live_backup is not None:
                bk = self._compare_live_backup
                self._compare_live_backup = None
                self._snapshot_apply_state(bk, push_undo=False)

    def _on_compare_orig_toggled(self, btn):
        if getattr(self, "_updating_extras", False):
            return
        self._compare_orig_on = bool(btn.get_active())
        self.drawing_area.queue_draw()

    def _on_export_snaps_toggled(self, btn):
        self.export_snaps_on = bool(btn.get_active())

    # ------------------------------------------------------------------
    # Extras UI Construction & Sync
    # ------------------------------------------------------------------

    def _build_extras_ui(self, ca, row2, options_box, sep, small_spin):
        row2.pack_start(sep(), False, False, 0)
        row2.pack_start(Gtk.Label(label="Opacity"), False, False, 0)

        self.opacity_scale = Gtk.Scale.new_with_range(
            Gtk.Orientation.HORIZONTAL, 0.0, 100.0, 1.0
        )
        self.opacity_scale.set_draw_value(False)
        self.opacity_scale.set_value(100.0)
        self.opacity_scale.set_size_request(90, -1)
        self.opacity_scale.set_tooltip_text(
            "Preview opacity (0-100%) to align over layers below/guides. "
            "Does not change final layer opacity."
        )
        self.opacity_scale.connect("value-changed", self._on_opacity_changed)
        row2.pack_start(self.opacity_scale, False, False, 0)

        self.opacity_val_label = Gtk.Label(label="100%")
        self.opacity_val_label.set_width_chars(4)
        row2.pack_start(self.opacity_val_label, False, False, 0)

        row2.pack_start(sep(), False, False, 0)

        gs_btn = Gtk.MenuButton()
        gs_btn.set_label("Guides/Snap")
        gs_btn.set_tooltip_text("Add/clear guides, alignment grid step, snapping options.")

        gs_pop = Gtk.Popover.new(gs_btn)
        gs_pop.set_position(Gtk.PositionType.BOTTOM)
        gs_pop.set_modal(True)
        try:
            gs_pop.get_style_context().add_class("pw-compact")
        except Exception:
            pass

        gs_box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        gs_box.set_border_width(6)

        g_btns = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)
        add_h = Gtk.Button(label="+ H Guide")
        add_h.connect("clicked", lambda b: self._add_local_guide("H"))
        g_btns.pack_start(add_h, False, False, 0)

        add_v = Gtk.Button(label="+ V Guide")
        add_v.connect("clicked", lambda b: self._add_local_guide("V"))
        g_btns.pack_start(add_v, False, False, 0)

        clr_g = Gtk.Button(label="Clear Local")
        clr_g.connect("clicked", lambda b: self._clear_local_guides())
        g_btns.pack_start(clr_g, False, False, 0)
        gs_box.pack_start(g_btns, False, False, 0)

        reload_g = Gtk.Button(label="Reload GIMP Guides")
        reload_g.connect(
            "clicked",
            lambda b: (self._load_gimp_guides(), self.drawing_area.queue_draw()),
        )
        gs_box.pack_start(reload_g, False, False, 0)

        gs_box.pack_start(
            Gtk.Separator(orientation=Gtk.Orientation.HORIZONTAL), False, False, 2
        )

        step_row = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)
        step_row.pack_start(Gtk.Label(label="Align Grid Step"), False, False, 0)
        self.align_step_spin = small_spin(4, 512, 2, 0, 4)
        self.align_step_spin.set_value(float(self.align_grid_step))
        self.align_step_spin.connect("value-changed", self._on_align_step_changed)
        step_row.pack_start(self.align_step_spin, False, False, 0)
        step_row.pack_start(Gtk.Label(label="px"), False, False, 0)
        gs_box.pack_start(step_row, False, False, 0)

        self.snap_guides_chk = Gtk.CheckButton(label="Snap to Guides")
        self.snap_guides_chk.set_active(self.snap_guides)
        self.snap_guides_chk.connect(
            "toggled", lambda b: setattr(self, "snap_guides", b.get_active())
        )
        gs_box.pack_start(self.snap_guides_chk, False, False, 0)

        self.snap_grid_chk = Gtk.CheckButton(label="Snap to Align Grid")
        self.snap_grid_chk.set_active(self.snap_grid)
        self.snap_grid_chk.connect(
            "toggled", lambda b: setattr(self, "snap_grid", b.get_active())
        )
        gs_box.pack_start(self.snap_grid_chk, False, False, 0)

        self.snap_pins_chk = Gtk.CheckButton(label="Smart Pin Alignment")
        self.snap_pins_chk.set_active(self.snap_pins)
        self.snap_pins_chk.connect(
            "toggled", lambda b: setattr(self, "snap_pins", b.get_active())
        )
        gs_box.pack_start(self.snap_pins_chk, False, False, 0)

        gs_box.show_all()
        gs_pop.add(gs_box)
        gs_btn.set_popover(gs_pop)
        row2.pack_start(gs_btn, False, False, 0)

        row2b = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=3)
        row2b.pack_start(Gtk.Label(label="Snap"), False, False, 0)

        self._snap_btns = {}
        for slot in ("A", "B", "C"):
            b = Gtk.Button(label=slot)
            b.set_tooltip_text(
                f"Slot {slot}: Click = Save/Load · Shift+Click or Right-Click = Overwrite · "
                "Alt+Click = Clear"
            )
            b.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
            b.connect("button-press-event", self._on_snapshot_btn_press, slot)
            row2b.pack_start(b, False, False, 0)
            self._snap_btns[slot] = b

        self.compare_ab_btn = Gtk.ToggleButton(label="A⇄B⇄C")
        self.compare_ab_btn.set_tooltip_text(
            "Toggle compare vs saved snapshot (Right-Click to cycle A → B → C → Live)."
        )
        self.compare_ab_btn.add_events(Gdk.EventMask.BUTTON_PRESS_MASK)
        self.compare_ab_btn.connect("button-press-event", self._on_compare_ab_btn_press)
        self._compare_ab_id = self.compare_ab_btn.connect(
            "toggled", self._on_compare_ab_toggled
        )
        row2b.pack_start(self.compare_ab_btn, False, False, 0)

        self.compare_cycle_btn = Gtk.Button(label="→")
        self.compare_cycle_btn.set_tooltip_text(
            "Cycle through saved snapshots: Live → A → B → C → Live (Key: `)."
        )
        self.compare_cycle_btn.connect("clicked", self._on_compare_cycle)
        row2b.pack_start(self.compare_cycle_btn, False, False, 0)

        self.compare_orig_btn = Gtk.ToggleButton(label="Orig")
        self.compare_orig_btn.set_tooltip_text(
            "Before/After toggle against unwarped original (Key: \\)."
        )
        self._compare_orig_id = self.compare_orig_btn.connect(
            "toggled", self._on_compare_orig_toggled
        )
        row2b.pack_start(self.compare_orig_btn, False, False, 0)

        self.export_snaps_chk = Gtk.CheckButton(label="Export A-C")
        self.export_snaps_chk.set_active(False)
        self.export_snaps_chk.set_tooltip_text(
            "When pressing OK, also export saved snapshots (A, B, C) as new non-destructive layers."
        )
        self.export_snaps_chk.connect("toggled", self._on_export_snaps_toggled)
        row2b.pack_start(self.export_snaps_chk, False, False, 0)

        row2b.pack_start(sep(), False, False, 0)

        self.snap_toggle_btn = Gtk.ToggleButton(label="Snap")
        self.snap_toggle_btn.set_active(self.snap_on)
        self.snap_toggle_btn.set_tooltip_text("Snap pins to guides, grid, and other pins.")
        self._snap_btn_id = self.snap_toggle_btn.connect(
            "toggled", self._on_snap_toggled
        )
        row2b.pack_start(self.snap_toggle_btn, False, False, 0)

        self.align_grid_btn = Gtk.ToggleButton(label="Align Grid")
        self.align_grid_btn.set_active(self.align_grid_on)
        self.align_grid_btn.set_tooltip_text("Show/hide alignment reference grid.")
        self._align_grid_id = self.align_grid_btn.connect(
            "toggled", self._on_align_grid_toggled
        )
        row2b.pack_start(self.align_grid_btn, False, False, 0)

        self.guides_btn = Gtk.ToggleButton(label="Guides")
        self.guides_btn.set_active(self.guides_on)
        self.guides_btn.set_tooltip_text(
            "Show/hide GIMP & local guides (Key: Ctrl+;). "
            "Drag top/left edge tabs to move local guides; right-click tab to delete."
        )
        self._guides_btn_id = self.guides_btn.connect(
            "toggled", self._on_guides_toggled
        )
        row2b.pack_start(self.guides_btn, False, False, 0)

        row2b.pack_start(sep(), False, False, 0)

        row2b.pack_start(Gtk.Label(label="Layers Below"), False, False, 0)
        self.bg_combo = Gtk.ComboBoxText()
        for _lab in ("Off", "Nearest", "Composite"):
            self.bg_combo.append_text(_lab)
        self.bg_combo.set_active(self._bg_index())
        self.bg_combo.set_size_request(86, -1)
        self.bg_combo.set_tooltip_text(
            "Preview layers below: Off = none, Nearest = first visible layer below, "
            "Composite = stack all visible layers below."
        )
        self._bg_id = self.bg_combo.connect("changed", self._on_bg_mode_changed)
        row2b.pack_start(self.bg_combo, False, False, 0)

        row2b.pack_start(Gtk.Label(label="Layers Above"), False, False, 0)
        self.fg_combo = Gtk.ComboBoxText()
        for _lab in ("Off", "Nearest", "Composite"):
            self.fg_combo.append_text(_lab)
        self.fg_combo.set_active(self._fg_index())
        self.fg_combo.set_size_request(86, -1)
        self.fg_combo.set_tooltip_text(
            "Preview layers above: Off = none, Nearest = first visible layer above, "
            "Composite = stack all visible layers above."
        )
        self._fg_id = self.fg_combo.connect("changed", self._on_fg_mode_changed)
        row2b.pack_start(self.fg_combo, False, False, 0)

        ca.pack_start(row2b, False, False, 0)
        self._sync_snapshot_buttons()

    def _sync_extras_ui(self):
        if getattr(self, "_updating_extras", False):
            return
        self._updating_extras = True
        try:
            if hasattr(self, "snap_toggle_btn"):
                self._set_blocked(
                    self.snap_toggle_btn, getattr(self, "_snap_btn_id", None),
                    lambda v: self.snap_toggle_btn.set_active(v), bool(self.snap_on)
                )
            if hasattr(self, "align_grid_btn"):
                self._set_blocked(
                    self.align_grid_btn, getattr(self, "_align_grid_id", None),
                    lambda v: self.align_grid_btn.set_active(v), bool(self.align_grid_on)
                )
            if hasattr(self, "guides_btn"):
                self._set_blocked(
                    self.guides_btn, getattr(self, "_guides_btn_id", None),
                    lambda v: self.guides_btn.set_active(v), bool(self.guides_on)
                )
            if hasattr(self, "bg_combo"):
                self._set_blocked(
                    self.bg_combo, getattr(self, "_bg_id", None),
                    lambda v: self.bg_combo.set_active(v), self._bg_index()
                )
            if hasattr(self, "fg_combo"):
                self._set_blocked(
                    self.fg_combo, getattr(self, "_fg_id", None),
                    lambda v: self.fg_combo.set_active(v), self._fg_index()
                )
            if hasattr(self, "align_step_spin"):
                self.align_step_spin.set_value(float(self.align_grid_step))
        finally:
            self._updating_extras = False

    def _on_opacity_changed(self, scale):
        val = max(0.0, min(100.0, scale.get_value()))
        self.preview_opacity = val / 100.0
        if hasattr(self, "opacity_val_label"):
            self.opacity_val_label.set_text(f"{int(round(val))}%")
        self.drawing_area.queue_draw()

    def _on_align_step_changed(self, spin):
        self.align_grid_step = max(4, min(512, int(spin.get_value())))
        self.drawing_area.queue_draw()

    def _on_snap_toggled(self, btn):
        if getattr(self, "_updating_extras", False):
            return
        self.snap_on = bool(btn.get_active())

    def _on_align_grid_toggled(self, btn):
        if getattr(self, "_updating_extras", False):
            return
        self.align_grid_on = bool(btn.get_active())
        self.drawing_area.queue_draw()

    def _on_guides_toggled(self, btn):
        if getattr(self, "_updating_extras", False):
            return
        self.guides_on = bool(btn.get_active())
        self.drawing_area.queue_draw()

    # ------------------------------------------------------------------
    # Screen-Space Overlay Drawing (Grid, Guides, Snap Highlights, Badge)
    # ------------------------------------------------------------------

    def _draw_extras_overlay(self, cr, dw, dh):
        z = self.zoom if self.zoom > 1e-4 else 1.0

        if getattr(self, "align_grid_on", False):
            step = max(4.0, float(self.align_grid_step))
            while step * z < 8.0:
                step *= 2.0

            wx0, wy0 = self._screen_to_world(0.0, 0.0)
            wx1, wy1 = self._screen_to_world(float(dw), float(dh))
            c0 = int(math.floor(wx0 / step))
            c1 = int(math.ceil(wx1 / step))
            r0 = int(math.floor(wy0 / step))
            r1 = int(math.ceil(wy1 / step))

            cr.save()
            cr.set_line_width(1.0)
            cr.set_source_rgba(1.0, 1.0, 1.0, 0.12)
            cr.new_path()
            for ci in range(c0, c1 + 1):
                if ci % 4 == 0:
                    continue
                sx, _ = self._world_to_screen(ci * step, 0.0)
                sx = math.floor(sx) + 0.5
                cr.move_to(sx, 0.0)
                cr.line_to(sx, float(dh))
            for ri in range(r0, r1 + 1):
                if ri % 4 == 0:
                    continue
                _, sy = self._world_to_screen(0.0, ri * step)
                sy = math.floor(sy) + 0.5
                cr.move_to(0.0, sy)
                cr.line_to(float(dw), sy)
            cr.stroke()

            cr.set_source_rgba(1.0, 1.0, 1.0, 0.24)
            cr.new_path()
            for ci in range(c0, c1 + 1):
                if ci % 4 != 0:
                    continue
                sx, _ = self._world_to_screen(ci * step, 0.0)
                sx = math.floor(sx) + 0.5
                cr.move_to(sx, 0.0)
                cr.line_to(sx, float(dh))
            for ri in range(r0, r1 + 1):
                if ri % 4 != 0:
                    continue
                _, sy = self._world_to_screen(0.0, ri * step)
                sy = math.floor(sy) + 0.5
                cr.move_to(0.0, sy)
                cr.line_to(float(dw), sy)
            cr.stroke()
            cr.restore()

        if getattr(self, "guides_on", True):
            cr.save()
            cr.set_line_width(1.0)

            for ori, pos in self._gimp_guides:
                cr.set_dash([5.0, 4.0])
                cr.set_source_rgba(0.20, 0.90, 1.0, 0.72)
                if ori == "V":
                    sx, _ = self._world_to_screen(pos, 0.0)
                    sx = math.floor(sx) + 0.5
                    cr.move_to(sx, 0.0)
                    cr.line_to(sx, float(dh))
                else:
                    _, sy = self._world_to_screen(0.0, pos)
                    sy = math.floor(sy) + 0.5
                    cr.move_to(0.0, sy)
                    cr.line_to(float(dw), sy)
                cr.stroke()

            for idx, (ori, pos) in enumerate(self._local_guides):
                active = (self._dragging_guide == idx or self._hovered_guide == idx)
                cr.set_dash([6.0, 3.0])
                cr.set_source_rgba(
                    1.0, 0.40, 0.85, 0.98 if active else 0.78
                )
                if ori == "V":
                    sx, _ = self._world_to_screen(pos, 0.0)
                    sx_l = math.floor(sx) + 0.5
                    cr.move_to(sx_l, 0.0)
                    cr.line_to(sx_l, float(dh))
                    cr.stroke()

                    cr.set_dash([])
                    cr.set_source_rgba(1.0, 0.40, 0.85, 0.95)
                    cr.move_to(sx - 6.0, 0.0)
                    cr.line_to(sx + 6.0, 0.0)
                    cr.line_to(sx, 11.0)
                    cr.close_path()
                    cr.fill()
                else:
                    _, sy = self._world_to_screen(0.0, pos)
                    sy_l = math.floor(sy) + 0.5
                    cr.move_to(0.0, sy_l)
                    cr.line_to(float(dw), sy_l)
                    cr.stroke()

                    cr.set_dash([])
                    cr.set_source_rgba(1.0, 0.40, 0.85, 0.95)
                    cr.move_to(0.0, sy - 6.0)
                    cr.line_to(0.0, sy + 6.0)
                    cr.line_to(11.0, sy)
                    cr.close_path()
                    cr.fill()

            if self._dragging_guide is not None and 0 <= self._dragging_guide < len(self._local_guides):
                ori, pos = self._local_guides[self._dragging_guide]
                lbl = f"{'X' if ori == 'V' else 'Y'}={pos:.0f}"
                cr.set_dash([])
                cr.set_font_size(12.0)
                if ori == "V":
                    sx, _ = self._world_to_screen(pos, 0.0)
                    tx, ty = max(14.0, min(dw - 60.0, sx + 8.0)), 22.0
                else:
                    _, sy = self._world_to_screen(0.0, pos)
                    tx, ty = 18.0, max(18.0, min(dh - 10.0, sy - 6.0))
                cr.set_source_rgba(0.0, 0.0, 0.0, 0.70)
                cr.rectangle(tx - 3.0, ty - 13.0, len(lbl) * 7.5 + 6.0, 17.0)
                cr.fill()
                cr.set_source_rgb(1.0, 0.65, 0.92)
                cr.move_to(tx, ty)
                cr.show_text(lbl)

            cr.restore()

        svx, shy = getattr(self, "_active_snap_guides", (None, None))
        if self.is_dragging_pin and (svx is not None or shy is not None):
            cr.save()
            cr.set_line_width(1.2)
            cr.set_dash([4.0, 3.0])
            cr.set_source_rgba(0.30, 1.0, 0.55, 0.95)
            if svx is not None:
                sx, _ = self._world_to_screen(svx, 0.0)
                sx = math.floor(sx) + 0.5
                cr.move_to(sx, 0.0)
                cr.line_to(sx, float(dh))
            if shy is not None:
                _, sy = self._world_to_screen(0.0, shy)
                sy = math.floor(sy) + 0.5
                cr.move_to(0.0, sy)
                cr.line_to(float(dw), sy)
            cr.stroke()
            cr.restore()

        badge = None
        if getattr(self, "_compare_orig_on", False):
            badge = "ORIGINAL (Before) — press \\ to return"
        elif getattr(self, "_compare_ab_on", False):
            slot = getattr(self, "_compare_peeking_slot", "?")
            avail = [k for k in ("A", "B", "C") if self._snapshots.get(k) is not None]
            if slot in avail:
                badge = f"COMPARE: Snapshot {slot} ({avail.index(slot) + 1}/{len(avail)}) — → / ` to cycle"
            else:
                badge = f"COMPARE: Snapshot {slot}"

        if badge:
            cr.save()
            cr.set_font_size(12.5)
            ext = cr.text_extents(badge)
            bx, by = 16.0, 16.0
            bw, bh = ext.width + 16.0, ext.height + 10.0
            cr.set_source_rgba(0.0, 0.0, 0.0, 0.75)
            cr.rectangle(bx, by, bw, bh)
            cr.fill()
            cr.set_source_rgba(1.0, 0.75, 0.20, 0.95)
            cr.set_line_width(1.2)
            cr.rectangle(bx, by, bw, bh)
            cr.stroke()
            cr.move_to(bx + 8.0, by + bh - 6.0)
            cr.show_text(badge)
            cr.restore()
