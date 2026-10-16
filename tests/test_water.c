/*
 * Optional water module core (pixelica/water.h, modules/water/water.c).
 *
 * Common-library test: it links Pixelica::water, which needs only
 * Pixelica::common, and never includes a renderer header.
 *
 * The sections marked [AUDIT n] are the regression tests for the five findings
 * of the 2026-10-15 independent audit:
 *   1 the travelling-wave sign (theta = kx - omega t, not + omega t)
 *   2 double time end to end (a float clock freezes at t = 1e6)
 *   3 the equilibrium draft with per-probe saturation and body-frame offsets
 *   4 drag is dissipative, the explicit integrator is only conditionally
 *     stable, and an unstable step reports instead of returning NaN
 *   5 the Eulerian dh/dt and the material Dh/Dt are both exposed and the one
 *     drag uses is the coherent kinematic pair
 */
#include "pixelica/water.h"
#include "test_util.h"

/* The raft of examples/hello_water, so the settling and wave-riding checks
 * below are about the parameters the demo actually ships. */
#define DEMO_MASS 800.0f
#define DEMO_AREA 1.0f
#define DEMO_MAX_DEPTH 0.35f
#define DEMO_HALF_Y 0.175f
#define DEMO_DRAG_N 3000.0f
#define DEMO_DRAG_T 500.0f
#define DEMO_SPAN 0.8f
#define DEMO_DT (1.0f / 60.0f)
/* draft = mass/(density*area) + offset.y = 0.20 - 0.175 = +0.025 m, and the
 * submerged depth of each probe at rest is draft - offset.y = 0.20 m. (This
 * hull floats deeper than half its thickness, so its centre of mass is just
 * BELOW the water line; test_equilibrium_draft also covers the commoner case
 * where the draft comes out negative.) */
#define DEMO_PROBE_DEPTH 0.20
#define RELEASE_HEIGHT 0.30f /* the demo's RAFT_DROP */

static PIXELICA_WaterProbe g_probes[PIXELICA_WATER_MAX_PROBES];

static PIXELICA_Vec3 vec3(float x, float y, float z)
{
    PIXELICA_Vec3 v;

    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}

static int finite_f(float v)
{
    return isfinite((double)v) != 0;
}

static double dot3(PIXELICA_Vec3 a, PIXELICA_Vec3 b)
{
    return (double)a.x * (double)b.x + (double)a.y * (double)b.y + (double)a.z * (double)b.z;
}

/* One wave along +X unless `dir_z` says otherwise. */
static PIXELICA_WaterSurface one_wave(float amplitude, float wavelength, float speed,
                                      float dir_x, float dir_z, float phase)
{
    PIXELICA_WaterSurface s;

    memset(&s, 0, sizeof s);
    s.wave_count = 1u;
    s.waves[0].dir_x = dir_x;
    s.waves[0].dir_z = dir_z;
    s.waves[0].amplitude = amplitude;
    s.waves[0].wavelength = wavelength;
    s.waves[0].speed = speed;
    s.waves[0].phase = phase;
    return s;
}

static void demo_rig(PIXELICA_WaterRig *rig)
{
    uint32_t i;

    memset(g_probes, 0, sizeof g_probes);
    for (i = 0; i < 4u; ++i) {
        g_probes[i].offset = vec3((i & 1u) ? DEMO_SPAN : -DEMO_SPAN, -DEMO_HALF_Y,
                                  (i & 2u) ? DEMO_SPAN : -DEMO_SPAN);
        g_probes[i].area = DEMO_AREA;
        g_probes[i].max_depth = DEMO_MAX_DEPTH;
        g_probes[i].drag_normal = DEMO_DRAG_N;
        g_probes[i].drag_tangential = DEMO_DRAG_T;
    }
    rig->probes = g_probes;
    rig->probe_count = 4u;
    rig->density = 1000.0f;
    rig->gravity = 9.81f;
}

static void demo_body(PIXELICA_WaterBody *body, float y)
{
    memset(body, 0, sizeof *body);
    pixelica_quat_identity(&body->orientation);
    body->position = vec3(0.0f, y, 0.0f);
    body->mass = DEMO_MASS;
    /* A 2 x 0.35 x 2 m board of uniform density: m/12 * (sum of the squares of
     * the two perpendicular edge lengths). */
    body->inertia = vec3(274.8f, 533.3f, 274.8f);
}

/* The world Y the centre of mass rests at on flat water at Y = 0. */
static float demo_rest_y(void)
{
    return (float)(DEMO_HALF_Y - DEMO_PROBE_DEPTH);
}

/* ---- surface basics ------------------------------------------------------ */

static void test_defaults_and_validation(void)
{
    PIXELICA_WaterSurface surface, broken;
    PIXELICA_WaterSample sample, keep;
    float period = -1.0f;

    CHECK(pixelica_water_surface_default(NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    CHECK(pixelica_water_surface_validate(&surface) == PIXELICA_OK);
    CHECK(pixelica_water_surface_validate(NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(surface.wave_count == 2u);

    /* A flat surface is a legal degenerate case, not an error. */
    memset(&broken, 0, sizeof broken);
    broken.base_height = 3.5f;
    CHECK(pixelica_water_surface_validate(&broken) == PIXELICA_OK);
    CHECK(pixelica_water_sample(&broken, 12.0f, -4.0f, 9.0, &sample) == PIXELICA_OK);
    CHECK_NEAR(sample.height, 3.5, 1.0e-6);
    CHECK_NEAR(sample.normal.x, 0.0, 1.0e-6);
    CHECK_NEAR(sample.normal.y, 1.0, 1.0e-6);
    CHECK_NEAR(sample.normal.z, 0.0, 1.0e-6);
    CHECK_NEAR(sample.slope_x, 0.0, 1.0e-9);
    CHECK_NEAR(sample.eulerian_dhdt, 0.0, 1.0e-9);
    CHECK_NEAR(sample.surface_velocity_y, 0.0, 1.0e-9);

    /* Every out-of-range field is refused, and `out` is left alone. */
    keep = sample;
    broken = one_wave(0.2f, 5.0f, 1.0f, 0.0f, 0.0f, 0.0f); /* zero direction */
    CHECK(pixelica_water_surface_validate(&broken) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_sample(&broken, 0.0f, 0.0f, 0.0, &sample) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&sample, &keep, sizeof sample) == 0);

    broken = one_wave(0.2f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f); /* wavelength 0 */
    CHECK(pixelica_water_surface_validate(&broken) == PIXELICA_ERR_INVALID_ARG);
    broken = one_wave(-0.2f, 5.0f, 1.0f, 1.0f, 0.0f, 0.0f); /* negative amplitude */
    CHECK(pixelica_water_surface_validate(&broken) == PIXELICA_ERR_INVALID_ARG);
    broken = one_wave(0.2f, 5.0f, pixelica_t_nan(), 1.0f, 0.0f, 0.0f);
    CHECK(pixelica_water_surface_validate(&broken) == PIXELICA_ERR_INVALID_ARG);
    broken = one_wave(0.2f, 5.0f, 1.0f, 1.0f, 0.0f, pixelica_t_inf());
    CHECK(pixelica_water_surface_validate(&broken) == PIXELICA_ERR_INVALID_ARG);
    broken = one_wave(0.2f, 5.0f, 1.0f, 1.0f, 0.0f, 0.0f);
    broken.wave_count = PIXELICA_WATER_MAX_WAVES + 1u;
    CHECK(pixelica_water_surface_validate(&broken) == PIXELICA_ERR_INVALID_ARG);

    /* A non-finite or out-of-range query is refused; time only has to be finite. */
    broken = one_wave(0.2f, 5.0f, 1.0f, 1.0f, 0.0f, 0.0f);
    CHECK(pixelica_water_sample(&broken, pixelica_t_nan(), 0.0f, 0.0, &sample) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_sample(&broken, 0.0f, pixelica_t_inf(), 0.0, &sample) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_sample(&broken, 2.0f * PIXELICA_WATER_MAX_COORD, 0.0f, 0.0, &sample) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_sample(&broken, 0.0f, 0.0f, (double)pixelica_t_inf(), &sample) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_sample(&broken, 0.0f, 0.0f, 1.0e30, &sample) == PIXELICA_OK);
    CHECK(pixelica_water_sample(NULL, 0.0f, 0.0f, 0.0, &sample) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_sample(&broken, 0.0f, 0.0f, 0.0, NULL) == PIXELICA_ERR_INVALID_ARG);

    /* wavelength / |speed|, and 0 for a standing wave. */
    CHECK(pixelica_water_wave_period(&broken, 0u, &period) == PIXELICA_OK);
    CHECK_NEAR(period, 5.0, 1.0e-6);
    CHECK(pixelica_water_wave_period(&broken, 1u, &period) == PIXELICA_ERR_INVALID_ARG);
    CHECK_NEAR(period, 5.0, 1.0e-6); /* untouched */
    broken = one_wave(0.2f, 5.0f, 0.0f, 1.0f, 0.0f, 0.0f);
    CHECK(pixelica_water_wave_period(&broken, 0u, &period) == PIXELICA_OK);
    CHECK_NEAR(period, 0.0, 1.0e-9);
}

/* [AUDIT 1] theta = k*x - omega*t, so a crest travels ALONG (dir_x, dir_z). */
static void test_travelling_wave_invariant(void)
{
    PIXELICA_WaterSurface surface;
    float at_origin = 0.0f, downstream = 0.0f, upstream = 0.0f;
    double dt;
    unsigned i;

    /* The audit's own repro: wavelength 10, speed 2, +X, phase 0.
     * h(0, 0) == 0 and a crest moves 2 m per second, so h(2, 1) == h(0, 0).
     * The pre-fix code returned sin(0.8 pi) = 0.587785 here. */
    surface = one_wave(1.0f, 10.0f, 2.0f, 1.0f, 0.0f, 0.0f);
    CHECK(pixelica_water_height(&surface, 0.0f, 0.0f, 0.0, &at_origin) == PIXELICA_OK);
    CHECK(pixelica_water_height(&surface, 2.0f, 0.0f, 1.0, &downstream) == PIXELICA_OK);
    CHECK_NEAR(at_origin, 0.0, 1.0e-6);
    CHECK_NEAR(downstream, at_origin, 1.0e-6);
    /* And the profile is NOT the same 2 m the other way, which is what the
     * sign error produced. */
    CHECK(pixelica_water_height(&surface, -2.0f, 0.0f, 1.0, &upstream) == PIXELICA_OK);
    CHECK(fabs((double)upstream - (double)at_origin) > 0.5);

    /* The invariant at a general phase, position and time, for several
     * directions: h(p + dhat * speed * dt, t + dt) == h(p, t). */
    for (i = 0; i < 4u; ++i) {
        static const float dir_x[4] = {1.0f, -1.0f, 0.0f, 0.6f};
        static const float dir_z[4] = {0.0f, 0.0f, 1.0f, -0.8f};
        const float speed = 2.6f;
        float base = 0.0f, moved = 0.0f;
        double length = sqrt((double)dir_x[i] * (double)dir_x[i] +
                             (double)dir_z[i] * (double)dir_z[i]);
        float ux = (float)((double)dir_x[i] / length);
        float uz = (float)((double)dir_z[i] / length);

        surface = one_wave(0.37f, 7.0f, speed, dir_x[i], dir_z[i], 1.234f);
        for (dt = 0.25; dt <= 2.0; dt += 0.75) {
            const float x = 3.5f, z = -2.25f;
            const double t = 11.5;

            CHECK(pixelica_water_height(&surface, x, z, t, &base) == PIXELICA_OK);
            CHECK(pixelica_water_height(&surface,
                                        (float)((double)x + (double)ux * speed * dt),
                                        (float)((double)z + (double)uz * speed * dt), t + dt,
                                        &moved) == PIXELICA_OK);
            CHECK_NEAR(moved, base, 2.0e-5);
        }
    }

    /* A negative speed travels the other way: the invariant then holds for
     * -dhat, which is the documented behaviour. */
    surface = one_wave(0.5f, 6.0f, -3.0f, 1.0f, 0.0f, 0.0f);
    CHECK(pixelica_water_height(&surface, 0.0f, 0.0f, 0.0, &at_origin) == PIXELICA_OK);
    CHECK(pixelica_water_height(&surface, -3.0f, 0.0f, 1.0, &downstream) == PIXELICA_OK);
    CHECK_NEAR(downstream, at_origin, 1.0e-5);
}

/* The height is the plain sum of waves, with the documented phase. */
static void test_height_matches_closed_form(void)
{
    PIXELICA_WaterSurface surface;
    const double two_pi = 6.283185307179586;
    float height = 0.0f;
    double expected;

    memset(&surface, 0, sizeof surface);
    surface.wave_count = 2u;
    surface.base_height = 1.25f;
    surface.waves[0].dir_x = 1.0f;
    surface.waves[0].amplitude = 0.4f;
    surface.waves[0].wavelength = 8.0f;
    surface.waves[0].speed = 2.0f;
    surface.waves[0].phase = 0.3f;
    surface.waves[1].dir_z = 1.0f;
    surface.waves[1].amplitude = 0.25f;
    surface.waves[1].wavelength = 5.0f;
    surface.waves[1].speed = -1.5f;
    surface.waves[1].phase = -0.7f;

    expected = 1.25 + 0.4 * sin(two_pi / 8.0 * 3.0 - two_pi / 8.0 * 2.0 * 1.75 + 0.3) +
               0.25 * sin(two_pi / 5.0 * (-2.0) - two_pi / 5.0 * (-1.5) * 1.75 - 0.7);
    CHECK(pixelica_water_height(&surface, 3.0f, -2.0f, 1.75, &height) == PIXELICA_OK);
    CHECK_NEAR(height, expected, 2.0e-6);
}

/* Analytic slopes, normal and dh/dt against central differences. */
static void test_derivatives_match_finite_differences(void)
{
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterSample sample, plus, minus;
    const float x = 2.3f, z = -1.4f;
    const double t = 3.75, h = 1.0e-3;
    double fd_x, fd_z, fd_t, length;

    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    CHECK(pixelica_water_sample(&surface, x, z, t, &sample) == PIXELICA_OK);

    CHECK(pixelica_water_sample(&surface, (float)((double)x + h), z, t, &plus) == PIXELICA_OK);
    CHECK(pixelica_water_sample(&surface, (float)((double)x - h), z, t, &minus) == PIXELICA_OK);
    fd_x = ((double)plus.height - (double)minus.height) / (2.0 * h);
    CHECK_NEAR(sample.slope_x, fd_x, 1.0e-4);

    CHECK(pixelica_water_sample(&surface, x, (float)((double)z + h), t, &plus) == PIXELICA_OK);
    CHECK(pixelica_water_sample(&surface, x, (float)((double)z - h), t, &minus) == PIXELICA_OK);
    fd_z = ((double)plus.height - (double)minus.height) / (2.0 * h);
    CHECK_NEAR(sample.slope_z, fd_z, 1.0e-4);

    CHECK(pixelica_water_sample(&surface, x, z, t + h, &plus) == PIXELICA_OK);
    CHECK(pixelica_water_sample(&surface, x, z, t - h, &minus) == PIXELICA_OK);
    fd_t = ((double)plus.height - (double)minus.height) / (2.0 * h);
    CHECK_NEAR(sample.eulerian_dhdt, fd_t, 1.0e-4);

    /* normal = normalize(-slope_x, 1, -slope_z), unit length, never overhanging. */
    length = sqrt(dot3(sample.normal, sample.normal));
    CHECK_NEAR(length, 1.0, 1.0e-6);
    CHECK(sample.normal.y > 0.0f);
    CHECK_NEAR(sample.normal.x / sample.normal.y, -(double)sample.slope_x, 1.0e-5);
    CHECK_NEAR(sample.normal.z / sample.normal.y, -(double)sample.slope_z, 1.0e-5);
}

/*
 * [AUDIT 5] The two vertical velocities are different quantities and both are
 * reported: the Eulerian partial derivative, and the material derivative that
 * the drag velocity is built from.
 */
static void test_velocity_conventions(void)
{
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterSample sample;
    double expected;

    /* With no current they coincide exactly. */
    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    CHECK(surface.current_x == 0.0f && surface.current_z == 0.0f);
    CHECK(pixelica_water_sample(&surface, 1.5f, 2.5f, 4.25, &sample) == PIXELICA_OK);
    CHECK_NEAR(sample.surface_velocity_y, sample.eulerian_dhdt, 1.0e-6);
    CHECK_NEAR(sample.velocity.y, sample.eulerian_dhdt, 1.0e-6);
    CHECK_NEAR(sample.velocity.x, 0.0, 1.0e-9);
    CHECK_NEAR(sample.velocity.z, 0.0, 1.0e-9);

    /* With a current, Dh/Dt = dh/dt + U . grad h, and the horizontal part of
     * `velocity` is exactly the current. */
    surface.current_x = 1.7f;
    surface.current_z = -0.9f;
    CHECK(pixelica_water_sample(&surface, 1.5f, 2.5f, 4.25, &sample) == PIXELICA_OK);
    expected = (double)sample.eulerian_dhdt + 1.7 * (double)sample.slope_x +
               (-0.9) * (double)sample.slope_z;
    CHECK_NEAR(sample.surface_velocity_y, expected, 1.0e-5);
    CHECK_NEAR(sample.velocity.y, expected, 1.0e-5);
    CHECK_NEAR(sample.velocity.x, 1.7, 1.0e-6);
    CHECK_NEAR(sample.velocity.z, -0.9, 1.0e-6);
    /* The Eulerian field quantity itself must NOT have moved. */
    {
        PIXELICA_WaterSample still;
        PIXELICA_WaterSurface no_current = surface;

        no_current.current_x = 0.0f;
        no_current.current_z = 0.0f;
        CHECK(pixelica_water_sample(&no_current, 1.5f, 2.5f, 4.25, &still) == PIXELICA_OK);
        CHECK_NEAR(sample.eulerian_dhdt, still.eulerian_dhdt, 1.0e-9);
    }

    /* A standing wave has no time dependence at all. */
    surface = one_wave(0.3f, 4.0f, 0.0f, 1.0f, 0.0f, 0.2f);
    CHECK(pixelica_water_sample(&surface, 0.6f, 0.0f, 0.0, &sample) == PIXELICA_OK);
    CHECK_NEAR(sample.eulerian_dhdt, 0.0, 1.0e-9);
    CHECK_NEAR(sample.surface_velocity_y, 0.0, 1.0e-9);
}

/* [AUDIT 2] double time: periodic equivalence, and real evolution after a
 * million seconds, where a float clock cannot even represent t + 1/60. */
static void test_time_handling(void)
{
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterSample late, later;
    float a = 0.0f, b = 0.0f;
    double fd, h = 1.0e-3;
    unsigned i;

    /* Two waves that share a 1 s period, so the whole surface repeats. */
    memset(&surface, 0, sizeof surface);
    surface.wave_count = 2u;
    surface.waves[0].dir_x = 1.0f;
    surface.waves[0].amplitude = 0.3f;
    surface.waves[0].wavelength = 7.0f;
    surface.waves[0].speed = 7.0f;
    surface.waves[1].dir_z = 1.0f;
    surface.waves[1].amplitude = 0.2f;
    surface.waves[1].wavelength = 3.0f;
    surface.waves[1].speed = 3.0f;
    for (i = 0; i < 4u; ++i) {
        const double t = 0.37 + (double)i * 0.21;

        CHECK(pixelica_water_height(&surface, 1.5f, -2.5f, t, &a) == PIXELICA_OK);
        CHECK(pixelica_water_height(&surface, 1.5f, -2.5f, t + 1.0, &b) == PIXELICA_OK);
        CHECK_NEAR(a, b, 2.0e-5);
        /* ... and 1000 periods later it is still the same surface. */
        CHECK(pixelica_water_height(&surface, 1.5f, -2.5f, t + 1000.0, &b) == PIXELICA_OK);
        CHECK_NEAR(a, b, 2.0e-4);
    }

    /* A long session still advances frame by frame. */
    CHECK(pixelica_water_surface_default(&surface) == PIXELICA_OK);
    CHECK((float)(1.0e6f + 1.0f / 60.0f) == 1.0e6f); /* why float time cannot work */
    CHECK(pixelica_water_sample(&surface, 0.5f, 0.5f, 1.0e6, &late) == PIXELICA_OK);
    CHECK(pixelica_water_sample(&surface, 0.5f, 0.5f, 1.0e6 + 1.0 / 60.0, &later) ==
          PIXELICA_OK);
    CHECK(fabs((double)late.height - (double)later.height) > 1.0e-4);

    /* And the derivative is still the derivative out there. */
    {
        PIXELICA_WaterSample plus, minus;

        CHECK(pixelica_water_sample(&surface, 0.5f, 0.5f, 1.0e6 + h, &plus) == PIXELICA_OK);
        CHECK(pixelica_water_sample(&surface, 0.5f, 0.5f, 1.0e6 - h, &minus) == PIXELICA_OK);
        fd = ((double)plus.height - (double)minus.height) / (2.0 * h);
        CHECK_NEAR(late.eulerian_dhdt, fd, 1.0e-3);
    }

    /* A far-away position keeps working too. */
    CHECK(pixelica_water_sample(&surface, 1.0e5f, -1.0e5f, 1.0e6, &late) == PIXELICA_OK);
    CHECK(finite_f(late.height));
}

/* ---- buoyancy ----------------------------------------------------------- */

/*
 * [AUDIT 3] The equilibrium draft is the exact root of the piecewise-linear
 * displaced-volume curve, with per-probe saturation and body-frame offsets,
 * and PIXELICA_ERR_CAPACITY only when the rig truly cannot float the mass.
 */
static void test_equilibrium_draft(void)
{
    PIXELICA_WaterRig rig;
    float draft = -1.0f, keep;

    /*
     * The uniform case has the simple closed form, shifted by the probes'
     * body-frame height: draft = mass/(density*area) + offset.y.
     */
    demo_rig(&rig);
    CHECK(pixelica_water_equilibrium_draft(&rig, DEMO_MASS, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, DEMO_PROBE_DEPTH - (double)DEMO_HALF_Y, 1.0e-6);
    CHECK_NEAR((double)draft + (double)DEMO_HALF_Y, DEMO_PROBE_DEPTH, 1.0e-6);
    /* A NEGATIVE draft is normal and documented: lighten the same hull until
     * it floats shallower than half its thickness and its centre of mass sits
     * above the water line. 560 kg -> 0.14 m deep -> draft -0.035 m. */
    CHECK(pixelica_water_equilibrium_draft(&rig, 560.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 0.14 - (double)DEMO_HALF_Y, 1.0e-6);
    CHECK(draft < 0.0f);
    CHECK(pixelica_water_equilibrium_draft(&rig, DEMO_MASS, &draft) == PIXELICA_OK);

    /* The audit's case: probes with DIFFERENT saturation depths. Total
     * capacity 1*1 + 1*3 = 4 m^3 = 4000 kg, so 2500 kg floats, and the draft
     * solves 1*min(d,1) + 1*min(d,3) = 2.5, i.e. 1 + d = 2.5 -> d = 1.5.
     * The pre-fix code answered PIXELICA_ERR_CAPACITY here. */
    memset(g_probes, 0, sizeof g_probes);
    g_probes[0].area = 1.0f;
    g_probes[0].max_depth = 1.0f;
    g_probes[1].area = 1.0f;
    g_probes[1].max_depth = 3.0f;
    rig.probes = g_probes;
    rig.probe_count = 2u;
    rig.density = 1000.0f;
    rig.gravity = 9.81f;
    CHECK(pixelica_water_equilibrium_draft(&rig, 2500.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 1.5, 1.0e-5);

    /* Below the first saturation the slope is the full area. */
    CHECK(pixelica_water_equilibrium_draft(&rig, 1000.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 0.5, 1.0e-5);
    /* Exactly at the first breakpoint. */
    CHECK(pixelica_water_equilibrium_draft(&rig, 2000.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 1.0, 1.0e-5);
    /* Exactly at capacity: the deepest probe's saturation depth. */
    CHECK(pixelica_water_equilibrium_draft(&rig, 4000.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 3.0, 1.0e-5);
    /* Only beyond capacity is it really unable to float, and `out` is kept. */
    keep = draft;
    CHECK(pixelica_water_equilibrium_draft(&rig, 4000.1f, &draft) == PIXELICA_ERR_CAPACITY);
    CHECK(draft == keep);
    CHECK(pixelica_water_equilibrium_draft(&rig, 1.0e6f, &draft) == PIXELICA_ERR_CAPACITY);
    CHECK(draft == keep);

    /* Body-frame Y offsets shift the curve: a probe 0.5 m above the other
     * only starts displacing once the draft passes 0.5 m. Capacity is
     * 1*1 + 1*1 = 2 m^3; for 1500 kg the root solves
     * min(d, 1) + clamp(d - 0.5, 0, 1) = 1.5 -> 1 + (d - 0.5) = 1.5 -> d = 1. */
    memset(g_probes, 0, sizeof g_probes);
    g_probes[0].area = 1.0f;
    g_probes[0].max_depth = 1.0f;
    g_probes[1].area = 1.0f;
    g_probes[1].max_depth = 1.0f;
    g_probes[1].offset.y = 0.5f;
    CHECK(pixelica_water_equilibrium_draft(&rig, 1500.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 1.0, 1.0e-5);
    /* 500 kg: only the lower probe is wet, d = 0.5. */
    CHECK(pixelica_water_equilibrium_draft(&rig, 500.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 0.5, 1.0e-5);

    /* A vertical GAP between probes gives V a flat stretch; the smallest
     * draft that reaches the target is returned. Lower probe: area 1,
     * max_depth 1, offset 0 -> saturates at d = 1. Upper probe starts at
     * d = 2. For 1000 kg (1 m^3) the answer is the start of the plateau. */
    g_probes[1].offset.y = 2.0f;
    CHECK(pixelica_water_equilibrium_draft(&rig, 1000.0f, &draft) == PIXELICA_OK);
    CHECK_NEAR(draft, 1.0, 1.0e-5);

    /* Invalid input keeps `out`. */
    keep = draft;
    CHECK(pixelica_water_equilibrium_draft(NULL, 100.0f, &draft) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_equilibrium_draft(&rig, 0.0f, &draft) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_equilibrium_draft(&rig, pixelica_t_nan(), &draft) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_equilibrium_draft(&rig, 100.0f, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(draft == keep);
}

/* At the equilibrium draft on flat water the body is in balance. */
static void test_load_equilibrium_and_saturation(void)
{
    PIXELICA_WaterSurface flat;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body;
    PIXELICA_WaterLoad load, deep, keep;
    float draft = 0.0f;

    memset(&flat, 0, sizeof flat);
    demo_rig(&rig);
    CHECK(pixelica_water_equilibrium_draft(&rig, DEMO_MASS, &draft) == PIXELICA_OK);
    demo_body(&body, -draft);

    CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &load) == PIXELICA_OK);
    CHECK(load.submerged_probes == 4u);
    CHECK_NEAR(load.submerged_volume, (double)DEMO_MASS / 1000.0, 1.0e-5);
    CHECK_NEAR(load.buoyancy.y, (double)DEMO_MASS * 9.81, 1.0e-2);
    CHECK_NEAR(load.force.y, 0.0, 1.0e-2); /* buoyancy cancels gravity */
    CHECK_NEAR(load.force.x, 0.0, 1.0e-9);
    CHECK_NEAR(load.drag.y, 0.0, 1.0e-9); /* at rest in still water */
    CHECK_NEAR(load.torque.x, 0.0, 1.0e-4);
    CHECK_NEAR(load.torque.z, 0.0, 1.0e-4);
    /* Gravity is recoverable, and contributes no torque. */
    CHECK_NEAR(load.force.y - load.buoyancy.y - load.drag.y, -(double)DEMO_MASS * 9.81, 1.0e-2);

    /* Pushed far under, the displaced volume SATURATES at area * max_depth. */
    demo_body(&body, -50.0f);
    CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &deep) == PIXELICA_OK);
    CHECK_NEAR(deep.submerged_volume, 4.0 * (double)DEMO_AREA * (double)DEMO_MAX_DEPTH, 1.0e-5);
    CHECK_NEAR(deep.submerged_area, 4.0 * (double)DEMO_AREA, 1.0e-5);
    /* Twice as deep is not twice the lift: that is the point of the bound. */
    {
        PIXELICA_WaterLoad deeper;

        demo_body(&body, -100.0f);
        CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &deeper) == PIXELICA_OK);
        CHECK_NEAR(deeper.buoyancy.y, deep.buoyancy.y, 1.0e-3);
    }

    /* Entirely out of the water: pure gravity, no torque, nothing submerged. */
    demo_body(&body, 10.0f);
    CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &load) == PIXELICA_OK);
    CHECK(load.submerged_probes == 0u);
    CHECK_NEAR(load.submerged_volume, 0.0, 1.0e-9);
    CHECK_NEAR(load.buoyancy.y, 0.0, 1.0e-9);
    CHECK_NEAR(load.force.y, -(double)DEMO_MASS * 9.81, 1.0e-2);
    CHECK_NEAR(load.torque.x, 0.0, 1.0e-9);
    CHECK_NEAR(load.torque.y, 0.0, 1.0e-9);
    CHECK_NEAR(load.torque.z, 0.0, 1.0e-9);

    /* Invalid input leaves the load untouched. */
    keep = load;
    demo_body(&body, -draft);
    CHECK(pixelica_water_load(NULL, &rig, &body, 0.0, &load) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_load(&flat, NULL, &body, 0.0, &load) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_load(&flat, &rig, NULL, 0.0, &load) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_load(&flat, &rig, &body, (double)pixelica_t_nan(), &load) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, NULL) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&load, &keep, sizeof load) == 0);
    {
        PIXELICA_WaterBody bad = body;
        PIXELICA_WaterRig bad_rig = rig;

        bad.mass = 0.0f;
        CHECK(pixelica_water_load(&flat, &rig, &bad, 0.0, &load) == PIXELICA_ERR_INVALID_ARG);
        bad = body;
        bad.inertia.y = 0.0f;
        CHECK(pixelica_water_load(&flat, &rig, &bad, 0.0, &load) == PIXELICA_ERR_INVALID_ARG);
        bad = body;
        memset(&bad.orientation, 0, sizeof bad.orientation); /* zero quaternion */
        CHECK(pixelica_water_load(&flat, &rig, &bad, 0.0, &load) == PIXELICA_ERR_INVALID_ARG);
        bad_rig.probe_count = 0u;
        CHECK(pixelica_water_load(&flat, &bad_rig, &body, 0.0, &load) ==
              PIXELICA_ERR_INVALID_ARG);
        bad_rig = rig;
        bad_rig.density = 0.0f;
        CHECK(pixelica_water_load(&flat, &bad_rig, &body, 0.0, &load) ==
              PIXELICA_ERR_INVALID_ARG);
        bad_rig = rig;
        bad_rig.probe_count = PIXELICA_WATER_MAX_PROBES + 1u;
        CHECK(pixelica_water_load(&flat, &bad_rig, &body, 0.0, &load) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(&load, &keep, sizeof load) == 0);
    }
}

/* A tilted surface produces a righting torque through r x F, not a heuristic. */
static void test_wave_torque_and_rotation_damping(void)
{
    PIXELICA_WaterSurface slope;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body;
    PIXELICA_WaterLoad load, no_drag;
    PIXELICA_WaterRig dry_rig;

    /*
     * A standing wave whose crest is toward +X and trough toward -X across the
     * raft: a quarter wavelength of 6.4 m over the 1.6 m probe span. The
     * amplitude is chosen so that BOTH sides stay submerged at the raft's
     * 0.20 m probe depth (0.05 * sin(2pi*0.8/6.4) = 0.035 m of surface
     * difference, so the two sides sit at 0.165 m and 0.235 m), which makes
     * the torque come from a depth DIFFERENCE and not from a probe leaving
     * the water.
     */
    slope = one_wave(0.05f, 6.4f, 0.0f, 1.0f, 0.0f, 0.0f);
    demo_rig(&rig);
    demo_body(&body, demo_rest_y());
    CHECK(pixelica_water_load(&slope, &rig, &body, 0.0, &load) == PIXELICA_OK);
    CHECK(load.submerged_probes == 4u);
    /* Deeper water on the +X side lifts that side, which rotates the body
     * about -Z (the right-hand rule on r x F with r along +X and F along +Y
     * gives +Z... with r = +X and F = +Y, r x F = +Z). */
    CHECK(load.torque.z > 1.0);
    CHECK_NEAR(load.torque.x, 0.0, 1.0e-3); /* nothing varies along Z */

    /* omega x r really is in the probe velocity: a body rolling about +X must
     * feel a drag torque about -X that opposes it. */
    demo_body(&body, demo_rest_y());
    body.angular_velocity = vec3(0.8f, 0.0f, 0.0f);
    {
        PIXELICA_WaterSurface flat;

        memset(&flat, 0, sizeof flat);
        CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &load) == PIXELICA_OK);
        dry_rig = rig;
        {
            PIXELICA_WaterProbe undamped[4];
            uint32_t i;

            for (i = 0; i < 4u; ++i) {
                undamped[i] = g_probes[i];
                undamped[i].drag_normal = 0.0f;
                undamped[i].drag_tangential = 0.0f;
            }
            dry_rig.probes = undamped;
            CHECK(pixelica_water_load(&flat, &dry_rig, &body, 0.0, &no_drag) == PIXELICA_OK);
            /* The drag part of the torque opposes the rotation. */
            CHECK(load.torque.x - no_drag.torque.x < -1.0);
        }
    }
}

/*
 * [AUDIT 4] The drag FORCE is dissipative: in the water's own frame its power
 * is never positive. That is a property of the force and is all this module
 * claims; stability of any integrator is a separate, conditional matter (see
 * test_integrator_stability_is_conditional).
 */
static void test_drag_is_dissipative(void)
{
    PIXELICA_WaterSurface flat, sea;
    PIXELICA_WaterRig rig, undamped_rig;
    PIXELICA_WaterProbe undamped[4];
    PIXELICA_WaterBody body;
    PIXELICA_WaterLoad load, dry;
    uint32_t i;

    memset(&flat, 0, sizeof flat);
    demo_rig(&rig);
    undamped_rig = rig;
    for (i = 0; i < 4u; ++i) {
        undamped[i] = g_probes[i];
        undamped[i].drag_normal = 0.0f;
        undamped[i].drag_tangential = 0.0f;
    }
    undamped_rig.probes = undamped;

    /*
     * Still water, so the water velocity is zero and the power the drag puts
     * into the body IS its power in the water frame:
     *   P = F_drag . v  +  tau_drag . omega   must be <= 0.
     */
    for (i = 0; i < 8u; ++i) {
        static const float vx[8] = {0.0f, 1.3f, -2.0f, 0.4f, 0.0f, -1.1f, 3.0f, -0.6f};
        static const float vy[8] = {1.0f, -0.7f, 0.5f, 0.0f, -2.2f, 1.4f, -0.3f, 0.9f};
        static const float vz[8] = {-0.5f, 0.2f, 1.1f, -1.7f, 0.6f, 0.0f, 0.8f, -2.4f};
        static const float wx[8] = {0.5f, -1.0f, 0.2f, 0.0f, 0.9f, -0.4f, 1.2f, -0.7f};
        static const float wy[8] = {0.0f, 0.3f, -0.6f, 1.1f, -0.2f, 0.8f, -1.0f, 0.5f};
        static const float wz[8] = {-0.8f, 0.6f, 0.0f, 0.4f, 1.3f, -1.2f, 0.3f, 0.1f};
        PIXELICA_Vec3 drag_torque;
        double power;

        demo_body(&body, demo_rest_y());
        body.velocity = vec3(vx[i], vy[i], vz[i]);
        body.angular_velocity = vec3(wx[i], wy[i], wz[i]);
        CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &load) == PIXELICA_OK);
        CHECK(pixelica_water_load(&flat, &undamped_rig, &body, 0.0, &dry) == PIXELICA_OK);
        drag_torque = vec3(load.torque.x - dry.torque.x, load.torque.y - dry.torque.y,
                           load.torque.z - dry.torque.z);
        power = dot3(load.drag, body.velocity) + dot3(drag_torque, body.angular_velocity);
        CHECK(power <= 1.0e-6);
    }

    /*
     * With waves AND a current the relative velocity is what matters. One
     * probe at the centre of mass makes the relative velocity observable, so
     * the dissipativity can be checked directly against it.
     */
    CHECK(pixelica_water_surface_default(&sea) == PIXELICA_OK);
    sea.current_x = 1.5f;
    sea.current_z = -0.8f;
    memset(g_probes, 0, sizeof g_probes);
    g_probes[0].area = 2.0f;
    g_probes[0].max_depth = 0.5f;
    g_probes[0].drag_normal = 500.0f;
    g_probes[0].drag_tangential = 300.0f;
    rig.probes = g_probes;
    rig.probe_count = 1u;
    for (i = 0; i < 6u; ++i) {
        static const float vx[6] = {0.0f, 2.5f, -1.0f, 1.5f, -3.0f, 0.7f};
        static const float vy[6] = {1.0f, -0.5f, 0.0f, 2.0f, 0.4f, -1.8f};
        static const float vz[6] = {-1.0f, 0.0f, 1.9f, -0.8f, 2.2f, 0.3f};
        const double t = 2.5 + (double)i * 0.37;
        PIXELICA_WaterSample sample;
        PIXELICA_Vec3 relative;
        double power;

        demo_body(&body, -0.2f);
        body.velocity = vec3(vx[i], vy[i], vz[i]);
        CHECK(pixelica_water_load(&sea, &rig, &body, t, &load) == PIXELICA_OK);
        if (load.submerged_probes == 0u) {
            continue; /* out of the water: nothing to dissipate */
        }
        CHECK(pixelica_water_sample(&sea, body.position.x, body.position.z, t, &sample) ==
              PIXELICA_OK);
        relative = vec3(body.velocity.x - sample.velocity.x, body.velocity.y - sample.velocity.y,
                        body.velocity.z - sample.velocity.z);
        power = dot3(load.drag, relative);
        CHECK(power <= 1.0e-6);
        /* And it is genuinely non-zero when the body moves through the water. */
        if (sqrt(dot3(relative, relative)) > 0.1) {
            CHECK(power < -1.0e-3);
        }
    }
}

/* The current changes the drag, because the drag is water-RELATIVE. */
static void test_current_changes_drag(void)
{
    PIXELICA_WaterSurface still, drifting;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body;
    PIXELICA_WaterLoad a, b;

    memset(&still, 0, sizeof still);
    drifting = still;
    drifting.current_x = 2.0f;
    demo_rig(&rig);
    demo_body(&body, demo_rest_y()); /* at rest in the world */
    CHECK(pixelica_water_load(&still, &rig, &body, 0.0, &a) == PIXELICA_OK);
    CHECK(pixelica_water_load(&drifting, &rig, &body, 0.0, &b) == PIXELICA_OK);
    CHECK_NEAR(a.drag.x, 0.0, 1.0e-9);
    /* The water now flows past the body in +X, so the drag pushes it along. */
    CHECK(b.drag.x > 1.0);
    CHECK_NEAR(a.buoyancy.y, b.buoyancy.y, 1.0e-6); /* buoyancy is unaffected */
}

/* ---- integration -------------------------------------------------------- */

static void test_step_basics(void)
{
    PIXELICA_WaterSurface flat;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body, keep;
    PIXELICA_WaterLoad load, bad_load;
    float draft = 0.0f;

    memset(&flat, 0, sizeof flat);
    demo_rig(&rig);
    CHECK(pixelica_water_equilibrium_draft(&rig, DEMO_MASS, &draft) == PIXELICA_OK);
    demo_body(&body, -draft);
    CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &load) == PIXELICA_OK);

    /* A step at equilibrium barely moves it. */
    keep = body;
    CHECK(pixelica_water_body_step(&body, &load, DEMO_DT) == PIXELICA_OK);
    CHECK_NEAR(body.position.y, keep.position.y, 1.0e-4);
    CHECK_NEAR(body.velocity.y, 0.0, 1.0e-3);

    /* Every rejected call leaves the body bit-identical. */
    body = keep;
    CHECK(pixelica_water_body_step(NULL, &load, DEMO_DT) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_body_step(&body, NULL, DEMO_DT) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_body_step(&body, &load, 0.0f) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_body_step(&body, &load, -DEMO_DT) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_body_step(&body, &load, pixelica_t_nan()) == PIXELICA_ERR_INVALID_ARG);
    CHECK(pixelica_water_body_step(&body, &load, PIXELICA_WATER_MAX_STEP + 1.0f) ==
          PIXELICA_ERR_INVALID_ARG);
    bad_load = load;
    bad_load.force.y = pixelica_t_nan();
    CHECK(pixelica_water_body_step(&body, &bad_load, DEMO_DT) == PIXELICA_ERR_INVALID_ARG);
    bad_load = load;
    bad_load.torque.x = pixelica_t_inf();
    CHECK(pixelica_water_body_step(&body, &bad_load, DEMO_DT) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(&body, &keep, sizeof body) == 0);

    /* Free rotation of a symmetric body keeps |omega|. */
    demo_body(&body, 100.0f); /* out of the water, so no load at all */
    body.inertia = vec3(50.0f, 50.0f, 50.0f);
    body.angular_velocity = vec3(0.3f, -0.5f, 0.2f);
    {
        PIXELICA_WaterLoad zero;
        double before = sqrt(dot3(body.angular_velocity, body.angular_velocity));
        uint32_t i;

        memset(&zero, 0, sizeof zero);
        for (i = 0; i < 120u; ++i) {
            CHECK(pixelica_water_body_step(&body, &zero, DEMO_DT) == PIXELICA_OK);
        }
        CHECK_NEAR(sqrt(dot3(body.angular_velocity, body.angular_velocity)), before, 1.0e-4);
    }

    /* Attitude of a known rotation: 0.2 rad about +Z leans the body toward +X. */
    demo_body(&body, 0.0f);
    {
        float roll = 9.0f, pitch = 9.0f, yaw = 9.0f;

        CHECK(pixelica_water_body_attitude(&body, &roll, &pitch, &yaw) == PIXELICA_OK);
        CHECK_NEAR(roll, 0.0, 1.0e-6);
        CHECK_NEAR(pitch, 0.0, 1.0e-6);
        CHECK_NEAR(yaw, 0.0, 1.0e-6);
        CHECK(pixelica_quat_from_axis_angle(&body.orientation, vec3(0.0f, 0.0f, 1.0f), 0.2f) ==
              PIXELICA_OK);
        CHECK(pixelica_water_body_attitude(&body, &roll, &pitch, NULL) == PIXELICA_OK);
        CHECK_NEAR(roll, 0.2, 1.0e-5);
        CHECK_NEAR(pitch, 0.0, 1.0e-5);
        /* NULL outputs are allowed; an invalid body writes nothing. */
        CHECK(pixelica_water_body_attitude(&body, NULL, NULL, NULL) == PIXELICA_OK);
        body.mass = 0.0f;
        roll = 7.0f;
        CHECK(pixelica_water_body_attitude(&body, &roll, NULL, NULL) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(roll == 7.0f);
        CHECK(pixelica_water_body_attitude(NULL, &roll, NULL, NULL) ==
              PIXELICA_ERR_INVALID_ARG);
    }
}

/*
 * [AUDIT 4] Calm settling with the DEMO's actual numbers - the specific claim
 * the documentation is allowed to make - plus a determinism check.
 */
static void test_calm_settling_with_demo_parameters(void)
{
    PIXELICA_WaterSurface flat;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body, replay;
    float draft = 0.0f;
    double worst_overshoot = 0.0;
    uint32_t i;

    memset(&flat, 0, sizeof flat);
    demo_rig(&rig);
    CHECK(pixelica_water_equilibrium_draft(&rig, DEMO_MASS, &draft) == PIXELICA_OK);

    /* Released 0.35 m above its draft, as the demo does. */
    demo_body(&body, -draft + RELEASE_HEIGHT);
    for (i = 0; i < 180u; ++i) {
        PIXELICA_WaterLoad load;
        double offset;

        CHECK(pixelica_water_load(&flat, &rig, &body, 0.0, &load) == PIXELICA_OK);
        CHECK(pixelica_water_body_step(&body, &load, DEMO_DT) == PIXELICA_OK);
        offset = fabs((double)body.position.y + (double)draft);
        if (offset > worst_overshoot) {
            worst_overshoot = offset;
        }
        CHECK(finite_f(body.position.y));
    }
    /* It settled, it stayed level, and it never ran away on the way down. */
    CHECK_NEAR(body.position.y, -(double)draft, 5.0e-3);
    CHECK_NEAR(body.velocity.y, 0.0, 1.0e-2);
    CHECK(worst_overshoot < 0.5);
    {
        float roll = 1.0f, pitch = 1.0f;

        CHECK(pixelica_water_body_attitude(&body, &roll, &pitch, NULL) == PIXELICA_OK);
        CHECK_NEAR(roll, 0.0, 1.0e-3);
        CHECK_NEAR(pitch, 0.0, 1.0e-3);
    }

    /* Identical inputs, identical trajectory, bit for bit. */
    demo_body(&replay, -draft + RELEASE_HEIGHT);
    for (i = 0; i < 180u; ++i) {
        PIXELICA_WaterLoad load;

        CHECK(pixelica_water_load(&flat, &rig, &replay, 0.0, &load) == PIXELICA_OK);
        CHECK(pixelica_water_body_step(&replay, &load, DEMO_DT) == PIXELICA_OK);
    }
    CHECK(memcmp(&body, &replay, sizeof body) == 0);
}

/* The raft really rides a wave surface: it heaves, it tilts, and it neither
 * flies out of the water nor sinks past its probes. */
static void test_raft_rides_waves(void)
{
    PIXELICA_WaterSurface sea;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body;
    float draft = 0.0f;
    double max_tilt = 0.0, max_heave = 0.0;
    uint32_t i, min_submerged = 5u;

    CHECK(pixelica_water_surface_default(&sea) == PIXELICA_OK);
    demo_rig(&rig);
    CHECK(pixelica_water_equilibrium_draft(&rig, DEMO_MASS, &draft) == PIXELICA_OK);
    demo_body(&body, -draft);

    for (i = 0; i < 600u; ++i) {
        const double t = (double)i * (double)DEMO_DT;
        PIXELICA_WaterLoad load;
        float roll = 0.0f, pitch = 0.0f;
        double tilt, heave;

        CHECK(pixelica_water_load(&sea, &rig, &body, t, &load) == PIXELICA_OK);
        if (load.submerged_probes < min_submerged) {
            min_submerged = load.submerged_probes;
        }
        CHECK(pixelica_water_body_step(&body, &load, DEMO_DT) == PIXELICA_OK);
        CHECK(pixelica_water_body_attitude(&body, &roll, &pitch, NULL) == PIXELICA_OK);
        tilt = fabs((double)roll) > fabs((double)pitch) ? fabs((double)roll)
                                                        : fabs((double)pitch);
        heave = fabs((double)body.position.y + (double)draft);
        if (i > 60u) { /* after the first transient */
            if (tilt > max_tilt) {
                max_tilt = tilt;
            }
            if (heave > max_heave) {
                max_heave = heave;
            }
        }
        CHECK(finite_f(body.position.y));
    }
    CHECK(max_tilt > 0.025);  /* it tilts with the waves */
    CHECK(max_tilt < 0.40);   /* but it does not capsize */
    CHECK(max_heave > 0.01);  /* it heaves */
    CHECK(max_heave < 0.50);
    /* Convincingly buoyant: with 0.20 m of draft under a 0.153 m swell every
     * probe stays wet for the whole run. (The same hull at 560 kg lifts a
     * corner clear, and at 240 kg leaves the water bodily - physical, but not
     * what the demo shows.) */
    CHECK(min_submerged == 4u);
    /* It stayed near the surface rather than sinking or hovering. */
    CHECK(fabs((double)body.position.y) < 1.0);
}

/*
 * [AUDIT 4] The explicit integrator is CONDITIONALLY stable, which the module
 * documents rather than papers over. What it does guarantee is that a step it
 * cannot take is reported, never silently turned into NaN: either the call
 * succeeds and the state is finite and in range, or it fails and the state is
 * bit-identical to what it was.
 */
static void test_integrator_stability_is_conditional(void)
{
    PIXELICA_WaterSurface flat;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body, before;
    PIXELICA_WaterProbe stiff[4];
    uint32_t i, steps_taken = 0u, steps_refused = 0u;

    memset(&flat, 0, sizeof flat);
    demo_rig(&rig);
    /* Deliberately far outside the demo's regime: a very stiff, very heavily
     * damped rig at the largest step the API accepts. */
    for (i = 0; i < 4u; ++i) {
        stiff[i] = g_probes[i];
        stiff[i].area = 1.0e5f;
        stiff[i].max_depth = 1.0e3f;
        stiff[i].drag_normal = 1.0e8f;
        stiff[i].drag_tangential = 1.0e8f;
    }
    rig.probes = stiff;
    demo_body(&body, -1.0f);
    body.mass = 1.0f; /* a feather on an enormous float: the stiffest case */

    for (i = 0; i < 200u; ++i) {
        PIXELICA_WaterLoad load;
        PIXELICA_Status status;

        if (pixelica_water_load(&flat, &rig, &body, 0.0, &load) != PIXELICA_OK) {
            break; /* also an acceptable, reported outcome */
        }
        before = body;
        status = pixelica_water_body_step(&body, &load, PIXELICA_WATER_MAX_STEP);
        if (status == PIXELICA_OK) {
            ++steps_taken;
            /* Whatever it did, the state is finite and in range. */
            CHECK(finite_f(body.position.y));
            CHECK(finite_f(body.velocity.y));
            CHECK(finite_f(body.orientation.w));
            CHECK(fabs((double)body.position.y) <= (double)PIXELICA_WATER_MAX_COORD);
        } else {
            ++steps_refused;
            CHECK(status == PIXELICA_ERR_INVALID_ARG);
            CHECK(memcmp(&body, &before, sizeof body) == 0);
            break;
        }
    }
    /* The point is not which happened, but that no NaN ever escaped. */
    CHECK(steps_taken + steps_refused > 0u);
}

int main(void)
{
    test_defaults_and_validation();
    test_travelling_wave_invariant();
    test_height_matches_closed_form();
    test_derivatives_match_finite_differences();
    test_velocity_conventions();
    test_time_handling();
    test_equilibrium_draft();
    test_load_equilibrium_and_saturation();
    test_wave_torque_and_rotation_damping();
    test_drag_is_dissipative();
    test_current_changes_drag();
    test_step_basics();
    test_calm_settling_with_demo_parameters();
    test_raft_rides_waves();
    test_integrator_stability_is_conditional();
    return pixelica_t_finish("test_water");
}
