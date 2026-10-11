/* Puppet Warp GEGL operation: inverse-mapped triangle deform driven by an
 * ARAP grid solve. Params arrive as one JSON string; the solved vertex grid
 * is cached across tiles/levels. Preview rasterization is exported separately
 * (pw_render_preview) for the Python UI to call via ctypes.
 *
 * Build (cairo required for the preview exporter):
 *   gcc -O2 -shared -fPIC -Wall -I. puppet-warp.c  arap_grid.c \
 *     -o puppet-warp.so -lm $(pkg-config --cflags --libs gegl-0.4 glib-2.0 cairo)
 * Debug: PW_DEBUG=1 gimp
 */
#include <gegl-plugin.h>
#include <babl/babl.h>
#include <cairo.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

typedef struct { float x, y; } ArapVec2;

/* Solver API (arap_grid.c). Optional entry points are weak so this .so links
 * against an older solver without them. */
extern void     *arap_grid_new (float x, float y, float w, float h, int cols, int rows);
extern void      arap_grid_add_pin (void *g, float sx, float sy, float ddx, float ddy);
extern int       arap_grid_solve (void *g, int a, int b, float c, int d);
extern ArapVec2 *arap_grid_verts (void *g, int *n);
extern void      arap_grid_free (void *g);
extern void      arap_grid_set_cell_weights (void *g, const float *w, int n)
  __attribute__ ((weak));
extern void      arap_grid_set_pin_edge (void *g, int on)
  __attribute__ ((weak));
extern void      arap_grid_add_pin_sim (void *g, float sx, float sy, float ddx, float ddy,
                                        float theta, float scale)
  __attribute__ ((weak));

/* Locate the value token following "key": in a flat JSON string. */
static const char *
find_key (const char *json, const char *key)
{
  char pat[64];
  const char *p;

  snprintf (pat, sizeof (pat), "\"%s\"", key);
  p = strstr (json, pat);
  if (!p) return NULL;
  p = strchr (p + strlen (pat), ':');
  return p ? p + 1 : NULL;
}

/* Read an integer field, or dflt when absent. */
static int
json_int (const char *json, const char *key, int dflt)
{
  const char *v = find_key (json, key);
  return v ? (int) strtol (v, NULL, 10) : dflt;
}

/* Read a numeric array field into out (up to maxn); return count parsed. */
static int
json_floats (const char *json, const char *key, float *out, int maxn)
{
  const char *v = find_key (json, key);
  int n = 0;

  if (!v) return 0;
  v = strchr (v, '[');
  if (!v) return 0;
  v++;
  while (n < maxn)
    {
      char *end = NULL;
      double d = strtod (v, &end);
      if (end == v) break;
      out[n++] = (float) d;
      v = end;
      while (*v && *v != ',' && *v != ']') v++;
      if (*v == ']') break;
      v++;
    }
  return n;
}

/* Read a quoted string field into out; TRUE when non-empty. */
static gboolean
json_str (const char *json, const char *key, char *out, int outlen)
{
  const char *v = find_key (json, key);
  int i = 0;

  if (!v) return FALSE;
  while (*v && *v != '"') v++;
  if (*v != '"') return FALSE;
  v++;
  while (*v && *v != '"' && i < outlen - 1)
    out[i++] = *v++;
  out[i] = 0;
  return i > 0;
}

typedef struct _PuppetWarp      PuppetWarp;
typedef struct _PuppetWarpClass PuppetWarpClass;

struct _PuppetWarp
{
  GeglOperationFilter parent_instance;
  gchar *params_json;
};

struct _PuppetWarpClass
{
  GeglOperationFilterClass parent_class;
};

enum { PROP_0, PROP_PARAMS_JSON };

static GTypeModule   *pw_module     = NULL;
static GType          pw_type       = 0;
static GObjectClass  *pw_parent_cls = NULL;

/* Solved-grid cache keyed by (params_json, level); shared across tiles. */
static GRWLock    pw_solve_lock;
static gchar     *pw_cache_json   = NULL;
static gint       pw_cache_level  = -1;
static float     *pw_cache_dx     = NULL;
static float     *pw_cache_dy     = NULL;
static int        pw_cache_nverts = 0;
static float      pw_cache_ow     = 0.f;
static float      pw_cache_oh     = 0.f;

/* Per-process raster diagnostics (printed under PW_DEBUG). */
static long pw_cnt_det = 0, pw_cnt_sc = 0, pw_cnt_mdet = 0,
            pw_cnt_bbox = 0, pw_cnt_draw = 0;

static void
puppet_warp_init (PuppetWarp *self)
{
  self->params_json = g_strdup ("{}");
}

static void
puppet_warp_finalize (GObject *object)
{
  PuppetWarp *self = (PuppetWarp *) object;
  g_free (self->params_json);
  G_OBJECT_CLASS (pw_parent_cls)->finalize (object);
}

static void
puppet_warp_set_property (GObject      *object,
                          guint         prop_id,
                          const GValue *value,
                          GParamSpec   *pspec)
{
  PuppetWarp *self = (PuppetWarp *) object;
  switch (prop_id)
    {
    case PROP_PARAMS_JSON:
      g_free (self->params_json);
      self->params_json = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
puppet_warp_get_property (GObject    *object,
                          guint       prop_id,
                          GValue     *value,
                          GParamSpec *pspec)
{
  PuppetWarp *self = (PuppetWarp *) object;
  switch (prop_id)
    {
    case PROP_PARAMS_JSON:
      g_value_set_string (value, self->params_json ? self->params_json : "{}");
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

/* Rasterize one source triangle onto its deformed destination into a tile.
 * Samples color once per pixel and derives alpha from the same buffer when the
 * format carries it inline (keeps NDE visibility toggles fast); an alpha mask
 * prevents a fainter overlap from erasing an already-written pixel. */
static void
warp_tri (GeglBuffer          *input,
          const Babl          *fmt,
          gint                 bpp,
          const Babl          *afmt,
          float                *amask,
          gboolean             alpha_from_px,
          int                  csize,
          int                  aoff,
          guchar               *tile,
          const GeglRectangle *sroi,
          float u0, float v0, float u1, float v1, float u2, float v2,
          float x0, float y0, float x1, float y1, float x2, float y2)
{
  double du0 = u0, dv0 = v0, du1 = u1, dv1 = v1, du2 = u2, dv2 = v2;
  double sx0 = x0, sy0 = y0, sx1 = x1, sy1 = y1, sx2 = x2, sy2 = y2;

  double det = (du1 - du0) * (dv2 - dv0) - (dv1 - dv0) * (du2 - du0);
  double inv, a, b, c, d, e, f, sc, m_det;
  double e0x, e0y, e1x, e1y, e2x, e2y;

  const double eps = 1e-3;

  int minx, maxx, miny, maxy, x, y;
  guchar px[64];

  if (fabs (det) < 0.5)
    {
      pw_cnt_det++;
      return;
    }

  if (bpp > 64)
    return;

  inv = 1.0 / det;

  a = inv * ((dv2 - dv0) * (sx1 - sx0) - (dv1 - dv0) * (sx2 - sx0));
  b = inv * ((dv2 - dv0) * (sy1 - sy0) - (dv1 - dv0) * (sy2 - sy0));
  c = inv * (-(du2 - du0) * (sx1 - sx0) + (du1 - du0) * (sx2 - sx0));
  d = inv * (-(du2 - du0) * (sy1 - sy0) + (du1 - du0) * (sy2 - sy0));
  e = sx0 - a * du0 - c * dv0;
  f = sy0 - b * du0 - d * dv0;

  sc = fabs (a);
  sc = fmax (sc, fabs (b));
  sc = fmax (sc, fabs (c));
  sc = fmax (sc, fabs (d));

  if (sc > 1e4 || sc < 1e-6)
    {
      pw_cnt_sc++;
      return;
    }

  m_det = a * d - b * c;
  if (!isfinite (m_det) || fabs (m_det) < 1e-12)
    {
      pw_cnt_mdet++;
      return;
    }

  minx = (int) floor (fmin (du0, fmin (du1, du2)));
  maxx = (int) ceil  (fmax (du0, fmax (du1, du2)));
  miny = MAX ((int) floor (fmin (dv0, fmin (dv1, dv2))), sroi->y);
  maxy = MIN ((int) ceil  (fmax (dv0, fmax (dv1, dv2))),
              sroi->y + sroi->height - 1);

  minx = MAX (minx, sroi->x);
  maxx = MIN (maxx, sroi->x + sroi->width - 1);

  if (minx > maxx || miny > maxy)
    {
      pw_cnt_bbox++;
      return;
    }

  pw_cnt_draw++;

  e0x = du1 - du0; e0y = dv1 - dv0;
  e1x = du2 - du1; e1y = dv2 - dv1;
  e2x = du0 - du2; e2y = dv0 - dv2;

  for (y = miny; y <= maxy; y++)
    for (x = minx; x <= maxx; x++)
      {
        double fx = x + 0.5, fy = y + 0.5;

        double w0 = e0x * (fy - dv0) - e0y * (fx - du0);
        double w1 = e1x * (fy - dv1) - e1y * (fx - du1);
        double w2 = e2x * (fy - dv2) - e2y * (fx - du2);

        gboolean inside =
          (w0 >= -eps && w1 >= -eps && w2 >= -eps) ||
          (w0 <=  eps && w1 <=  eps && w2 <=  eps);

        gsize pidx, idx;
        double srcx, srcy;

        if (!inside)
          continue;

        srcx = a * fx + c * fy + e;
        srcy = b * fx + d * fy + f;

        pidx = (gsize) (y - sroi->y) * sroi->width + (x - sroi->x);
        idx  = pidx * bpp;

        gegl_buffer_sample (input, srcx, srcy, NULL, px, fmt,
                            GEGL_ABYSS_NONE, GEGL_SAMPLER_LINEAR);

        if (alpha_from_px)
          {
            float sa;

            if (csize == 4)
              memcpy (&sa, px + aoff, sizeof sa);
            else
              sa = (float) px[aoff] * 0.00392156862745098f; /* 1/255 */

            if (!(sa > 0.0f) || !isfinite ((double) sa))
              continue;            /* transparent source: keep prior pixel */

            if (sa > 1.0f)
              sa = 1.0f;

            if (sa < amask[pidx])
              continue;            /* fainter than written: skip, no erase */

            memcpy (tile + idx, px, bpp);
            amask[pidx] = sa;
          }
        else if (afmt != NULL && amask != NULL)
          {
            float sa = 0.0f;

            gegl_buffer_sample (input, srcx, srcy, NULL, &sa, afmt,
                                GEGL_ABYSS_NONE, GEGL_SAMPLER_LINEAR);

            if (!(sa > 0.0f) || !isfinite ((double) sa))
              continue;
            if (sa > 1.0f)
              sa = 1.0f;
            if (sa < amask[pidx])
              continue;

            memcpy (tile + idx, px, bpp);
            amask[pidx] = sa;
          }
        else
          {
            memcpy (tile + idx, px, bpp);
          }
      }
}

/* One preview triangle via cairo; returns 1 when drawn, 0 when skipped by a
 * guard mirroring warp_tri so the C preview equals the Python one. */
static int
pw_tri_c (cairo_t *cr, cairo_pattern_t *pat,
          double u0, double v0, double u1, double v1, double u2, double v2,
          double x0, double y0, double x1, double y1, double x2, double y2)
{
  cairo_matrix_t mx;
  double det = (u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0);
  double inv, fa, fb, fc, fd, fe, ff, sc, m_det;

  if (fabs (det) < 0.5)                 return 0;

  inv = 1.0 / det;

  fa = inv * ((v2 - v0) * (x1 - x0) - (v1 - v0) * (x2 - x0));
  fb = inv * ((v2 - v0) * (y1 - y0) - (v1 - v0) * (y2 - y0));
  fc = inv * (-(u2 - u0) * (x1 - x0) + (u1 - u0) * (x2 - x0));
  fd = inv * (-(u2 - u0) * (y1 - y0) + (u1 - u0) * (y2 - y0));
  fe = x0 - fa * u0 - fc * v0;
  ff = y0 - fb * u0 - fd * v0;

  sc = fabs (fa);
  sc = fmax (sc, fabs (fb));
  sc = fmax (sc, fabs (fc));
  sc = fmax (sc, fabs (fd));
  if (sc > 1e4 || sc < 1e-6)            return 0;

  m_det = fa * fd - fb * fc;
  if (!isfinite (m_det) || fabs (m_det) < 1e-12) return 0;
  if (!isfinite (fa) || !isfinite (fb) || !isfinite (fc) ||
      !isfinite (fd) || !isfinite (fe) || !isfinite (ff))   return 0;

  cairo_matrix_init (&mx, fa, fb, fc, fd, fe, ff);
  cairo_pattern_set_matrix (pat, &mx);

  cairo_set_source (cr, pat);
  cairo_new_path (cr);
  cairo_move_to  (cr, u0, v0);
  cairo_line_to  (cr, u1, v1);
  cairo_line_to  (cr, u2, v2);
  cairo_close_path (cr);
  cairo_fill (cr);

  return 1;
}

/* Render the warped preview into an ARGB32 surface from a flat vertex array.
 * Exported for the Python UI; unrelated to the GEGL apply path. */
int
pw_render_preview (void *src, int sw, int sh, int sstride,
                   void *dst, int dw, int dh, int dstride,
                   int cols, int rows,
                   const float *verts, int nverts,
                   float px, float py, float pw, float ph)
{
  cairo_surface_t *ss, *ds;
  cairo_t *cr;
  cairo_pattern_t *pat;
  int r, c, draw = 0;

  if (!src || !dst || !verts || cols < 1 || rows < 1)
    return 0;
  if ((long long) (cols + 1) * (rows + 1) > (long long) nverts)
    return 0;

  ss = cairo_image_surface_create_for_data ((unsigned char *) src,
                                            CAIRO_FORMAT_ARGB32, sw, sh, sstride);
  if (cairo_surface_status (ss) != CAIRO_STATUS_SUCCESS)
    { cairo_surface_destroy (ss); return 0; }

  ds = cairo_image_surface_create_for_data ((unsigned char *) dst,
                                            CAIRO_FORMAT_ARGB32, dw, dh, dstride);
  if (cairo_surface_status (ds) != CAIRO_STATUS_SUCCESS)
    { cairo_surface_destroy (ss); cairo_surface_destroy (ds); return 0; }

  cr = cairo_create (ds);

  cairo_set_operator (cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint (cr);
  cairo_set_operator (cr, CAIRO_OPERATOR_OVER);

  pat = cairo_pattern_create_for_surface (ss);
  cairo_pattern_set_filter (pat, CAIRO_FILTER_BILINEAR);
  cairo_pattern_set_extend (pat, CAIRO_EXTEND_NONE);

  for (r = 0; r < rows; r++)
    for (c = 0; c < cols; c++)
      {
        int i00 = r * (cols + 1) + c;
        int i10 = i00 + 1;
        int i01 = i00 + (cols + 1);
        int i11 = i01 + 1;

        double su0 = px + (double) c       * pw / cols, sv0 = py + (double) r       * ph / rows;
        double su1 = px + (double) (c + 1) * pw / cols, sv1 = sv0;
        double su2 = su0,                               sv2 = py + (double) (r + 1) * ph / rows;
        double su3 = su1,                               sv3 = sv2;

        double du0 = verts[2 * i00], dv0 = verts[2 * i00 + 1];
        double du1 = verts[2 * i10], dv1 = verts[2 * i10 + 1];
        double du2 = verts[2 * i01], dv2 = verts[2 * i01 + 1];
        double du3 = verts[2 * i11], dv3 = verts[2 * i11 + 1];

        draw += pw_tri_c (cr, pat,
                          du0, dv0, du1, dv1, du2, dv2,
                          su0, sv0, su1, sv1, su2, sv2);
        draw += pw_tri_c (cr, pat,
                          du1, dv1, du3, dv3, du2, dv2,
                          su1, sv1, su3, sv3, su2, sv2);
      }

  cairo_pattern_destroy (pat);
  cairo_surface_flush (ds);
  cairo_destroy (cr);
  cairo_surface_destroy (ss);
  cairo_surface_destroy (ds);

  return draw;
}

/* Tile processor: obtain the solved grid (cache -> sidecar -> json -> solve),
 * then inverse-map every cell triangle into the output band. */
static gboolean
puppet_warp_process (GeglOperation       *operation,
                     GeglBuffer          *input,
                     GeglBuffer          *output,
                     const GeglRectangle *roi,
                     gint                 level)
{
  PuppetWarp *self = (PuppetWarp *) operation;
  const char *json = self->params_json ? self->params_json : "{}";

  int cols = json_int (json, "cols", 15);
  int rows = json_int (json, "rows", 15);
  int pin_edge = json_int (json, "pin_edge", 0);
  int rotate_on = json_int (json, "rotate_on", 1);

  float ow = (float) json_int (json, "orig_width",  roi->width);
  float oh = (float) json_int (json, "orig_height", roi->height);

  float *pins = g_malloc (256 * 4 * sizeof (float));
  float *cw   = g_malloc (64 * 64 * sizeof (float));
  float *prot = g_malloc (256 * sizeof (float));
  float *pisc = g_malloc (256 * sizeof (float));

  int npins = json_floats (json, "pins_flat", pins, 256 * 4) / 4;
  int ncw   = json_floats (json, "mask_flat", cw, 64 * 64);
  int nrot  = json_floats (json, "pin_rot", prot, 256);
  int nsc   = json_floats (json, "pin_scale", pisc, 256);

  int nverts = (cols + 1) * (rows + 1);
  gdouble s = 1.0 / (1 << level);

  const Babl *fmt;
  const Babl *afmt = NULL;
  gint bpp;
  gboolean has_alpha;
  gboolean alpha_from_px = FALSE;
  int csz = 0, aoff = 0;

  float *dxs, *dys;
  int sy0, r, c;

  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;

  if (getenv ("PW_DEBUG"))
    {
      pw_cnt_det = pw_cnt_sc = pw_cnt_mdet = pw_cnt_bbox = pw_cnt_draw = 0;
      fprintf (stderr,
               "[puppet-warp] pins=%d nrot=%d nsc=%d ncw=%d roi=%dx%d level=%d\n",
               npins, nrot, nsc, ncw, roi->width, roi->height, level);
    }

  /* Output format must come from the pad, not a hardcoded RGBA float. */
  fmt = gegl_operation_get_format (operation, "output");
  if (!fmt)
    fmt = babl_format ("RGBA float");

  bpp = babl_format_get_bytes_per_pixel (fmt);
  has_alpha = babl_format_has_alpha (fmt);

  {
    int ncomp = babl_format_get_n_components (fmt);
    int csize = (ncomp > 0) ? (bpp / ncomp) : 0;

    alpha_from_px = (has_alpha && ncomp == 4 && (csize == 4 || csize == 1));
    csz  = csize;
    aoff = alpha_from_px ? (3 * csize) : 0;
  }

  afmt = (has_alpha && !alpha_from_px) ? babl_format ("A float") : NULL;

  int hold_writer = 0;
  g_rw_lock_reader_lock (&pw_solve_lock);

  if (!(pw_cache_json != NULL &&
        strcmp (pw_cache_json, json) == 0 &&
        pw_cache_level == level &&
        pw_cache_nverts == nverts))
    {
      g_rw_lock_reader_unlock (&pw_solve_lock);
      g_rw_lock_writer_lock (&pw_solve_lock);
      hold_writer = 1; 

      if (!(pw_cache_json != NULL &&
            strcmp (pw_cache_json, json) == 0 &&
            pw_cache_level == level &&
            pw_cache_nverts == nverts))
        {
          float *dx = g_new (float, nverts);
          float *dy = g_new (float, nverts);
          int vert_src = 0;   /* 0=solve, 1=json, 2=sidecar */
          int rr, cc, ii;

          for (rr = 0; rr <= rows; rr++)
            for (cc = 0; cc <= cols; cc++)
              {
                dx[rr * (cols + 1) + cc] = (float) cc * ow / cols;
                dy[rr * (cols + 1) + cc] = (float) rr * oh / rows;
              }

          /* Priority 1: binary sidecar addressed by token. */
          {
            char tok[96] = { 0 };

            if (json_str (json, "verts_token", tok, sizeof tok))
              {
                char *q;

                for (q = tok; *q; q++)
                  {
                    if (!g_ascii_isalnum ((guchar) *q) &&
                        *q != '-' && *q != '_')
                      *q = '_';
                  }

                gchar *fname = g_strdup_printf ("puppet-warp-verts-%s.bin", tok);
                gchar *path  = g_build_filename (g_get_tmp_dir (), fname, NULL);
                gchar *data  = NULL;
                gsize len    = 0;

                g_free (fname);

                if (g_file_get_contents (path, &data, &len, NULL))
                  {
                    if (len >= 76 &&
                        strncmp (data, tok, strlen (tok)) == 0)
                      {
                        guint32 c2, r2, n2;

                        memcpy (&c2, data + 64, 4);
                        memcpy (&r2, data + 68, 4);
                        memcpy (&n2, data + 72, 4);

                        if ((int) c2 == cols &&
                            (int) r2 == rows &&
                            (int) n2 == nverts &&
                            len == 76 + (gsize) nverts * 8)
                          {
                            const float *fv = (const float *) (data + 76);

                            for (ii = 0; ii < nverts; ii++)
                              {
                                dx[ii] = fv[ii * 2];
                                dy[ii] = fv[ii * 2 + 1];
                              }

                            vert_src = 2;
                          }
                      }

                    g_free (data);
                  }

                g_free (path);
              }
          }

          /* Priority 2: vertex array embedded in the json. */
          if (vert_src == 0)
            {
              float *vf = g_new (float, nverts * 2);
              int nvf = json_floats (json, "verts_flat", vf, nverts * 2);

              if (nvf == nverts * 2)
                {
                  for (ii = 0; ii < nverts; ii++)
                    {
                      dx[ii] = vf[ii * 2];
                      dy[ii] = vf[ii * 2 + 1];
                    }

                  vert_src = 1;
                }

              g_free (vf);
            }

          /* Priority 3: solve from pins (safety net when neither above exists). */
          if (vert_src == 0)
            {
              void *grid = arap_grid_new (0.f, 0.f, ow, oh, cols, rows);

              if (grid)
                {
                  if (ncw == cols * rows && arap_grid_set_cell_weights)
                    arap_grid_set_cell_weights (grid, cw, ncw);

                  if (pin_edge && arap_grid_set_pin_edge)
                    arap_grid_set_pin_edge (grid, 1);

                  /* A pin with theta!=0 or scale!=1 (and rotate enabled) uses the
                   * similarity ring; otherwise the plain single-vertex pin. */
                  for (ii = 0; ii < npins; ii++)
                    {
                      float th = (rotate_on && ii < nrot) ? prot[ii] : 0.f;
                      float sc = (rotate_on && ii < nsc) ? pisc[ii] : 1.0f;

                      if ((th != 0.f || sc != 1.f) && arap_grid_add_pin_sim)
                        arap_grid_add_pin_sim (grid,
                                               pins[ii * 4],
                                               pins[ii * 4 + 1],
                                               pins[ii * 4 + 2],
                                               pins[ii * 4 + 3],
                                               th,
                                               sc);
                      else
                        arap_grid_add_pin (grid,
                                           pins[ii * 4],
                                           pins[ii * 4 + 1],
                                           pins[ii * 4 + 2],
                                           pins[ii * 4 + 3]);
                    }

                  arap_grid_solve (grid, 30, 150, 0.05f, 0);

                  {
                    int n = 0;
                    ArapVec2 *v = arap_grid_verts (grid, &n);

                    if (v && n == nverts)
                      {
                        for (ii = 0; ii < nverts; ii++)
                          {
                            dx[ii] = v[ii].x;
                            dy[ii] = v[ii].y;
                          }
                      }
                  }

                  arap_grid_free (grid);
                }
            }

          if (getenv ("PW_DEBUG"))
            fprintf (stderr,
                     "[pw-op] vert_src=%d nverts=%d cols=%d rows=%d\n",
                     vert_src, nverts, cols, rows);

          for (ii = 0; ii < nverts; ii++)
            {
              dx[ii] = (float) (dx[ii] * s);
              dy[ii] = (float) (dy[ii] * s);
            }

          g_free (pw_cache_dx);
          g_free (pw_cache_dy);
          g_free (pw_cache_json);

          pw_cache_json   = g_strdup (json);
          pw_cache_level  = level;
          pw_cache_dx     = dx;
          pw_cache_dy     = dy;
          pw_cache_nverts = nverts;
          pw_cache_ow     = (float) (ow * s);
          pw_cache_oh     = (float) (oh * s);
        }
    }

  dxs = pw_cache_dx;
  dys = pw_cache_dy;
  ow  = pw_cache_ow;
  oh  = pw_cache_oh;

  /* Render in 64-row bands. Do not force threaded=FALSE: keeping it on is what
   * preserves fast NDE visibility toggles. */
  for (sy0 = roi->y; sy0 < roi->y + roi->height; sy0 += 64)
    {
      GeglRectangle band;
      guchar *tile;
      float *amask = NULL;
      gsize npix;

      band.x = roi->x;
      band.y = sy0;
      band.width = roi->width;
      band.height = MIN (64, roi->y + roi->height - sy0);

      npix = (gsize) band.width * band.height;
      tile = g_new0 (guchar, npix * bpp);

      if (has_alpha)
        amask = g_new0 (float, npix);

      for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
          {
            int i00 = r * (cols + 1) + c;
            int i10 = i00 + 1;
            int i01 = i00 + (cols + 1);
            int i11 = i01 + 1;

            float x0 = (float) c       * ow / cols;
            float y0 = (float) r       * oh / rows;
            float x1 = (float) (c + 1) * ow / cols;
            float y1 = y0;
            float x2 = x0;
            float y2 = (float) (r + 1) * oh / rows;
            float x3 = x1;
            float y3 = y2;

            warp_tri (input, fmt, bpp, afmt, amask,
                      alpha_from_px, csz, aoff,
                      tile, &band,
                      dxs[i00], dys[i00],
                      dxs[i10], dys[i10],
                      dxs[i01], dys[i01],
                      x0, y0,
                      x1, y1,
                      x2, y2);

            warp_tri (input, fmt, bpp, afmt, amask,
                      alpha_from_px, csz, aoff,
                      tile, &band,
                      dxs[i10], dys[i10],
                      dxs[i11], dys[i11],
                      dxs[i01], dys[i01],
                      x1, y1,
                      x3, y3,
                      x2, y2);
          }

      gegl_buffer_set (output, &band, 0, fmt, tile, GEGL_AUTO_ROWSTRIDE);

      g_free (tile);
      if (amask)
        g_free (amask);
    }

  if (hold_writer)
    g_rw_lock_writer_unlock (&pw_solve_lock);
  else
    g_rw_lock_reader_unlock (&pw_solve_lock);

  if (getenv ("PW_DEBUG"))
    fprintf (stderr,
             "[pw-c-tri] draw=%ld det=%ld sc=%ld mdet=%ld bbox=%ld\n",
             pw_cnt_draw, pw_cnt_det, pw_cnt_sc, pw_cnt_mdet, pw_cnt_bbox);

  g_free (pins);
  g_free (cw);
  g_free (prot);
  g_free (pisc);

  /* dxs/dys point into the shared cache: never free them here. */
  return TRUE;
}

static void
puppet_warp_class_init (PuppetWarpClass *klass, gpointer class_data)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  GeglOperationClass *opc = GEGL_OPERATION_CLASS (klass);
  GeglOperationFilterClass *fc = GEGL_OPERATION_FILTER_CLASS (klass);

  pw_parent_cls = g_type_class_peek_parent (klass);

  oc->set_property = puppet_warp_set_property;
  oc->get_property = puppet_warp_get_property;
  oc->finalize     = puppet_warp_finalize;

  g_object_class_install_property (
      oc,
      PROP_PARAMS_JSON,
      g_param_spec_string ("params-json",
                           "params-json",
                           "JSON encoded puppet warp parameters",
                           "{}",
                           G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  fc->process         = puppet_warp_process;
  opc->opencl_support = FALSE;

  gegl_operation_class_set_keys (opc,
                                 "name",        "custom:puppet-warp",
                                 "title",       "Puppet Warp (ARAP)",
                                 "categories",  "deform",
                                 "description", "ARAP-based puppet warp (inverse mapped triangles, transparent holes)",
                                 NULL);
}

static GType
puppet_warp_get_type (void)
{
  if (pw_type == 0)
    {
      static const GTypeInfo info =
        {
          sizeof (PuppetWarpClass),
          NULL, NULL,
          (GClassInitFunc) puppet_warp_class_init,
          NULL, NULL,
          sizeof (PuppetWarp),
          0,
          (GInstanceInitFunc) puppet_warp_init,
          NULL
        };

      pw_type = g_type_module_register_type (pw_module,
                                             GEGL_TYPE_OPERATION_FILTER,
                                             "GeglPuppetWarp",
                                             &info,
                                             0);
    }

  return pw_type;
}

const GeglModuleInfo *
gegl_module_query (GTypeModule *module)
{
  static const GeglModuleInfo info = { GEGL_MODULE_ABI_VERSION };
  return &info;
}

gboolean
gegl_module_register (GTypeModule *module)
{
  pw_module = module;
  g_type_module_set_name (module, "puppet-warp");
  return puppet_warp_get_type () != 0;
}
