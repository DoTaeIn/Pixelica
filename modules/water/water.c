/*
 * Pixelica - ocean surface heightfield + sample-point buoyancy (Pixelica::water).
 *
 * See include/pixelica/water.h for the model, the units and the conventions.
 * Implementation notes that are NOT part of the contract:
 *
 *   - Time is double end to end. It is never narrowed to float anywhere in
 *     here, and each wave's phase is reduced modulo 2*pi (in double, with
 *     fmod, which is exact for IEEE doubles) before sin/cos. A world far from
 *     the origin and a session hours long therefore keep the accuracy of the
 *     first frame.
 *   - theta = k * (dhat . p) - omega * t + phase. The MINUS is what makes the
 *     wave travel along dhat; d/dt of that is -A * omega * cos(theta), so the
 *     vertical velocity carries the same sign convention.
 *   - Every accumulation runs in double and is narrowed once, at the end.
 *   - Nothing is written to an output object until every value in it is known
 *     to be finite: results are assembled in a local and committed in one
 *     assignment. That is what makes the "on error the output is untouched"
 *     promise in the header true by construction rather than by inspection.
 *   - No heap, no file, no global mutable state, no recursion, no VLA.
 */
#include "pixelica/water.h"

#include <math.h>
#include <string.h>

#define WATER_TWO_PI 6.283185307179586476925286766559

/* ---- small helpers ------------------------------------------------------- */

static int water_finite(float v)
{
    return isfinite((double)v) != 0;
}

static int water_finite_vec3(PIXELICA_Vec3 v)
{
    return water_finite(v.x) && water_finite(v.y) && water_finite(v.z);
}

/* Finite and lo <= v <= hi. */
static int water_in_range(float v, float lo, float hi)
{
    return water_finite(v) && v >= lo && v <= hi;
}

static PIXELICA_Vec3 water_vec3(double x, double y, double z)
{
    PIXELICA_Vec3 v;

    v.x = (float)x;
    v.y = (float)y;
    v.z = (float)z;
    return v;
}

/* fmod keeps the magnitude bounded without losing the low bits of `v`. */
static double water_wrap_phase(double v)
{
    return fmod(v, WATER_TWO_PI);
}

/* ---- validation --------------------------------------------------------- */

static PIXELICA_Status water_check_surface(const PIXELICA_WaterSurface *surface)
{
    uint32_t i;

    if (surface == NULL || surface->wave_count > PIXELICA_WATER_MAX_WAVES) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_in_range(surface->base_height, -PIXELICA_WATER_MAX_COORD,
                        PIXELICA_WATER_MAX_COORD) ||
        !water_in_range(surface->current_x, -PIXELICA_WATER_MAX_SPEED,
                        PIXELICA_WATER_MAX_SPEED) ||
        !water_in_range(surface->current_z, -PIXELICA_WATER_MAX_SPEED,
                        PIXELICA_WATER_MAX_SPEED)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < surface->wave_count; ++i) {
        const PIXELICA_WaterWave *w = &surface->waves[i];
        double length2;

        if (!water_finite(w->dir_x) || !water_finite(w->dir_z)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        length2 = (double)w->dir_x * (double)w->dir_x + (double)w->dir_z * (double)w->dir_z;
        if (!(length2 > 0.0) || !isfinite(length2)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (!water_in_range(w->amplitude, 0.0f, PIXELICA_WATER_MAX_AMPLITUDE) ||
            !water_in_range(w->wavelength, PIXELICA_WATER_MIN_WAVELENGTH,
                            PIXELICA_WATER_MAX_WAVELENGTH) ||
            !water_in_range(w->speed, -PIXELICA_WATER_MAX_SPEED, PIXELICA_WATER_MAX_SPEED) ||
            !water_finite(w->phase)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

static PIXELICA_Status water_check_rig(const PIXELICA_WaterRig *rig)
{
    uint32_t i;

    if (rig == NULL || rig->probes == NULL || rig->probe_count == 0u ||
        rig->probe_count > PIXELICA_WATER_MAX_PROBES) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_in_range(rig->density, 0.0f, PIXELICA_WATER_MAX_DENSITY) ||
        rig->density <= 0.0f ||
        !water_in_range(rig->gravity, 0.0f, PIXELICA_WATER_MAX_GRAVITY) ||
        rig->gravity <= 0.0f) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    for (i = 0; i < rig->probe_count; ++i) {
        const PIXELICA_WaterProbe *p = &rig->probes[i];

        if (!water_in_range(p->offset.x, -PIXELICA_WATER_MAX_OFFSET, PIXELICA_WATER_MAX_OFFSET) ||
            !water_in_range(p->offset.y, -PIXELICA_WATER_MAX_OFFSET, PIXELICA_WATER_MAX_OFFSET) ||
            !water_in_range(p->offset.z, -PIXELICA_WATER_MAX_OFFSET, PIXELICA_WATER_MAX_OFFSET)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        if (!water_in_range(p->area, 0.0f, PIXELICA_WATER_MAX_AREA) || p->area <= 0.0f ||
            !water_in_range(p->max_depth, 0.0f, PIXELICA_WATER_MAX_DEPTH) ||
            p->max_depth <= 0.0f ||
            !water_in_range(p->drag_normal, 0.0f, PIXELICA_WATER_MAX_DRAG) ||
            !water_in_range(p->drag_tangential, 0.0f, PIXELICA_WATER_MAX_DRAG)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
    }
    return PIXELICA_OK;
}

static PIXELICA_Status water_check_body(const PIXELICA_WaterBody *body)
{
    double q2;

    if (body == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_finite_vec3(body->velocity) || !water_finite_vec3(body->angular_velocity)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_in_range(body->position.x, -PIXELICA_WATER_MAX_COORD, PIXELICA_WATER_MAX_COORD) ||
        !water_in_range(body->position.y, -PIXELICA_WATER_MAX_COORD, PIXELICA_WATER_MAX_COORD) ||
        !water_in_range(body->position.z, -PIXELICA_WATER_MAX_COORD, PIXELICA_WATER_MAX_COORD)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_in_range(body->mass, 0.0f, PIXELICA_WATER_MAX_MASS) || body->mass <= 0.0f) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_in_range(body->inertia.x, 0.0f, PIXELICA_WATER_MAX_INERTIA) ||
        !water_in_range(body->inertia.y, 0.0f, PIXELICA_WATER_MAX_INERTIA) ||
        !water_in_range(body->inertia.z, 0.0f, PIXELICA_WATER_MAX_INERTIA) ||
        body->inertia.x <= 0.0f || body->inertia.y <= 0.0f || body->inertia.z <= 0.0f) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_finite(body->orientation.x) || !water_finite(body->orientation.y) ||
        !water_finite(body->orientation.z) || !water_finite(body->orientation.w)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    q2 = (double)body->orientation.x * (double)body->orientation.x +
         (double)body->orientation.y * (double)body->orientation.y +
         (double)body->orientation.z * (double)body->orientation.z +
         (double)body->orientation.w * (double)body->orientation.w;
    if (!(q2 > 0.0) || !isfinite(q2)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    return PIXELICA_OK;
}

/* ---- surface ------------------------------------------------------------- */

PIXELICA_Status pixelica_water_surface_default(PIXELICA_WaterSurface *out)
{
    PIXELICA_WaterSurface surface;

    if (out == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    memset(&surface, 0, sizeof surface);
    surface.wave_count = 2u;
    surface.base_height = 0.0f;
    surface.current_x = 0.0f;
    surface.current_z = 0.0f;
    /*
     * Two swells crossing at about 70 degrees. Both wavelengths are several
     * metres, so a boat-sized hull rides them instead of averaging them away,
     * and the two amplitudes are deliberately comparable so the sea tilts a
     * hull about BOTH horizontal axes rather than only one.
     * Max slope per wave: 0.095 * 2pi/8 = 0.075 rad and 0.058 * 2pi/4.5 =
     * 0.081 rad, i.e. about 4.5 degrees each, up to about 9 degrees where
     * they add.
     */
    surface.waves[0].dir_x = 1.0f;
    surface.waves[0].dir_z = 0.25f;
    surface.waves[0].amplitude = 0.095f;
    surface.waves[0].wavelength = 8.0f;
    surface.waves[0].speed = 3.0f;
    surface.waves[0].phase = 0.0f;
    surface.waves[1].dir_x = 0.35f;
    surface.waves[1].dir_z = -1.0f;
    surface.waves[1].amplitude = 0.058f;
    surface.waves[1].wavelength = 4.5f;
    surface.waves[1].speed = 2.2f;
    surface.waves[1].phase = 1.1f;
    *out = surface;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_water_surface_validate(const PIXELICA_WaterSurface *surface)
{
    return water_check_surface(surface);
}

PIXELICA_Status pixelica_water_wave_period(const PIXELICA_WaterSurface *surface,
                                           uint32_t index, float *out_seconds)
{
    const PIXELICA_WaterWave *wave;
    double speed, period;

    if (out_seconds == NULL || water_check_surface(surface) != PIXELICA_OK ||
        index >= surface->wave_count) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    wave = &surface->waves[index];
    speed = fabs((double)wave->speed);
    if (speed == 0.0) {
        *out_seconds = 0.0f; /* a standing wave does not repeat: it never moves */
        return PIXELICA_OK;
    }
    period = (double)wave->wavelength / speed;
    if (!isfinite(period)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_seconds = (float)period;
    return PIXELICA_OK;
}

/*
 * The shared evaluation. `need_derivatives` lets pixelica_water_height() skip
 * the cos() calls; the height itself is bit-identical either way because it
 * only uses sin().
 */
static PIXELICA_Status water_evaluate(const PIXELICA_WaterSurface *surface, float x, float z,
                                      double time_seconds, int need_derivatives,
                                      PIXELICA_WaterSample *out)
{
    PIXELICA_WaterSample sample;
    double height, slope_x, slope_z, dhdt, material_y, length, inverse;
    uint32_t i;

    if (out == NULL || water_check_surface(surface) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    /* The position is bounded so the spatial phase stays computable; time is
     * only required to be finite (see "TIME IS double" in the header). */
    if (!water_in_range(x, -PIXELICA_WATER_MAX_COORD, PIXELICA_WATER_MAX_COORD) ||
        !water_in_range(z, -PIXELICA_WATER_MAX_COORD, PIXELICA_WATER_MAX_COORD) ||
        isfinite(time_seconds) == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    height = (double)surface->base_height;
    slope_x = 0.0;
    slope_z = 0.0;
    dhdt = 0.0;
    for (i = 0; i < surface->wave_count; ++i) {
        const PIXELICA_WaterWave *w = &surface->waves[i];
        double dir_x = (double)w->dir_x;
        double dir_z = (double)w->dir_z;
        double amplitude = (double)w->amplitude;
        double direction = sqrt(dir_x * dir_x + dir_z * dir_z);
        double k, omega, theta;

        dir_x /= direction; /* water_check_surface proved direction > 0 */
        dir_z /= direction;
        k = WATER_TWO_PI / (double)w->wavelength;
        omega = k * (double)w->speed;
        /*
         * theta = k * (dhat . p) - omega * t + phase.
         *
         * The minus sign is the travelling-wave convention: a feature of the
         * profile keeps theta constant, hence moves along +dhat at `speed`.
         * Each term is reduced before they are added, so neither a far-away
         * position nor a large time can swamp the others' low bits.
         */
        theta = water_wrap_phase(k * (dir_x * (double)x + dir_z * (double)z)) -
                water_wrap_phase(omega * time_seconds) + water_wrap_phase((double)w->phase);
        height += amplitude * sin(theta);
        if (need_derivatives) {
            double cosine = cos(theta);

            slope_x += amplitude * k * dir_x * cosine;
            slope_z += amplitude * k * dir_z * cosine;
            dhdt += -amplitude * omega * cosine; /* d/dt of sin(... - omega t) */
        }
    }
    if (!isfinite(height) || !isfinite(slope_x) || !isfinite(slope_z) || !isfinite(dhdt)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /*
     * The material (kinematic) vertical velocity of the surface point when the
     * whole surface is advected by the uniform current:
     *     D h / D t = partial h / partial t + U . grad h.
     * With no current this is exactly the Eulerian partial derivative.
     */
    material_y = dhdt + (double)surface->current_x * slope_x +
                 (double)surface->current_z * slope_z;
    if (!isfinite(material_y)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /* normal = normalize(-dh/dx, 1, -dh/dz): always finite, always +Y up. */
    length = sqrt(slope_x * slope_x + 1.0 + slope_z * slope_z);
    if (!isfinite(length) || !(length > 0.0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    inverse = 1.0 / length;
    sample.height = (float)height;
    sample.normal = water_vec3(-slope_x * inverse, inverse, -slope_z * inverse);
    sample.slope_x = (float)slope_x;
    sample.slope_z = (float)slope_z;
    sample.eulerian_dhdt = (float)dhdt;
    sample.surface_velocity_y = (float)material_y;
    sample.velocity =
        water_vec3((double)surface->current_x, material_y, (double)surface->current_z);
    if (!water_finite(sample.height) || !water_finite_vec3(sample.normal) ||
        !water_finite(sample.slope_x) || !water_finite(sample.slope_z) ||
        !water_finite(sample.eulerian_dhdt) || !water_finite(sample.surface_velocity_y) ||
        !water_finite_vec3(sample.velocity)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = sample;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_water_sample(const PIXELICA_WaterSurface *surface, float x, float z,
                                      double time_seconds, PIXELICA_WaterSample *out)
{
    return water_evaluate(surface, x, z, time_seconds, 1, out);
}

PIXELICA_Status pixelica_water_height(const PIXELICA_WaterSurface *surface, float x, float z,
                                      double time_seconds, float *out_height)
{
    PIXELICA_WaterSample sample;
    PIXELICA_Status status;

    if (out_height == NULL) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    status = water_evaluate(surface, x, z, time_seconds, 0, &sample);
    if (status != PIXELICA_OK) {
        return status;
    }
    *out_height = sample.height;
    return PIXELICA_OK;
}

/* ---- rotations ---------------------------------------------------------- */

/*
 * The column-major rotation matrix of a (not necessarily unit) quaternion, via
 * the common library so the convention cannot drift from the rest of the
 * engine. R * v rotates body -> world, R^T * v world -> body.
 */
static PIXELICA_Status water_rotation(PIXELICA_Quat q, float out[16])
{
    return pixelica_quat_to_mat4(out, q);
}

static PIXELICA_Vec3 water_rotate(const float r[16], PIXELICA_Vec3 v)
{
    return water_vec3(
        (double)r[0] * (double)v.x + (double)r[4] * (double)v.y + (double)r[8] * (double)v.z,
        (double)r[1] * (double)v.x + (double)r[5] * (double)v.y + (double)r[9] * (double)v.z,
        (double)r[2] * (double)v.x + (double)r[6] * (double)v.y + (double)r[10] * (double)v.z);
}

static PIXELICA_Vec3 water_rotate_inverse(const float r[16], PIXELICA_Vec3 v)
{
    return water_vec3(
        (double)r[0] * (double)v.x + (double)r[1] * (double)v.y + (double)r[2] * (double)v.z,
        (double)r[4] * (double)v.x + (double)r[5] * (double)v.y + (double)r[6] * (double)v.z,
        (double)r[8] * (double)v.x + (double)r[9] * (double)v.y + (double)r[10] * (double)v.z);
}

/* Hamilton product a * b, matching pixelica/math.h (w = scalar part). */
static PIXELICA_Quat water_quat_multiply(PIXELICA_Quat a, PIXELICA_Quat b)
{
    PIXELICA_Quat q;

    q.w = (float)((double)a.w * (double)b.w - (double)a.x * (double)b.x -
                  (double)a.y * (double)b.y - (double)a.z * (double)b.z);
    q.x = (float)((double)a.w * (double)b.x + (double)a.x * (double)b.w +
                  (double)a.y * (double)b.z - (double)a.z * (double)b.y);
    q.y = (float)((double)a.w * (double)b.y - (double)a.x * (double)b.z +
                  (double)a.y * (double)b.w + (double)a.z * (double)b.x);
    q.z = (float)((double)a.w * (double)b.z + (double)a.x * (double)b.y -
                  (double)a.y * (double)b.x + (double)a.z * (double)b.w);
    return q;
}

/* ---- buoyancy ----------------------------------------------------------- */

PIXELICA_Status pixelica_water_load(const PIXELICA_WaterSurface *surface,
                                    const PIXELICA_WaterRig *rig,
                                    const PIXELICA_WaterBody *body, double time_seconds,
                                    PIXELICA_WaterLoad *out)
{
    PIXELICA_WaterLoad load;
    float rotation[16];
    double buoyancy_y = 0.0;
    double drag[3] = {0.0, 0.0, 0.0};
    double torque[3] = {0.0, 0.0, 0.0};
    double volume = 0.0, wetted_area = 0.0;
    uint32_t submerged = 0u;
    uint32_t i;

    if (out == NULL || water_check_surface(surface) != PIXELICA_OK ||
        water_check_rig(rig) != PIXELICA_OK || water_check_body(body) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (isfinite(time_seconds) == 0) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (water_rotation(body->orientation, rotation) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    for (i = 0; i < rig->probe_count; ++i) {
        const PIXELICA_WaterProbe *probe = &rig->probes[i];
        PIXELICA_WaterSample sample;
        PIXELICA_Vec3 arm = water_rotate(rotation, probe->offset);
        double point_x, point_y, point_z;
        double depth, fraction, displaced, wetted;
        double omega_cross[3], relative[3], force[3];

        if (!water_finite_vec3(arm)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        point_x = (double)body->position.x + (double)arm.x;
        point_y = (double)body->position.y + (double)arm.y;
        point_z = (double)body->position.z + (double)arm.z;
        if (!isfinite(point_x) || !isfinite(point_y) || !isfinite(point_z)) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        /* Rendering and physics go through this very call, with this very
         * `time_seconds`: that is the whole point of one sampling entry point.
         * It also range-checks the probe position for us. */
        if (pixelica_water_sample(surface, (float)point_x, (float)point_z, time_seconds,
                                  &sample) != PIXELICA_OK) {
            return PIXELICA_ERR_INVALID_ARG;
        }

        depth = (double)sample.height - point_y;
        if (!(depth > 0.0)) {
            continue; /* above the surface: no buoyancy, no drag */
        }
        fraction = depth / (double)probe->max_depth;
        if (fraction > 1.0) {
            fraction = 1.0; /* the per-probe volume limit */
        }
        displaced = (double)probe->area * (double)probe->max_depth * fraction;
        wetted = (double)probe->area * fraction;

        /* Hydrostatic lift: strictly vertical, never tilted with the normal. */
        force[0] = 0.0;
        force[1] = (double)rig->density * (double)rig->gravity * displaced;
        force[2] = 0.0;
        buoyancy_y += force[1];

        /* v_point = v + omega x r, so a rotating body is damped too. */
        omega_cross[0] = (double)body->angular_velocity.y * (double)arm.z -
                         (double)body->angular_velocity.z * (double)arm.y;
        omega_cross[1] = (double)body->angular_velocity.z * (double)arm.x -
                         (double)body->angular_velocity.x * (double)arm.z;
        omega_cross[2] = (double)body->angular_velocity.x * (double)arm.y -
                         (double)body->angular_velocity.y * (double)arm.x;
        /* The water velocity is the coherent kinematic pair documented on
         * PIXELICA_WaterSample: (current, Dh/Dt). */
        relative[0] = (double)body->velocity.x + omega_cross[0] - (double)sample.velocity.x;
        relative[1] = (double)body->velocity.y + omega_cross[1] - (double)sample.velocity.y;
        relative[2] = (double)body->velocity.z + omega_cross[2] - (double)sample.velocity.z;

        {
            double normal_drag = -(double)probe->drag_normal * wetted * relative[1];
            double tangential = -(double)probe->drag_tangential * wetted;

            drag[0] += tangential * relative[0];
            drag[1] += normal_drag;
            drag[2] += tangential * relative[2];
            force[0] += tangential * relative[0];
            force[1] += normal_drag;
            force[2] += tangential * relative[2];
        }

        torque[0] += (double)arm.y * force[2] - (double)arm.z * force[1];
        torque[1] += (double)arm.z * force[0] - (double)arm.x * force[2];
        torque[2] += (double)arm.x * force[1] - (double)arm.y * force[0];

        volume += displaced;
        wetted_area += wetted;
        ++submerged;
    }

    load.buoyancy = water_vec3(0.0, buoyancy_y, 0.0);
    load.drag = water_vec3(drag[0], drag[1], drag[2]);
    /* Gravity acts at the centre of mass: it is part of the force and
     * contributes nothing to the torque. */
    load.force = water_vec3(
        drag[0], buoyancy_y + drag[1] - (double)body->mass * (double)rig->gravity, drag[2]);
    load.torque = water_vec3(torque[0], torque[1], torque[2]);
    load.submerged_volume = (float)volume;
    load.submerged_area = (float)wetted_area;
    load.submerged_probes = submerged;
    if (!water_finite_vec3(load.force) || !water_finite_vec3(load.torque) ||
        !water_finite_vec3(load.buoyancy) || !water_finite_vec3(load.drag) ||
        !water_finite(load.submerged_volume) || !water_finite(load.submerged_area)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out = load;
    return PIXELICA_OK;
}

/*
 * V(draft) = sum_i area_i * clamp(draft - offset_i.y, 0, max_depth_i), the
 * displaced volume of an UPRIGHT rig whose centre of mass sits `draft` below a
 * flat surface. Continuous, piecewise linear, non-decreasing.
 */
static double water_displaced_volume(const PIXELICA_WaterRig *rig, double draft)
{
    double total = 0.0;
    uint32_t i;

    for (i = 0; i < rig->probe_count; ++i) {
        double depth = draft - (double)rig->probes[i].offset.y;
        double limit = (double)rig->probes[i].max_depth;

        if (!(depth > 0.0)) {
            continue;
        }
        if (depth > limit) {
            depth = limit;
        }
        total += (double)rig->probes[i].area * depth;
    }
    return total;
}

PIXELICA_Status pixelica_water_equilibrium_draft(const PIXELICA_WaterRig *rig, float mass,
                                                 float *out_draft)
{
    double breakpoints[2u * PIXELICA_WATER_MAX_PROBES];
    double target, capacity = 0.0, draft = 0.0;
    uint32_t count = 0u, i, j;
    int solved = 0;

    if (out_draft == NULL || water_check_rig(rig) != PIXELICA_OK ||
        !water_in_range(mass, 0.0f, PIXELICA_WATER_MAX_MASS) || mass <= 0.0f) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    target = (double)mass / (double)rig->density;
    for (i = 0; i < rig->probe_count; ++i) {
        capacity += (double)rig->probes[i].area * (double)rig->probes[i].max_depth;
    }
    if (!isfinite(target) || !isfinite(capacity) || !(capacity > 0.0)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (target > capacity) {
        /* Even fully submerged, every probe saturated, the rig displaces less
         * than this mass. This is the only genuine "cannot float" case. */
        return PIXELICA_ERR_CAPACITY;
    }

    /* The breakpoints of V: where a probe starts and where it saturates. */
    for (i = 0; i < rig->probe_count; ++i) {
        breakpoints[count++] = (double)rig->probes[i].offset.y;
        breakpoints[count++] =
            (double)rig->probes[i].offset.y + (double)rig->probes[i].max_depth;
    }
    for (i = 1u; i < count; ++i) { /* insertion sort, count <= 64 */
        double key = breakpoints[i];

        j = i;
        while (j > 0u && breakpoints[j - 1u] > key) {
            breakpoints[j] = breakpoints[j - 1u];
            --j;
        }
        breakpoints[j] = key;
    }

    /*
     * V(breakpoints[0]) == 0 < target, and V(breakpoints[count-1]) == capacity
     * >= target, so the first segment whose right end reaches the target
     * brackets the root. Reading it off that segment's line is exact, and
     * taking the FIRST such segment returns the smallest draft when V has a
     * flat stretch at exactly the target.
     */
    for (j = 0u; j + 1u < count; ++j) {
        double lo = breakpoints[j], hi = breakpoints[j + 1u];
        double volume_lo, volume_hi, slope;

        if (!(hi > lo)) {
            continue; /* duplicate breakpoint: zero-width segment */
        }
        volume_hi = water_displaced_volume(rig, hi);
        if (volume_hi < target) {
            continue;
        }
        volume_lo = water_displaced_volume(rig, lo);
        slope = (volume_hi - volume_lo) / (hi - lo);
        draft = (slope > 0.0) ? lo + (target - volume_lo) / slope : lo;
        solved = 1;
        break;
    }
    if (!solved || !isfinite(draft) || !water_finite((float)draft)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *out_draft = (float)draft;
    return PIXELICA_OK;
}

/* ---- integration -------------------------------------------------------- */

PIXELICA_Status pixelica_water_body_step(PIXELICA_WaterBody *body,
                                         const PIXELICA_WaterLoad *load, float dt)
{
    PIXELICA_WaterBody next;
    PIXELICA_Quat delta, spun;
    PIXELICA_Vec3 omega_body, torque_body, axis;
    float rotation[16];
    double step, inertia[3], spin[3], angle;

    if (body == NULL || load == NULL || water_check_body(body) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_finite_vec3(load->force) || !water_finite_vec3(load->torque)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (!water_finite(dt) || !(dt > 0.0f) || dt > PIXELICA_WATER_MAX_STEP) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (water_rotation(body->orientation, rotation) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    step = (double)dt;
    next = *body;

    /* Semi-implicit Euler: the position uses the UPDATED velocity. */
    next.velocity =
        water_vec3((double)body->velocity.x + (double)load->force.x / (double)body->mass * step,
                   (double)body->velocity.y + (double)load->force.y / (double)body->mass * step,
                   (double)body->velocity.z + (double)load->force.z / (double)body->mass * step);
    next.position = water_vec3((double)body->position.x + (double)next.velocity.x * step,
                               (double)body->position.y + (double)next.velocity.y * step,
                               (double)body->position.z + (double)next.velocity.z * step);

    /* Angular part in the body frame, where the inertia is diagonal, including
     * the Euler gyroscopic term omega x (I omega). */
    omega_body = water_rotate_inverse(rotation, body->angular_velocity);
    torque_body = water_rotate_inverse(rotation, load->torque);
    inertia[0] = (double)body->inertia.x;
    inertia[1] = (double)body->inertia.y;
    inertia[2] = (double)body->inertia.z;
    spin[0] = inertia[0] * (double)omega_body.x;
    spin[1] = inertia[1] * (double)omega_body.y;
    spin[2] = inertia[2] * (double)omega_body.z;
    omega_body = water_vec3(
        (double)omega_body.x +
            step * ((double)torque_body.x -
                    ((double)omega_body.y * spin[2] - (double)omega_body.z * spin[1])) /
                inertia[0],
        (double)omega_body.y +
            step * ((double)torque_body.y -
                    ((double)omega_body.z * spin[0] - (double)omega_body.x * spin[2])) /
                inertia[1],
        (double)omega_body.z +
            step * ((double)torque_body.z -
                    ((double)omega_body.x * spin[1] - (double)omega_body.y * spin[0])) /
                inertia[2]);
    if (!water_finite_vec3(omega_body)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    next.angular_velocity = water_rotate(rotation, omega_body);
    if (!water_finite_vec3(next.angular_velocity)) {
        return PIXELICA_ERR_INVALID_ARG;
    }

    /* Rotate by |omega| * dt about omega's own axis; an (almost) still body
     * keeps its orientation bit-exactly instead of normalizing noise. */
    angle = sqrt((double)next.angular_velocity.x * (double)next.angular_velocity.x +
                 (double)next.angular_velocity.y * (double)next.angular_velocity.y +
                 (double)next.angular_velocity.z * (double)next.angular_velocity.z) *
            step;
    if (isfinite(angle) && angle > 0.0) {
        axis = next.angular_velocity;
        if (pixelica_quat_from_axis_angle(&delta, axis, (float)angle) != PIXELICA_OK) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        spun = water_quat_multiply(delta, body->orientation);
        if (pixelica_quat_normalize(&spun, spun) != PIXELICA_OK) {
            return PIXELICA_ERR_INVALID_ARG;
        }
        next.orientation = spun;
    }

    if (water_check_body(&next) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    *body = next;
    return PIXELICA_OK;
}

PIXELICA_Status pixelica_water_body_attitude(const PIXELICA_WaterBody *body, float *out_roll,
                                             float *out_pitch, float *out_yaw)
{
    PIXELICA_Vec3 up, forward;
    float rotation[16];

    if (water_check_body(body) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (water_rotation(body->orientation, rotation) != PIXELICA_OK) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    up = water_rotate(rotation, water_vec3(0.0, 1.0, 0.0));
    forward = water_rotate(rotation, water_vec3(0.0, 0.0, -1.0));
    if (!water_finite_vec3(up) || !water_finite_vec3(forward)) {
        return PIXELICA_ERR_INVALID_ARG;
    }
    if (out_roll != NULL) {
        *out_roll = (float)atan2(-(double)up.x, (double)up.y);
    }
    if (out_pitch != NULL) {
        *out_pitch = (float)atan2((double)up.z, (double)up.y);
    }
    if (out_yaw != NULL) {
        *out_yaw = (float)atan2((double)forward.x, -(double)forward.z);
    }
    return PIXELICA_OK;
}
