/*
 * Pixelica - optional ocean surface + buoyancy module (Pixelica::water).
 *
 * Renderer-independent portable C11. This header needs only the common library
 * (pixelica/status.h, pixelica/math.h); it includes no renderer header and the
 * library links no renderer. The optional 3D adapter that tessellates and draws
 * the surface lives in a SEPARATE library and header (pixelica/water3d.h).
 *
 * WHAT THIS IS
 *   A deterministic analytic HEIGHTFIELD - a sum of travelling sine waves -
 *   plus an explicit sample-point displacement buoyancy model that reports the
 *   force and torque acting on one rigid body. Rendering and physics call the
 *   same pixelica_water_sample() at the same time, so the drawn surface and the
 *   floating body cannot disagree about where the water is.
 *
 * WHAT THIS IS NOT
 *   Not a fluid solver. There is no pressure projection, no free-surface
 *   tracking, no breaking waves and no splash. A heightfield is a single-valued
 *   function of (x, z): it CANNOT overhang, curl or tube. It also carries no
 *   wave ORBITAL velocity field; see "velocity conventions" below for exactly
 *   what the reported water velocity is and is not.
 *   pixelica_water_load() integrates NOTHING: it only reports loads.
 *   pixelica_water_body_step() is an explicit single-body integrator for
 *   game-grade use, not a general rigid-body solver: no contacts, no
 *   constraints, no stacking, no collision response, and no unconditional
 *   stability (see its own comment for the step-size condition).
 *
 * MEMORY AND STATE
 *   No heap, no globals, no hidden state. Every struct is caller-owned plain
 *   data; the only pointers any function dereferences are ones the caller
 *   passed in, and arrays are borrowed for the duration of the call. Wave and
 *   probe counts are bounded by the constants below, so nothing here can grow
 *   without the caller asking. Two threads may sample the SAME const surface
 *   concurrently (nothing is mutated); they must not step the same body.
 *
 * UNITS
 *   Length m, time s, mass kg, force N, torque N*m, angle rad, density kg/m^3,
 *   gravity m/s^2 as a POSITIVE magnitude acting along -Y. World axes are the
 *   engine's: +X right, +Y up, -Z into the screen (pixelica/math.h), so the
 *   water lies in the XZ plane and height is Y.
 *
 * TIME IS double, DELIBERATELY
 *   Every entry point that takes a time takes a `double`, and the caller's
 *   value is never narrowed to float anywhere inside. A float clock dies in a
 *   long session: at t = 1.0e6 s a float cannot even represent t + 1/60, so
 *   `(float)(1.0e6f + 1.0f/60.0f) == 1.0e6f` and the sea would freeze solid.
 *   Internally each wave's phase is reduced modulo 2*pi in double before
 *   sin/cos, so accuracy does not decay with t the way naive accumulation
 *   does. The remaining error is the rounding of the product omega * t, about
 *   |omega * t| * 2^-53 radians: for a 1.6 rad/s wave at t = 1.0e9 s that is
 *   ~2e-7 rad, i.e. still exact to float. There is no hidden upper bound on
 *   time: any finite double is accepted.
 *
 * ERROR POLICY
 *   Every function validates all of its inputs (NULL, non-finite, out of
 *   range) BEFORE writing anything and returns PIXELICA_ERR_INVALID_ARG with
 *   every output object left exactly as it was. Results are also checked for
 *   finiteness: a parameter set that would produce a non-finite height,
 *   normal, force or torque is reported instead of handing back a NaN.
 *   Degenerate-but-meaningful configurations are NOT errors: wave_count == 0 is
 *   a flat surface, amplitude 0 is a wave that does nothing, speed 0 is a
 *   standing (non-travelling) wave, and a probe above the water simply
 *   contributes no load.
 */
#ifndef PIXELICA_WATER_H
#define PIXELICA_WATER_H

#include <stdint.h>

#include "pixelica/math.h"
#include "pixelica/status.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PIXELICA_WATER_MAX_WAVES 8u   /* waves per surface */
#define PIXELICA_WATER_MAX_PROBES 32u /* sample points per rig */

/*
 * Accepted parameter ranges (inclusive; everything must also be finite).
 * These are explicit so that nothing is rejected by a bound the header does
 * not name. Note PIXELICA_WATER_MAX_COORD bounds the QUERY POSITION only;
 * `time_seconds` is unbounded (any finite double).
 */
#define PIXELICA_WATER_MAX_COORD 1.0e6f /* |x|, |z| of a query, and |position| */
#define PIXELICA_WATER_MIN_WAVELENGTH 1.0e-3f
#define PIXELICA_WATER_MAX_WAVELENGTH 1.0e6f
#define PIXELICA_WATER_MAX_AMPLITUDE 1.0e4f
#define PIXELICA_WATER_MAX_SPEED 1.0e4f
#define PIXELICA_WATER_MAX_DENSITY 1.0e6f
#define PIXELICA_WATER_MAX_GRAVITY 1.0e4f
#define PIXELICA_WATER_MAX_AREA 1.0e6f
#define PIXELICA_WATER_MAX_DEPTH 1.0e4f
#define PIXELICA_WATER_MAX_OFFSET 1.0e4f
#define PIXELICA_WATER_MAX_DRAG 1.0e9f
#define PIXELICA_WATER_MAX_MASS 1.0e9f
#define PIXELICA_WATER_MAX_INERTIA 1.0e12f
#define PIXELICA_WATER_MAX_STEP 0.25f /* seconds per pixelica_water_body_step */

/*
 * One travelling sine wave.
 *
 *   k     = 2*pi / wavelength            (rad/m, the wave number)
 *   omega = k * speed                    (rad/s, the angular frequency)
 *   theta = k * (dhat . (x, z)) - omega * t + phase
 *   its contribution to the height is   amplitude * sin(theta)
 *
 * The MINUS sign on omega * t is what makes the wave travel along
 * (dir_x, dir_z): a feature of the profile stays at constant theta, so it
 * satisfies dhat . (x, z) = speed * t + const. Equivalently
 *
 *   h(p + dhat * speed * dt, t + dt) == h(p, t)
 *
 * exactly - the travelling-wave invariant, which tests/test_water.c asserts.
 * (With a plus sign the crest would move the other way, i.e. AGAINST the
 * direction this field documents.)
 *
 * (dir_x, dir_z) is the horizontal propagation direction in world XZ. Only its
 * DIRECTION matters: it is normalized internally and need not be unit length,
 * but it must be finite and non-zero. A negative `speed` makes the wave travel
 * against (dir_x, dir_z); speed 0 is a standing wave (no time dependence).
 * The crest-to-trough height of this wave alone is 2 * amplitude, and the
 * single wave repeats with period wavelength / |speed|.
 */
typedef struct PIXELICA_WaterWave {
    float dir_x, dir_z; /* propagation direction in world XZ, non-zero */
    float amplitude;    /* m, 0 .. PIXELICA_WATER_MAX_AMPLITUDE */
    float wavelength;   /* m, PIXELICA_WATER_MIN_WAVELENGTH .. _MAX_WAVELENGTH */
    float speed;        /* phase speed, m/s, |speed| <= PIXELICA_WATER_MAX_SPEED */
    float phase;        /* rad at t = 0, finite */
} PIXELICA_WaterWave;

/*
 * The whole surface: a still-water plane at `base_height` plus `wave_count`
 * waves, and a uniform horizontal current.
 *
 * `current_x` / `current_z` are the horizontal velocity of the WATER (m/s).
 * They are the caller's explicit statement that the whole body of water
 * drifts; they do NOT move the waves and are not derived from them. Leave them
 * 0 for still water with waves running through it. What they change is the
 * reported water velocity, and therefore drag - see PIXELICA_WaterSample.
 *
 * The struct is plain data and holds its waves inline, so it can live on the
 * stack, in a static, or inside a save file. Copying it is a full copy.
 */
typedef struct PIXELICA_WaterSurface {
    PIXELICA_WaterWave waves[PIXELICA_WATER_MAX_WAVES];
    uint32_t wave_count; /* 0 .. PIXELICA_WATER_MAX_WAVES; 0 = a flat plane */
    float base_height;   /* still-water level, world Y, m */
    float current_x;     /* uniform horizontal water velocity, m/s */
    float current_z;
} PIXELICA_WaterSurface;

/*
 * Everything one world XZ position and one time say about the surface.
 *
 * VELOCITY CONVENTIONS. This is the part that is easy to get wrong and easy to
 * overclaim, so it is spelled out exactly. TWO different vertical velocities
 * are reported, because they are genuinely different numbers and a caller
 * deserves to choose:
 *
 *   eulerian_dhdt = (partial h / partial t) at CONSTANT (x, z)
 *       How fast the water LEVEL at this fixed spot in the world rises (m/s).
 *       This is a pure Eulerian field quantity. It is NOT the vertical
 *       velocity of any piece of water, and it ignores the current entirely.
 *
 *   surface_velocity_y = D h / D t = partial h / partial t
 *                        + current_x * slope_x + current_z * slope_z
 *       The vertical velocity of the MATERIAL surface point, for a surface
 *       whose shape is h and which is advected horizontally by the uniform
 *       current. This is the kinematically coherent companion of the
 *       horizontal velocity (current_x, current_z): together they form a
 *       velocity field that actually keeps the material surface on the
 *       surface. It is what drag should be measured against, and it is what
 *       `velocity` carries.
 *
 *   velocity = (current_x, surface_velocity_y, current_z)
 *       The water velocity pixelica_water_load() uses for relative-velocity
 *       drag. HONEST SCOPE: this is a coherent kinematic approximation for a
 *       heightfield carried by a uniform current - not a general fluid
 *       velocity field. In particular there is NO wave orbital motion in it:
 *       a wave passing a floating body will heave the body (correct) but will
 *       not surge it back and forth under the crest (a real wave would). With
 *       current_x == current_z == 0 - the default - the horizontal part is
 *       zero and surface_velocity_y reduces exactly to eulerian_dhdt.
 *
 *   slope_x / slope_z = partial h / partial x, partial h / partial z
 *       The analytic partial derivatives (dimensionless), and the normal is
 *       built from them: normal = normalize(-slope_x, 1, -slope_z), so it
 *       always has normal.y > 0 - a heightfield never overhangs.
 */
typedef struct PIXELICA_WaterSample {
    float height;             /* world Y of the surface at the query XZ, m */
    PIXELICA_Vec3 normal;     /* analytic unit normal, normal.y > 0 */
    float slope_x, slope_z;   /* d h / d x, d h / d z */
    float eulerian_dhdt;      /* partial h / partial t at fixed XZ, m/s */
    float surface_velocity_y; /* D h / D t under the uniform current, m/s */
    PIXELICA_Vec3 velocity;   /* (current_x, surface_velocity_y, current_z) */
} PIXELICA_WaterSample;

/*
 * One sample point of a floating body: a small horizontal plate, fixed in the
 * body, standing in for a piece of the hull.
 *
 *   offset     position in the BODY frame, relative to the centre of mass (m).
 *   area       the horizontal plan area this probe represents (m^2). The sum
 *              over the probes should be the body's waterplane area.
 *   max_depth  the submergence depth at which this probe's displaced volume
 *              SATURATES (m). The displaced volume is
 *                  V = area * min(depth, max_depth)
 *              so a probe can never displace more than area * max_depth no
 *              matter how deep it is pushed. That bound is what keeps the
 *              model bounded when a body is slammed under: without it the
 *              buoyant force would grow without limit. Set it to the local
 *              hull thickness (for a raft, its board thickness).
 *   drag_normal / drag_tangential
 *              LINEAR damping coefficients, in N per (m/s) per m^2 of WETTED
 *              area, i.e. kg*m^-2*s^-1. The drag force of this probe is
 *                  F = -wetted * (drag_normal * v_rel_y * yhat
 *                                 + drag_tangential * v_rel_horizontal)
 *              with wetted = area * min(depth, max_depth) / max_depth, the
 *              same 0..1 submergence fraction the buoyancy uses, and v_rel the
 *              water-relative velocity of the probe POINT (below). This is a
 *              linearized damping law, not quadratic form drag.
 *              It is DISSIPATIVE as a force: F . v_rel <= 0 always, so the
 *              instantaneous mechanical power it does on the body is never
 *              positive (tests/test_water.c asserts this). That is a property
 *              of the force, NOT a stability guarantee for any integrator -
 *              see pixelica_water_body_step() for the step-size condition.
 *              Both coefficients must be >= 0; 0 disables that axis.
 */
typedef struct PIXELICA_WaterProbe {
    PIXELICA_Vec3 offset;  /* body-frame offset from the centre of mass, m */
    float area;            /* m^2, > 0 .. PIXELICA_WATER_MAX_AREA */
    float max_depth;       /* m, > 0 .. PIXELICA_WATER_MAX_DEPTH */
    float drag_normal;     /* kg*m^-2*s^-1, >= 0 */
    float drag_tangential; /* kg*m^-2*s^-1, >= 0 */
} PIXELICA_WaterProbe;

/*
 * The float: the probe set plus the two fluid constants. `probes` is BORROWED
 * and must stay valid for the duration of the call; nothing is copied.
 * density 1000 is fresh water, 1025 sea water. gravity is a positive
 * magnitude; it acts along -Y.
 */
typedef struct PIXELICA_WaterRig {
    const PIXELICA_WaterProbe *probes; /* borrowed, probe_count entries */
    uint32_t probe_count;              /* 1 .. PIXELICA_WATER_MAX_PROBES */
    float density;                     /* kg/m^3, > 0 .. _MAX_DENSITY */
    float gravity;                     /* m/s^2, > 0 .. _MAX_GRAVITY, along -Y */
} PIXELICA_WaterRig;

/*
 * The rigid-body state pixelica_water_load() reads (and pixelica_water_body_step()
 * advances). `orientation` maps BODY coordinates to WORLD; only its direction
 * matters (it is normalized internally). `inertia` is the DIAGONAL of the
 * body-frame inertia tensor, so the body frame must be the principal axes -
 * which it is for the box-like hulls this module is for. `velocity` and
 * `angular_velocity` are both in WORLD coordinates.
 */
typedef struct PIXELICA_WaterBody {
    PIXELICA_Vec3 position;         /* centre of mass, world, m */
    PIXELICA_Quat orientation;      /* body -> world rotation, non-zero */
    PIXELICA_Vec3 velocity;         /* world, m/s */
    PIXELICA_Vec3 angular_velocity; /* omega, world, rad/s */
    float mass;                     /* kg, > 0 .. PIXELICA_WATER_MAX_MASS */
    PIXELICA_Vec3 inertia;          /* body-frame diagonal, kg*m^2, each > 0 */
} PIXELICA_WaterBody;

/*
 * What the water (and gravity) do to the body right now. Everything is in
 * WORLD coordinates and the torque is about the body's CENTRE OF MASS.
 *
 *   force    = buoyancy + drag + gravity, so gravity is recoverable as
 *              force - buoyancy - drag = (0, -mass * gravity, 0).
 *   torque   from buoyancy and drag only: gravity acts at the centre of mass
 *              and therefore contributes exactly zero torque about it.
 *   submerged_volume  the displaced volume the buoyancy was computed from
 *              (m^3), after per-probe saturation.
 *   submerged_area    the summed wetted area of the submerged probes (m^2),
 *              i.e. the area the drag was scaled by.
 *   submerged_probes  how many probes were below the surface.
 */
typedef struct PIXELICA_WaterLoad {
    PIXELICA_Vec3 force;
    PIXELICA_Vec3 torque;
    PIXELICA_Vec3 buoyancy; /* hydrostatic part of `force`, N */
    PIXELICA_Vec3 drag;     /* damping part of `force`, N */
    float submerged_volume; /* m^3 */
    float submerged_area;   /* m^2 */
    uint32_t submerged_probes;
} PIXELICA_WaterLoad;

/* ---- surface ------------------------------------------------------------ */

/*
 * A deterministic two-wave swell: base_height 0, no current, and two waves
 * crossing at about 70 degrees -
 *
 *   dir (1, 0.25),  amplitude 0.095 m, wavelength 8.0 m, speed 3.0 m/s
 *   dir (0.35, -1), amplitude 0.058 m, wavelength 4.5 m, speed 2.2 m/s
 *
 * so the surface stays inside +/-0.153 m of the still-water line and the
 * steepest slope of either wave is about 4.5 degrees (about 9 degrees where
 * the two add, which is what makes the swell legible). Both wavelengths are
 * several metres and the two amplitudes are comparable on purpose: a hull a
 * couple of metres across rides these waves (rather than averaging a short
 * chop away) and is tilted about BOTH horizontal axes, not just one.
 *
 * What that means for a floating body is NOT a property of the surface alone -
 * it depends on the body's draft, span and damping. For the 2 x 2 m, 0.14 m
 * draft raft in examples/hello_water it measures as a few degrees of roll and
 * pitch and about 0.1 m of heave, and the raft stays fully in the water. A
 * much lighter body (a draft small against the 0.153 m wave height) WILL leave
 * the water on a trough; that is real behaviour of a light hull in a swell,
 * not a defect, and the module reports it through
 * PIXELICA_WaterLoad::submerged_probes rather than hiding it.
 *
 * NULL -> PIXELICA_ERR_INVALID_ARG.
 */
PIXELICA_Status pixelica_water_surface_default(PIXELICA_WaterSurface *out);

/*
 * Checks every field of `surface` against the documented ranges. Returns
 * PIXELICA_OK for an acceptable surface (including a flat one with
 * wave_count == 0) and PIXELICA_ERR_INVALID_ARG otherwise. Every other
 * function here performs the same check itself; this one just lets a caller
 * validate data loaded from a file or an editor before using it.
 */
PIXELICA_Status pixelica_water_surface_validate(const PIXELICA_WaterSurface *surface);

/*
 * The period of wave `index` (wavelength / |speed|, in seconds), or 0 for a
 * standing wave (speed == 0) - a standing wave has no period because it never
 * changes. PIXELICA_ERR_INVALID_ARG (out untouched) for NULL, an invalid
 * surface or index >= wave_count.
 *
 * A surface as a whole repeats only when its waves' periods have a common
 * multiple; that is the caller's arithmetic, and the usual way to get an
 * exactly looping sea is to pick speeds so that every wavelength/|speed| is
 * the same number. Sampling does not depend on this at all (see "TIME IS
 * double" at the top): a non-looping sea is perfectly fine and stays accurate.
 */
PIXELICA_Status pixelica_water_wave_period(const PIXELICA_WaterSurface *surface,
                                           uint32_t index, float *out_seconds);

/*
 * The one sampling entry point: the surface at world (x, z) and absolute time
 * `time_seconds`. This is what BOTH the renderer adapter and the buoyancy use,
 * which is what makes the drawn water and the floating body agree.
 *
 * PIXELICA_ERR_INVALID_ARG, with *out untouched: NULL surface/out; an invalid
 * surface; a non-finite x, z or time_seconds; an |x| or |z| beyond
 * PIXELICA_WATER_MAX_COORD; or a result that is not finite. `time_seconds` has
 * no range limit beyond being finite.
 */
PIXELICA_Status pixelica_water_sample(const PIXELICA_WaterSurface *surface, float x,
                                      float z, double time_seconds,
                                      PIXELICA_WaterSample *out);

/*
 * pixelica_water_sample() for callers that only want the height (same
 * validation, same value, no normal or velocity work).
 */
PIXELICA_Status pixelica_water_height(const PIXELICA_WaterSurface *surface, float x,
                                      float z, double time_seconds, float *out_height);

/* ---- buoyancy ----------------------------------------------------------- */

/*
 * The explicit sample-point displacement model. For every probe:
 *
 *   r      = R * probe.offset                     (body -> world, R from
 *                                                  body->orientation)
 *   p      = body->position + r                   the probe point, world
 *   depth  = surface height at (p.x, p.z) - p.y   > 0 means submerged
 *   f      = min(depth, max_depth) / max_depth    submergence fraction, 0..1
 *   V      = area * max_depth * f                 displaced volume, m^3
 *   F_b    = (0, density * gravity * V, 0)        Archimedes, strictly vertical
 *   v_p    = body->velocity + omega x r           velocity of the probe POINT
 *   v_rel  = v_p - sample.velocity                water-relative velocity
 *   F_d    = -(area * f) * (drag_normal * v_rel.y * yhat
 *                           + drag_tangential * (v_rel.x, 0, v_rel.z))
 *   torque += r x (F_b + F_d)
 *
 * and finally force += (0, -mass * gravity, 0).
 *
 * Note the three deliberate modelling choices:
 *   - The buoyant force is strictly along +Y, not along the surface normal.
 *     Hydrostatic pressure on a horizontal plan area is vertical; tilting the
 *     force with the wave would be wrong. The body still tilts with the waves,
 *     because the probes sit at different depths and r x F therefore produces a
 *     torque. That is the real mechanism, not a heuristic.
 *   - `omega x r` is included in the probe velocity, so rotation is damped by
 *     the same coefficients that damp translation; without it a spinning body
 *     would never settle.
 *   - The water velocity subtracted is sample.velocity, i.e. the kinematically
 *     coherent (current, Dh/Dt) pair documented on PIXELICA_WaterSample - with
 *     its stated limitation that it contains no wave orbital motion.
 *
 * A probe with depth <= 0 is skipped entirely (no buoyancy, no drag): it is out
 * of the water. With every probe out of the water the load is pure gravity,
 * which is the correct answer, not an error.
 *
 * `time_seconds` must be the same time the renderer draws, or the body will
 * float relative to the wrong surface.
 *
 * PIXELICA_ERR_INVALID_ARG, with *out untouched: NULL argument; an invalid
 * surface / rig / body (see the range comments on each struct); a probe point
 * whose world position is not finite or leaves PIXELICA_WATER_MAX_COORD; or any
 * resulting force or torque component that is not finite.
 */
PIXELICA_Status pixelica_water_load(const PIXELICA_WaterSurface *surface,
                                    const PIXELICA_WaterRig *rig,
                                    const PIXELICA_WaterBody *body,
                                    double time_seconds, PIXELICA_WaterLoad *out);

/*
 * The equilibrium DRAFT of a rig carrying `mass`, defined precisely as:
 *
 *   the depth of the body's CENTRE OF MASS below the still-water line, for an
 *   UPRIGHT body (orientation = identity) resting on a FLAT surface.
 *
 * With the centre of mass at world Y = base_height - draft, probe i sits at
 * depth (draft - offset_i.y), so the displaced volume as a function of draft is
 *
 *   V(d) = sum_i area_i * clamp(d - offset_i.y, 0, max_depth_i)
 *
 * and the draft returned is the solution of V(d) = mass / density. V is
 * continuous, piecewise linear and non-decreasing, so the root is solved
 * EXACTLY: the breakpoints (offset_i.y and offset_i.y + max_depth_i) are
 * sorted, the bracketing segment is found, and the answer is read off that
 * segment's line. Per-probe saturation is therefore fully accounted for, and
 * probes at different body-frame heights are too.
 *
 * This is the number a demo or a test compares a settled body against, and the
 * number to initialize a body with so it starts balanced instead of dropping:
 * release the centre of mass at `base_height - draft`.
 *
 * The draft MAY BE NEGATIVE, and routinely is. It is the depth of the centre of
 * mass, not of the hull bottom, so a flat board whose probes sit at
 * offset.y = -0.175 m and which floats 0.14 m deep has its centre of mass
 * 0.035 m ABOVE the water: draft = -0.035. The submerged depth of a probe is
 * always `draft - offset_i.y`, which is the positive number to compare a probe
 * against.
 *
 * Returns PIXELICA_ERR_CAPACITY (with *out_draft untouched) ONLY when the rig
 * genuinely cannot float the mass, i.e. when
 *   mass / density > sum_i area_i * max_depth_i,
 * the total displaceable volume. A rig whose individual probes saturate at
 * different depths still floats as long as that total is enough - e.g. probes
 * (area 1, max_depth 1) and (area 1, max_depth 3) at density 1000 carry
 * 2500 kg at a draft of exactly 1.5 m, and only above 4000 kg is the answer
 * PIXELICA_ERR_CAPACITY.
 *
 * When V has a flat stretch at exactly the target volume (possible when a gap
 * separates the probes vertically), the SMALLEST draft that reaches the target
 * is returned.
 *
 * PIXELICA_ERR_INVALID_ARG (out untouched) for NULL, an invalid rig, a mass
 * outside 0 .. PIXELICA_WATER_MAX_MASS, or a non-finite result.
 */
PIXELICA_Status pixelica_water_equilibrium_draft(const PIXELICA_WaterRig *rig, float mass,
                                                 float *out_draft);

/*
 * ONE explicit semi-implicit (symplectic) Euler step of ONE rigid body under a
 * load the CALLER computed, with the caller's own fixed `dt`. It exists so a
 * game that wants a raft on the water does not have to re-derive quaternion
 * integration, and it is kept deliberately small and visible:
 *
 *   velocity         += (load->force / mass) * dt
 *   position         += velocity * dt                     (the new velocity)
 *   omega_body        = R^T * angular_velocity
 *   alpha_body        = I^-1 * (R^T * torque - omega_body x (I * omega_body))
 *   omega_body       += alpha_body * dt
 *   angular_velocity  = R * omega_body
 *   orientation       = normalize(dq * orientation), dq = rotation of
 *                       |angular_velocity| * dt about its own axis
 *
 * The Euler gyroscopic term omega x (I omega) is included, so the rotation is
 * the correct free motion of a body with principal axes along its own frame.
 *
 * STABILITY - no blanket guarantee. This is an EXPLICIT integrator, so it is
 * conditionally stable and the condition depends on the caller's own numbers,
 * not on anything this module can enforce. For heave of a body of mass m on
 * probes of total area A at density rho with vertical drag coefficient c_n,
 * the linearized motion is a damped oscillator with
 *
 *   omega_n = sqrt(rho * g * A / m)        (undamped heave frequency)
 *   2 * zeta * omega_n = c_n * A_wet / m   (damping rate)
 *
 * and semi-implicit Euler needs roughly
 *
 *   dt < 2 / omega_n      AND      dt < 2 * m / (c_n * A_wet)
 *
 * i.e. a stiffer float or a heavier drag coefficient demands a smaller step.
 * The permitted parameter ranges in this header are wide enough to violate
 * both at 60 Hz, so a caller who pushes them must reduce `dt` or substep;
 * `dt` is accepted up to PIXELICA_WATER_MAX_STEP because clamping it to
 * something "safe" would be a lie for some rigs and a straitjacket for others.
 * The raft in examples/hello_water sits well inside both bounds at 60 Hz and
 * the demo asserts that it settles.
 *
 * Also NOT a rigid-body solver: it knows nothing about contacts, constraints,
 * joints, other bodies or a sea floor, and will happily push a body through
 * anything. `dt` must be finite and in (0, PIXELICA_WATER_MAX_STEP].
 *
 * PIXELICA_ERR_INVALID_ARG, with *body COMPLETELY unchanged: NULL argument, an
 * invalid body, a non-finite load, a dt out of range, or any resulting state
 * component that is not finite or out of range. The new state is assembled in
 * a local copy and only committed once all of it validates, so a diverging
 * step reports the failure instead of poisoning the body.
 */
PIXELICA_Status pixelica_water_body_step(PIXELICA_WaterBody *body,
                                         const PIXELICA_WaterLoad *load, float dt);

/*
 * The body's roll (about world +X), pitch (about world +Z) and yaw (about
 * world +Y) in radians, derived from `orientation` - a convenience for demos
 * and tests that want to report or assert a tilt. Roll and pitch are the
 * signed angles of the body's own +Y axis away from world +Y:
 *
 *   up     = R * (0, 1, 0)
 *   roll   = atan2(-up.x, up.y)     positive = leaning toward +X
 *   pitch  = atan2( up.z, up.y)     positive = leaning toward +Z
 *   yaw    = atan2(fx.x, -fx.z) with fx = R * (0, 0, -1)
 *
 * so a level body gives (0, 0, yaw) and the tilt magnitude is bounded by pi.
 * Any `out_*` may be NULL. PIXELICA_ERR_INVALID_ARG (nothing written) for a
 * NULL or invalid body.
 */
PIXELICA_Status pixelica_water_body_attitude(const PIXELICA_WaterBody *body,
                                             float *out_roll, float *out_pitch,
                                             float *out_yaw);

#ifdef __cplusplus
}
#endif

#endif /* PIXELICA_WATER_H */
