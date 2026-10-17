/* ARAP grid solver: banded Cholesky with cached factor. */

#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <glib.h>

typedef struct { float x, y; } ArapVec2;

#define ARAP_PIN_W    1.0e3f
#define ARAP_EDGE_W   1.0e3f
#define ARAP_REG      1.0e-2f
#define ARAP_EDGES    6
#define ARAP_CHOL_MIN 1.0e-6
#define ROT_CELLS     3.0f
#define ROT_MIN_PX    24.0f
#define ARAP_TAPER_RANGE 3.0f

void arap_grid_free (void *handle);

typedef struct {
    int cols, rows, nv, nc, band;

    float *sx, *sy, *dx, *dy;
    float *cw;
    unsigned long w_ver;

    int *ea, *eb;
    float *ex, *ey;

    int *pv;
    float *ptx, *pty;
    int npin, pcap;

    int pin_edge;

    float *sp;
    float *spt;
    int nsim, scap;
    unsigned long sim_sig;

    unsigned char *hard;
    float *accW, *accTX, *accTY;
    float acc_max;

    double *A, *L;
    unsigned long fact_sig;
    int fact_ok;

    int nbnd;
    int *bnd;
    double *bndRX, *bndRY;

    double *regRX, *regRY;
    double *accRX, *accRY;
    int acc_dirty;

    double *rx, *ry, *xx, *yy;
    ArapVec2 *out;
} ArapGrid;

static inline unsigned long
hash_u32 (unsigned int x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return (unsigned long)x;
}

static inline unsigned long
hash_float (float f)
{
    unsigned int u;
    memcpy (&u, &f, sizeof u);
    return hash_u32 (u);
}

static int
grow_pins (ArapGrid *g, int ncap)
{
    int *pv;
    float *ptx, *pty;

    if (ncap <= g->pcap)
        return 1;

    pv = malloc ((size_t)ncap * sizeof (int));
    ptx = malloc ((size_t)ncap * sizeof (float));
    pty = malloc ((size_t)ncap * sizeof (float));

    if (!pv || !ptx || !pty)
    {
        free (pv);
        free (ptx);
        free (pty);
        return 0;
    }

    if (g->npin > 0)
    {
        memcpy (pv, g->pv, (size_t)g->npin * sizeof (int));
        memcpy (ptx, g->ptx, (size_t)g->npin * sizeof (float));
        memcpy (pty, g->pty, (size_t)g->npin * sizeof (float));
    }

    free (g->pv);
    free (g->ptx);
    free (g->pty);

    g->pv = pv;
    g->ptx = ptx;
    g->pty = pty;
    g->pcap = ncap;

    return 1;
}

static int
grow_sim (ArapGrid *g, int ncap)
{
    float *sp, *spt;

    if (ncap <= g->scap)
        return 1;

    sp = malloc ((size_t)ncap * 6 * sizeof (float));
    if (!sp)
        return 0;

    spt = malloc ((size_t)ncap * sizeof (float));
    if (!spt)
    {
        free (sp);
        return 0;
    }

    if (g->nsim > 0)
    {
        memcpy (sp, g->sp, (size_t)g->nsim * 6 * sizeof (float));
        memcpy (spt, g->spt, (size_t)g->nsim * sizeof (float));
    }

    free (g->sp);
    free (g->spt);

    g->sp = sp;
    g->spt = spt;
    g->scap = ncap;

    return 1;
}

void *
arap_grid_new (float x, float y, float w, float h, int cols, int rows)
{
    ArapGrid *g;
    int r, c, i;

    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    g = calloc (1, sizeof (ArapGrid));
    if (!g)
        return NULL;

    g->cols = cols;
    g->rows = rows;
    g->nv = (cols + 1) * (rows + 1);
    g->nc = cols * rows;
    g->band = cols + 2;

    g->sx = malloc ((size_t)g->nv * sizeof (float));
    g->sy = malloc ((size_t)g->nv * sizeof (float));
    g->dx = malloc ((size_t)g->nv * sizeof (float));
    g->dy = malloc ((size_t)g->nv * sizeof (float));
    g->cw = malloc ((size_t)g->nc * sizeof (float));

    g->ea = malloc ((size_t)g->nc * ARAP_EDGES * sizeof (int));
    g->eb = malloc ((size_t)g->nc * ARAP_EDGES * sizeof (int));
    g->ex = malloc ((size_t)g->nc * ARAP_EDGES * sizeof (float));
    g->ey = malloc ((size_t)g->nc * ARAP_EDGES * sizeof (float));

    g->out = malloc ((size_t)g->nv * sizeof (ArapVec2));

    g->rx = malloc ((size_t)g->nv * sizeof (double));
    g->ry = malloc ((size_t)g->nv * sizeof (double));
    g->xx = malloc ((size_t)g->nv * sizeof (double));
    g->yy = malloc ((size_t)g->nv * sizeof (double));

    g->A = malloc ((size_t)g->nv * (g->band + 1) * sizeof (double));
    g->L = malloc ((size_t)g->nv * (g->band + 1) * sizeof (double));

    g->accW  = calloc ((size_t)g->nv, sizeof (float));
    g->accTX = calloc ((size_t)g->nv, sizeof (float));
    g->accTY = calloc ((size_t)g->nv, sizeof (float));
    g->hard  = calloc ((size_t)g->nv, sizeof (unsigned char));

    g->bnd    = malloc ((size_t)g->nv * sizeof (int));
    g->bndRX  = malloc ((size_t)g->nv * sizeof (double));
    g->bndRY  = malloc ((size_t)g->nv * sizeof (double));

    g->regRX  = malloc ((size_t)g->nv * sizeof (double));
    g->regRY  = malloc ((size_t)g->nv * sizeof (double));

    g->accRX  = calloc ((size_t)g->nv, sizeof (double));
    g->accRY  = calloc ((size_t)g->nv, sizeof (double));

    if (!g->sx || !g->sy || !g->dx || !g->dy || !g->cw ||
        !g->ea || !g->eb || !g->ex || !g->ey || !g->out ||
        !g->rx || !g->ry || !g->xx || !g->yy ||
        !g->A || !g->L || !g->accW || !g->accTX || !g->accTY || !g->hard ||
        !g->bnd || !g->bndRX || !g->bndRY ||
        !g->regRX || !g->regRY || !g->accRX || !g->accRY)
    {
        arap_grid_free (g);
        return NULL;
    }

    for (r = 0; r <= rows; r++)
    {
        for (c = 0; c <= cols; c++)
        {
            i = r * (cols + 1) + c;
            g->sx[i] = x + (float)c * w / cols;
            g->sy[i] = y + (float)r * h / rows;
            g->dx[i] = g->sx[i];
            g->dy[i] = g->sy[i];
        }
    }

    for (i = 0; i < g->nc; i++)
        g->cw[i] = 1.0f;

    for (r = 0; r < rows; r++)
    {
        for (c = 0; c < cols; c++)
        {
            int i00 = r * (cols + 1) + c;
            int i10 = i00 + 1;
            int i01 = i00 + (cols + 1);
            int i11 = i01 + 1;
            int idx = (r * cols + c) * ARAP_EDGES;

            int *a = g->ea + idx;
            int *b = g->eb + idx;
            float *fx = g->ex + idx;
            float *fy = g->ey + idx;

            a[0] = i00; b[0] = i10;
            a[1] = i10; b[1] = i11;
            a[2] = i11; b[2] = i01;
            a[3] = i01; b[3] = i00;
            a[4] = i00; b[4] = i11;
            a[5] = i10; b[5] = i01;

            for (i = 0; i < ARAP_EDGES; i++)
            {
                int va = a[i], vb = b[i];
                fx[i] = g->sx[vb] - g->sx[va];
                fy[i] = g->sy[vb] - g->sy[va];
            }
        }
    }

    g->nbnd = 0;
    for (r = 0; r <= rows; r++)
    {
        for (c = 0; c <= cols; c++)
        {
            if (r == 0 || r == rows || c == 0 || c == cols)
            {
                int v = r * (cols + 1) + c;

                g->bnd[g->nbnd] = v;
                g->bndRX[g->nbnd] = ARAP_EDGE_W * (double)g->sx[v];
                g->bndRY[g->nbnd] = ARAP_EDGE_W * (double)g->sy[v];
                g->nbnd++;
            }
        }
    }

    for (i = 0; i < g->nv; i++)
    {
        g->regRX[i] = (double)(ARAP_REG * g->sx[i]);
        g->regRY[i] = (double)(ARAP_REG * g->sy[i]);
    }

    g->w_ver = 1;
    g->pin_edge = 0;
    g->sim_sig = 0;
    g->acc_dirty = 0;
    g->acc_max = 0.0f;

    return g;
}

void
arap_grid_set_cell_weights (void *handle, const float *w, int n)
{
    ArapGrid *g = handle;
    int i;
    int changed = 0;

    if (!g || !w || n != g->nc)
        return;

    for (i = 0; i < n; i++)
    {
        float v = w[i] > 0.03f ? w[i] : 0.0f;

        if (g->cw[i] != v)
        {
            g->cw[i] = v;
            changed = 1;
        }
    }

    if (changed)
        g->w_ver++;
}

void
arap_grid_set_pin_edge (void *handle, int on)
{
    ArapGrid *g = handle;

    if (!g)
        return;

    on = on ? 1 : 0;

    if (g->pin_edge != on)
        g->pin_edge = on;
}

void
arap_grid_reset_pins (void *handle)
{
    ArapGrid *g = handle;

    if (!g)
        return;

    g->npin = 0;
    g->nsim = 0;
    g->sim_sig = 0;
    g->acc_dirty = 0;
    g->acc_max = 0.0f;
}

void
arap_grid_add_pin (void *handle, float sx, float sy, float ddx, float ddy)
{
    ArapGrid *g = handle;
    int c, r, v, i;
    float cellw, cellh;

    if (!g)
        return;

    cellw = g->sx[1] - g->sx[0];
    cellh = g->sy[g->cols + 1] - g->sy[0];

    if (cellw == 0.0f) cellw = 1.0f;
    if (cellh == 0.0f) cellh = 1.0f;

    c = (int)floorf ((sx - g->sx[0]) / cellw + 0.5f);
    r = (int)floorf ((sy - g->sy[0]) / cellh + 0.5f);

    if (c < 0) c = 0;
    if (c > g->cols) c = g->cols;
    if (r < 0) r = 0;
    if (r > g->rows) r = g->rows;

    v = r * (g->cols + 1) + c;

    for (i = 0; i < g->npin; i++)
        if (g->pv[i] == v)
            break;

    if (i == g->npin)
    {
        int ncap = g->pcap ? g->pcap * 2 : 16;

        if (!grow_pins (g, ncap))
            return;

        g->npin++;
    }

    g->pv[i]  = v;
    g->ptx[i] = g->sx[v] + ddx;
    g->pty[i] = g->sy[v] + ddy;
}

void
arap_grid_add_pin_sim (void *handle, float sx, float sy, float ddx, float ddy,
                       float theta, float scale)
{
    ArapGrid *g = handle;
    float *q;
    unsigned long h;

    if (!g)
        return;

    if (theta == 0.0f && scale == 1.0f)
    {
        arap_grid_add_pin (handle, sx, sy, ddx, ddy);
        return;
    }

    if (g->nsim == g->scap)
    {
        int ncap = g->scap ? g->scap * 2 : 16;

        if (!grow_sim (g, ncap))
            return;
    }

    q = g->sp + g->nsim * 6;
    q[0] = sx;
    q[1] = sy;
    q[2] = ddx;
    q[3] = ddy;
    q[4] = theta;
    q[5] = scale;
    g->nsim++;

    /* Only the similarity center affects the linear operator.
       Target offset, rotation and scale only affect the right-hand side. */
    h = (hash_float (sx) * 2654435761u) ^ hash_float (sy);
    g->sim_sig = g->sim_sig * 131u + h;
}

void
arap_grid_add_pin_taper (void *handle, float sx, float sy, float ddx, float ddy,
                         float theta, float scale, float taper)
{
    ArapGrid *g = handle;
    float *q;
    unsigned long h;

    if (!g)
        return;

    /* taper<=0 */
    if (!(taper > 0.0f))
    {
        if (theta == 0.0f && scale == 1.0f)
            arap_grid_add_pin (handle, sx, sy, ddx, ddy);
        else
            arap_grid_add_pin_sim (handle, sx, sy, ddx, ddy, theta, scale);
        return;
    }

    /* taper>0: */
    arap_grid_add_pin (handle, sx, sy, ddx, ddy);

    if (g->nsim == g->scap)
    {
        int ncap = g->scap ? g->scap * 2 : 16;

        if (!grow_sim (g, ncap))
            return;
    }

    q = g->sp + g->nsim * 6;
    q[0] = sx;
    q[1] = sy;
    q[2] = ddx;
    q[3] = ddy;
    q[4] = theta;
    q[5] = scale;
    g->spt[g->nsim] = taper;
    g->nsim++;

    h = (hash_float (sx) * 2654435761u) ^ hash_float (sy) ^ hash_float (taper);
    g->sim_sig = g->sim_sig * 131u + h;
}

static void
apply_sim (ArapGrid *g)
{
    int s, v, i;
    float cellw, cellh, R;
    float mx;

    if (!g)
        return;

    if (g->nsim == 0)
    {
        g->acc_dirty = 0;
        g->acc_max = 0.0f;
        return;
    }

    cellw = g->sx[1] - g->sx[0];
    cellh = g->sy[g->cols + 1] - g->sy[0];

    if (cellw <= 0.0f) cellw = 1.0f;
    if (cellh <= 0.0f) cellh = 1.0f;

    R = ROT_CELLS * sqrtf (cellw * cellh);
    if (R < ROT_MIN_PX) R = ROT_MIN_PX;

    memset (g->hard,  0, (size_t)g->nv * sizeof (unsigned char));
    memset (g->accW,  0, (size_t)g->nv * sizeof (float));
    memset (g->accTX, 0, (size_t)g->nv * sizeof (float));
    memset (g->accTY, 0, (size_t)g->nv * sizeof (float));

    for (i = 0; i < g->npin; i++)
        g->hard[g->pv[i]] = 1;

    for (s = 0; s < g->nsim; s++)
    {
        const float *q = g->sp + s * 6;
        float sx  = q[0];
        float sy  = q[1];
        float ddx = q[2];
        float ddy = q[3];
        float th  = q[4];
        float sc  = q[5];
        float tap = g->spt ? g->spt[s] : 0.0f;
        float Rs = R * (1.0f + tap * ARAP_TAPER_RANGE);
        float R2s = Rs * Rs;
        float ct = cosf (th), st = sinf (th);
        float cx = sx + ddx, cy = sy + ddy;
        float wsum = 0.0f;

        for (v = 0; v < g->nv; v++)
        {
            float ox, oy, d2, u, k;

            if (g->hard[v])
                continue;

            ox = g->sx[v] - sx;
            oy = g->sy[v] - sy;
            d2 = ox * ox + oy * oy;

            if (d2 >= R2s)
                continue;

            u = d2 / R2s;
            k = (1.0f - u) * (1.0f - u);
            wsum += k;
        }

        if (wsum <= 1e-6f)
            continue;

        for (v = 0; v < g->nv; v++)
        {
            float ox, oy, d2, u, k, w, allow, newW, tx, ty;

            if (g->hard[v])
                continue;

            ox = g->sx[v] - sx;
            oy = g->sy[v] - sy;
            d2 = ox * ox + oy * oy;

            if (d2 >= R2s)
                continue;

            u = d2 / R2s;
            k = (1.0f - u) * (1.0f - u);
            w = ARAP_PIN_W * k / wsum;

            allow = ARAP_PIN_W - g->accW[v];
            if (allow <= 0.0f)
                continue;

            if (w > allow)
                w = allow;

            tx = cx + sc * (ct * ox - st * oy);
            ty = cy + sc * (st * ox + ct * oy);

            newW = g->accW[v] + w;
            g->accTX[v] = (g->accTX[v] * g->accW[v] + tx * w) / newW;
            g->accTY[v] = (g->accTY[v] * g->accW[v] + ty * w) / newW;
            g->accW[v]  = newW;
        }
    }

    mx = 0.0f;

    for (i = 0; i < g->nv; i++)
    {
        if (g->accW[i] > 0.0f)
        {
            g->accRX[i] = (double)g->accW[i] * (double)g->accTX[i];
            g->accRY[i] = (double)g->accW[i] * (double)g->accTY[i];

            if (g->accW[i] > mx)
                mx = g->accW[i];
        }
        else
        {
            g->accRX[i] = 0.0;
            g->accRY[i] = 0.0;
        }
    }

    g->acc_max = mx;
    g->acc_dirty = 1;
}

/* Returns 1 if the factor was rebuilt, 0 if reused. */
static int
ensure_factor (ArapGrid *g)
{
    unsigned long sig;
    int i, j, t;
    int stride = g->band + 1;

    sig = g->w_ver * 131u;

    for (i = 0; i < g->npin; i++)
        sig += (unsigned long)(g->pv[i] + 1) * 2654435761u;

    sig += (unsigned long)g->pin_edge * 7919u;
    sig += (unsigned long)g->nsim * 7919u + g->sim_sig;

    if (g->fact_ok && sig == g->fact_sig)
        return 0;

    memset (g->A, 0, (size_t)g->nv * stride * sizeof (double));

    for (i = 0; i < g->nc; i++)
    {
        double w = g->cw[i];
        int e;
        int base = i * ARAP_EDGES;

        if (w <= 0.0)
            continue;

        for (e = 0; e < ARAP_EDGES; e++)
        {
            int a = g->ea[base + e];
            int b = g->eb[base + e];
            int hi = a > b ? a : b;
            int lo = a < b ? a : b;

            g->A[(size_t)hi * stride + 0]         += w;
            g->A[(size_t)lo * stride + 0]         += w;
            g->A[(size_t)hi * stride + (hi - lo)] -= w;
        }
    }

    for (i = 0; i < g->npin; i++)
        g->A[(size_t)g->pv[i] * stride + 0] += ARAP_PIN_W;

    if (g->pin_edge)
    {
        for (i = 0; i < g->nbnd; i++)
        {
            int v = g->bnd[i];
            g->A[(size_t)v * stride + 0] += ARAP_EDGE_W;
        }
    }

    for (i = 0; i < g->nv; i++)
    {
        double d = ARAP_REG;

        if (g->acc_dirty && g->accW[i] > 0.0f)
            d += (double)g->accW[i];

        g->A[(size_t)i * stride + 0] += d;
    }

    for (i = 0; i < g->nv; i++)
    {
        int jmin = i - g->band;
        double *Ai, *Li;

        if (jmin < 0)
            jmin = 0;

        Ai = g->A + (size_t)i * stride;
        Li = g->L + (size_t)i * stride;

        for (j = jmin; j <= i; j++)
        {
            double s = Ai[i - j];
            double *Lj = g->L + (size_t)j * stride;

            for (t = jmin; t < j; t++)
                s -= Li[i - t] * Lj[j - t];

            if (j == i)
            {
                if (s < ARAP_CHOL_MIN)
                    s = ARAP_CHOL_MIN;

                Li[0] = sqrt (s);
            }
            else
            {
                Li[i - j] = s / Lj[0];
            }
        }
    }

    g->fact_sig = sig;
    g->fact_ok = 1;

    return 1;
}

static void
band_solve2 (ArapGrid *g, double *rx, double *ry, double *x, double *y)
{
    int i, t;
    int n = g->nv;
    int bw = g->band;
    int stride = bw + 1;
    double *L = g->L;

    for (i = 0; i < n; i++)
    {
        double sx = rx[i];
        double sy = ry[i];
        int t0 = i - bw;
        double d;
        double *Li;

        if (t0 < 0)
            t0 = 0;

        Li = L + (size_t)i * stride;

        for (t = t0; t < i; t++)
        {
            double l = Li[i - t];
            sx -= l * x[t];
            sy -= l * y[t];
        }

        d = Li[0];
        x[i] = sx / d;
        y[i] = sy / d;
    }

    for (i = n - 1; i >= 0; i--)
    {
        double sx = x[i];
        double sy = y[i];
        int t1 = i + bw;
        double d;

        if (t1 > n - 1)
            t1 = n - 1;

        d = L[(size_t)i * stride];

        for (t = i + 1; t <= t1; t++)
        {
            double l = L[(size_t)t * stride + (t - i)];
            sx -= l * x[t];
            sy -= l * y[t];
        }

        x[i] = sx / d;
        y[i] = sy / d;
    }
}

static void
build_rhs (ArapGrid *g)
{
    int c, i, e;
    double *rx = g->rx;
    double *ry = g->ry;

    memset (rx, 0, (size_t)g->nv * sizeof (double));
    memset (ry, 0, (size_t)g->nv * sizeof (double));

    for (c = 0; c < g->nc; c++)
    {
        double w = g->cw[c];
        double cs = 0.0, ss = 0.0, nrm, cr, sr;
        int base = c * ARAP_EDGES;

        if (w <= 0.0)
            continue;

        for (e = 0; e < ARAP_EDGES; e++)
        {
            int a = g->ea[base + e];
            int b = g->eb[base + e];
            double svx = g->ex[base + e];
            double svy = g->ey[base + e];
            double dvx = g->dx[b] - g->dx[a];
            double dvy = g->dy[b] - g->dy[a];

            cs += svx * dvx + svy * dvy;
            ss += svx * dvy - svy * dvx;
        }

        nrm = sqrt (cs * cs + ss * ss);

        if (nrm > 1e-9)
        {
            cr = cs / nrm;
            sr = ss / nrm;
        }
        else
        {
            cr = 1.0;
            sr = 0.0;
        }

        for (e = 0; e < ARAP_EDGES; e++)
        {
            int a = g->ea[base + e];
            int b = g->eb[base + e];
            double svx = g->ex[base + e];
            double svy = g->ey[base + e];
            double tx = cr * svx - sr * svy;
            double ty = sr * svx + cr * svy;

            rx[a] -= w * tx;
            rx[b] += w * tx;
            ry[a] -= w * ty;
            ry[b] += w * ty;
        }
    }

    for (i = 0; i < g->npin; i++)
    {
        rx[g->pv[i]] += ARAP_PIN_W * g->ptx[i];
        ry[g->pv[i]] += ARAP_PIN_W * g->pty[i];
    }

    if (g->pin_edge)
    {
        for (i = 0; i < g->nbnd; i++)
        {
            int v = g->bnd[i];
            rx[v] += g->bndRX[i];
            ry[v] += g->bndRY[i];
        }
    }

    if (g->acc_dirty)
    {
        for (i = 0; i < g->nv; i++)
        {
            rx[i] += g->accRX[i];
            ry[i] += g->accRY[i];

            rx[i] += g->regRX[i];
            ry[i] += g->regRY[i];
        }
    }
    else
    {
        for (i = 0; i < g->nv; i++)
        {
            rx[i] += g->regRX[i];
            ry[i] += g->regRY[i];
        }
    }
}

static void
fill_out (ArapGrid *g)
{
    int i;

    for (i = 0; i < g->nv; i++)
    {
        g->out[i].x = g->dx[i];
        g->out[i].y = g->dy[i];
    }
}

int
arap_grid_solve (void *handle, int iters, int unused0, float tol, int warm)
{
    ArapGrid *g = handle;
    double max_pin_disp = 0.0, limit;
    int it, i;
    int factor_new;

    (void)unused0;

    if (!g)
        return 0;

    if (iters < 1)
        iters = 1;

    if (g->npin == 0 && g->nsim == 0 && !g->pin_edge)
    {
        memcpy (g->dx, g->sx, (size_t)g->nv * sizeof (float));
        memcpy (g->dy, g->sy, (size_t)g->nv * sizeof (float));

        g->acc_dirty = 0;
        g->acc_max = 0.0f;

        fill_out (g);
        return 0;
    }

    for (i = 0; i < g->npin; i++)
    {
        double dpx = g->ptx[i] - g->sx[g->pv[i]];
        double dpy = g->pty[i] - g->sy[g->pv[i]];
        double dd = sqrt (dpx * dpx + dpy * dpy);

        if (dd > max_pin_disp)
            max_pin_disp = dd;
    }

    {
        float cw2 = g->sx[1] - g->sx[0];
        float ch2 = g->sy[g->cols + 1] - g->sy[0];
        float rad;

        if (cw2 <= 0.0f) cw2 = 1.0f;
        if (ch2 <= 0.0f) ch2 = 1.0f;

        rad = ROT_CELLS * sqrtf (cw2 * ch2);
        if (rad < ROT_MIN_PX)
            rad = ROT_MIN_PX;

        for (i = 0; i < g->nsim; i++)
        {
            const float *q = g->sp + i * 6;
            float ddx = q[2];
            float ddy = q[3];
            float th  = q[4];
            double dd = sqrt ((double)ddx * ddx + (double)ddy * ddy)
                      + 2.0 * (double)rad * fabs (sin (th * 0.5f));

            if (dd > max_pin_disp)
                max_pin_disp = dd;
        }
    }

    limit = 4.0 * max_pin_disp + 100.0;

    if (warm == 0)
    {
        memcpy (g->dx, g->sx, (size_t)g->nv * sizeof (float));
        memcpy (g->dy, g->sy, (size_t)g->nv * sizeof (float));
    }

    apply_sim (g);
    factor_new = ensure_factor (g);

    for (it = 0; it < iters; it++)
    {
        int bad = 0;
        double maxstep = 0.0;

        build_rhs (g);
        band_solve2 (g, g->rx, g->ry, g->xx, g->yy);

        for (i = 0; i < g->nv; i++)
        {
            double nx = g->xx[i];
            double ny = g->yy[i];
            double st;

            if (!isfinite (nx) || !isfinite (ny))
            {
                bad = 1;
                break;
            }

            st = fabs (nx - g->dx[i]) + fabs (ny - g->dy[i]);

            if (st > maxstep)
                maxstep = st;
        }

        if (bad || maxstep > limit)
            break;

        for (i = 0; i < g->nv; i++)
        {
            g->dx[i] = (float)g->xx[i];
            g->dy[i] = (float)g->yy[i];
        }

        if (maxstep < (double)tol)
        {
            it++;
            break;
        }
    }

    for (i = 0; i < g->nv; i++)
    {
        if (!isfinite (g->dx[i]) || !isfinite (g->dy[i]))
        {
            memcpy (g->dx, g->sx, (size_t)g->nv * sizeof (float));
            memcpy (g->dy, g->sy, (size_t)g->nv * sizeof (float));
            break;
        }
    }

    fill_out (g);

    if (getenv ("PW_DEBUG"))
    {
        double md = 0.0;

        for (i = 0; i < g->nv; i++)
        {
            double d = fabs ((double)g->dx[i] - g->sx[i]) +
                       fabs ((double)g->dy[i] - g->sy[i]);

            if (d > md)
                md = d;
        }

        fprintf (stderr,
                 "[arap] npin=%d nsim=%d max_accW=%.1f pin_edge=%d warm=%d "
                 "max_pin_disp=%.1f max_disp=%.1f iters=%d\n",
                 g->npin, g->nsim, (double)g->acc_max, g->pin_edge,
                 (warm != 0), max_pin_disp, md, it);
    }

    if (getenv ("PW_ARAP_PROF"))
    {
        fprintf (stderr,
                 "[arap-prof] factor_new=%d iters=%d npin=%d nsim=%d "
                 "acc_dirty=%d w_ver=%lu pin_edge=%d\n",
                 factor_new, it, g->npin, g->nsim,
                 g->acc_dirty, g->w_ver, g->pin_edge);
    }

    return it;
}

ArapVec2 *
arap_grid_verts (void *handle, int *n)
{
    ArapGrid *g = handle;

    if (!g)
    {
        if (n)
            *n = 0;

        return NULL;
    }

    if (n)
        *n = g->nv;

    return g->out;
}

void
arap_grid_free (void *handle)
{
    ArapGrid *g = handle;

    if (!g)
        return;

    free (g->sx);
    free (g->sy);
    free (g->dx);
    free (g->dy);
    free (g->cw);

    free (g->ea);
    free (g->eb);
    free (g->ex);
    free (g->ey);

    free (g->pv);
    free (g->ptx);
    free (g->pty);
    free (g->sp);
    free (g->spt);

    free (g->hard);
    free (g->accW);
    free (g->accTX);
    free (g->accTY);

    free (g->A);
    free (g->L);

    free (g->bnd);
    free (g->bndRX);
    free (g->bndRY);

    free (g->regRX);
    free (g->regRY);

    free (g->accRX);
    free (g->accRY);

    free (g->rx);
    free (g->ry);
    free (g->xx);
    free (g->yy);

    free (g->out);
    free (g);
}
