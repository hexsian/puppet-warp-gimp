/* puppet-warp.c - ARAP puppet warp GEGL op and fast preview rasterizer. */

#include <gegl-plugin.h>
#include <babl/babl.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#ifdef _OPENMP
#include <omp.h>
#endif

typedef struct { float x, y; } ArapVec2;

#if defined(__GNUC__) && !defined(_WIN32)
# define PW_WEAK __attribute__((weak))
#else
# define PW_WEAK
#endif

#if defined(_WIN32)
# define PW_EXPORT __declspec(dllexport)
#else
# define PW_EXPORT __attribute__((visibility("default")))
#endif

extern void     *arap_grid_new (float x, float y, float w, float h, int cols, int rows);
extern void      arap_grid_add_pin (void *g, float sx, float sy, float ddx, float ddy);
extern int       arap_grid_solve (void *g, int a, int b, float c, int d);
extern ArapVec2 *arap_grid_verts (void *g, int *n);
extern void      arap_grid_free (void *g);

extern void      arap_grid_set_cell_weights (void *g, const float *w, int n) PW_WEAK;
extern void      arap_grid_set_pin_edge (void *g, int on) PW_WEAK;
extern void      arap_grid_add_pin_sim (void *g, float sx, float sy, float ddx, float ddy,
                                        float theta, float scale) PW_WEAK;
extern void      arap_grid_add_pin_taper (void *g, float sx, float sy, float ddx, float ddy,
                                          float theta, float scale, float taper) PW_WEAK;

static const char *
find_key (const char *json, const char *key)
{
  char pat[64];
  const char *p;

  snprintf (pat, sizeof (pat), "\"%s\"", key);
  p = strstr (json, pat);
  if (!p)
    return NULL;

  p = strchr (p + strlen (pat), ':');
  return p ? p + 1 : NULL;
}

static int
json_int (const char *json, const char *key, int dflt)
{
  const char *v = find_key (json, key);
  return v ? (int) strtol (v, NULL, 10) : dflt;
}

static int
json_floats (const char *json, const char *key, float *out, int maxn)
{
  const char *v = find_key (json, key);
  int n = 0;

  if (!v)
    return 0;

  v = strchr (v, '[');
  if (!v)
    return 0;
  v++;

  while (n < maxn)
    {
      char *end = NULL;
      double d = strtod (v, &end);

      if (end == (char *) v)
        break;

      out[n++] = (float) d;
      v = end;

      while (*v && *v != ',' && *v != ']')
        v++;

      if (*v == ']')
        break;

      v++;
    }

  return n;
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

/* solved grid cache, shared across tiles/levels */
static GRWLock    pw_solve_lock;
static gsize      pw_lock_once = 0;

static gchar     *pw_cache_json   = NULL;
static gint       pw_cache_level  = -1;
static float     *pw_cache_dx     = NULL;
static float     *pw_cache_dy     = NULL;
static int        pw_cache_nverts = 0;
static float      pw_cache_ow     = 0.f;
static float      pw_cache_oh     = 0.f;
static int       *pw_cache_vdepth = NULL;
static int        pw_cache_depth_on = 0;

static void
pw_ensure_lock (void)
{
  if (g_once_init_enter (&pw_lock_once))
    {
      g_rw_lock_init (&pw_solve_lock);
      g_once_init_leave (&pw_lock_once, 1);
    }
}

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

static int
pw_needle (double u0, double v0, double u1, double v1, double u2, double v2,
           double x0, double y0, double x1, double y1, double x2, double y2)
{
  double du, dv, dx, dy;

  du = u1 - u0; dv = v1 - v0; dx = x1 - x0; dy = y1 - y0;
  if (du * du + dv * dv > 64.0 * (dx * dx + dy * dy))
    return 1;

  du = u2 - u1; dv = v2 - v1; dx = x2 - x1; dy = y2 - y1;
  if (du * du + dv * dv > 64.0 * (dx * dx + dy * dy))
    return 1;

  du = u0 - u2; dv = v0 - v2; dx = x0 - x2; dy = y0 - y2;
  if (du * du + dv * dv > 64.0 * (dx * dx + dy * dy))
    return 1;

  return 0;
}

/* apply path: inverse map one triangle into a GEGL tile */
static void
warp_tri (GeglBuffer          *input,
          const Babl          *fmt,
          gint                 bpp,
          const Babl          *afmt,
          float               *amask,
          gboolean             alpha_from_px,
          int                  csize,
          int                  aoff,
          guchar              *tile,
          const GeglRectangle *sroi,
          float u0, float v0, float u1, float v1, float u2, float v2,
          float x0, float y0, float x1, float y1, float x2, float y2,
          gboolean             clamp_src,
          double               cx0, double cy0, double cx1, double cy1,
          double det_min,
          int                  tri_depth,
          int                 *zbuf)
{
  double du0 = u0, dv0 = v0, du1 = u1, dv1 = v1, du2 = u2, dv2 = v2;
  double sx0 = x0, sy0 = y0, sx1 = x1, sy1 = y1, sx2 = x2, sy2 = y2;
  double det = (du1 - du0) * (dv2 - dv0) - (dv1 - dv0) * (du2 - du0);
  double inv, a, b, c, d, e, f, sc, m_det;
  double e0x, e0y, e1x, e1y, e2x, e2y;
  const double eps = 1e-3;
  int minx, maxx, miny, maxy, x, y;
  guchar px[64];

  if (!isfinite (du0) || !isfinite (dv0) ||
      !isfinite (du1) || !isfinite (dv1) ||
      !isfinite (du2) || !isfinite (dv2) ||
      !isfinite (sx0) || !isfinite (sy0) ||
      !isfinite (sx1) || !isfinite (sy1) ||
      !isfinite (sx2) || !isfinite (sy2))
    return;

  /* Signed area only. det_min is in the same coordinate space as det. */
  if (!isfinite (det) || !(det >= det_min))
    return;

  if (pw_needle (du0, dv0, du1, dv1, du2, dv2,
                 sx0, sy0, sx1, sy1, sx2, sy2))
    return;

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

  if (!isfinite (sc) || sc > 1e4 || sc < 1e-6)
    return;

  m_det = a * d - b * c;
  if (!isfinite (m_det) || fabs (m_det) < 1e-12)
    return;

  minx = (int) floor (fmin (du0, fmin (du1, du2)));
  maxx = (int) ceil  (fmax (du0, fmax (du1, du2)));
  miny = MAX ((int) floor (fmin (dv0, fmin (dv1, dv2))), sroi->y);
  maxy = MIN ((int) ceil  (fmax (dv0, fmax (dv1, dv2))),
              sroi->y + sroi->height - 1);

  minx = MAX (minx, sroi->x);
  maxx = MIN (maxx, sroi->x + sroi->width - 1);

  if (minx > maxx || miny > maxy)
    return;

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

        if (clamp_src)
          {
            if (srcx < cx0) srcx = cx0;
            else if (srcx > cx1) srcx = cx1;

            if (srcy < cy0) srcy = cy0;
            else if (srcy > cy1) srcy = cy1;
          }

        pidx = (gsize) (y - sroi->y) * sroi->width + (x - sroi->x);
        idx  = pidx * bpp;

        gegl_buffer_sample (input, srcx, srcy, NULL, px, fmt,
                            GEGL_ABYSS_NONE, GEGL_SAMPLER_LINEAR);

        if (zbuf != NULL)
          {
            /* pin depth: front-most opaque wins; transparent lets lower show */
            float za = 1.0f;
            if (alpha_from_px)
              {
                if (csize == 4)
                  memcpy (&za, px + aoff, sizeof za);
                else
                  za = (float) px[aoff] * 0.00392156862745098f;
              }
            else if (afmt != NULL)
              gegl_buffer_sample (input, srcx, srcy, NULL, &za, afmt,
                                  GEGL_ABYSS_NONE, GEGL_SAMPLER_LINEAR);

            if (za > 0.0f && tri_depth >= zbuf[pidx])
              {
                memcpy (tile + idx, px, bpp);
                zbuf[pidx] = tri_depth;
              }
            continue;
          }

        if (alpha_from_px)
          {
            float sa;

            if (csize == 4)
              memcpy (&sa, px + aoff, sizeof sa);
            else
              sa = (float) px[aoff] * 0.00392156862745098f;

            if (!(sa > 0.0f) || !isfinite ((double) sa))
              continue;

            if (sa > 1.0f)
              sa = 1.0f;

            if (sa < amask[pidx])
              continue;

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

/* preview rasterizer: ARGB32, premultiplied, bilinear */

static inline uint32_t
ld32 (const uint8_t *p)
{
  uint32_t v;
  memcpy (&v, p, sizeof v);
  return v;
}

static inline void
st32 (uint8_t *p, uint32_t v)
{
  memcpy (p, &v, sizeof v);
}

static inline uint32_t
get_pixel (const uint8_t *src, int stride, int sw, int sh, int x, int y)
{
  if ((unsigned) x >= (unsigned) sw || (unsigned) y >= (unsigned) sh)
    return 0u;

  return ld32 (src + (size_t) y * (size_t) stride + (size_t) x * 4u);
}

static inline uint32_t
sample_bilinear (const uint8_t *src, int stride, int sw, int sh,
                 double sx, double sy)
{
  double fx, fy;
  int ix, iy, tx, ty;
  int w00, w10, w01, w11;
  uint32_t c00, c10, c01, c11;
  uint32_t a, r, g, b;
  uint32_t sa, sr, sg, sb;

  if (!isfinite (sx) || !isfinite (sy))
    return 0u;

  if (sx < 0.0 || sx >= (double) sw || sy < 0.0 || sy >= (double) sh)
    return 0u;

  fx = sx - 0.5;
  fy = sy - 0.5;

  ix = (int) floor (fx);
  iy = (int) floor (fy);

  tx = (int) ((fx - (double) ix) * 256.0 + 0.5);
  ty = (int) ((fy - (double) iy) * 256.0 + 0.5);

  if (tx < 0) tx = 0;
  else if (tx > 256) tx = 256;

  if (ty < 0) ty = 0;
  else if (ty > 256) ty = 256;

  w00 = ((256 - tx) * (256 - ty)) >> 8;
  w10 = (tx * (256 - ty)) >> 8;
  w01 = ((256 - tx) * ty) >> 8;
  w11 = 256 - w00 - w10 - w01;
  if (w11 < 0)
    w11 = 0;

  if (ix >= 0 && ix + 1 < sw && iy >= 0 && iy + 1 < sh)
    {
      const uint8_t *row0 = src + (size_t) iy * (size_t) stride;
      const uint8_t *row1 = row0 + (size_t) stride;

      c00 = ld32 (row0 + (size_t) ix * 4u);
      c10 = ld32 (row0 + (size_t) (ix + 1) * 4u);
      c01 = ld32 (row1 + (size_t) ix * 4u);
      c11 = ld32 (row1 + (size_t) (ix + 1) * 4u);
    }
  else
    {
      c00 = get_pixel (src, stride, sw, sh, ix, iy);
      c10 = get_pixel (src, stride, sw, sh, ix + 1, iy);
      c01 = get_pixel (src, stride, sw, sh, ix, iy + 1);
      c11 = get_pixel (src, stride, sw, sh, ix + 1, iy + 1);
    }

  sa = 0;
  sr = 0;
  sg = 0;
  sb = 0;

  if (w00)
    {
      sa += (c00 >> 24) * (uint32_t) w00;
      sr += ((c00 >> 16) & 255u) * (uint32_t) w00;
      sg += ((c00 >> 8) & 255u) * (uint32_t) w00;
      sb += (c00 & 255u) * (uint32_t) w00;
    }

  if (w10)
    {
      sa += (c10 >> 24) * (uint32_t) w10;
      sr += ((c10 >> 16) & 255u) * (uint32_t) w10;
      sg += ((c10 >> 8) & 255u) * (uint32_t) w10;
      sb += (c10 & 255u) * (uint32_t) w10;
    }

  if (w01)
    {
      sa += (c01 >> 24) * (uint32_t) w01;
      sr += ((c01 >> 16) & 255u) * (uint32_t) w01;
      sg += ((c01 >> 8) & 255u) * (uint32_t) w01;
      sb += (c01 & 255u) * (uint32_t) w01;
    }

  if (w11)
    {
      sa += (c11 >> 24) * (uint32_t) w11;
      sr += ((c11 >> 16) & 255u) * (uint32_t) w11;
      sg += ((c11 >> 8) & 255u) * (uint32_t) w11;
      sb += (c11 & 255u) * (uint32_t) w11;
    }

  a = sa >> 8;
  r = sr >> 8;
  g = sg >> 8;
  b = sb >> 8;

  if (a > 255u) a = 255u;
  if (r > 255u) r = 255u;
  if (g > 255u) g = 255u;
  if (b > 255u) b = 255u;

  return (a << 24) | (r << 16) | (g << 8) | b;
}

static inline void
blend_over (uint8_t *dp, uint32_t src)
{
  uint32_t sa = src >> 24;
  uint32_t dst, inv, oa, cr, cg, cb;

  if (sa == 0u)
    return;

  dst = ld32 (dp);

  if (sa == 255u)
    {
      st32 (dp, src);
      return;
    }

  inv = 255u - sa;

  oa = sa + (((dst >> 24) * inv + 127u) / 255u);

  cr = ((src >> 16) & 255u) +
       ((((dst >> 16) & 255u) * inv + 127u) / 255u);

  cg = ((src >> 8) & 255u) +
       ((((dst >> 8) & 255u) * inv + 127u) / 255u);

  cb = (src & 255u) +
       (((dst & 255u) * inv + 127u) / 255u);

  if (oa > 255u) oa = 255u;
  if (cr > 255u) cr = 255u;
  if (cg > 255u) cg = 255u;
  if (cb > 255u) cb = 255u;

  st32 (dp, (oa << 24) | (cr << 16) | (cg << 8) | cb);
}

static int
warp_tri_fast_clip (const uint8_t *src, int sstride, int sw, int sh,
                    uint8_t *dst, int dstride, int dw, int dh,
                    double u0, double v0, double u1, double v1, double u2, double v2,
                    double x0, double y0, double x1, double y1, double x2, double y2,
                    double vsc,
                    int clip_y0, int clip_y1, int clamp_edges,
                    int tri_depth,
                    uint16_t *zbuf)
{
  double det = (u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0);
  double du, dv, dx, dy;
  double inv, a, b, c, d, e, f, sc, m_det;
  double minxu, maxxu, minyv, maxyv;
  double vsc2, min_det, needle_k;
  const double eps = 1e-4;
  const double eu[3] = { u0, u1, u2 };
  const double ev[3] = { v0, v1, v2 };
  int minx, maxx, miny, maxy, x, y;

  if (dw <= 0 || dh <= 0 || clip_y0 >= clip_y1)
    return 0;

  if (!(vsc > 0.0) || !isfinite (vsc))
    return 0;

  /*
   * u/v are screen-space destination coordinates.
   * x/y are source-space coordinates.
   *
   * Python fallback validates triangles in world space with:
   *   det >= 0.5
   *   dest_edge^2 <= 64 * source_edge^2
   *
   * Here det/edge are screen-space, so thresholds are scaled by vsc^2.
   * This keeps drag preview consistent with release/apply.
   */
  vsc2 = vsc * vsc;
  min_det = 0.5 * vsc2;
  needle_k = 64.0 * vsc2;

  /* Signed area only. Flipped triangles create the spike/tear artifact. */
  if (!isfinite (det) || det < min_det)
    return 0;

  if (!isfinite (u0) || !isfinite (v0) ||
      !isfinite (u1) || !isfinite (v1) ||
      !isfinite (u2) || !isfinite (v2) ||
      !isfinite (x0) || !isfinite (y0) ||
      !isfinite (x1) || !isfinite (y1) ||
      !isfinite (x2) || !isfinite (y2))
    return 0;

  du = u1 - u0; dv = v1 - v0; dx = x1 - x0; dy = y1 - y0;
  if (du * du + dv * dv > needle_k * (dx * dx + dy * dy))
    return 0;

  du = u2 - u1; dv = v2 - v1; dx = x2 - x1; dy = y2 - y1;
  if (du * du + dv * dv > needle_k * (dx * dx + dy * dy))
    return 0;

  du = u0 - u2; dv = v0 - v2; dx = x0 - x2; dy = y0 - y2;
  if (du * du + dv * dv > needle_k * (dx * dx + dy * dy))
    return 0;

  inv = 1.0 / det;

  a = inv * ((v2 - v0) * (x1 - x0) - (v1 - v0) * (x2 - x0));
  b = inv * ((v2 - v0) * (y1 - y0) - (v1 - v0) * (y2 - y0));
  c = inv * (-(u2 - u0) * (x1 - x0) + (u1 - u0) * (x2 - x0));
  d = inv * (-(u2 - u0) * (y1 - y0) + (u1 - u0) * (y2 - y0));
  e = x0 - a * u0 - c * v0;
  f = y0 - b * u0 - d * v0;

  sc = fabs (a);
  sc = fmax (sc, fabs (b));
  sc = fmax (sc, fabs (c));
  sc = fmax (sc, fabs (d));

  /* Convert screen-space affine magnitude back to world-space. */
  sc *= vsc;

  if (!(sc >= 1e-6 && sc <= 1e4))
    return 0;

  m_det = a * d - b * c;
  if (!isfinite (m_det) || fabs (m_det) < 1e-12)
    return 0;

  if (!isfinite (a) || !isfinite (b) || !isfinite (c) ||
      !isfinite (d) || !isfinite (e) || !isfinite (f))
    return 0;

  minxu = fmin (u0, fmin (u1, u2));
  maxxu = fmax (u0, fmax (u1, u2));
  minyv = fmin (v0, fmin (v1, v2));
  maxyv = fmax (v0, fmax (v1, v2));

  if (minxu < -1e9) minxu = -1e9;
  if (maxxu >  1e9) maxxu =  1e9;
  if (minyv < -1e9) minyv = -1e9;
  if (maxyv >  1e9) maxyv =  1e9;

  minx = (int) floor (minxu);
  maxx = (int) ceil  (maxxu);
  miny = (int) floor (minyv);
  maxy = (int) ceil  (maxyv);

  if (minx < 0) minx = 0;
  if (maxx >= dw) maxx = dw - 1;
  if (miny < clip_y0) miny = clip_y0;
  if (maxy >= clip_y1) maxy = clip_y1 - 1;

  if (minx > maxx || miny > maxy)
    return 0;

  for (y = miny; y <= maxy; y++)
    {
      double fy = (double) y + 0.5;
      double xs[3];
      int nx = 0;
      double low, high;
      int xstart, xend;

      for (int edge = 0; edge < 3; edge++)
        {
          int next = (edge == 2) ? 0 : edge + 1;
          double ya = ev[edge];
          double yb = ev[next];
          double xa = eu[edge];
          double xb = eu[next];

          if (((ya <= fy && fy < yb) || (yb <= fy && fy < ya)) &&
              fabs (yb - ya) > 1e-12)
            {
              if (nx < 3)
                xs[nx++] = xa + (fy - ya) * (xb - xa) / (yb - ya);
            }
        }

      if (nx < 2)
        continue;

      for (int i = 1; i < nx; i++)
        {
          double key = xs[i];
          int j = i - 1;
          while (j >= 0 && xs[j] > key)
            {
              xs[j + 1] = xs[j];
              j--;
            }
          xs[j + 1] = key;
        }

      low  = xs[0];
      high = xs[nx - 1];

      if (low > high)
        continue;

      if (low < -1e9) low = -1e9;
      if (high >  1e9) high =  1e9;

      xstart = (int) ceil (low - 0.5 - eps);
      xend   = (int) floor (high - 0.5 + eps);

      if (xstart < minx) xstart = minx;
      if (xend > maxx) xend = maxx;

      if (xstart > xend)
        continue;

      {
        double fx = (double) xstart + 0.5;
        double srcx = a * fx + c * fy + e;
        double srcy = b * fx + d * fy + f;
        double sx_s, sy_s;
        uint8_t *row = dst + (size_t) y * (size_t) dstride;

        for (x = xstart; x <= xend; x++)
          {
            sx_s = srcx;
            sy_s = srcy;

            /* smear the border like EXTEND_PAD / the apply clamp */
            if (clamp_edges)
              {
                if (sx_s < 0.5) sx_s = 0.5;
                else if (sx_s > (double) sw - 0.5) sx_s = (double) sw - 0.5;

                if (sy_s < 0.5) sy_s = 0.5;
                else if (sy_s > (double) sh - 0.5) sy_s = (double) sh - 0.5;
              }

            uint32_t col = sample_bilinear (src, sstride, sw, sh, sx_s, sy_s);
            if (col)
              {
                if (zbuf != NULL)
                  {
                    /* Depth preview: front-most opaque wins, transparent shows lower. */
                    size_t zidx = (size_t) (y - clip_y0) * (size_t) dw + (size_t) x;
                    int enc = tri_depth + 1000;
                    uint16_t zv;

                    if (enc < 1) enc = 1;
                    if (enc > 65534) enc = 65534;
                    zv = (uint16_t) enc;

                    if (zv >= zbuf[zidx])
                      {
                        st32 (row + (size_t) x * 4u, col);
                        zbuf[zidx] = zv;
                      }
                  }
                else
                  blend_over (row + (size_t) x * 4u, col);
              }

            srcx += a;
            srcy += b;
          }
      }
    }

  return 1;
}

static int
pw_render_preview_view_impl (void *src, int sw, int sh, int sstride,
                             void *dst, int dw, int dh, int dstride,
                             int cols, int rows,
                             const float *verts, int nverts,
                             float px, float py, float pw, float ph,
                             float view_ox, float view_oy, float view_scale,
                             int clamp_edges,
                             int depth_on,
                             const int *vdepth)
{
  int draw = 0;
  int r, c;
  double invc, invr;
  double vox, voy, vsc;
  double cell_w, cell_h;
  int nbands = 1;
  int band_h;
  int b;

  if (!src || !dst || !verts || cols < 1 || rows < 1)
    return 0;

  if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
    return 0;

  if (!(view_scale > 0.0f) || !isfinite ((double) view_scale))
    return 0;

  if ((long long) (cols + 1) * (rows + 1) > (long long) nverts)
    return 0;

  if (depth_on && !vdepth)
    return 0;

  memset (dst, 0, (size_t) dh * (size_t) dstride);

  invc = 1.0 / (double) cols;
  invr = 1.0 / (double) rows;

  vox = (double) view_ox;
  voy = (double) view_oy;
  vsc = (double) view_scale;

  cell_w = (double) pw * invc;
  cell_h = (double) ph * invr;

#ifdef _OPENMP
  nbands = omp_get_max_threads ();
  if (nbands < 1)
    nbands = 1;

  if (nbands > 16)
    nbands = 16;

  {
    int min_band = 16;
    if (nbands * min_band > dh)
      nbands = (dh + min_band - 1) / min_band;

    if (nbands < 1)
      nbands = 1;
  }
#endif

  band_h = (dh + nbands - 1) / nbands;
  if (band_h < 1)
    band_h = 1;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) reduction(+:draw) private(r, c)
#endif
  for (b = 0; b < nbands; b++)
    {
      int y0 = b * band_h;
      int y1 = y0 + band_h;

      if (y1 > dh)
        y1 = dh;

      if (y0 >= y1)
        continue;

      uint16_t *zbuf = NULL;
      if (depth_on)
        {
          zbuf = calloc ((size_t) (y1 - y0) * (size_t) dw, sizeof (uint16_t));
          if (!zbuf)
            continue;
        }

      for (r = 0; r < rows; r++)
        {
          double sv0 = (double) py + (double) r * cell_h;
          double sv2 = sv0 + cell_h;
          int row_off = r * (cols + 1);

          for (c = 0; c < cols; c++)
            {
              int i00 = row_off + c;
              int i10 = i00 + 1;
              int i01 = i00 + (cols + 1);
              int i11 = i01 + 1;

              double su0 = (double) px + (double) c * cell_w;
              double su1 = su0 + cell_w;
              double su2 = su0;
              double su3 = su1;

              double wu0 = (double) verts[2 * i00];
              double wv0 = (double) verts[2 * i00 + 1];
              double wu1 = (double) verts[2 * i10];
              double wv1 = (double) verts[2 * i10 + 1];
              double wu2 = (double) verts[2 * i01];
              double wv2 = (double) verts[2 * i01 + 1];
              double wu3 = (double) verts[2 * i11];
              double wv3 = (double) verts[2 * i11 + 1];

              double du0 = (wu0 - vox) * vsc;
              double dv0 = (wv0 - voy) * vsc;
              double du1 = (wu1 - vox) * vsc;
              double dv1 = (wv1 - voy) * vsc;
              double du2 = (wu2 - vox) * vsc;
              double dv2 = (wv2 - voy) * vsc;
              double du3 = (wu3 - vox) * vsc;
              double dv3 = (wv3 - voy) * vsc;

              double qminx = fmin (fmin (du0, du1), fmin (du2, du3));
              double qmaxx = fmax (fmax (du0, du1), fmax (du2, du3));
              double qminy = fmin (fmin (dv0, dv1), fmin (dv2, dv3));
              double qmaxy = fmax (fmax (dv0, dv1), fmax (dv2, dv3));

              if (qmaxx < 0.0 || qminx >= (double) dw ||
                  qmaxy < (double) y0 || qminy >= (double) y1)
                continue;

              int td0 = 0, td1 = 0;
              if (depth_on)
                {
                  int a0 = vdepth[i00], a1 = vdepth[i10], a2 = vdepth[i01];
                  int b0 = vdepth[i10], b1 = vdepth[i11], b2 = vdepth[i01];
                  td0 = a0 >= a1 && a0 >= a2 ? a0 : (a1 >= a2 ? a1 : a2);
                  td1 = b0 >= b1 && b0 >= b2 ? b0 : (b1 >= b2 ? b1 : b2);
                }

              draw += warp_tri_fast_clip ((const uint8_t *) src, sstride, sw, sh,
                                          (uint8_t *) dst, dstride, dw, dh,
                                          du0, dv0, du1, dv1, du2, dv2,
                                          su0, sv0, su1, sv0, su2, sv2,
                                          vsc,
                                          y0, y1, clamp_edges,
                                          td0, zbuf);

              draw += warp_tri_fast_clip ((const uint8_t *) src, sstride, sw, sh,
                                          (uint8_t *) dst, dstride, dw, dh,
                                          du1, dv1, du3, dv3, du2, dv2,
                                          su1, sv0, su3, sv2, su2, sv2,
                                          vsc,
                                          y0, y1, clamp_edges,
                                          td1, zbuf);
            }
        }

      if (zbuf)
        free (zbuf);
    }

  return draw;
}

PW_EXPORT int
pw_render_preview_view (void *src, int sw, int sh, int sstride,
                        void *dst, int dw, int dh, int dstride,
                        int cols, int rows,
                        const float *verts, int nverts,
                        float px, float py, float pw, float ph,
                        float view_ox, float view_oy, float view_scale,
                        int clamp_edges)
{
  return pw_render_preview_view_impl (src, sw, sh, sstride,
                                      dst, dw, dh, dstride,
                                      cols, rows,
                                      verts, nverts,
                                      px, py, pw, ph,
                                      view_ox, view_oy, view_scale,
                                      clamp_edges, 0, NULL);
}

PW_EXPORT int
pw_render_preview_view_depth (void *src, int sw, int sh, int sstride,
                              void *dst, int dw, int dh, int dstride,
                              int cols, int rows,
                              const float *verts, int nverts,
                              float px, float py, float pw, float ph,
                              float view_ox, float view_oy, float view_scale,
                              int clamp_edges,
                              int depth_on,
                              const int *vdepth)
{
  return pw_render_preview_view_impl (src, sw, sh, sstride,
                                      dst, dw, dh, dstride,
                                      cols, rows,
                                      verts, nverts,
                                      px, py, pw, ph,
                                      view_ox, view_oy, view_scale,
                                      clamp_edges, depth_on, vdepth);
}

PW_EXPORT int
pw_render_preview (void *src, int sw, int sh, int sstride,
                   void *dst, int dw, int dh, int dstride,
                   int cols, int rows,
                   const float *verts, int nverts,
                   float px, float py, float pw, float ph)
{
  return pw_render_preview_view (src, sw, sh, sstride,
                                 dst, dw, dh, dstride,
                                 cols, rows,
                                 verts, nverts,
                                 px, py, pw, ph,
                                 0.0f, 0.0f, 1.0f, 0);
}

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
  int extend = json_int (json, "extend", 0);
  int pad_l = json_int (json, "pad_left", 0);
  int pad_t = json_int (json, "pad_top", 0);
  int pad_r = json_int (json, "pad_right", 0);
  int pad_b = json_int (json, "pad_bottom", 0);
  int ow_i = json_int (json, "orig_width", 0);
  int oh_i = json_int (json, "orig_height", 0);
  float ow;
  float oh;
  float pad_sx = 0.0f;
  float pad_sy = 0.0f;
  gboolean clamp_src = FALSE;
  double cx0 = 0.0, cy0 = 0.0, cx1 = 0.0, cy1 = 0.0;

  if (pad_l < 0) pad_l = 0;
  if (pad_t < 0) pad_t = 0;
  if (pad_r < 0) pad_r = 0;
  if (pad_b < 0) pad_b = 0;

  if (ow_i > 0)
    ow = (float) ow_i;
  else
    ow = (float) (roi->width - pad_l - pad_r);

  if (oh_i > 0)
    oh = (float) oh_i;
  else
    oh = (float) (roi->height - pad_t - pad_b);

  if (ow <= 0.0f)
    ow = (float) (roi->width - pad_l - pad_r);
  if (oh <= 0.0f)
    oh = (float) (roi->height - pad_t - pad_b);
  if (ow <= 0.0f) ow = (float) roi->width;
  if (oh <= 0.0f) oh = (float) roi->height;
  float *pins = g_malloc (256 * 4 * sizeof (float));
  float *cw   = g_malloc (64 * 64 * sizeof (float));
  float *prot = g_malloc (256 * sizeof (float));
  float *pisc = g_malloc (256 * sizeof (float));
  float *ptap = g_malloc (256 * sizeof (float));
  float *pdep = g_malloc (256 * sizeof (float));
  int npins = json_floats (json, "pins_flat", pins, 256 * 4) / 4;
  int ncw   = json_floats (json, "mask_flat", cw, 64 * 64);
  int nrot  = json_floats (json, "pin_rot", prot, 256);
  int nsc   = json_floats (json, "pin_scale", pisc, 256);
  int ntap  = json_floats (json, "pin_taper", ptap, 256);
  int ndep  = json_floats (json, "pin_depth", pdep, 256);
  int nverts;
  gdouble s;
  const Babl *fmt;
  const Babl *afmt = NULL;
  gint bpp;
  gboolean has_alpha;
  gboolean alpha_from_px = FALSE;
  int csz = 0, aoff = 0;
  float *dxs, *dys;
  int sy0, r, c;
  int hold_writer = 0;
  double det_min;

  pw_ensure_lock ();

  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols > 64) cols = 64;
  if (rows > 64) rows = 64;

  nverts = (cols + 1) * (rows + 1);
  s = 1.0 / (1 << level);

  if (getenv ("PW_DEBUG"))
    fprintf (stderr,
             "[puppet-warp] pins=%d nrot=%d nsc=%d ncw=%d roi=%dx%d level=%d\n",
             npins, nrot, nsc, ncw, roi->width, roi->height, level);

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
          int vert_src = 0;
          int rr, cc, ii;

          for (rr = 0; rr <= rows; rr++)
            for (cc = 0; cc <= cols; cc++)
              {
                dx[rr * (cols + 1) + cc] = (float) cc * ow / cols;
                dy[rr * (cols + 1) + cc] = (float) rr * oh / rows;
              }

          /* json verts: verts_flat is the only embedded source */
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

          /* finally solve from pins */
          if (vert_src == 0)
            {
              void *grid = arap_grid_new (0.f, 0.f, ow, oh, cols, rows);

              if (grid)
                {
                  if (ncw == cols * rows && arap_grid_set_cell_weights)
                    arap_grid_set_cell_weights (grid, cw, ncw);

                  if (pin_edge && arap_grid_set_pin_edge)
                    arap_grid_set_pin_edge (grid, 1);

                  for (ii = 0; ii < npins; ii++)
                    {
                      float th = (rotate_on && ii < nrot) ? prot[ii] : 0.f;
                      float sc = (rotate_on && ii < nsc) ? pisc[ii] : 1.0f;
                      float tp = (ii < ntap) ? ptap[ii] : 0.f;

                      if (arap_grid_add_pin_taper)
                        arap_grid_add_pin_taper (grid,
                                                 pins[ii * 4],
                                                 pins[ii * 4 + 1],
                                                 pins[ii * 4 + 2],
                                                 pins[ii * 4 + 3],
                                                 th,
                                                 sc,
                                                 tp);
                      else if ((th != 0.f || sc != 1.f) && arap_grid_add_pin_sim)
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

          /* pin depth: per-vertex = nearest pin (rest pos), tie lowest idx */
          {
            int depth_any = 0;
            int *vd;
            int rr2, cc2, vi, pi;
            float cellw = ow / (float) cols;
            float cellh = oh / (float) rows;

            g_free (pw_cache_vdepth);
            pw_cache_vdepth = NULL;
            pw_cache_depth_on = 0;

            for (pi = 0; pi < npins; pi++)
              if ((int) pdep[pi] != 0) { depth_any = 1; break; }

            if (depth_any)
              {
                vd = g_new (int, nverts);
                vi = 0;
                for (rr2 = 0; rr2 <= rows; rr2++)
                  for (cc2 = 0; cc2 <= cols; cc2++, vi++)
                    {
                      float vx = (float) cc2 * cellw;
                      float vy = (float) rr2 * cellh;
                      float best = -1.0f;
                      int bdep = 0;
                      for (pi = 0; pi < npins; pi++)
                        {
                          float ddx = vx - pins[pi * 4];
                          float ddy = vy - pins[pi * 4 + 1];
                          float d2 = ddx * ddx + ddy * ddy;
                          if (best < 0.0f || d2 < best)
                            {
                              best = d2;
                              bdep = (int) pdep[pi];
                            }
                        }
                      vd[vi] = bdep;
                    }
                pw_cache_vdepth = vd;
                pw_cache_depth_on = 1;
              }
          }

          /* shift cached verts into padded layer space */
          pad_sx = (float) pad_l * s;
          pad_sy = (float) pad_t * s;

          for (ii = 0; ii < nverts; ii++)
            {
              dx[ii] = (float) (dx[ii] * s + pad_sx);
              dy[ii] = (float) (dy[ii] * s + pad_sy);
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

  if (!pw_cache_dx || !pw_cache_dy)
    {
      if (hold_writer)
        g_rw_lock_writer_unlock (&pw_solve_lock);
      else
        g_rw_lock_reader_unlock (&pw_solve_lock);

      g_free (pins);
      g_free (cw);
      g_free (prot);
      g_free (pisc);
      g_free (ptap);
      g_free (pdep);
      return FALSE;
    }

  dxs = pw_cache_dx;
  dys = pw_cache_dy;
  ow  = pw_cache_ow;
  oh  = pw_cache_oh;

  det_min = 0.5 * s * s;

  /* Pad is only an apply offset; pins/verts stay in original coordinates. */
  pad_sx = (float) pad_l * s;
  pad_sy = (float) pad_t * s;
  clamp_src = (extend != 0);
  cx0 = pad_sx + 0.5 * s;
  cy0 = pad_sy + 0.5 * s;
  cx1 = pad_sx + ow - 0.5 * s;
  cy1 = pad_sy + oh - 0.5 * s;

  if (!(cx1 > cx0))
    cx0 = cx1 = pad_sx + ow * 0.5;
  if (!(cy1 > cy0))
    cy0 = cy1 = pad_sy + oh * 0.5;

  int don = pw_cache_depth_on;
  int *vdep = don ? pw_cache_vdepth : NULL;

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

      {
        int *zbuf = NULL;
        if (don)
          {
            gsize zi;
            zbuf = g_new (int, npix);
            for (zi = 0; zi < npix; zi++)
              zbuf[zi] = -1000000;
          }

        for (r = 0; r < rows; r++)
          for (c = 0; c < cols; c++)
            {
              int i00 = r * (cols + 1) + c;
              int i10 = i00 + 1;
              int i01 = i00 + (cols + 1);
              int i11 = i01 + 1;

              float x0 = (float) c       * ow / cols + pad_sx;
              float y0 = (float) r       * oh / rows + pad_sy;
              float x1 = (float) (c + 1) * ow / cols + pad_sx;
              float y1 = y0;
              float x2 = x0;
              float y2 = (float) (r + 1) * oh / rows + pad_sy;
              float x3 = x1;
              float y3 = y2;

              int td0 = 0, td1 = 0;
              if (don)
                {
                  int a0 = vdep[i00], a1 = vdep[i10], a2 = vdep[i01];
                  int b0 = vdep[i10], b1 = vdep[i11], b2 = vdep[i01];
                  td0 = a0 >= a1 && a0 >= a2 ? a0 : (a1 >= a2 ? a1 : a2);
                  td1 = b0 >= b1 && b0 >= b2 ? b0 : (b1 >= b2 ? b1 : b2);
                }

              warp_tri (input, fmt, bpp, afmt, amask,
                        alpha_from_px, csz, aoff,
                        tile, &band,
                        dxs[i00], dys[i00],
                        dxs[i10], dys[i10],
                        dxs[i01], dys[i01],
                        x0, y0,
                        x1, y1,
                        x2, y2,
                        clamp_src, cx0, cy0, cx1, cy1,
                        det_min, td0, zbuf);

              warp_tri (input, fmt, bpp, afmt, amask,
                        alpha_from_px, csz, aoff,
                        tile, &band,
                        dxs[i10], dys[i10],
                        dxs[i11], dys[i11],
                        dxs[i01], dys[i01],
                        x1, y1,
                        x3, y3,
                        x2, y2,
                        clamp_src, cx0, cy0, cx1, cy1,
                        det_min, td1, zbuf);
            }

        if (zbuf)
          g_free (zbuf);
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

  g_free (pins);
  g_free (cw);
  g_free (prot);
  g_free (pisc);
  g_free (ptap);
  g_free (pdep);

  return TRUE;
}

static void
puppet_warp_class_init (PuppetWarpClass *klass, gpointer class_data)
{
  GObjectClass *oc = G_OBJECT_CLASS (klass);
  GeglOperationClass *opc = GEGL_OPERATION_CLASS (klass);
  GeglOperationFilterClass *fc = GEGL_OPERATION_FILTER_CLASS (klass);

  (void) class_data;

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
    "name",         "custom:puppet-warp",
    "title",        "Puppet Warp (ARAP)",
    "categories",   "deform",
    "description",  "ARAP-based puppet warp (inverse mapped triangles, transparent holes)",
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
  (void) module;
  return &info;
}

gboolean
gegl_module_register (GTypeModule *module)
{
  pw_ensure_lock ();
  pw_module = module;
  g_type_module_set_name (module, "puppet-warp");
  return puppet_warp_get_type () != 0;
}

gboolean
gegl_module_unregister (GTypeModule *module)
{
  (void) module;
  return TRUE;
}
