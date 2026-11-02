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

extern void      arap_grid_set_cell_weights (void *g, const float *w, int n);
extern void      arap_grid_set_pin_edge (void *g, int on);
extern void      arap_grid_add_pin_sim (void *g, float sx, float sy, float ddx, float ddy,
                                        float theta, float scale);
extern void      arap_grid_add_pin_taper (void *g, float sx, float sy, float ddx, float ddy,
                                          float theta, float scale, float taper);

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

  if (!v)
    return dflt;

  while (*v == ' ' || *v == '\t')
    v++;

  /* Python json.dumps writes booleans as true/false. */
  if (strncmp (v, "true", 4) == 0)
    return 1;
  if (strncmp (v, "false", 5) == 0)
    return 0;

  return (int) strtol (v, NULL, 10);
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

/* Cache key: only the fields that change the solved mesh or its depth.
   Display-only settings (mesh colour/style, background mode) are left out,
   so changing them does not trigger a new solve. Arrays are copied verbatim
   from the JSON text, which keeps the key exact. */
static gchar *
geom_key (const char *json)
{
  static const char *const keys[] = {
    "cols", "rows", "pin_edge", "rotate_on", "extend",
    "pad_left", "pad_top", "pad_right", "pad_bottom",
    "orig_width", "orig_height",
    "pins_flat", "mask_flat", "verts_flat",
    "pin_rot", "pin_scale", "pin_taper", "pin_depth",
    NULL
  };
  GString *s = g_string_new (NULL);
  int i;

  for (i = 0; keys[i]; i++)
    {
      const char *v = find_key (json, keys[i]);
      const char *p;
      int depth = 0;

      g_string_append (s, keys[i]);
      g_string_append_c (s, ':');

      if (!v)
        {
          g_string_append_c (s, ';');
          continue;
        }

      while (*v == ' ' || *v == '\t')
        v++;

      for (p = v; *p; p++)
        {
          if (*p == '[')
            depth++;
          else if (*p == ']')
            {
              depth--;
              if (depth == 0)
                {
                  p++;
                  break;
                }
            }
          else if (depth == 0 && (*p == ',' || *p == '}'))
            break;
        }

      g_string_append_len (s, v, p - v);
      g_string_append_c (s, ';');
    }

  return g_string_free (s, FALSE);
}

/* PW_DEBUG is read once; getenv in the render path is wasteful. */
static gboolean
pw_debug_on (void)
{
  static gsize once = 0;
  static gboolean on = FALSE;

  if (g_once_init_enter (&once))
    {
      on = getenv ("PW_DEBUG") != NULL;
      g_once_init_leave (&once, 1);
    }

  return on;
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

/* Needle test: true if any edge stretches more than sqrt(k) times its
   source length. u/v are destination, x/y are source corners. */
static int
pw_is_needle (const double *u, const double *v,
              const double *x, const double *y, double k)
{
  int e;

  for (e = 0; e < 3; e++)
    {
      int n = (e + 1) % 3;
      double du = u[n] - u[e];
      double dv = v[n] - v[e];
      double dx = x[n] - x[e];
      double dy = y[n] - y[e];

      if (du * du + dv * dv > k * (dx * dx + dy * dy))
        return 1;
    }

  return 0;
}

static int
pw_needle (double u0, double v0, double u1, double v1, double u2, double v2,
           double x0, double y0, double x1, double y1, double x2, double y2)
{
  return 0; /* TẠM THỜI VÔ HIỆU HÓA */

  const double u[3] = { u0, u1, u2 }, v[3] = { v0, v1, v2 };
  const double x[3] = { x0, x1, x2 }, y[3] = { y0, y1, y2 };

  return pw_is_needle (u, v, x, y, 64.0);
}

#define PW_BAND_H 128

/* Source pixels for one band, fetched once from GEGL as RGBA float. */
typedef struct
{
  const float   *buf;    /* NULL when no source data is available */
  GeglRectangle  rect;   /* area of the input that buf covers */
} PwSrc;

static float *
pw_fetch (GeglBuffer *input, const GeglRectangle *r)
{
  float *buf = g_new (float, (gsize) r->width * (gsize) r->height * 4);

  gegl_buffer_get (input, r, 1.0, babl_format ("RGBA float"), buf,
                   GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);

  /* Premultiply so transparent pixels cannot bleed their colour into edges. */
  {
    gsize n = (gsize) r->width * (gsize) r->height;
    gsize i;

    for (i = 0; i < n; i++)
      {
        float *q = buf + i * 4;

        q[0] *= q[3];
        q[1] *= q[3];
        q[2] *= q[3];
      }
  }

  return buf;
}

/* Bilinear sample from PwSrc (premultiplied source), same pixel-centre
   convention as GEGL_SAMPLER_LINEAR. Returns straight RGBA. Outside the
   fetched area the result is transparent. */
static void
pw_sample (const PwSrc *src, double sx, double sy, float out[4])
{
  double fx, fy, tx, ty;
  double acc[4] = { 0.0, 0.0, 0.0, 0.0 };
  int ix, iy, i, j, k;

  out[0] = out[1] = out[2] = out[3] = 0.0f;

  if (!src->buf || !isfinite (sx) || !isfinite (sy))
    return;

  fx = sx - 0.5;
  fy = sy - 0.5;
  ix = (int) floor (fx);
  iy = (int) floor (fy);
  tx = fx - ix;
  ty = fy - iy;

  for (j = 0; j < 2; j++)
    for (i = 0; i < 2; i++)
      {
        int px = ix + i - src->rect.x;
        int py = iy + j - src->rect.y;
        double w = (i ? tx : 1.0 - tx) * (j ? ty : 1.0 - ty);
        const float *p;

        if (w <= 0.0)
          continue;

        if (px < 0) px = 0;
        else if (px >= src->rect.width) px = src->rect.width - 1;

        if (py < 0) py = 0;
        else if (py >= src->rect.height) py = src->rect.height - 1;

        p = src->buf + ((size_t) py * (size_t) src->rect.width + (size_t) px) * 4;

        for (k = 0; k < 4; k++)
          acc[k] += w * p[k];
      }

  if (acc[3] > 0.0)
    {
      /* back to straight colour; alpha is the weighted coverage */
      out[0] = (float) (acc[0] / acc[3]);
      out[1] = (float) (acc[1] / acc[3]);
      out[2] = (float) (acc[2] / acc[3]);
      out[3] = (float) acc[3];
    }
}

static gboolean
pw_band_source (const float *dxs, const float *dys, int cols, int rows,
                float ow, float oh, float pad_sx, float pad_sy,
                const GeglRectangle *band, const GeglRectangle *ext,
                gboolean clamp_src,
                double cx0, double cy0, double cx1, double cy1,
                GeglRectangle *out)
{
  double minx = 1e300, miny = 1e300, maxx = -1e300, maxy = -1e300;
  gboolean any = FALSE;
  int r, c;

  for (r = 0; r < rows; r++)
    for (c = 0; c < cols; c++)
      {
        int i00 = r * (cols + 1) + c;
        int i10 = i00 + 1;
        int i01 = i00 + (cols + 1);
        int i11 = i01 + 1;
        double bx0 = fmin (fmin (dxs[i00], dxs[i10]), fmin (dxs[i01], dxs[i11]));
        double bx1 = fmax (fmax (dxs[i00], dxs[i10]), fmax (dxs[i01], dxs[i11]));
        double by0 = fmin (fmin (dys[i00], dys[i10]), fmin (dys[i01], dys[i11]));
        double by1 = fmax (fmax (dys[i00], dys[i10]), fmax (dys[i01], dys[i11]));
        double sx0, sx1, sy0, sy1;

        if (bx1 < band->x || bx0 > band->x + band->width ||
            by1 < band->y || by0 > band->y + band->height)
          continue;

        sx0 = (double) c * ow / cols + pad_sx;
        sx1 = (double) (c + 1) * ow / cols + pad_sx;
        sy0 = (double) r * oh / rows + pad_sy;
        sy1 = (double) (r + 1) * oh / rows + pad_sy;

        minx = fmin (minx, sx0);
        miny = fmin (miny, sy0);
        maxx = fmax (maxx, sx1);
        maxy = fmax (maxy, sy1);

        /* No clamp_src union here: inverse-mapped points of a triangle always
           lie inside its source triangle, so the cell's own source rect is
           enough. Unioning the whole content made every band fetch the
           entire image when Extend was on. */
        (void) clamp_src; (void) cx0; (void) cy0; (void) cx1; (void) cy1;

        any = TRUE;
      }

  if (!any)
    return FALSE;

  {
    GeglRectangle want;

    want.x = (int) floor (minx) - 2;
    want.y = (int) floor (miny) - 2;
    want.width = (int) ceil (maxx) - want.x + 2;
    want.height = (int) ceil (maxy) - want.y + 2;

    if (want.width <= 0 || want.height <= 0)
      return FALSE;
      
    return gegl_rectangle_intersect (out, &want, ext);
  }
}

/* apply path: inverse map one triangle into a float RGBA tile */
static void
warp_tri (const PwSrc *src, float *tile, const GeglRectangle *band,
          float u0, float v0, float u1, float v1, float u2, float v2,
          float x0, float y0, float x1, float y1, float x2, float y2,
          gboolean clamp_src,
          double cx0, double cy0, double cx1, double cy1,
          double det_min, int tri_depth, int *zbuf)
{
  double du0 = u0, dv0 = v0, du1 = u1, dv1 = v1, du2 = u2, dv2 = v2;
  double sx0 = x0, sy0 = y0, sx1 = x1, sy1 = y1, sx2 = x2, sy2 = y2;
  double det = (du1 - du0) * (dv2 - dv0) - (dv1 - dv0) * (du2 - du0);
  double inv, a, b, c, d, e, f, sc, m_det;
  double e0x, e0y, e1x, e1y, e2x, e2y;
  const double eps = 1e-3;
  int minx, maxx, miny, maxy, x, y;

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

  if (!isfinite (sc) || sc > 1e9 || sc < 1e-6) /* Nới lỏng giới hạn lên 1e9 */
    return;

  m_det = a * d - b * c;
  if (!isfinite (m_det) || fabs (m_det) < 1e-12)
    return;

  minx = (int) floor (fmin (du0, fmin (du1, du2)));
  maxx = (int) ceil  (fmax (du0, fmax (du1, du2)));
  miny = MAX ((int) floor (fmin (dv0, fmin (dv1, dv2))), band->y);
  maxy = MIN ((int) ceil  (fmax (dv0, fmax (dv1, dv2))),
              band->y + band->height - 1);

  minx = MAX (minx, band->x);
  maxx = MIN (maxx, band->x + band->width - 1);

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
        gsize pidx;
        gsize idx;
        double srcx, srcy;
        float px[4];

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

        pidx = (gsize) (y - band->y) * (gsize) band->width + (gsize) (x - band->x);
        idx  = pidx * 4;

        pw_sample (src, srcx, srcy, px);

        if (zbuf != NULL)
          {
            /* pin depth: front-most opaque wins; transparent lets lower show */
            if (px[3] > 0.0f && tri_depth >= zbuf[pidx])
              {
                memcpy (tile + idx, px, sizeof px);
                zbuf[pidx] = tri_depth;
              }
            continue;
          }

        if (!(px[3] > 0.0f) || !isfinite ((double) px[3]))
          continue;

        if (px[3] > 1.0f)
          px[3] = 1.0f;

        /* highest alpha wins, as the old per-pixel mask did */
        if (px[3] < tile[idx + 3])
          continue;

        memcpy (tile + idx, px, sizeof px);
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
  double inv, a, b, c, d, e, f, sc, m_det;
  double minxu, maxxu, minyv, maxyv;
  double vsc2, min_det, needle_k;
  const double eps = 1e-4;
  const double eu[3] = { u0, u1, u2 };
  const double ev[3] = { v0, v1, v2 };
  const double ex[3] = { x0, x1, x2 };
  const double ey[3] = { y0, y1, y2 };
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

  if (pw_is_needle (eu, ev, ex, ey, needle_k))
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

  if (!(sc >= 1e-6 && sc <= 1e9)) /* Nới lỏng giới hạn lên 1e9 */
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

static gboolean
puppet_warp_process (GeglOperation       *operation,
                     GeglBuffer          *input,
                     GeglBuffer          *output,
                     const GeglRectangle *roi,
                     gint                 level)
{
  PuppetWarp *self = (PuppetWarp *) operation;
  const char *json = self->params_json ? self->params_json : "{}";
  gchar *gkey = geom_key (json);
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
  json_floats (json, "pin_depth", pdep, 256);
  int nverts;
  gdouble s;
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
  /* level is a mip level; clamp so the shift stays well defined */
  s = ldexp (1.0, -(level < 0 ? 0 : (level > 30 ? 30 : level)));

  if (pw_debug_on ())
    fprintf (stderr,
             "[puppet-warp] pins=%d nrot=%d nsc=%d ncw=%d roi=%dx%d level=%d\n",
             npins, nrot, nsc, ncw, roi->width, roi->height, level);

  g_rw_lock_reader_lock (&pw_solve_lock);

  if (!(pw_cache_json != NULL &&
        strcmp (pw_cache_json, gkey) == 0 &&
        pw_cache_level == level &&
        pw_cache_nverts == nverts))
    {
      g_rw_lock_reader_unlock (&pw_solve_lock);
      g_rw_lock_writer_lock (&pw_solve_lock);
      hold_writer = 1;

      if (!(pw_cache_json != NULL &&
            strcmp (pw_cache_json, gkey) == 0 &&
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
                  if (ncw == cols * rows)
                    arap_grid_set_cell_weights (grid, cw, ncw);

                  if (pin_edge)
                    arap_grid_set_pin_edge (grid, 1);

                  for (ii = 0; ii < npins; ii++)
                    {
                      float th = (rotate_on && ii < nrot) ? prot[ii] : 0.f;
                      float sc = (rotate_on && ii < nsc) ? pisc[ii] : 1.0f;
                      float tp = (ii < ntap) ? ptap[ii] : 0.f;

                      arap_grid_add_pin_taper (grid,
                                               pins[ii * 4],
                                               pins[ii * 4 + 1],
                                               pins[ii * 4 + 2],
                                               pins[ii * 4 + 3],
                                               th, sc, tp);
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

          if (pw_debug_on ())
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

          pw_cache_json   = g_strdup (gkey);
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
      g_free (gkey);
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
  GeglRectangle ext = *gegl_buffer_get_extent (input);

  if (pw_debug_on ())
    fprintf (stderr,
             "[pw-ext] roi=%d,%d %dx%d ext=%d,%d %dx%d pad=%d,%d ow=%.0f oh=%.0f\n",
             roi->x, roi->y, roi->width, roi->height,
             ext.x, ext.y, ext.width, ext.height,
             pad_l, pad_t, (double) ow, (double) oh);

  for (sy0 = roi->y; sy0 < roi->y + roi->height; sy0 += PW_BAND_H)
    {
      GeglRectangle band;
      GeglRectangle need;
      PwSrc src;
      float *band_src = NULL;
      float *tile;
      gsize npix;

      band.x = roi->x;
      band.y = sy0;
      band.width = roi->width;
      band.height = MIN (PW_BAND_H, roi->y + roi->height - sy0);

      npix = (gsize) band.width * (gsize) band.height;
      tile = g_new0 (float, npix * 4);

      if (pw_band_source (dxs, dys, cols, rows, ow, oh, pad_sx, pad_sy,
                          &band, &ext, clamp_src, cx0, cy0, cx1, cy1, &need))
        {
          band_src = pw_fetch (input, &need);
          src.buf = band_src;
          src.rect = need;
        }
      else
        {
          src.buf = NULL;

          if (pw_debug_on ())
            fprintf (stderr,
                     "[pw-nosrc] band=%d,%d %dx%d ext=%d,%d %dx%d\n",
                     band.x, band.y, band.width, band.height,
                     ext.x, ext.y, ext.width, ext.height);
        }

      if (src.buf)
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

                warp_tri (&src, tile, &band,
                          dxs[i00], dys[i00],
                          dxs[i10], dys[i10],
                          dxs[i01], dys[i01],
                          x0, y0,
                          x1, y1,
                          x2, y2,
                          clamp_src, cx0, cy0, cx1, cy1,
                          det_min, td0, zbuf);

                warp_tri (&src, tile, &band,
                          dxs[i10], dys[i10],
                          dxs[i11], dys[i11],
                          dxs[i01], dys[i01],
                          x1, y1,
                          x3, y3,
                          x2, y2,
                          clamp_src, cx0, cy0, cx1, cy1,
                          det_min, td1, zbuf);
              }

          g_free (zbuf);
        }

      gegl_buffer_set (output, &band, 0, babl_format ("RGBA float"), tile,
                       GEGL_AUTO_ROWSTRIDE);

      g_free (tile);
      g_free (band_src);
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
  g_free (gkey);

  return TRUE;
}

/* Warp is non-local: output pixels read source pixels from anywhere in the
   input, so the whole input must be requested, not just the output ROI. */
static GeglRectangle
puppet_warp_get_required_for_output (GeglOperation       *operation,
                                     const gchar         *input_pad,
                                     const GeglRectangle *roi)
{
  const GeglRectangle *in_rect =
    gegl_operation_source_get_bounding_box (operation, "input");

  (void) input_pad;

  if (!in_rect || in_rect->width <= 0 || in_rect->height <= 0)
    return *roi;

  return *in_rect;
}

static GeglRectangle
puppet_warp_get_invalidated_by_change (GeglOperation       *operation,
                                       const gchar         *input_pad,
                                       const GeglRectangle *input_roi)
{
  GeglRectangle r = gegl_operation_get_bounding_box (operation);

  (void) input_pad;

  if (r.width <= 0 || r.height <= 0)
    return *input_roi;

  return r;
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
  opc->get_required_for_output   = puppet_warp_get_required_for_output;
  opc->get_invalidated_by_change = puppet_warp_get_invalidated_by_change;
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
