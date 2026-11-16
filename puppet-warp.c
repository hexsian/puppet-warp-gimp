/* puppet-warp.c - ARAP puppet warp GEGL op, mipmap pyramid, and 1:1 viewport compositor. */

#include <gegl-plugin.h>
#include <babl/babl.h>
#include <glib.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
# include <immintrin.h>
# define PW_HAVE_X86_SIMD 1
# define PW_TARGET_AVX2 __attribute__((target("avx2,fma")))
#endif

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
extern void      arap_grid_set_mode (void *g, int mode);
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

  if (strncmp (v, "true", 4) == 0)
    return 1;
  if (strncmp (v, "false", 5) == 0)
    return 0;

  return (int) g_ascii_strtoll (v, NULL, 10);
}

static int
json_deform_mode (const char *json)
{
  const char *v = find_key (json, "deform_mode");

  if (!v)
    return 0;

  while (*v == ' ' || *v == '\t')
    v++;

  if (*v == '"')
    {
      v++;
      if (g_ascii_strncasecmp (v, "rigid", 5) == 0)   return 1;
      if (g_ascii_strncasecmp (v, "distort", 7) == 0) return 2;
      if (g_ascii_strncasecmp (v, "organic", 7) == 0) return 3;
      if (g_ascii_strncasecmp (v, "hybrid", 6) == 0)  return 4;
      return 0;
    }

  {
    int m = (int) g_ascii_strtoll (v, NULL, 10);
    return (m >= 0 && m <= 4) ? m : 0;
  }
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
      gchar *end = NULL;
      double d = g_ascii_strtod (v, &end);

      if (end == (gchar *) v)
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

static gchar *
geom_key (const char *json)
{
  static const char *const keys[] = {
    "cols", "rows", "pin_edge", "rotate_on", "extend", "deform_mode",
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

static uint64_t
pw_fnv1a (const char *s, gsize len)
{
  uint64_t h = 1469598103934665603ULL;
  gsize i;

  for (i = 0; i < len; i++)
    {
      h ^= (uint8_t) s[i];
      h *= 1099511628211ULL;
    }

  return h;
}

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

static gboolean
pw_apply_bicubic_on (void)
{
  static gsize once = 0;
  static gboolean on = FALSE;

  if (g_once_init_enter (&once))
    {
      on = getenv ("PW_APPLY_BICUBIC") != NULL;
      g_once_init_leave (&once, 1);
    }

  return on;
}

typedef struct _PuppetWarp      PuppetWarp;
typedef struct _PuppetWarpClass PuppetWarpClass;

typedef struct
{
  int cols, rows, pin_edge, rotate_on, extend, deform_mode;
  int pad_l, pad_t, pad_r, pad_b;
  int ow_i, oh_i;
} PwScalars;

struct _PuppetWarp
{
  GeglOperationFilter parent_instance;
  gchar *params_json;
  gchar *gkey;
  uint64_t gkey_hash;
  gsize gkey_len;
  PwScalars sc;
};

struct _PuppetWarpClass
{
  GeglOperationFilterClass parent_class;
};

enum { PROP_0, PROP_PARAMS_JSON };

static GTypeModule   *pw_module     = NULL;
static GType          pw_type       = 0;
static GObjectClass  *pw_parent_cls = NULL;

static GRWLock    pw_solve_lock;
static gsize      pw_lock_once = 0;

static gchar     *pw_cache_json      = NULL;
static uint64_t   pw_cache_gkey_hash = 0;
static gsize      pw_cache_gkey_len  = 0;
static gint       pw_cache_level     = -1;
static float     *pw_cache_dx        = NULL;
static float     *pw_cache_dy        = NULL;
static int        pw_cache_nverts    = 0;
static float      pw_cache_ow        = 0.f;
static float      pw_cache_oh        = 0.f;
static int       *pw_cache_vdepth    = NULL;
static int        pw_cache_depth_on  = 0;

static GMutex     pw_vp_wbuf_lock;
static uint8_t   *pw_vp_wbuf     = NULL;
static size_t     pw_vp_wbuf_cap = 0;

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
pw_params_refresh (PuppetWarp *self)
{
  const char *json = self->params_json ? self->params_json : "{}";

  g_free (self->gkey);
  self->gkey = geom_key (json);
  self->gkey_len = self->gkey ? strlen (self->gkey) : 0;
  self->gkey_hash = self->gkey ? pw_fnv1a (self->gkey, self->gkey_len) : 0;

  self->sc.cols        = json_int (json, "cols", 15);
  self->sc.rows        = json_int (json, "rows", 15);
  self->sc.pin_edge    = json_int (json, "pin_edge", 0);
  self->sc.rotate_on   = json_int (json, "rotate_on", 1);
  self->sc.extend      = json_int (json, "extend", 0);
  self->sc.deform_mode = json_deform_mode (json);
  self->sc.pad_l       = json_int (json, "pad_left", 0);
  self->sc.pad_t       = json_int (json, "pad_top", 0);
  self->sc.pad_r       = json_int (json, "pad_right", 0);
  self->sc.pad_b       = json_int (json, "pad_bottom", 0);
  self->sc.ow_i        = json_int (json, "orig_width", 0);
  self->sc.oh_i        = json_int (json, "orig_height", 0);
}

static void
puppet_warp_init (PuppetWarp *self)
{
  self->params_json = g_strdup ("{}");
  self->gkey = NULL;
  self->gkey_hash = 0;
  self->gkey_len = 0;
  pw_params_refresh (self);
}

static void
puppet_warp_finalize (GObject *object)
{
  PuppetWarp *self = (PuppetWarp *) object;
  g_free (self->params_json);
  g_free (self->gkey);
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
      pw_params_refresh (self);
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
  const double u[3] = { u0, u1, u2 }, v[3] = { v0, v1, v2 };
  const double x[3] = { x0, x1, x2 }, y[3] = { y0, y1, y2 };

  return pw_is_needle (u, v, x, y, 1024.0);
}

#define PW_BAND_H 128

typedef struct
{
  const float   *buf;
  GeglRectangle  rect;
} PwSrc;

typedef struct
{
  int    r, c, which, depth, seq;
  double qminx, qmaxx, qminy, qmaxy;
} PwTriItem;

static int
pw_cmp_tri_item (const void *a, const void *b)
{
  const PwTriItem *ta = (const PwTriItem *) a;
  const PwTriItem *tb = (const PwTriItem *) b;

  if (ta->depth != tb->depth)
    return (ta->depth < tb->depth) ? -1 : 1;

  return (ta->seq < tb->seq) ? -1 : (ta->seq > tb->seq ? 1 : 0);
}

static float *
pw_try_fetch (GeglBuffer *input, const GeglRectangle *r)
{
  gsize npix;
  float *buf;

  if (r->width <= 0 || r->height <= 0)
    return NULL;

  npix = (gsize) r->width * (gsize) r->height;
  if (npix > G_MAXSIZE / (4 * sizeof (float)))
    return NULL;

  buf = g_try_new (float, npix * 4);
  if (!buf)
    return NULL;

  gegl_buffer_get (input, r, 1.0, babl_format ("RaGaBaA float"), buf,
                   GEGL_AUTO_ROWSTRIDE, GEGL_ABYSS_NONE);

  return buf;
}

static void
pw_sample (const PwSrc *src, double sx, double sy, gboolean clamp_src, float out[4])
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

        if (clamp_src)
          {
            if (px < 0) px = 0;
            else if (px >= src->rect.width) px = src->rect.width - 1;

            if (py < 0) py = 0;
            else if (py >= src->rect.height) py = src->rect.height - 1;
          }
        else
          {
            if (px < 0 || px >= src->rect.width || py < 0 || py >= src->rect.height)
              continue;
          }

        p = src->buf + ((size_t) py * (size_t) src->rect.width + (size_t) px) * 4;

        for (k = 0; k < 4; k++)
          acc[k] += w * p[k];
      }

  if (acc[3] > 1e-6)
    {
      double inva = 1.0 / acc[3];
      out[0] = (float) (acc[0] * inva);
      out[1] = (float) (acc[1] * inva);
      out[2] = (float) (acc[2] * inva);
      out[3] = (float) (acc[3] > 1.0 ? 1.0 : acc[3]);
    }
}

static inline void
pw_catmull_weights_f (float t, float w[4])
{
  float t2 = t * t;
  float t3 = t2 * t;

  w[0] = -0.5f * t3 +        t2 - 0.5f * t;
  w[1] =  1.5f * t3 - 2.5f * t2 + 1.0f;
  w[2] = -1.5f * t3 + 2.0f * t2 + 0.5f * t;
  w[3] =  0.5f * t3 - 0.5f * t2;
}

static void
pw_sample_bicubic (const PwSrc *src, double sx, double sy, gboolean clamp_src, float out[4])
{
  double fx, fy;
  float tx, ty;
  float wx[4], wy[4];
  float acc[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
  int ix, iy, px0, py0, j, i;
  const int rw = src->rect.width;
  const int rh = src->rect.height;

  out[0] = out[1] = out[2] = out[3] = 0.0f;

  if (!src->buf || !isfinite (sx) || !isfinite (sy))
    return;

  fx = sx - 0.5;
  fy = sy - 0.5;
  ix = (int) floor (fx);
  iy = (int) floor (fy);
  tx = (float) (fx - ix);
  ty = (float) (fy - iy);

  pw_catmull_weights_f (tx, wx);
  pw_catmull_weights_f (ty, wy);

  px0 = ix - 1 - src->rect.x;
  py0 = iy - 1 - src->rect.y;

  if (px0 >= 0 && px0 + 3 < rw && py0 >= 0 && py0 + 3 < rh)
    {
      const float *row = src->buf + ((size_t) py0 * (size_t) rw + (size_t) px0) * 4;

      for (j = 0; j < 4; j++, row += (size_t) rw * 4)
        {
          float wyj = wy[j];
          float r0 = 0.0f, r1 = 0.0f, r2 = 0.0f, r3 = 0.0f;

          for (i = 0; i < 4; i++)
            {
              float w = wx[i];
              const float *p = row + i * 4;
              r0 += w * p[0];
              r1 += w * p[1];
              r2 += w * p[2];
              r3 += w * p[3];
            }

          acc[0] += wyj * r0;
          acc[1] += wyj * r1;
          acc[2] += wyj * r2;
          acc[3] += wyj * r3;
        }
    }
  else
    {
      for (j = 0; j < 4; j++)
        {
          int py = py0 + j;
          float wyj = wy[j];

          if (clamp_src)
            {
              if (py < 0) py = 0;
              else if (py >= rh) py = rh - 1;
            }
          else if (py < 0 || py >= rh)
            continue;

          const float *row = src->buf + (size_t) py * (size_t) rw * 4;

          for (i = 0; i < 4; i++)
            {
              int px = px0 + i;
              float w;
              const float *p;

              if (clamp_src)
                {
                  if (px < 0) px = 0;
                  else if (px >= rw) px = rw - 1;
                }
              else if (px < 0 || px >= rw)
                continue;

              w = wx[i] * wyj;
              p = row + (size_t) px * 4;
              acc[0] += w * p[0];
              acc[1] += w * p[1];
              acc[2] += w * p[2];
              acc[3] += w * p[3];
            }
        }
    }

  if (acc[3] > 1e-6f)
    {
      float a = acc[3] > 1.0f ? 1.0f : acc[3];
      float inva = 1.0f / acc[3];
      float c0 = acc[0] * inva;
      float c1 = acc[1] * inva;
      float c2 = acc[2] * inva;

      out[0] = c0 < 0.0f ? 0.0f : (c0 > 1.0f ? 1.0f : c0);
      out[1] = c1 < 0.0f ? 0.0f : (c1 > 1.0f ? 1.0f : c1);
      out[2] = c2 < 0.0f ? 0.0f : (c2 > 1.0f ? 1.0f : c2);
      out[3] = a;
    }
}

#define PW_SPAN_CHUNK 256

typedef void (*PwSpanFn) (const PwSrc *src,
                          double sx, double sy, double dsx, double dsy,
                          int n, gboolean clamp,
                          double cx0, double cy0, double cx1, double cy1,
                          float *out);

static int
pw_use_avx2 (void)
{
#ifdef PW_HAVE_X86_SIMD
  static gsize once = 0;
  static int on = 0;

  if (g_once_init_enter (&once))
    {
      __builtin_cpu_init ();
      on = __builtin_cpu_supports ("avx2") && __builtin_cpu_supports ("fma") &&
           getenv ("PW_NO_AVX2") == NULL;
      g_once_init_leave (&once, 1);
    }

  return on;
#else
  return 0;
#endif
}

static int
pw_omp_threads (int nbands)
{
#ifdef _OPENMP
  static gsize once = 0;
  static int cap = 1;

  if (g_once_init_enter (&once))
    {
      const char *e = getenv ("PW_THREADS");

      cap = e ? atoi (e) : 0;
      if (cap <= 0)
        cap = omp_get_max_threads ();
      if (cap < 1)
        cap = 1;
      g_once_init_leave (&once, 1);
    }

  return MAX (1, MIN (cap, nbands));
#else
  (void) nbands;
  return 1;
#endif
}

static void
pw_span_scalar (const PwSrc *src,
                double sx, double sy, double dsx, double dsy,
                int n, gboolean clamp,
                double cx0, double cy0, double cx1, double cy1,
                float *out)
{
  int i;

  for (i = 0; i < n; i++, sx += dsx, sy += dsy)
    {
      double x = sx, y = sy;

      if (clamp)
        {
          if (x < cx0) x = cx0;
          else if (x > cx1) x = cx1;

          if (y < cy0) y = cy0;
          else if (y > cy1) y = cy1;
        }

      pw_sample (src, x, y, clamp, out + (size_t) i * 4);
    }
}

static void
pw_span_bicubic (const PwSrc *src,
                 double sx, double sy, double dsx, double dsy,
                 int n, gboolean clamp,
                 double cx0, double cy0, double cx1, double cy1,
                 float *out)
{
  int i;

  for (i = 0; i < n; i++, sx += dsx, sy += dsy)
    {
      double x = sx, y = sy;

      if (clamp)
        {
          if (x < cx0) x = cx0;
          else if (x > cx1) x = cx1;

          if (y < cy0) y = cy0;
          else if (y > cy1) y = cy1;
        }

      pw_sample_bicubic (src, x, y, clamp, out + (size_t) i * 4);
    }
}

#ifdef PW_HAVE_X86_SIMD
static PW_TARGET_AVX2 void
pw_span_avx2 (const PwSrc *src,
              double sx, double sy, double dsx, double dsy,
              int n, gboolean clamp,
              double cx0, double cy0, double cx1, double cy1,
              float *out)
{
  const float *buf = src->buf;
  const int rx = src->rect.x, ry = src->rect.y;
  const int rw = src->rect.width, rh = src->rect.height;
  int i;

  for (i = 0; i < n; i++, sx += dsx, sy += dsy)
    {
      double x = sx, y = sy;
      float *o = out + (size_t) i * 4;
      double fx, fy;
      int ix, iy, x0, y0;

      if (clamp)
        {
          if (x < cx0) x = cx0;
          else if (x > cx1) x = cx1;

          if (y < cy0) y = cy0;
          else if (y > cy1) y = cy1;
        }

      if (!buf || !isfinite (x) || !isfinite (y))
        {
          o[0] = o[1] = o[2] = o[3] = 0.0f;
          continue;
        }

      fx = x - 0.5;
      fy = y - 0.5;
      ix = (int) floor (fx);
      iy = (int) floor (fy);
      x0 = ix - rx;
      y0 = iy - ry;

      if (x0 >= 0 && y0 >= 0 && x0 + 1 < rw && y0 + 1 < rh)
        {
          const float *p0 = buf + ((size_t) y0 * (size_t) rw + (size_t) x0) * 4;
          const float *p1 = p0 + (size_t) rw * 4;
          float tx = (float) (fx - (double) ix);
          float ty = (float) (fy - (double) iy);
          __m256 r0 = _mm256_loadu_ps (p0);
          __m256 r1 = _mm256_loadu_ps (p1);
          __m256 wx = _mm256_insertf128_ps (
                        _mm256_castps128_ps256 (_mm_set1_ps (1.0f - tx)),
                        _mm_set1_ps (tx), 1);
          __m256 acc = _mm256_mul_ps (r0, _mm256_mul_ps (wx, _mm256_set1_ps (1.0f - ty)));
          __m128 s, va;

          acc = _mm256_fmadd_ps (r1, _mm256_mul_ps (wx, _mm256_set1_ps (ty)), acc);
          s = _mm_add_ps (_mm256_castps256_ps128 (acc),
                          _mm256_extractf128_ps (acc, 1));
          va = _mm_shuffle_ps (s, s, _MM_SHUFFLE (3, 3, 3, 3));

          if (_mm_cvtss_f32 (va) > 1e-6f)
            _mm_storeu_ps (o, _mm_blend_ps (_mm_div_ps (s, va), s, 0x8));
          else
            _mm_storeu_ps (o, _mm_setzero_ps ());
        }
      else
        pw_sample (src, x, y, clamp, o);
    }
}
#endif

static PwSpanFn
pw_get_span_fn (void)
{
  if (pw_apply_bicubic_on ())
    return pw_span_bicubic;

#ifdef PW_HAVE_X86_SIMD
  if (pw_use_avx2 ())
    return pw_span_avx2;
#endif
  return pw_span_scalar;
}

static gboolean
pw_band_source (const float *dxs, const float *dys, int cols, int rows,
                float ow, float oh, float pad_sx, float pad_sy,
                const GeglRectangle *band, const GeglRectangle *ext,
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

        any = TRUE;
      }

  if (!any)
    return FALSE;

  {
    GeglRectangle want;

    want.x = (int) floor (minx) - 2;
    want.y = (int) floor (miny) - 2;
    want.width = (int) ceil (maxx) - want.x + 3;
    want.height = (int) ceil (maxy) - want.y + 3;

    if (want.width <= 0 || want.height <= 0)
      return FALSE;

    return gegl_rectangle_intersect (out, &want, ext);
  }
}

static void
warp_tri (const PwSrc *src, float *tile, const GeglRectangle *band,
          float u0, float v0, float u1, float v1, float u2, float v2,
          float x0, float y0, float x1, float y1, float x2, float y2,
          gboolean clamp_src,
          double cx0, double cy0, double cx1, double cy1,
          double det_min, int tri_depth, int *zbuf, PwSpanFn span)
{
  double du0 = u0, dv0 = v0, du1 = u1, dv1 = v1, du2 = u2, dv2 = v2;
  double sx0 = x0, sy0 = y0, sx1 = x1, sy1 = y1, sx2 = x2, sy2 = y2;
  double det = (du1 - du0) * (dv2 - dv0) - (dv1 - dv0) * (du2 - du0);
  double inv, a, b, c, d, e, f, sc, m_det;
  double lx, hx, ly, hy;
  const double eu[3] = { du0, du1, du2 };
  const double ev[3] = { dv0, dv1, dv2 };
  const double xeps = 1e-3;
  float sbuf[PW_SPAN_CHUNK * 4];
  int minx, maxx, miny, maxy, y;

  if (!isfinite (du0) || !isfinite (dv0) ||
      !isfinite (du1) || !isfinite (dv1) ||
      !isfinite (du2) || !isfinite (dv2) ||
      !isfinite (sx0) || !isfinite (sy0) ||
      !isfinite (sx1) || !isfinite (sy1) ||
      !isfinite (sx2) || !isfinite (sy2))
    return;

  if (!isfinite (det) || !(det >= det_min))
    return;

  lx = fmax (floor (fmin (du0, fmin (du1, du2))), (double) band->x);
  hx = fmin (ceil  (fmax (du0, fmax (du1, du2))),
             (double) (band->x + band->width - 1));
  ly = fmax (floor (fmin (dv0, fmin (dv1, dv2))), (double) band->y);
  hy = fmin (ceil  (fmax (dv0, fmax (dv1, dv2))),
             (double) (band->y + band->height - 1));

  if (lx > hx || ly > hy)
    return;

  minx = (int) lx;
  maxx = (int) hx;
  miny = (int) ly;
  maxy = (int) hy;

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

  if (!isfinite (sc) || sc > 1e4 || sc < 1e-6)
    return;

  m_det = a * d - b * c;
  if (!isfinite (m_det) || fabs (m_det) < 1e-12)
    return;

  for (y = miny; y <= maxy; y++)
    {
      double fy = y + 0.5;
      double lo = 1e300, hi = -1e300;
      int k, xs, xe, x;
      double fx, srcx, srcy;

      for (k = 0; k < 3; k++)
        {
          int k1 = (k == 2) ? 0 : k + 1;
          double xa = eu[k], ya = ev[k];
          double xb = eu[k1], yb = ev[k1];
          double ylo = fmin (ya, yb), yhi = fmax (ya, yb);

          if (fy < ylo || fy > yhi)
            continue;

          if (yhi - ylo < 1e-12)
            {
              lo = fmin (lo, fmin (xa, xb));
              hi = fmax (hi, fmax (xa, xb));
            }
          else
            {
              double xi = xa + (fy - ya) * (xb - xa) / (yb - ya);

              lo = fmin (lo, xi);
              hi = fmax (hi, xi);
            }
        }

      if (lo > hi)
        continue;

      xs = (int) ceil (lo - 0.5 - xeps);
      xe = (int) floor (hi - 0.5 + xeps);

      if (xs < minx) xs = minx;
      if (xe > maxx) xe = maxx;

      if (xs > xe)
        continue;

      fx   = (double) xs + 0.5;
      srcx = a * fx + c * fy + e;
      srcy = b * fx + d * fy + f;
      x    = xs;

      while (x <= xe)
        {
          int n = MIN (xe - x + 1, PW_SPAN_CHUNK);
          int j;

          span (src, srcx, srcy, a, b, n, clamp_src, cx0, cy0, cx1, cy1, sbuf);

          for (j = 0; j < n; j++)
            {
              float *px = sbuf + (size_t) j * 4;
              gsize pidx = (gsize) (y - band->y) * (gsize) band->width
                         + (gsize) (x + j - band->x);
              gsize idx = pidx * 4;

              if (!(px[3] > 0.0f) || !isfinite ((double) px[3]))
                continue;

              if (px[3] > 1.0f)
                px[3] = 1.0f;

              if (zbuf != NULL)
                {
                  if (tri_depth > zbuf[pidx])
                    {
                      if (zbuf[pidx] <= -1000000 || px[3] >= 1.0f || tile[idx + 3] <= 0.0f)
                        memcpy (tile + idx, px, 4 * sizeof (float));
                      else
                        {
                          float sa = px[3];
                          float da = tile[idx + 3];
                          float oa = sa + da * (1.0f - sa);

                          if (oa > 1e-6f)
                            {
                              float inv_oa = 1.0f / oa;
                              float wda = da * (1.0f - sa);

                              tile[idx + 0] = (px[0] * sa + tile[idx + 0] * wda) * inv_oa;
                              tile[idx + 1] = (px[1] * sa + tile[idx + 1] * wda) * inv_oa;
                              tile[idx + 2] = (px[2] * sa + tile[idx + 2] * wda) * inv_oa;
                              tile[idx + 3] = oa > 1.0f ? 1.0f : oa;
                            }
                        }

                      zbuf[pidx] = tri_depth;
                    }
                  else if (tri_depth == zbuf[pidx] && px[3] >= tile[idx + 3])
                    {
                      memcpy (tile + idx, px, 4 * sizeof (float));
                    }

                  continue;
                }

              if (px[3] < tile[idx + 3])
                continue;

              memcpy (tile + idx, px, 4 * sizeof (float));
            }

          x    += n;
          srcx += a * n;
          srcy += b * n;
        }
    }
}

/* ------------------------------------------------------------------------
 * Preview rasterizer + Mipmap pyramid + 1:1 Viewport Compositor (ARGB32)
 * ---------------------------------------------------------------------- */

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

#define PW_MIP_LEVELS 4

typedef struct
{
  int      w[PW_MIP_LEVELS];
  int      h[PW_MIP_LEVELS];
  int      stride[PW_MIP_LEVELS];
  double   sx[PW_MIP_LEVELS];
  double   sy[PW_MIP_LEVELS];
  uint8_t *data[PW_MIP_LEVELS]; /* data[0] is NULL (borrowed at render time); 1..3 owned */
} PwMip;

PW_EXPORT void
pw_mip_free (void *handle)
{
  PwMip *m = (PwMip *) handle;
  int i;

  if (!m)
    return;

  for (i = 1; i < PW_MIP_LEVELS; i++)
    free (m->data[i]);

  free (m);
}

PW_EXPORT void *
pw_mip_new (const void *src_void, int w, int h, int stride)
{
  const uint8_t *prev = (const uint8_t *) src_void;
  PwMip *m;
  int pw, ph, pstride, lvl;

  if (!prev || w < 64 || h < 64 || stride < w * 4)
    return NULL;

  m = (PwMip *) calloc (1, sizeof (PwMip));
  if (!m)
    return NULL;

  m->w[0] = w;
  m->h[0] = h;
  m->stride[0] = stride;
  m->sx[0] = 1.0;
  m->sy[0] = 1.0;

  pw = w;
  ph = h;
  pstride = stride;

  for (lvl = 1; lvl < PW_MIP_LEVELS; lvl++)
    {
      int nw = (pw + 1) >> 1;
      int nh = (ph + 1) >> 1;
      int nstride;
      uint8_t *buf;
      int y;

      if (nw < 32 || nh < 32)
        break;

      nstride = nw * 4;
      buf = (uint8_t *) malloc ((size_t) nh * (size_t) nstride);
      if (!buf)
        break;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) if(nh >= 64)
#endif
      for (y = 0; y < nh; y++)
        {
          int y0 = y * 2;
          int y1 = (y0 + 1 < ph) ? y0 + 1 : y0;
          const uint8_t *r0 = prev + (size_t) y0 * (size_t) pstride;
          const uint8_t *r1 = prev + (size_t) y1 * (size_t) pstride;
          uint8_t *dst = buf + (size_t) y * (size_t) nstride;
          int x;

          for (x = 0; x < nw; x++)
            {
              int x0 = x * 2;
              int x1 = (x0 + 1 < pw) ? x0 + 1 : x0;
              uint32_t c00 = ld32 (r0 + (size_t) x0 * 4u);
              uint32_t c10 = ld32 (r0 + (size_t) x1 * 4u);
              uint32_t c01 = ld32 (r1 + (size_t) x0 * 4u);
              uint32_t c11 = ld32 (r1 + (size_t) x1 * 4u);
              uint32_t a = ((c00 >> 24) + (c10 >> 24) + (c01 >> 24) + (c11 >> 24) + 2u) >> 2;
              uint32_t r = (((c00 >> 16) & 255u) + ((c10 >> 16) & 255u) +
                            ((c01 >> 16) & 255u) + ((c11 >> 16) & 255u) + 2u) >> 2;
              uint32_t g = (((c00 >> 8) & 255u) + ((c10 >> 8) & 255u) +
                            ((c01 >> 8) & 255u) + ((c11 >> 8) & 255u) + 2u) >> 2;
              uint32_t b = ((c00 & 255u) + (c10 & 255u) +
                            (c01 & 255u) + (c11 & 255u) + 2u) >> 2;

              st32 (dst + (size_t) x * 4u, (a << 24) | (r << 16) | (g << 8) | b);
            }
        }

      m->w[lvl] = nw;
      m->h[lvl] = nh;
      m->stride[lvl] = nstride;
      m->sx[lvl] = (double) nw / (double) w;
      m->sy[lvl] = (double) nh / (double) h;
      m->data[lvl] = buf;

      prev = buf;
      pw = nw;
      ph = nh;
      pstride = nstride;
    }

  return m;
}

static inline void
pw_pick_mip (const PwMip *mip, double zoom,
             const uint8_t **buf, int *w, int *h, int *stride,
             double *sx, double *sy)
{
  int lvl;

  *sx = 1.0;
  *sy = 1.0;

  if (!mip)
    return;

  for (lvl = PW_MIP_LEVELS - 1; lvl >= 1; lvl--)
    {
      if (mip->data[lvl] && zoom <= mip->sx[lvl] * 1.05)
        {
          *buf = mip->data[lvl];
          *w = mip->w[lvl];
          *h = mip->h[lvl];
          *stride = mip->stride[lvl];
          *sx = mip->sx[lvl];
          *sy = mip->sy[lvl];
          return;
        }
    }
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

typedef void (*PwPrevSpanFn) (const uint8_t *src, int stride, int sw, int sh,
                              double sx, double sy, double dsx, double dsy,
                              int n, int clamp_edges, uint32_t *out);

static void
pw_prev_span_scalar (const uint8_t *src, int stride, int sw, int sh,
                     double sx, double sy, double dsx, double dsy,
                     int n, int clamp_edges, uint32_t *out)
{
  int i;

  for (i = 0; i < n; i++, sx += dsx, sy += dsy)
    {
      double x = sx, y = sy;

      if (clamp_edges)
        {
          if (x < 0.5) x = 0.5;
          else if (x > (double) sw - 0.5) x = (double) sw - 0.5;

          if (y < 0.5) y = 0.5;
          else if (y > (double) sh - 0.5) y = (double) sh - 0.5;
        }

      out[i] = sample_bilinear (src, stride, sw, sh, x, y);
    }
}

#ifdef PW_HAVE_X86_SIMD
static PW_TARGET_AVX2 void
pw_prev_span_avx2 (const uint8_t *src, int stride, int sw, int sh,
                   double sx, double sy, double dsx, double dsy,
                   int n, int clamp_edges, uint32_t *out)
{
  int i;

  for (i = 0; i < n; i++, sx += dsx, sy += dsy)
    {
      double x = sx, y = sy;
      double fx, fy;
      int ix, iy;

      if (clamp_edges)
        {
          if (x < 0.5) x = 0.5;
          else if (x > (double) sw - 0.5) x = (double) sw - 0.5;

          if (y < 0.5) y = 0.5;
          else if (y > (double) sh - 0.5) y = (double) sh - 0.5;
        }

      if (!isfinite (x) || !isfinite (y) ||
          x < 0.0 || x >= (double) sw || y < 0.0 || y >= (double) sh)
        {
          out[i] = 0u;
          continue;
        }

      fx = x - 0.5;
      fy = y - 0.5;
      ix = (int) floor (fx);
      iy = (int) floor (fy);

      if (ix >= 0 && ix + 1 < sw && iy >= 0 && iy + 1 < sh)
        {
          int tx = (int) ((fx - (double) ix) * 256.0 + 0.5);
          int ty = (int) ((fy - (double) iy) * 256.0 + 0.5);
          int w00, w10, w01, w11;
          const uint8_t *row0 = src + (size_t) iy * (size_t) stride
                                    + (size_t) ix * 4u;
          const uint8_t *row1 = row0 + (size_t) stride;
          __m128i p0, p1, wa, wb, s;

          if (tx < 0) tx = 0; else if (tx > 256) tx = 256;
          if (ty < 0) ty = 0; else if (ty > 256) ty = 256;

          w00 = ((256 - tx) * (256 - ty)) >> 8;
          w10 = (tx * (256 - ty)) >> 8;
          w01 = ((256 - tx) * ty) >> 8;
          w11 = 256 - w00 - w10 - w01;
          if (w11 < 0)
            w11 = 0;

          p0 = _mm_cvtepu8_epi16 (_mm_loadl_epi64 ((const __m128i *) row0));
          p1 = _mm_cvtepu8_epi16 (_mm_loadl_epi64 ((const __m128i *) row1));
          wa = _mm_set_epi16 ((short) w10, (short) w10, (short) w10, (short) w10,
                              (short) w00, (short) w00, (short) w00, (short) w00);
          wb = _mm_set_epi16 ((short) w11, (short) w11, (short) w11, (short) w11,
                              (short) w01, (short) w01, (short) w01, (short) w01);

          s = _mm_add_epi16 (_mm_mullo_epi16 (p0, wa), _mm_mullo_epi16 (p1, wb));
          s = _mm_add_epi16 (s, _mm_srli_si128 (s, 8));
          s = _mm_srli_epi16 (s, 8);
          out[i] = (uint32_t) _mm_cvtsi128_si32 (_mm_packus_epi16 (s, s));
        }
      else
        out[i] = sample_bilinear (src, stride, sw, sh, x, y);
    }
}
#endif

static PwPrevSpanFn
pw_get_prev_span_fn (void)
{
#ifdef PW_HAVE_X86_SIMD
  if (pw_use_avx2 ())
    return pw_prev_span_avx2;
#endif
  return pw_prev_span_scalar;
}

static inline void
blend_over (uint8_t *dp, uint32_t src)
{
  uint32_t sa = src >> 24;
  uint32_t dst, inv, oa, cr, cg, cb;

  if (sa == 0u)
    return;

  dst = ld32 (dp);

  if (sa == 255u || dst == 0u)
    {
      st32 (dp, src);
      return;
    }

  inv = 255u - sa;

  oa = sa + (((dst >> 24) * inv + 127u) / 255u);
  cr = ((src >> 16) & 255u) + ((((dst >> 16) & 255u) * inv + 127u) / 255u);
  cg = ((src >> 8) & 255u) + ((((dst >> 8) & 255u) * inv + 127u) / 255u);
  cb = (src & 255u) + (((dst & 255u) * inv + 127u) / 255u);

  if (oa > 255u) oa = 255u;
  if (cr > 255u) cr = 255u;
  if (cg > 255u) cg = 255u;
  if (cb > 255u) cb = 255u;

  st32 (dp, (oa << 24) | (cr << 16) | (cg << 8) | cb);
}

static inline uint32_t
pw_scale_argb32 (uint32_t col, uint32_t a255)
{
  uint32_t a = (((col >> 24)       ) * a255 + 127u) / 255u;
  uint32_t r = (((col >> 16) & 255u) * a255 + 127u) / 255u;
  uint32_t g = (((col >> 8)  & 255u) * a255 + 127u) / 255u;
  uint32_t b = (( col        & 255u) * a255 + 127u) / 255u;

  return (a << 24) | (r << 16) | (g << 8) | b;
}

static int
warp_tri_fast_clip (const uint8_t *src, int sstride, int sw, int sh,
                    uint8_t *dst, int dstride, int dw, int dh,
                    double u0, double v0, double u1, double v1, double u2, double v2,
                    double x0, double y0, double x1, double y1, double x2, double y2,
                    double scr_vsc, double src_sc,
                    int clip_y0, int clip_y1, int dst_y_off, int clamp_edges,
                    int tri_depth,
                    uint16_t *zbuf,
                    PwPrevSpanFn span)
{
  double det = (u1 - u0) * (v2 - v0) - (v1 - v0) * (u2 - u0);
  double inv, a, b, c, d, e, f, sc, m_det;
  double minxu, maxxu, minyv, maxyv;
  double eff_vsc, min_det, needle_k;
  const double eu[3] = { u0, u1, u2 };
  const double ev[3] = { v0, v1, v2 };
  const double ex[3] = { x0, x1, x2 };
  const double ey[3] = { y0, y1, y2 };
  int minx, maxx, miny, maxy, x, y;

  if (dw <= 0 || dh <= 0 || clip_y0 >= clip_y1)
    return 0;

  if (!(scr_vsc > 0.0) || !(src_sc > 0.0) || !isfinite (scr_vsc) || !isfinite (src_sc))
    return 0;

  eff_vsc = scr_vsc / src_sc;
  min_det = 0.5 * scr_vsc * scr_vsc;
  needle_k = 1024.0 * eff_vsc * eff_vsc;

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

  sc *= eff_vsc;

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

          /* Sort endpoints so shared edges compute bit-identical x intersections. */
          if (ya > yb)
            {
              double t = ya; ya = yb; yb = t;
              t = xa; xa = xb; xb = t;
            }

          if (ya <= fy && fy < yb && (yb - ya) > 1e-12)
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

      /* Half-open [low, high) pixel-center rule: watertight, zero seam overlap. */
      xstart = (int) ceil (low - 0.5);
      xend   = (int) ceil (high - 0.5) - 1;

      if (xstart < minx) xstart = minx;
      if (xend > maxx) xend = maxx;

      if (xstart > xend)
        continue;

      {
        double fx = (double) xstart + 0.5;
        double srcx = a * fx + c * fy + e;
        double srcy = b * fx + d * fy + f;
        uint8_t *row = dst + (size_t) (y - dst_y_off) * (size_t) dstride;
        uint32_t cbuf[PW_SPAN_CHUNK];

        x = xstart;

        while (x <= xend)
          {
            int n = MIN (xend - x + 1, PW_SPAN_CHUNK);
            int j;

            span (src, sstride, sw, sh, srcx, srcy, a, b, n, clamp_edges, cbuf);

            for (j = 0; j < n; j++)
              {
                uint32_t col = cbuf[j];
                int xx = x + j;

                if (!col)
                  continue;

                if (zbuf != NULL)
                  {
                    size_t zidx = (size_t) (y - clip_y0) * (size_t) dw + (size_t) xx;
                    int enc = tri_depth + 1000;
                    uint16_t zv;

                    if (enc < 1) enc = 1;
                    if (enc > 65534) enc = 65534;
                    zv = (uint16_t) enc;

                    if (zv > zbuf[zidx])
                      {
                        blend_over (row + (size_t) xx * 4u, col);
                        zbuf[zidx] = zv;
                      }
                    else if (zv == zbuf[zidx])
                      {
                        if ((col >> 24) >= (ld32 (row + (size_t) xx * 4u) >> 24))
                          st32 (row + (size_t) xx * 4u, col);
                      }
                  }
                else
                  blend_over (row + (size_t) xx * 4u, col);
              }

            x    += n;
            srcx += a * n;
            srcy += b * n;
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
                             const int *vdepth,
                             const uint8_t *cell_active)
{
  int draw = 0;
  int r, c;
  double invc, invr;
  double vox, voy, vsc;
  double cell_w, cell_h;
  int nbands = 1;
  int band_h;
  int b;
  PwPrevSpanFn span;
  PwTriItem *tris = NULL;
  int ntris = 0;

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

  span = pw_get_prev_span_fn ();

  invc = 1.0 / (double) cols;
  invr = 1.0 / (double) rows;

  vox = (double) view_ox;
  voy = (double) view_oy;
  vsc = (double) view_scale;

  cell_w = (double) pw * invc;
  cell_h = (double) ph * invr;

  if (depth_on)
    {
      tris = (PwTriItem *) malloc ((size_t) cols * (size_t) rows * 2u * sizeof (PwTriItem));
      if (!tris)
        return 0;

      for (r = 0; r < rows; r++)
        {
          int row_off = r * (cols + 1);
          int cell_row = r * cols;

          for (c = 0; c < cols; c++)
            {
              int i00, i10, i01, i11;
              double du0, dv0, du1, dv1, du2, dv2, du3, dv3;
              double qminx, qmaxx, qminy, qmaxy;
              int a0, a1, a2, b0, b1, b2, td0, td1;

              if (cell_active && !cell_active[cell_row + c])
                continue;

              i00 = row_off + c;
              i10 = i00 + 1;
              i01 = i00 + (cols + 1);
              i11 = i01 + 1;

              du0 = ((double) verts[2 * i00]     - vox) * vsc;
              dv0 = ((double) verts[2 * i00 + 1] - voy) * vsc;
              du1 = ((double) verts[2 * i10]     - vox) * vsc;
              dv1 = ((double) verts[2 * i10 + 1] - voy) * vsc;
              du2 = ((double) verts[2 * i01]     - vox) * vsc;
              dv2 = ((double) verts[2 * i01 + 1] - voy) * vsc;
              du3 = ((double) verts[2 * i11]     - vox) * vsc;
              dv3 = ((double) verts[2 * i11 + 1] - voy) * vsc;

              qminx = fmin (fmin (du0, du1), fmin (du2, du3));
              qmaxx = fmax (fmax (du0, du1), fmax (du2, du3));
              qminy = fmin (fmin (dv0, dv1), fmin (dv2, dv3));
              qmaxy = fmax (fmax (dv0, dv1), fmax (dv2, dv3));

              if (qmaxx < 0.0 || qminx >= (double) dw ||
                  qmaxy < 0.0 || qminy >= (double) dh)
                continue;

              a0 = vdepth[i00]; a1 = vdepth[i10]; a2 = vdepth[i01];
              b0 = vdepth[i10]; b1 = vdepth[i11]; b2 = vdepth[i01];
              td0 = a0 >= a1 && a0 >= a2 ? a0 : (a1 >= a2 ? a1 : a2);
              td1 = b0 >= b1 && b0 >= b2 ? b0 : (b1 >= b2 ? b1 : b2);

              tris[ntris].r = r;
              tris[ntris].c = c;
              tris[ntris].which = 0;
              tris[ntris].depth = td0;
              tris[ntris].seq = (r * cols + c) * 2;
              tris[ntris].qminx = qminx;
              tris[ntris].qmaxx = qmaxx;
              tris[ntris].qminy = qminy;
              tris[ntris].qmaxy = qmaxy;
              ntris++;

              tris[ntris].r = r;
              tris[ntris].c = c;
              tris[ntris].which = 1;
              tris[ntris].depth = td1;
              tris[ntris].seq = (r * cols + c) * 2 + 1;
              tris[ntris].qminx = qminx;
              tris[ntris].qmaxx = qmaxx;
              tris[ntris].qminy = qminy;
              tris[ntris].qmaxy = qmaxy;
              ntris++;
            }
        }

      if (ntris > 1)
        qsort (tris, (size_t) ntris, sizeof (PwTriItem), pw_cmp_tri_item);
    }

#ifdef _OPENMP
  nbands = omp_get_max_threads ();
  if (nbands < 1) nbands = 1;
  if (nbands > 16) nbands = 16;

  {
    int min_band = 16;
    if (nbands * min_band > dh)
      nbands = (dh + min_band - 1) / min_band;
    if (nbands < 1) nbands = 1;
  }
#endif

  band_h = (dh + nbands - 1) / nbands;
  if (band_h < 1) band_h = 1;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) reduction(+:draw) private(r, c)
#endif
  for (b = 0; b < nbands; b++)
    {
      int y0 = b * band_h;
      int y1 = y0 + band_h;

      if (y1 > dh) y1 = dh;
      if (y0 >= y1) continue;

      for (int yy = y0; yy < y1; yy++)
        memset ((uint8_t *) dst + (size_t) yy * (size_t) dstride, 0, (size_t) dw * 4u);

      if (depth_on)
        {
          uint16_t *zbuf = (uint16_t *) calloc ((size_t) (y1 - y0) * (size_t) dw, sizeof (uint16_t));
          int ti;

          if (!zbuf)
            continue;

          for (ti = 0; ti < ntris; ti++)
            {
              const PwTriItem *it = &tris[ti];
              int rr, cc, i00, i10, i01, i11;
              double sv0, sv2, su0, su1;
              double du0, dv0, du1, dv1, du2, dv2, du3, dv3;

              if (it->qmaxy < (double) y0 || it->qminy >= (double) y1)
                continue;

              rr = it->r;
              cc = it->c;
              i00 = rr * (cols + 1) + cc;
              i10 = i00 + 1;
              i01 = i00 + (cols + 1);
              i11 = i01 + 1;

              sv0 = (double) py + (double) rr * cell_h;
              sv2 = sv0 + cell_h;
              su0 = (double) px + (double) cc * cell_w;
              su1 = su0 + cell_w;

              if (it->which == 0)
                {
                  du0 = ((double) verts[2 * i00]     - vox) * vsc;
                  dv0 = ((double) verts[2 * i00 + 1] - voy) * vsc;
                  du1 = ((double) verts[2 * i10]     - vox) * vsc;
                  dv1 = ((double) verts[2 * i10 + 1] - voy) * vsc;
                  du2 = ((double) verts[2 * i01]     - vox) * vsc;
                  dv2 = ((double) verts[2 * i01 + 1] - voy) * vsc;

                  draw += warp_tri_fast_clip ((const uint8_t *) src, sstride, sw, sh,
                                              (uint8_t *) dst, dstride, dw, dh,
                                              du0, dv0, du1, dv1, du2, dv2,
                                              su0, sv0, su1, sv0, su0, sv2,
                                              vsc, 1.0,
                                              y0, y1, 0, clamp_edges,
                                              it->depth, zbuf, span);
                }
              else
                {
                  du1 = ((double) verts[2 * i10]     - vox) * vsc;
                  dv1 = ((double) verts[2 * i10 + 1] - voy) * vsc;
                  du3 = ((double) verts[2 * i11]     - vox) * vsc;
                  dv3 = ((double) verts[2 * i11 + 1] - voy) * vsc;
                  du2 = ((double) verts[2 * i01]     - vox) * vsc;
                  dv2 = ((double) verts[2 * i01 + 1] - voy) * vsc;

                  draw += warp_tri_fast_clip ((const uint8_t *) src, sstride, sw, sh,
                                              (uint8_t *) dst, dstride, dw, dh,
                                              du1, dv1, du3, dv3, du2, dv2,
                                              su1, sv0, su1, sv2, su0, sv2,
                                              vsc, 1.0,
                                              y0, y1, 0, clamp_edges,
                                              it->depth, zbuf, span);
                }
            }

          free (zbuf);
        }
      else
        {
          for (r = 0; r < rows; r++)
            {
              double sv0 = (double) py + (double) r * cell_h;
              double sv2 = sv0 + cell_h;
              int row_off = r * (cols + 1);
              int cell_row = r * cols;

              for (c = 0; c < cols; c++)
                {
                  if (cell_active && !cell_active[cell_row + c])
                    continue;

                  int i00 = row_off + c;
                  int i10 = i00 + 1;
                  int i01 = i00 + (cols + 1);
                  int i11 = i01 + 1;

                  double su0 = (double) px + (double) c * cell_w;
                  double su1 = su0 + cell_w;

                  double du0 = ((double) verts[2 * i00]     - vox) * vsc;
                  double dv0 = ((double) verts[2 * i00 + 1] - voy) * vsc;
                  double du1 = ((double) verts[2 * i10]     - vox) * vsc;
                  double dv1 = ((double) verts[2 * i10 + 1] - voy) * vsc;
                  double du2 = ((double) verts[2 * i01]     - vox) * vsc;
                  double dv2 = ((double) verts[2 * i01 + 1] - voy) * vsc;
                  double du3 = ((double) verts[2 * i11]     - vox) * vsc;
                  double dv3 = ((double) verts[2 * i11 + 1] - voy) * vsc;

                  double qminx = fmin (fmin (du0, du1), fmin (du2, du3));
                  double qmaxx = fmax (fmax (du0, du1), fmax (du2, du3));
                  double qminy = fmin (fmin (dv0, dv1), fmin (dv2, dv3));
                  double qmaxy = fmax (fmax (dv0, dv1), fmax (dv2, dv3));

                  if (qmaxx < 0.0 || qminx >= (double) dw ||
                      qmaxy < (double) y0 || qminy >= (double) y1)
                    continue;

                  draw += warp_tri_fast_clip ((const uint8_t *) src, sstride, sw, sh,
                                              (uint8_t *) dst, dstride, dw, dh,
                                              du0, dv0, du1, dv1, du2, dv2,
                                              su0, sv0, su1, sv0, su0, sv2,
                                              vsc, 1.0,
                                              y0, y1, 0, clamp_edges,
                                              0, NULL, span);

                  draw += warp_tri_fast_clip ((const uint8_t *) src, sstride, sw, sh,
                                              (uint8_t *) dst, dstride, dw, dh,
                                              du1, dv1, du3, dv3, du2, dv2,
                                              su1, sv0, su1, sv2, su0, sv2,
                                              vsc, 1.0,
                                              y0, y1, 0, clamp_edges,
                                              0, NULL, span);
                }
            }
        }
    }

  free (tris);
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
                                      clamp_edges, 0, NULL, NULL);
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
                                      clamp_edges, depth_on, vdepth, NULL);
}

PW_EXPORT int
pw_render_preview_view_ex (void *src, int sw, int sh, int sstride,
                           void *dst, int dw, int dh, int dstride,
                           int cols, int rows,
                           const float *verts, int nverts,
                           float px, float py, float pw, float ph,
                           float view_ox, float view_oy, float view_scale,
                           int clamp_edges,
                           int depth_on,
                           const int *vdepth,
                           const uint8_t *cell_active)
{
  return pw_render_preview_view_impl (src, sw, sh, sstride,
                                      dst, dw, dh, dstride,
                                      cols, rows,
                                      verts, nverts,
                                      px, py, pw, ph,
                                      view_ox, view_oy, view_scale,
                                      clamp_edges, depth_on, vdepth, cell_active);
}

static void
pw_blit_layer_band (uint8_t *dst, int dw, int dstride, int y0, int y1,
                    double pan_x, double pan_y, double zoom,
                    const uint8_t *lsrc, int lw, int lh, int lstride,
                    const PwMip *lmip, double lox, double loy, float lalpha,
                    PwPrevSpanFn span)
{
  const uint8_t *lbuf = lsrc;
  int mw = lw, mh = lh, mstride = lstride;
  double sx_sc = 1.0, sy_sc = 1.0;
  int sx0, sx1, sy0, sy1;
  uint32_t a255;
  int y;

  if (!lsrc || lw <= 0 || lh <= 0 || lalpha <= 0.001f)
    return;

  if (lalpha > 1.0f) lalpha = 1.0f;
  a255 = (uint32_t) lroundf (lalpha * 255.0f);
  if (a255 == 0u)
    return;

  pw_pick_mip (lmip, zoom, &lbuf, &mw, &mh, &mstride, &sx_sc, &sy_sc);

  sx0 = (int) floor (lox * zoom + pan_x) - 1;
  sx1 = (int) ceil  ((lox + (double) lw) * zoom + pan_x) + 1;
  sy0 = (int) floor (loy * zoom + pan_y) - 1;
  sy1 = (int) ceil  ((loy + (double) lh) * zoom + pan_y) + 1;

  if (sx0 < 0) sx0 = 0;
  if (sx1 > dw) sx1 = dw;
  if (sy0 < y0) sy0 = y0;
  if (sy1 > y1) sy1 = y1;

  if (sx0 >= sx1 || sy0 >= sy1)
    return;

  {
    double dsx = sx_sc / zoom;
    double wx_start = ((double) sx0 + 0.5 - pan_x) / zoom;
    double srcx_start = (wx_start - lox) * sx_sc;
    uint32_t cbuf[PW_SPAN_CHUNK];

    for (y = sy0; y < sy1; y++)
      {
        double wy = ((double) y + 0.5 - pan_y) / zoom;
        double srcy = (wy - loy) * sy_sc;
        uint8_t *row = dst + (size_t) y * (size_t) dstride;
        double srcx = srcx_start;
        int x = sx0;

        while (x < sx1)
          {
            int n = MIN (sx1 - x, PW_SPAN_CHUNK);
            int j;

            span (lbuf, mstride, mw, mh, srcx, srcy, dsx, 0.0, n, 0, cbuf);

            if (a255 >= 255u)
              {
                for (j = 0; j < n; j++)
                  {
                    if (cbuf[j])
                      blend_over (row + (size_t) (x + j) * 4u, cbuf[j]);
                  }
              }
            else
              {
                for (j = 0; j < n; j++)
                  {
                    if (cbuf[j])
                      {
                        uint32_t sc = pw_scale_argb32 (cbuf[j], a255);
                        if (sc >> 24)
                          blend_over (row + (size_t) (x + j) * 4u, sc);
                      }
                  }
              }

            x += n;
            srcx += dsx * (double) n;
          }
      }
  }
}

PW_EXPORT int
pw_render_viewport (void *dst_void, int dw, int dh, int dstride,
                    float pan_x_f, float pan_y_f, float zoom_f,
                    const void *src_void, int sw, int sh, int sstride, const void *src_mip_void,
                    int cols, int rows, const float *verts, int nverts,
                    float px, float py, float pw, float ph,
                    int clamp_edges, float opacity, int orig_mode,
                    int depth_on, const int *vdepth, const uint8_t *cell_active,
                    const void *bg_void, int bg_w, int bg_h, int bg_stride, const void *bg_mip_void,
                    float bg_ox, float bg_oy, float bg_alpha,
                    const void *fg_void, int fg_w, int fg_h, int fg_stride, const void *fg_mip_void,
                    float fg_ox, float fg_oy, float fg_alpha)
{
  uint8_t *dst = (uint8_t *) dst_void;
  const uint8_t *src = (const uint8_t *) src_void;
  const PwMip *src_mip = (const PwMip *) src_mip_void;
  const uint8_t *bg = (const uint8_t *) bg_void;
  const PwMip *bg_mip = (const PwMip *) bg_mip_void;
  const uint8_t *fg = (const uint8_t *) fg_void;
  const PwMip *fg_mip = (const PwMip *) fg_mip_void;
  double pan_x = (double) pan_x_f;
  double pan_y = (double) pan_y_f;
  double zoom = (double) zoom_f;
  const uint8_t *sbuf = src;
  int msw = sw, msh = sh, msstride = sstride;
  double msx_sc = 1.0, msy_sc = 1.0;
  double src_sc_avg;
  double invc, invr, cell_w, cell_h, ck_step;
  int cx0, cx1, cy0, cy1, ck_w;
  int nbands = 1, band_h, b;
  int draw = 0;
  uint32_t op_a255;
  PwPrevSpanFn span;
  uint32_t *ck_rows = NULL;
  double *svx = NULL, *svy = NULL;
  PwTriItem *tris = NULL;
  int ntris = 0;
  uint8_t *wbuf = NULL;
  int wbuf_locked = 0;
  const uint32_t bg_win = 0xFF212124u;
  const uint32_t ck_a   = 0xFFD9D9D9u;
  const uint32_t ck_b   = 0xFFA6A6A6u;

  if (!dst || dw <= 0 || dh <= 0 || dstride < dw * 4)
    return 0;

  if (!(zoom > 0.0) || !isfinite (zoom) || !isfinite (pan_x) || !isfinite (pan_y))
    return 0;

  if (!orig_mode)
    {
      if (!src || sw <= 0 || sh <= 0 || !verts || cols < 1 || rows < 1)
        return 0;
      if ((long long) (cols + 1) * (rows + 1) > (long long) nverts)
        return 0;
      if (depth_on && !vdepth)
        return 0;
    }

  span = pw_get_prev_span_fn ();

  if (!orig_mode && src)
    pw_pick_mip (src_mip, zoom, &sbuf, &msw, &msh, &msstride, &msx_sc, &msy_sc);

  src_sc_avg = 0.5 * (msx_sc + msy_sc);

  if (opacity < 0.0f) opacity = 0.0f;
  if (opacity > 1.0f) opacity = 1.0f;
  op_a255 = (uint32_t) lroundf (opacity * 255.0f);

  if (op_a255 >= 255u && pw_vp_wbuf_cap > 32u * 1024u * 1024u)
    {
      g_mutex_lock (&pw_vp_wbuf_lock);
      if (pw_vp_wbuf_cap > 32u * 1024u * 1024u)
        {
          free (pw_vp_wbuf);
          pw_vp_wbuf = NULL;
          pw_vp_wbuf_cap = 0;
        }
      g_mutex_unlock (&pw_vp_wbuf_lock);
    }

  /* Checkerboard world rect [0, sw) x [0, sh) in screen coords */
  cx0 = (int) floor (pan_x);
  cx1 = (int) ceil  ((double) sw * zoom + pan_x);
  cy0 = (int) floor (pan_y);
  cy1 = (int) ceil  ((double) sh * zoom + pan_y);
  if (cx0 < 0) cx0 = 0;
  if (cx1 > dw) cx1 = dw;
  if (cy0 < 0) cy0 = 0;
  if (cy1 > dh) cy1 = dh;

  ck_step = 16.0;
  while (ck_step * zoom < 5.0)
    ck_step *= 2.0;

  ck_w = cx1 - cx0;
  if (ck_w > 0 && cy0 < cy1)
    {
      ck_rows = (uint32_t *) malloc ((size_t) ck_w * 2u * sizeof (uint32_t));
      if (ck_rows)
        {
          double du = 1.0 / (zoom * ck_step);
          double u = (((double) cx0 + 0.5 - pan_x) / zoom) / ck_step;
          int xx;

          for (xx = 0; xx < ck_w; xx++, u += du)
            {
              int cck = (int) floor (u);
              ck_rows[xx]        = (cck & 1) ? ck_b : ck_a;
              ck_rows[ck_w + xx] = (cck & 1) ? ck_a : ck_b;
            }
        }
    }

  invc = 1.0 / (double) MAX (1, cols);
  invr = 1.0 / (double) MAX (1, rows);
  cell_w = (double) pw * invc;
  cell_h = (double) ph * invr;

  /* Precompute screen-space vertices and visible active triangles once */
  if (!orig_mode && op_a255 > 0u && src)
    {
      int nv = (cols + 1) * (rows + 1);
      int r, c, i;

      svx = (double *) malloc ((size_t) nv * sizeof (double));
      svy = (double *) malloc ((size_t) nv * sizeof (double));
      tris = (PwTriItem *) malloc ((size_t) cols * (size_t) rows * 2u * sizeof (PwTriItem));

      if (!svx || !svy || !tris)
        {
          free (ck_rows);
          free (svx);
          free (svy);
          free (tris);
          return 0;
        }

      for (i = 0; i < nv; i++)
        {
          svx[i] = (double) verts[2 * i]     * zoom + pan_x;
          svy[i] = (double) verts[2 * i + 1] * zoom + pan_y;
        }

      for (r = 0; r < rows; r++)
        {
          int row_off = r * (cols + 1);
          int cell_row = r * cols;

          for (c = 0; c < cols; c++)
            {
              int i00, i10, i01, i11;
              double du0, dv0, du1, dv1, du2, dv2, du3, dv3;
              double tminx, tmaxx, tminy, tmaxy;

              if (cell_active && !cell_active[cell_row + c])
                continue;

              i00 = row_off + c;
              i10 = i00 + 1;
              i01 = i00 + (cols + 1);
              i11 = i01 + 1;

              du0 = svx[i00]; dv0 = svy[i00];
              du1 = svx[i10]; dv1 = svy[i10];
              du2 = svx[i01]; dv2 = svy[i01];
              du3 = svx[i11]; dv3 = svy[i11];

              /* Triangle 0: (i00, i10, i01) */
              tminx = fmin (du0, fmin (du1, du2));
              tmaxx = fmax (du0, fmax (du1, du2));
              tminy = fmin (dv0, fmin (dv1, dv2));
              tmaxy = fmax (dv0, fmax (dv1, dv2));
              if (tmaxx >= 0.0 && tminx < (double) dw &&
                  tmaxy >= 0.0 && tminy < (double) dh)
                {
                  int td0 = 0;
                  if (depth_on)
                    {
                      int a0 = vdepth[i00], a1 = vdepth[i10], a2 = vdepth[i01];
                      td0 = a0 >= a1 && a0 >= a2 ? a0 : (a1 >= a2 ? a1 : a2);
                    }
                  tris[ntris].r = r;
                  tris[ntris].c = c;
                  tris[ntris].which = 0;
                  tris[ntris].depth = td0;
                  tris[ntris].seq = (r * cols + c) * 2;
                  tris[ntris].qminx = tminx;
                  tris[ntris].qmaxx = tmaxx;
                  tris[ntris].qminy = tminy;
                  tris[ntris].qmaxy = tmaxy;
                  ntris++;
                }

              /* Triangle 1: (i10, i11, i01) */
              tminx = fmin (du1, fmin (du3, du2));
              tmaxx = fmax (du1, fmax (du3, du2));
              tminy = fmin (dv1, fmin (dv3, dv2));
              tmaxy = fmax (dv1, fmax (dv3, dv2));
              if (tmaxx >= 0.0 && tminx < (double) dw &&
                  tmaxy >= 0.0 && tminy < (double) dh)
                {
                  int td1 = 0;
                  if (depth_on)
                    {
                      int b0 = vdepth[i10], b1 = vdepth[i11], b2 = vdepth[i01];
                      td1 = b0 >= b1 && b0 >= b2 ? b0 : (b1 >= b2 ? b1 : b2);
                    }
                  tris[ntris].r = r;
                  tris[ntris].c = c;
                  tris[ntris].which = 1;
                  tris[ntris].depth = td1;
                  tris[ntris].seq = (r * cols + c) * 2 + 1;
                  tris[ntris].qminx = tminx;
                  tris[ntris].qmaxx = tmaxx;
                  tris[ntris].qminy = tminy;
                  tris[ntris].qmaxy = tmaxy;
                  ntris++;
                }
            }
        }

      if (depth_on && ntris > 1)
        qsort (tris, (size_t) ntris, sizeof (PwTriItem), pw_cmp_tri_item);

      if (op_a255 < 255u)
        {
          size_t need = (size_t) dh * (size_t) dw * 4u;

          g_mutex_lock (&pw_vp_wbuf_lock);
          wbuf_locked = 1;
          if (need > pw_vp_wbuf_cap)
            {
              uint8_t *nb = (uint8_t *) realloc (pw_vp_wbuf, need);
              if (nb)
                {
                  pw_vp_wbuf = nb;
                  pw_vp_wbuf_cap = need;
                }
            }
          if (pw_vp_wbuf_cap >= need)
            wbuf = pw_vp_wbuf;
        }
    }

#ifdef _OPENMP
  nbands = omp_get_max_threads ();
  if (nbands < 1) nbands = 1;
  if (nbands > 16) nbands = 16;

  {
    int min_band = 16;
    if (nbands * min_band > dh)
      nbands = (dh + min_band - 1) / min_band;
    if (nbands < 1) nbands = 1;
  }
#endif

  band_h = (dh + nbands - 1) / nbands;
  if (band_h < 1) band_h = 1;

#ifdef _OPENMP
#pragma omp parallel for schedule(static) reduction(+:draw)
#endif
  for (b = 0; b < nbands; b++)
    {
      int y0 = b * band_h;
      int y1 = y0 + band_h;
      int bh;
      int yy, xx;
      double ck_inv_zstep = 1.0 / (zoom * ck_step);

      if (y1 > dh) y1 = dh;
      if (y0 >= y1) continue;
      bh = y1 - y0;

      /* 1. Window background + precomputed checkerboard */
      for (yy = y0; yy < y1; yy++)
        {
          uint8_t *row = dst + (size_t) yy * (size_t) dstride;
          uint32_t *row32 = (uint32_t *) row;

          if (ck_rows && yy >= cy0 && yy < cy1)
            {
              int rck = (int) floor (((double) yy + 0.5 - pan_y) * ck_inv_zstep);

              for (xx = 0; xx < cx0; xx++)
                row32[xx] = bg_win;

              memcpy (row32 + cx0,
                      ck_rows + (size_t) (rck & 1) * (size_t) ck_w,
                      (size_t) ck_w * sizeof (uint32_t));

              for (xx = cx1; xx < dw; xx++)
                row32[xx] = bg_win;
            }
          else
            {
              for (xx = 0; xx < dw; xx++)
                row32[xx] = bg_win;
            }
        }

      /* 2. Layers Below */
      if (bg)
        pw_blit_layer_band (dst, dw, dstride, y0, y1,
                            pan_x, pan_y, zoom,
                            bg, bg_w, bg_h, bg_stride, bg_mip,
                            (double) bg_ox, (double) bg_oy, bg_alpha, span);

      /* 3. Active Layer (Original or Warped, with Opacity) */
      if (op_a255 > 0u && src)
        {
          if (orig_mode)
            {
              pw_blit_layer_band (dst, dw, dstride, y0, y1,
                                  pan_x, pan_y, zoom,
                                  src, sw, sh, sstride, src_mip,
                                  0.0, 0.0, opacity, span);
              draw++;
            }
          else
            {
              uint8_t *wtarget = dst;
              int wstride = dstride;
              int ti;

              if (wbuf)
                {
                  wstride = dw * 4;
                  wtarget = wbuf;
                  memset (wbuf + (size_t) y0 * (size_t) wstride, 0,
                          (size_t) bh * (size_t) wstride);
                }

              for (ti = 0; ti < ntris; ti++)
                {
                  const PwTriItem *it = &tris[ti];
                  int rr, cc, i00, i10, i01, i11;
                  double sv0, sv2, su0, su1;

                  if (it->qmaxy < (double) y0 || it->qminy >= (double) y1)
                    continue;

                  rr = it->r;
                  cc = it->c;
                  i00 = rr * (cols + 1) + cc;
                  i10 = i00 + 1;
                  i01 = i00 + (cols + 1);
                  i11 = i01 + 1;

                  sv0 = ((double) py + (double) rr * cell_h) * msy_sc;
                  sv2 = sv0 + cell_h * msy_sc;
                  su0 = ((double) px + (double) cc * cell_w) * msx_sc;
                  su1 = su0 + cell_w * msx_sc;

                  if (it->which == 0)
                    {
                      draw += warp_tri_fast_clip (sbuf, msstride, msw, msh,
                                                  wtarget, wstride, dw, dh,
                                                  svx[i00], svy[i00],
                                                  svx[i10], svy[i10],
                                                  svx[i01], svy[i01],
                                                  su0, sv0, su1, sv0, su0, sv2,
                                                  zoom, src_sc_avg,
                                                  y0, y1, 0, clamp_edges,
                                                  0, NULL, span);
                    }
                  else
                    {
                      draw += warp_tri_fast_clip (sbuf, msstride, msw, msh,
                                                  wtarget, wstride, dw, dh,
                                                  svx[i10], svy[i10],
                                                  svx[i11], svy[i11],
                                                  svx[i01], svy[i01],
                                                  su1, sv0, su1, sv2, su0, sv2,
                                                  zoom, src_sc_avg,
                                                  y0, y1, 0, clamp_edges,
                                                  0, NULL, span);
                    }
                }

              if (wbuf)
                {
                  for (yy = y0; yy < y1; yy++)
                    {
                      const uint32_t *wrow = (const uint32_t *) (wbuf + (size_t) yy * (size_t) wstride);
                      uint8_t *drow = dst + (size_t) yy * (size_t) dstride;
                      for (xx = 0; xx < dw; xx++)
                        {
                          uint32_t col = wrow[xx];
                          if (col)
                            {
                              uint32_t sc = pw_scale_argb32 (col, op_a255);
                              if (sc >> 24)
                                blend_over (drow + (size_t) xx * 4u, sc);
                            }
                        }
                    }
                }
            }
        }

      /* 4. Layers Above */
      if (fg)
        pw_blit_layer_band (dst, dw, dstride, y0, y1,
                            pan_x, pan_y, zoom,
                            fg, fg_w, fg_h, fg_stride, fg_mip,
                            (double) fg_ox, (double) fg_oy, fg_alpha, span);
    }

  if (wbuf_locked)
    g_mutex_unlock (&pw_vp_wbuf_lock);

  free (ck_rows);
  free (svx);
  free (svy);
  free (tris);

  return draw > 0 ? draw : 1;
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
  const gchar *gkey = self->gkey;
  uint64_t gkey_hash = self->gkey_hash;
  gsize gkey_len = self->gkey_len;
  int cols = self->sc.cols;
  int rows = self->sc.rows;
  int pin_edge = self->sc.pin_edge;
  int rotate_on = self->sc.rotate_on;
  int extend = self->sc.extend;
  int deform_mode = self->sc.deform_mode;
  int pad_l = self->sc.pad_l;
  int pad_t = self->sc.pad_t;
  int pad_r = self->sc.pad_r;
  int pad_b = self->sc.pad_b;
  int ow_i = self->sc.ow_i;
  int oh_i = self->sc.oh_i;
  GeglRectangle ext = *gegl_buffer_get_extent (input);
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
  else if (ext.width > pad_l + pad_r)
    ow = (float) (ext.width - pad_l - pad_r);
  else
    ow = (float) (roi->width - pad_l - pad_r);

  if (oh_i > 0)
    oh = (float) oh_i;
  else if (ext.height > pad_t + pad_b)
    oh = (float) (ext.height - pad_t - pad_b);
  else
    oh = (float) (roi->height - pad_t - pad_b);

  if (ow <= 0.0f) ow = (float) (ext.width > 0 ? ext.width : roi->width);
  if (oh <= 0.0f) oh = (float) (ext.height > 0 ? ext.height : roi->height);

  float *pins = NULL, *cw = NULL, *prot = NULL, *pisc = NULL;
  float *ptap = NULL, *pdep = NULL;
  int npins = 0, ncw = 0, nrot = 0, nsc = 0, ntap = 0;
  int nverts;
  gdouble s;
  float *dxs = NULL, *dys = NULL;
  int don = 0;
  int *vdep = NULL;
  int hold_writer = 0;
  double det_min;
  PwTriItem *sorted_tris = NULL;
  int nsorted_tris = 0;

  pw_ensure_lock ();

  if (cols < 1) cols = 1;
  if (rows < 1) rows = 1;
  if (cols > 64) cols = 64;
  if (rows > 64) rows = 64;

  nverts = (cols + 1) * (rows + 1);
  s = ldexp (1.0, -(level < 0 ? 0 : (level > 30 ? 30 : level)));

  if (pw_debug_on ())
    fprintf (stderr, "[puppet-warp] roi=%dx%d level=%d\n",
             roi->width, roi->height, level);

  g_rw_lock_reader_lock (&pw_solve_lock);

  if (!(pw_cache_json != NULL &&
        pw_cache_gkey_hash == gkey_hash &&
        pw_cache_gkey_len == gkey_len &&
        pw_cache_level == level &&
        pw_cache_nverts == nverts &&
        strcmp (pw_cache_json, gkey) == 0))
    {
      g_rw_lock_reader_unlock (&pw_solve_lock);
      g_rw_lock_writer_lock (&pw_solve_lock);
      hold_writer = 1;

      if (!(pw_cache_json != NULL &&
            pw_cache_gkey_hash == gkey_hash &&
            pw_cache_gkey_len == gkey_len &&
            pw_cache_level == level &&
            pw_cache_nverts == nverts &&
            strcmp (pw_cache_json, gkey) == 0))
        {
          float *dx = g_new (float, nverts);
          float *dy = g_new (float, nverts);
          int vert_src = 0;
          int rr, cc, ii;

          pins = g_malloc (256 * 4 * sizeof (float));
          cw   = g_malloc (64 * 64 * sizeof (float));
          prot = g_malloc (256 * sizeof (float));
          pisc = g_malloc (256 * sizeof (float));
          ptap = g_malloc (256 * sizeof (float));
          pdep = g_malloc0 (256 * sizeof (float));

          npins = json_floats (json, "pins_flat", pins, 256 * 4) / 4;
          ncw   = json_floats (json, "mask_flat", cw, 64 * 64);
          nrot  = json_floats (json, "pin_rot", prot, 256);
          nsc   = json_floats (json, "pin_scale", pisc, 256);
          ntap  = json_floats (json, "pin_taper", ptap, 256);
          json_floats (json, "pin_depth", pdep, 256);

          for (rr = 0; rr <= rows; rr++)
            for (cc = 0; cc <= cols; cc++)
              {
                dx[rr * (cols + 1) + cc] = (float) cc * ow / cols;
                dy[rr * (cols + 1) + cc] = (float) rr * oh / rows;
              }

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

          if (vert_src == 0)
            {
              void *grid = arap_grid_new (0.f, 0.f, ow, oh, cols, rows);

              if (grid)
                {
                  if (ncw == cols * rows)
                    arap_grid_set_cell_weights (grid, cw, ncw);

                  if (pin_edge)
                    arap_grid_set_pin_edge (grid, 1);

                  arap_grid_set_mode (grid, deform_mode);

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

          pw_cache_json      = g_strdup (gkey);
          pw_cache_gkey_hash = gkey_hash;
          pw_cache_gkey_len  = gkey_len;
          pw_cache_level     = level;
          pw_cache_dx        = dx;
          pw_cache_dy        = dy;
          pw_cache_nverts    = nverts;
          pw_cache_ow        = (float) (ow * s);
          pw_cache_oh        = (float) (oh * s);
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

  dxs = g_new (float, nverts);
  dys = g_new (float, nverts);
  memcpy (dxs, pw_cache_dx, (size_t) nverts * sizeof (float));
  memcpy (dys, pw_cache_dy, (size_t) nverts * sizeof (float));
  ow  = pw_cache_ow;
  oh  = pw_cache_oh;

  if (pw_cache_depth_on && pw_cache_vdepth)
    {
      don = 1;
      vdep = g_new (int, nverts);
      memcpy (vdep, pw_cache_vdepth, (size_t) nverts * sizeof (int));
    }

  if (hold_writer)
    g_rw_lock_writer_unlock (&pw_solve_lock);
  else
    g_rw_lock_reader_unlock (&pw_solve_lock);

  det_min = 0.5 * s * s;

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

  if (don)
    {
      int r, c;

      sorted_tris = g_new (PwTriItem, (gsize) cols * (gsize) rows * 2);
      for (r = 0; r < rows; r++)
        for (c = 0; c < cols; c++)
          {
            int i00 = r * (cols + 1) + c;
            int i10 = i00 + 1;
            int i01 = i00 + (cols + 1);
            int i11 = i01 + 1;
            int a0 = vdep[i00], a1 = vdep[i10], a2 = vdep[i01];
            int b0 = vdep[i10], b1 = vdep[i11], b2 = vdep[i01];
            int td0 = a0 >= a1 && a0 >= a2 ? a0 : (a1 >= a2 ? a1 : a2);
            int td1 = b0 >= b1 && b0 >= b2 ? b0 : (b1 >= b2 ? b1 : b2);

            sorted_tris[nsorted_tris].r = r;
            sorted_tris[nsorted_tris].c = c;
            sorted_tris[nsorted_tris].which = 0;
            sorted_tris[nsorted_tris].depth = td0;
            sorted_tris[nsorted_tris].seq = (r * cols + c) * 2;
            sorted_tris[nsorted_tris].qminx = fmin (dxs[i00], fmin (dxs[i10], dxs[i01]));
            sorted_tris[nsorted_tris].qmaxx = fmax (dxs[i00], fmax (dxs[i10], dxs[i01]));
            sorted_tris[nsorted_tris].qminy = fmin (dys[i00], fmin (dys[i10], dys[i01]));
            sorted_tris[nsorted_tris].qmaxy = fmax (dys[i00], fmax (dys[i10], dys[i01]));
            nsorted_tris++;

            sorted_tris[nsorted_tris].r = r;
            sorted_tris[nsorted_tris].c = c;
            sorted_tris[nsorted_tris].which = 1;
            sorted_tris[nsorted_tris].depth = td1;
            sorted_tris[nsorted_tris].seq = (r * cols + c) * 2 + 1;
            sorted_tris[nsorted_tris].qminx = fmin (dxs[i10], fmin (dxs[i11], dxs[i01]));
            sorted_tris[nsorted_tris].qmaxx = fmax (dxs[i10], fmax (dxs[i11], dxs[i01]));
            sorted_tris[nsorted_tris].qminy = fmin (dys[i10], fmin (dys[i11], dys[i01]));
            sorted_tris[nsorted_tris].qmaxy = fmax (dys[i10], fmax (dys[i11], dys[i01]));
            nsorted_tris++;
          }

      if (nsorted_tris > 1)
        qsort (sorted_tris, (size_t) nsorted_tris, sizeof (PwTriItem), pw_cmp_tri_item);
    }

  gsize ext_pix = (ext.width > 0 && ext.height > 0)
                ? (gsize) ext.width * (gsize) ext.height
                : 0;
  gsize roi_pix = (roi->width > 0 && roi->height > 0)
                ? (gsize) roi->width * (gsize) roi->height
                : 0;
  float *shared_src = NULL;
  if (ext_pix > 0 && ext_pix <= (gsize) 32000000 && roi_pix * 2 >= ext_pix)
    shared_src = pw_try_fetch (input, &ext);

  PwSpanFn span_fn = pw_get_span_fn ();

  {
    int nbands = (roi->height + PW_BAND_H - 1) / PW_BAND_H;
    int nthreads = pw_omp_threads (nbands);
    int bi;

    /* Avoid multi-thread RAM explosion when per-band buffers replace shared_src. */
    if (shared_src == NULL && ext_pix > (gsize) 16000000 && nthreads > 4)
      nthreads = 4;

#ifdef _OPENMP
#pragma omp parallel for schedule(dynamic, 1) num_threads(nthreads)
#endif
    for (bi = 0; bi < nbands; bi++)
      {
        GeglRectangle band;
        GeglRectangle need;
        PwSrc src;
        float *band_src = NULL;
        float *tile;
        gsize npix;
        int r, c;

        band.x = roi->x;
        band.y = roi->y + bi * PW_BAND_H;
        band.width = roi->width;
        band.height = MIN (PW_BAND_H, roi->y + roi->height - band.y);

        npix = (gsize) band.width * (gsize) band.height;
        tile = g_try_new0 (float, npix * 4);
        if (!tile)
          continue;

        if (shared_src != NULL)
          {
            src.buf = shared_src;
            src.rect = ext;
          }
        else if (pw_band_source (dxs, dys, cols, rows, ow, oh, pad_sx, pad_sy,
                                 &band, &ext, &need))
          {
            band_src = pw_try_fetch (input, &need);
            src.buf = band_src;
            src.rect = need;
          }
        else
          {
            src.buf = NULL;
          }

        if (src.buf)
          {
            if (don)
              {
                int *zbuf = g_try_new (int, npix);
                gsize zi;
                int ti;

                if (zbuf)
                  {
                    for (zi = 0; zi < npix; zi++)
                      zbuf[zi] = -1000000;

                    for (ti = 0; ti < nsorted_tris; ti++)
                      {
                        const PwTriItem *it = &sorted_tris[ti];
                        int i00, i10, i01, i11;
                        float x0, y0, x1, y1, x2, y2, x3, y3;

                        if (it->qmaxx < (double) band.x ||
                            it->qminx > (double) (band.x + band.width) ||
                            it->qmaxy < (double) band.y ||
                            it->qminy > (double) (band.y + band.height))
                          continue;

                        r = it->r;
                        c = it->c;
                        i00 = r * (cols + 1) + c;
                        i10 = i00 + 1;
                        i01 = i00 + (cols + 1);
                        i11 = i01 + 1;

                        x0 = (float) c       * ow / cols + pad_sx;
                        y0 = (float) r       * oh / rows + pad_sy;
                        x1 = (float) (c + 1) * ow / cols + pad_sx;
                        y1 = y0;
                        x2 = x0;
                        y2 = (float) (r + 1) * oh / rows + pad_sy;
                        x3 = x1;
                        y3 = y2;

                        if (it->which == 0)
                          warp_tri (&src, tile, &band,
                                    dxs[i00], dys[i00],
                                    dxs[i10], dys[i10],
                                    dxs[i01], dys[i01],
                                    x0, y0,
                                    x1, y1,
                                    x2, y2,
                                    clamp_src, cx0, cy0, cx1, cy1,
                                    det_min, it->depth, zbuf, span_fn);
                        else
                          warp_tri (&src, tile, &band,
                                    dxs[i10], dys[i10],
                                    dxs[i11], dys[i11],
                                    dxs[i01], dys[i01],
                                    x1, y1,
                                    x3, y3,
                                    x2, y2,
                                    clamp_src, cx0, cy0, cx1, cy1,
                                    det_min, it->depth, zbuf, span_fn);
                      }

                    g_free (zbuf);
                  }
              }
            else
              {
                for (r = 0; r < rows; r++)
                  for (c = 0; c < cols; c++)
                    {
                      int i00 = r * (cols + 1) + c;
                      int i10 = i00 + 1;
                      int i01 = i00 + (cols + 1);
                      int i11 = i01 + 1;

                      float qminx = fminf (fminf (dxs[i00], dxs[i10]), fminf (dxs[i01], dxs[i11]));
                      float qmaxx = fmaxf (fmaxf (dxs[i00], dxs[i10]), fmaxf (dxs[i01], dxs[i11]));
                      float qminy = fminf (fminf (dys[i00], dys[i10]), fminf (dys[i01], dys[i11]));
                      float qmaxy = fmaxf (fmaxf (dys[i00], dys[i10]), fmaxf (dys[i01], dys[i11]));

                      if (qmaxx < (float) band.x || qminx > (float) (band.x + band.width) ||
                          qmaxy < (float) band.y || qminy > (float) (band.y + band.height))
                        continue;

                      float x0 = (float) c       * ow / cols + pad_sx;
                      float y0 = (float) r       * oh / rows + pad_sy;
                      float x1 = (float) (c + 1) * ow / cols + pad_sx;
                      float y1 = y0;
                      float x2 = x0;
                      float y2 = (float) (r + 1) * oh / rows + pad_sy;
                      float x3 = x1;
                      float y3 = y2;

                      warp_tri (&src, tile, &band,
                                dxs[i00], dys[i00],
                                dxs[i10], dys[i10],
                                dxs[i01], dys[i01],
                                x0, y0,
                                x1, y1,
                                x2, y2,
                                clamp_src, cx0, cy0, cx1, cy1,
                                det_min, 0, NULL, span_fn);

                      warp_tri (&src, tile, &band,
                                dxs[i10], dys[i10],
                                dxs[i11], dys[i11],
                                dxs[i01], dys[i01],
                                x1, y1,
                                x3, y3,
                                x2, y2,
                                clamp_src, cx0, cy0, cx1, cy1,
                                det_min, 0, NULL, span_fn);
                    }
              }
          }

        gegl_buffer_set (output, &band, 0, babl_format ("RGBA float"), tile,
                         GEGL_AUTO_ROWSTRIDE);

        g_free (tile);
        g_free (band_src);
      }
  }

  g_free (sorted_tris);
  g_free (shared_src);
  g_free (dxs);
  g_free (dys);
  g_free (vdep);

  g_free (pins);
  g_free (cw);
  g_free (prot);
  g_free (pisc);
  g_free (ptap);
  g_free (pdep);

  return TRUE;
}

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
                         "State (read-only)",
                         "Internal Puppet Warp state. To edit interactively, reopen Filters -> Deform -> Puppet Warp (ARAP)...",
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

PW_EXPORT const GeglModuleInfo *
gegl_module_query (GTypeModule *module)
{
  static const GeglModuleInfo info = { GEGL_MODULE_ABI_VERSION };
  (void) module;
  return &info;
}

PW_EXPORT gboolean
gegl_module_register (GTypeModule *module)
{
  pw_ensure_lock ();
  pw_module = module;
  g_type_module_set_name (module, "puppet-warp");
  return puppet_warp_get_type () != 0;
}

PW_EXPORT gboolean
gegl_module_unregister (GTypeModule *module)
{
  (void) module;

  pw_ensure_lock ();
  g_rw_lock_writer_lock (&pw_solve_lock);
  g_free (pw_cache_json);
  g_free (pw_cache_dx);
  g_free (pw_cache_dy);
  g_free (pw_cache_vdepth);
  pw_cache_json = NULL;
  pw_cache_dx = NULL;
  pw_cache_dy = NULL;
  pw_cache_vdepth = NULL;
  pw_cache_nverts = 0;
  pw_cache_level = -1;
  pw_cache_depth_on = 0;
  g_rw_lock_writer_unlock (&pw_solve_lock);

  g_mutex_lock (&pw_vp_wbuf_lock);
  free (pw_vp_wbuf);
  pw_vp_wbuf = NULL;
  pw_vp_wbuf_cap = 0;
  g_mutex_unlock (&pw_vp_wbuf_lock);
  return TRUE;
}
