/* As-rigid-as-possible grid solver: a regular (cols+1)x(rows+1) vertex grid
 * over a region, deformed by Laplacian-coordinate-style ARAP iterations solved
 * with a banded Cholesky factorization. Pins are hard (Dirichlet) constraints;
 * optional similarity rings add rotate/scale. Exposed as an opaque handle
 * (void*) so callers never depend on the struct layout.
 *
 * Build (linked into puppet-warp.so alongside puppet-warp.c):
 *   gcc -O2 -shared -fPIC -Wall -I. puppet-warp.c arap_grid.c \
 *     -o puppet-warp.so -lm $(pkg-config --cflags --libs gegl-0.4 glib-2.0 cairo)
 * Debug: PW_DEBUG=1 gimp
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <glib.h>

typedef struct { float x, y; } ArapVec2;

#define ARAP_PIN_W    1.0e3   /* weight of one hard pin / one full sim ring */
#define ARAP_EDGE_W   1.0e3   /* weight pulling the border back to rest     */
#define ARAP_REG      1.0e-2  /* Tikhonov regularization toward rest        */
#define ARAP_EDGES    6       /* edges per cell (4 sides + 2 diagonals)     */
#define ARAP_CHOL_MIN 1.0e-6  /* floor on the Cholesky pivot                */
#define ROT_CELLS     3.0     /* similarity ring radius, in cells           */
#define ROT_MIN_PX    24.0    /* similarity ring radius, pixel floor        */

typedef struct {
  int cols, rows, nv, nc, band;
  float *sx, *sy, *dx, *dy;     /* rest and current positions */
  float *cw;                    /* per-cell stiffness weight  */
  unsigned long w_ver;          /* bump on weight change -> refactor */
  int *ea, *eb;                 /* cell edge endpoint indices        */
  int *pv; float *ptx, *pty;    /* hard pins: vertex + target        */
  int npin, pcap;
  int pin_edge;                 /* border-pinning switch             */
  float *sp;                    /* sim pins, 6 floats each           */
  int nsim, scap;
  unsigned long sim_ver;        /* bump on sim change -> refactor    */
  float *accW, *accTX, *accTY;  /* per-vertex similarity accumulators */
  double *A, *L;                /* band matrix + its Cholesky factor  */
  unsigned long fact_sig; int fact_ok;
  double *rx, *ry, *xx, *yy, *yx; /* rhs + solution + scratch         */
  ArapVec2 *out;                /* flat output buffer for callers     */
} ArapGrid;

/* Pointer into row i, column offset k of the banded storage. */
static double *
LAt (ArapGrid *g, int i, int k)
{ return &g->L[(size_t) i * (g->band + 1) + k]; }

/* Allocate and initialize a grid over (x,y,w,h) with (cols+1)*(rows+1) verts. */
void *
arap_grid_new (float x, float y, float w, float h, int cols, int rows)
{
  ArapGrid *g = calloc (1, sizeof (ArapGrid));
  int r, c, i;
  if (!g) return NULL;
  g->cols = cols; g->rows = rows;
  g->nv = (cols + 1) * (rows + 1);
  g->nc = cols * rows;
  g->band = cols + 2;
  g->sx = malloc (g->nv * sizeof (float));
  g->sy = malloc (g->nv * sizeof (float));
  g->dx = malloc (g->nv * sizeof (float));
  g->dy = malloc (g->nv * sizeof (float));
  g->cw = malloc (g->nc * sizeof (float));
  g->ea = malloc (g->nc * ARAP_EDGES * sizeof (int));
  g->eb = malloc (g->nc * ARAP_EDGES * sizeof (int));
  g->out = malloc (g->nv * sizeof (ArapVec2));
  g->rx = malloc (g->nv * sizeof (double));
  g->ry = malloc (g->nv * sizeof (double));
  g->xx = malloc (g->nv * sizeof (double));
  g->yy = malloc (g->nv * sizeof (double));
  g->yx = malloc (g->nv * sizeof (double));
  g->A  = malloc ((size_t) g->nv * (g->band + 1) * sizeof (double));
  g->L  = malloc ((size_t) g->nv * (g->band + 1) * sizeof (double));
  g->accW  = calloc (g->nv, sizeof (float));
  g->accTX = calloc (g->nv, sizeof (float));
  g->accTY = calloc (g->nv, sizeof (float));
  for (r = 0; r <= rows; r++)
    for (c = 0; c <= cols; c++)
      {
        i = r * (cols + 1) + c;
        g->sx[i] = x + (float) c * w / cols;
        g->sy[i] = y + (float) r * h / rows;
        g->dx[i] = g->sx[i];
        g->dy[i] = g->sy[i];
      }
  for (i = 0; i < g->nc; i++) g->cw[i] = 1.0f;
  for (r = 0; r < rows; r++)
    for (c = 0; c < cols; c++)
      {
        int i00 = r * (cols + 1) + c, i10 = i00 + 1;
        int i01 = i00 + (cols + 1),   i11 = i01 + 1;
        int *a = g->ea + (r * cols + c) * ARAP_EDGES;
        int *b = g->eb + (r * cols + c) * ARAP_EDGES;
        a[0] = i00; b[0] = i10;  a[1] = i10; b[1] = i11;
        a[2] = i11; b[2] = i01;  a[3] = i01; b[3] = i00;
        a[4] = i00; b[4] = i11;  a[5] = i10; b[5] = i01;
      }
  g->w_ver = 1;
  g->pin_edge = 0;
  return g;
}

/* Set per-cell stiffness weights (n must equal cols*rows); low -> soft. */
void
arap_grid_set_cell_weights (void *handle, const float *w, int n)
{
  ArapGrid *g = handle;
  int i;
  if (!g || !w || n != g->nc) return;
  for (i = 0; i < n; i++) g->cw[i] = w[i] > 0.03f ? w[i] : 0.0f;
  g->w_ver++;
}

/* Toggle border pinning (pulls the outer ring of vertices back to rest). */
void
arap_grid_set_pin_edge (void *handle, int on)
{
  ArapGrid *g = handle;
  if (g) g->pin_edge = on ? 1 : 0;
}

/* Drop all hard pins and sim pins (used when reusing a live grid). */
void
arap_grid_reset_pins (void *handle)
{
  ArapGrid *g = handle;
  if (!g) return;
  g->npin = 0;
  g->nsim = 0;
}

/* Add a hard pin: snap (sx,sy) to the nearest rest vertex, target = +disp.
 * Re-pinning the same vertex overwrites its target. */
void
arap_grid_add_pin (void *handle, float sx, float sy, float ddx, float ddy)
{
  ArapGrid *g = handle;
  int c, r, v, i;
  if (!g) return;
  c = (int) floorf ((sx - g->sx[0]) / (g->sx[1] - g->sx[0]) + 0.5f);
  r = (int) floorf ((sy - g->sy[0]) / (g->sy[g->cols + 1] - g->sy[0]) + 0.5f);
  c = c < 0 ? 0 : (c > g->cols ? g->cols : c);
  r = r < 0 ? 0 : (r > g->rows ? g->rows : r);
  v = r * (g->cols + 1) + c;
  for (i = 0; i < g->npin; i++)
    if (g->pv[i] == v) break;
  if (i == g->npin)
    {
      if (g->npin == g->pcap)
        {
          g->pcap = g->pcap ? g->pcap * 2 : 16;
          g->pv  = realloc (g->pv,  g->pcap * sizeof (int));
          g->ptx = realloc (g->ptx, g->pcap * sizeof (float));
          g->pty = realloc (g->pty, g->pcap * sizeof (float));
        }
      g->npin++;
    }
  g->pv[i]  = v;
  g->ptx[i] = sx + ddx + (g->sx[v] - sx);
  g->pty[i] = sy + ddy + (g->sy[v] - sy);
}

/* Add a similarity pin (rotate/scale). When theta==0 and scale==1 it
 * delegates to the plain hard pin, so a non-rotating session is bit-identical
 * to the hard-pin path. Otherwise the ring is materialized at solve time by
 * apply_sim(). */
void
arap_grid_add_pin_sim (void *handle, float sx, float sy, float ddx, float ddy,
                       float theta, float scale)
{
  ArapGrid *g = handle;
  if (!g) return;
  if (theta == 0.f && scale == 1.f)
    { arap_grid_add_pin (handle, sx, sy, ddx, ddy); return; }
  if (g->nsim == g->scap)
    {
      g->scap = g->scap ? g->scap * 2 : 16;
      g->sp = realloc (g->sp, (size_t) g->scap * 6 * sizeof (float));
    }
  g->sp[g->nsim * 6 + 0] = sx;    g->sp[g->nsim * 6 + 1] = sy;
  g->sp[g->nsim * 6 + 2] = ddx;   g->sp[g->nsim * 6 + 3] = ddy;
  g->sp[g->nsim * 6 + 4] = theta; g->sp[g->nsim * 6 + 5] = scale;
  g->nsim++;
  g->sim_ver++;
}

/* True if v is a hard-pin vertex (hard pins outrank similarity rings). */
static int
sim_is_hard (ArapGrid *g, int v)
{
  int i;
  for (i = 0; i < g->npin; i++)
    if (g->pv[i] == v) return 1;
  return 0;
}

/* Materialize similarity rings into per-vertex accumulators. Invariants: each
 * vertex holds at most one sim entry; total ring weight is capped at
 * ARAP_PIN_W; hard-pin vertices are skipped; overlapping rings merge by
 * weighted average then re-cap. Called once per solve, before ensure_factor. */
static void
apply_sim (ArapGrid *g)
{
  int s, v;
  float cellw = g->sx[1] - g->sx[0];
  float cellh = g->sy[g->cols + 1] - g->sy[0];
  float R;
  if (cellw <= 0.f) cellw = 1.f;
  if (cellh <= 0.f) cellh = 1.f;
  R = ROT_CELLS * sqrtf (cellw * cellh);
  if (R < ROT_MIN_PX) R = ROT_MIN_PX;

  memset (g->accW,  0, (size_t) g->nv * sizeof (float));
  memset (g->accTX, 0, (size_t) g->nv * sizeof (float));
  memset (g->accTY, 0, (size_t) g->nv * sizeof (float));

  for (s = 0; s < g->nsim; s++)
    {
      float sx  = g->sp[s * 6 + 0], sy  = g->sp[s * 6 + 1];
      float ddx = g->sp[s * 6 + 2], ddy = g->sp[s * 6 + 3];
      float th  = g->sp[s * 6 + 4], sc  = g->sp[s * 6 + 5];
      float ct = cosf (th), st = sinf (th);
      float cx = sx + ddx, cy = sy + ddy;
      float wsum = 0.f;

      for (v = 0; v < g->nv; v++)
        {
          float ox = g->sx[v] - sx, oy = g->sy[v] - sy;
          float d2 = ox * ox + oy * oy, u, k;
          if (d2 >= R * R) continue;
          if (sim_is_hard (g, v)) continue;
          u = d2 / (R * R);
          k = (1.f - u) * (1.f - u);   /* smooth C1 falloff, zero at R */
          wsum += k;
        }
      if (wsum <= 1e-6f) continue;

      for (v = 0; v < g->nv; v++)
        {
          float ox = g->sx[v] - sx, oy = g->sy[v] - sy;
          float d2 = ox * ox + oy * oy, u, k, w, allow, newW, tx, ty;
          if (d2 >= R * R) continue;
          if (sim_is_hard (g, v)) continue;
          u = d2 / (R * R);
          k = (1.f - u) * (1.f - u);
          w = ARAP_PIN_W * k / wsum;          /* ring totals ARAP_PIN_W */
          allow = ARAP_PIN_W - g->accW[v];    /* enforce the cap        */
          if (allow <= 0.f) continue;
          if (w > allow) w = allow;
          tx = cx + sc * (ct * ox - st * oy); /* target = c + s*R(th)*o */
          ty = cy + sc * (st * ox + ct * oy);
          newW = g->accW[v] + w;
          g->accTX[v] = (g->accTX[v] * g->accW[v] + tx * w) / newW;
          g->accTY[v] = (g->accTY[v] * g->accW[v] + ty * w) / newW;
          g->accW[v]  = newW;
        }
    }
}

/* Assemble the band matrix and Cholesky-factor it, reusing the factor while
 * the structural signature (weights, pins, border flag, sim pins) is unchanged. */
static void
ensure_factor (ArapGrid *g)
{
  unsigned long sig = g->w_ver * 131u;
  int i, j, t;
  for (i = 0; i < g->npin; i++)
    sig += (unsigned long) (g->pv[i] + 1) * 2654435761u;
  sig += (unsigned long) g->pin_edge * 7919u;
  sig += g->sim_ver * 131u + (unsigned long) g->nsim * 7919u;
  if (g->fact_ok && sig == g->fact_sig) return;

  memset (g->A, 0, (size_t) g->nv * (g->band + 1) * sizeof (double));

  for (i = 0; i < g->nc; i++)
    {
      double w = g->cw[i];
      int e;
      if (w <= 0.0) continue;
      for (e = 0; e < ARAP_EDGES; e++)
        {
          int a = g->ea[i * ARAP_EDGES + e], b = g->eb[i * ARAP_EDGES + e];
          int hi = a > b ? a : b, lo = a < b ? a : b;
          g->A[(size_t) hi * (g->band + 1) + 0]         += w;
          g->A[(size_t) lo * (g->band + 1) + 0]         += w;
          g->A[(size_t) hi * (g->band + 1) + (hi - lo)] -= w;
        }
    }

  for (i = 0; i < g->npin; i++)
    g->A[(size_t) g->pv[i] * (g->band + 1) + 0] += ARAP_PIN_W;

  if (g->pin_edge)
    {
      int r, c;
      for (r = 0; r <= g->rows; r++)
        for (c = 0; c <= g->cols; c++)
          if (r == 0 || r == g->rows || c == 0 || c == g->cols)
            {
              int v = r * (g->cols + 1) + c;
              g->A[(size_t) v * (g->band + 1) + 0] += ARAP_EDGE_W;
            }
    }

  for (i = 0; i < g->nv; i++)                 /* sim diagonal */
    if (g->accW[i] > 0.f)
      g->A[(size_t) i * (g->band + 1) + 0] += (double) g->accW[i];

  for (i = 0; i < g->nv; i++)
    g->A[(size_t) i * (g->band + 1) + 0] += ARAP_REG;

  for (i = 0; i < g->nv; i++)                 /* banded Cholesky, in order */
    {
      int jmin = i - g->band > 0 ? i - g->band : 0;
      int t0   = jmin;
      for (j = jmin; j <= i; j++)
        {
          double s = g->A[(size_t) i * (g->band + 1) + (i - j)];
          for (t = t0; t < j; t++)
            s -= *LAt (g, i, i - t) * *LAt (g, j, j - t);
          if (j == i)
            {
              if (s < ARAP_CHOL_MIN) s = ARAP_CHOL_MIN;
              *LAt (g, i, 0) = sqrt (s);
            }
          else
            *LAt (g, i, i - j) = s / *LAt (g, j, 0);
        }
    }

  g->fact_sig = sig;
  g->fact_ok = 1;
}

/* Forward/back substitution on the factored band system. */
static void
band_solve (ArapGrid *g, double *r, double *x, double *tmp)
{
  int i, t;
  for (i = 0; i < g->nv; i++)
    {
      double s = r[i];
      int t0 = i - g->band > 0 ? i - g->band : 0;
      for (t = t0; t < i; t++) s -= *LAt (g, i, i - t) * tmp[t];
      tmp[i] = s / *LAt (g, i, 0);
    }
  for (i = g->nv - 1; i >= 0; i--)
    {
      double s = tmp[i];
      int t1 = i + g->band < g->nv - 1 ? i + g->band : g->nv - 1;
      for (t = i + 1; t <= t1; t++) s -= *LAt (g, t, t - i) * x[t];
      x[i] = s / *LAt (g, i, 0);
    }
}

/* Build the ARAP right-hand side: per-cell Procrustes rotation applied to rest
 * edges, plus hard-pin / border / sim Dirichlet terms and regularization. */
static void
build_rhs (ArapGrid *g)
{
  int c, i, e;
  memset (g->rx, 0, (size_t) g->nv * sizeof (double));
  memset (g->ry, 0, (size_t) g->nv * sizeof (double));

  for (c = 0; c < g->nc; c++)
    {
      double w = g->cw[c], cs = 0.0, ss = 0.0, nrm, cr, sr;
      if (w <= 0.0) continue;
      for (e = 0; e < ARAP_EDGES; e++)
        {
          int a = g->ea[c * ARAP_EDGES + e], b = g->eb[c * ARAP_EDGES + e];
          double svx = g->sx[b] - g->sx[a], svy = g->sy[b] - g->sy[a];
          double dvx = g->dx[b] - g->dx[a], dvy = g->dy[b] - g->dy[a];
          cs += svx * dvx + svy * dvy;
          ss += svx * dvy - svy * dvx;
        }
      nrm = sqrt (cs * cs + ss * ss);
      if (nrm > 1e-9) { cr = cs / nrm; sr = ss / nrm; }
      else            { cr = 1.0;      sr = 0.0; }
      for (e = 0; e < ARAP_EDGES; e++)
        {
          int a = g->ea[c * ARAP_EDGES + e], b = g->eb[c * ARAP_EDGES + e];
          double svx = g->sx[b] - g->sx[a], svy = g->sy[b] - g->sy[a];
          double tx = cr * svx - sr * svy, ty = sr * svx + cr * svy;
          g->rx[a] -= w * tx; g->rx[b] += w * tx;
          g->ry[a] -= w * ty; g->ry[b] += w * ty;
        }
    }

  for (i = 0; i < g->npin; i++)
    {
      g->rx[g->pv[i]] += ARAP_PIN_W * g->ptx[i];
      g->ry[g->pv[i]] += ARAP_PIN_W * g->pty[i];
    }

  if (g->pin_edge)
    {
      int r, c;
      for (r = 0; r <= g->rows; r++)
        for (c = 0; c <= g->cols; c++)
          if (r == 0 || r == g->rows || c == 0 || c == g->cols)
            {
              int v = r * (g->cols + 1) + c;
              g->rx[v] += ARAP_EDGE_W * (double) g->sx[v];
              g->ry[v] += ARAP_EDGE_W * (double) g->sy[v];
            }
    }

  for (i = 0; i < g->nv; i++)                 /* sim rhs */
    if (g->accW[i] > 0.f)
      {
        g->rx[i] += (double) g->accW[i] * (double) g->accTX[i];
        g->ry[i] += (double) g->accW[i] * (double) g->accTY[i];
      }

  for (i = 0; i < g->nv; i++)
    {
      g->rx[i] += ARAP_REG * g->sx[i];
      g->ry[i] += ARAP_REG * g->sy[i];
    }
}

/* Copy the current positions into the flat output buffer. */
static void
fill_out (ArapGrid *g)
{
  int i;
  for (i = 0; i < g->nv; i++)
    { g->out[i].x = g->dx[i]; g->out[i].y = g->dy[i]; }
}

/* Run up to `iters` ARAP iterations. `warm` = 0 restarts from rest (cold);
 * nonzero keeps the current positions (warm start). Returns iterations done.
 * A NaN or runaway step aborts early and falls back to rest. */
int
arap_grid_solve (void *handle, int iters, int unused0, float tol, int warm)
{
  ArapGrid *g = handle;
  double max_pin_disp = 0.0, limit, max_acc = 0.0;
  int it, i;
  (void) unused0;
  if (!g) return 0;
  if (iters < 1) iters = 1;

  if (g->npin == 0 && g->nsim == 0 && !g->pin_edge)
    {
      memcpy (g->dx, g->sx, (size_t) g->nv * sizeof (float));
      memcpy (g->dy, g->sy, (size_t) g->nv * sizeof (float));
      fill_out (g);
      return 0;
    }

  for (i = 0; i < g->npin; i++)
    {
      double dpx = g->ptx[i] - g->sx[g->pv[i]];
      double dpy = g->pty[i] - g->sy[g->pv[i]];
      double dd = sqrt (dpx * dpx + dpy * dpy);
      if (dd > max_pin_disp) max_pin_disp = dd;
    }
  /* Rotation displacement is independent of drag distance, so it must also
   * feed the runaway guard; otherwise a large-angle ring at coarse mesh would
   * trip the limit and snap back to rest. */
  {
    float cw2 = g->sx[1] - g->sx[0];
    float ch2 = g->sy[g->cols + 1] - g->sy[0];
    float R2;
    if (cw2 <= 0.f) cw2 = 1.f;
    if (ch2 <= 0.f) ch2 = 1.f;
    R2 = ROT_CELLS * sqrtf (cw2 * ch2);
    if (R2 < ROT_MIN_PX) R2 = ROT_MIN_PX;
    for (i = 0; i < g->nsim; i++)
      {
        float ddx = g->sp[i * 6 + 2], ddy = g->sp[i * 6 + 3];
        float th  = g->sp[i * 6 + 4];
        double dd = sqrt ((double) ddx * ddx + (double) ddy * ddy)
                  + 2.0 * (double) R2 * fabs (sin (th * 0.5));
        if (dd > max_pin_disp) max_pin_disp = dd;
      }
  }
  limit = 4.0 * max_pin_disp + 100.0;

  if (warm == 0)
    {
      memcpy (g->dx, g->sx, (size_t) g->nv * sizeof (float));
      memcpy (g->dy, g->sy, (size_t) g->nv * sizeof (float));
    }

  apply_sim (g);                       /* before ensure_factor: diag sees accW */
  for (i = 0; i < g->nv; i++)
    if ((double) g->accW[i] > max_acc) max_acc = (double) g->accW[i];

  ensure_factor (g);

  for (it = 0; it < iters; it++)
    {
      int bad = 0;
      double maxstep = 0.0;
      build_rhs (g);
      band_solve (g, g->rx, g->xx, g->yx);
      band_solve (g, g->ry, g->yy, g->yx);
      for (i = 0; i < g->nv; i++)
        {
          double nx = g->xx[i], ny = g->yy[i], st;
          if (!isfinite (nx) || !isfinite (ny)) { bad = 1; break; }
          st = fabs (nx - g->dx[i]) + fabs (ny - g->dy[i]);
          if (st > maxstep) maxstep = st;
        }
      if (bad || maxstep > limit)
        break;
      for (i = 0; i < g->nv; i++)
        { g->dx[i] = (float) g->xx[i]; g->dy[i] = (float) g->yy[i]; }
      if (maxstep < (double) tol) { it++; break; }
    }

  for (i = 0; i < g->nv; i++)
    if (!isfinite (g->dx[i]) || !isfinite (g->dy[i]))
      {
        memcpy (g->dx, g->sx, (size_t) g->nv * sizeof (float));
        memcpy (g->dy, g->sy, (size_t) g->nv * sizeof (float));
        break;
      }

  fill_out (g);

  if (getenv ("PW_DEBUG"))
    {
      double md = 0.0;
      for (i = 0; i < g->nv; i++)
        {
          double d = fabs ((double) g->dx[i] - g->sx[i]) +
                     fabs ((double) g->dy[i] - g->sy[i]);
          if (d > md) md = d;
        }
      fprintf (stderr,
               "[arap] npin=%d nsim=%d max_accW=%.1f pin_edge=%d warm=%d "
               "max_pin_disp=%.1f max_disp=%.1f iters=%d\n",
               g->npin, g->nsim, max_acc, g->pin_edge, (warm != 0),
               max_pin_disp, md, it);
    }
  return it;
}

/* Return the deformed vertices (n verts) in a caller-owned flat buffer. */
ArapVec2 *
arap_grid_verts (void *handle, int *n)
{
  ArapGrid *g = handle;
  if (!g) { if (n) *n = 0; return NULL; }
  if (n) *n = g->nv;
  return g->out;
}

/* Release all memory owned by the grid. */
void
arap_grid_free (void *handle)
{
  ArapGrid *g = handle;
  if (!g) return;
  free (g->sx); free (g->sy); free (g->dx); free (g->dy); free (g->cw);
  free (g->ea); free (g->eb); free (g->pv); free (g->ptx); free (g->pty);
  free (g->sp);
  free (g->accW); free (g->accTX); free (g->accTY);
  free (g->A);  free (g->L);
  free (g->rx); free (g->ry); free (g->xx); free (g->yy); free (g->yx);
  free (g->out);
  free (g);
}

/* END arap_grid.c (cleaned) - if this line is missing the file was truncated */
