/*
 * Pixelica - optional analytic two-bone IK. See pixelica/ik.h for the contract.
 * All geometry runs in double; only the final joint/end are rounded to float.
 */
#include "pixelica/ik.h"

#include <float.h>
#include <math.h>
#include <stddef.h>

/* Pole offsets whose off-axis part is below this fraction use the fallback. */
#define PIXELICA_IK_POLE_EPS 1e-12

typedef struct IKVec {
    double x, y, z;
} IKVec;

static IKVec ik_vec(PIXELICA_Vec3 v)
{
    IKVec r;

    r.x = (double)v.x;
    r.y = (double)v.y;
    r.z = (double)v.z;
    return r;
}

static IKVec ik_sub(IKVec a, IKVec b)
{
    IKVec r;

    r.x = a.x - b.x;
    r.y = a.y - b.y;
    r.z = a.z - b.z;
    return r;
}

/* a + b * s */
static IKVec ik_mad(IKVec a, IKVec b, double s)
{
    IKVec r;

    r.x = a.x + b.x * s;
    r.y = a.y + b.y * s;
    r.z = a.z + b.z * s;
    return r;
}

static double ik_dot(IKVec a, IKVec b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static double ik_len(IKVec a)
{
    return sqrt(ik_dot(a, a));
}

static IKVec ik_div(IKVec a, double s)
{
    IKVec r;

    r.x = a.x / s;
    r.y = a.y / s;
    r.z = a.z / s;
    return r;
}

static bool ik_finite3(PIXELICA_Vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static bool ik_fits_float(IKVec v)
{
    /* Also false for NaN. */
    return fabs(v.x) <= (double)FLT_MAX && fabs(v.y) <= (double)FLT_MAX &&
           fabs(v.z) <= (double)FLT_MAX;
}

static PIXELICA_Vec3 ik_to_float(IKVec v)
{
    PIXELICA_Vec3 r;

    r.x = (float)v.x;
    r.y = (float)v.y;
    r.z = (float)v.z;
    return r;
}

/* Remove the component of v along unit f; applied twice to cancel roundoff. */
static IKVec ik_reject(IKVec v, IKVec f)
{
    v = ik_mad(v, f, -ik_dot(v, f));
    return ik_mad(v, f, -ik_dot(v, f));
}

/* Unit vector perpendicular to unit `f`, pointing toward `p` (offset of the
 * pole from the root), with the documented deterministic axis fallback. */
static IKVec ik_bend_direction(IKVec f, IKVec p)
{
    double pn = ik_len(p);
    IKVec axis;
    IKVec perp;
    double n;

    if (pn > 0.0) {
        perp = ik_reject(p, f);
        n = ik_len(perp);
        if (n > PIXELICA_IK_POLE_EPS * pn) {
            return ik_div(perp, n);
        }
    }

    axis.x = axis.y = axis.z = 0.0;
    if (fabs(f.x) <= fabs(f.y) && fabs(f.x) <= fabs(f.z)) {
        axis.x = 1.0;
    } else if (fabs(f.y) <= fabs(f.z)) {
        axis.y = 1.0;
    } else {
        axis.z = 1.0;
    }
    perp = ik_reject(axis, f);
    return ik_div(perp, ik_len(perp));
}

PIXELICA_Status pixelica_ik_two_bone(const PIXELICA_IKTwoBone *chain, PIXELICA_IKTwoBoneResult *out)
{
    IKVec root;
    IKVec target;
    IKVec delta;
    IKVec fwd;
    IKVec bend;
    IKVec joint;
    IKVec end;
    double l0;
    double l1;
    double lo;
    double hi;
    double d;
    double dc;
    double a;
    double h;
    bool clamped;

    if (chain == NULL || out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!ik_finite3(chain->root) || !ik_finite3(chain->target) ||
        !ik_finite3(chain->pole) || !isfinite(chain->upper_length) ||
        !isfinite(chain->lower_length) || !(chain->upper_length > 0.0f) ||
        !(chain->lower_length > 0.0f)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    root = ik_vec(chain->root);
    target = ik_vec(chain->target);
    l0 = (double)chain->upper_length;
    l1 = (double)chain->lower_length;

    delta = ik_sub(target, root);
    d = ik_len(delta);
    if (d > 0.0) {
        fwd = ik_div(delta, d);
    } else {
        fwd.x = 1.0;
        fwd.y = 0.0;
        fwd.z = 0.0;
    }

    lo = fabs(l0 - l1);
    hi = l0 + l1;
    clamped = d < lo || d > hi;
    dc = d;
    if (d < lo) {
        dc = lo;
    } else if (d > hi) {
        dc = hi;
    }

    end = clamped ? ik_mad(root, fwd, dc) : target;
    bend = ik_bend_direction(fwd, ik_sub(ik_vec(chain->pole), root));

    if (dc > 0.0) {
        a = 0.5 * (dc + (l0 - l1) * (l0 + l1) / dc);
        if (a > l0) {
            a = l0;
        } else if (a < -l0) {
            a = -l0;
        }
        h = (l0 - a) * (l0 + a);
        h = h > 0.0 ? sqrt(h) : 0.0;
    } else {
        /* Equal lengths folded back onto the root. */
        a = 0.0;
        h = l0;
    }
    joint = ik_mad(ik_mad(root, fwd, a), bend, h);

    if (!ik_fits_float(joint) || !ik_fits_float(end)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    out->joint = ik_to_float(joint);
    out->end = ik_to_float(end);
    out->clamped = clamped;
    return PIXELICA_OK;
}
