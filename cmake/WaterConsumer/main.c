/* External consumer of the installed water CORE component: no renderer, no
 * engine internals, only pixelica/water.h from the install prefix. */
#include <math.h>
#include <string.h>

#include "pixelica/water.h"

static PIXELICA_WaterProbe g_probes[4];

int main(void)
{
    PIXELICA_WaterSurface surface;
    PIXELICA_WaterRig rig;
    PIXELICA_WaterBody body;
    PIXELICA_WaterLoad load;
    float draft = 0.0f, flat_height = 0.0f, a = 0.0f, b = 0.0f;
    unsigned i;

    if (pixelica_water_surface_default(&surface) != PIXELICA_OK ||
        pixelica_water_surface_validate(&surface) != PIXELICA_OK) {
        return 1;
    }

    /* A travelling wave really travels along (dir_x, dir_z): a 10 m wave at
     * 2 m/s moves exactly 2 m per second, so the profile repeats. */
    memset(&surface, 0, sizeof surface);
    surface.wave_count = 1u;
    surface.waves[0].dir_x = 1.0f;
    surface.waves[0].amplitude = 1.0f;
    surface.waves[0].wavelength = 10.0f;
    surface.waves[0].speed = 2.0f;
    if (pixelica_water_height(&surface, 0.0f, 0.0f, 0.0, &a) != PIXELICA_OK ||
        pixelica_water_height(&surface, 2.0f, 0.0f, 1.0, &b) != PIXELICA_OK) {
        return 2;
    }
    if (fabs((double)a - (double)b) > 1.0e-6) {
        return 3;
    }

    /* A 100 kg raft on 1 m^2 of fresh water floats 0.1 m down, and at that
     * draft the vertical force balances. */
    memset(g_probes, 0, sizeof g_probes);
    for (i = 0; i < 4u; ++i) {
        g_probes[i].offset.x = (i & 1u) ? 0.5f : -0.5f;
        g_probes[i].offset.z = (i & 2u) ? 0.5f : -0.5f;
        g_probes[i].area = 0.25f;
        g_probes[i].max_depth = 0.2f;
        g_probes[i].drag_normal = 400.0f;
        g_probes[i].drag_tangential = 60.0f;
    }
    rig.probes = g_probes;
    rig.probe_count = 4u;
    rig.density = 1000.0f;
    rig.gravity = 9.81f;
    if (pixelica_water_equilibrium_draft(&rig, 100.0f, &draft) != PIXELICA_OK) {
        return 4;
    }
    if (fabs((double)draft - 0.1) > 1.0e-6) {
        return 5;
    }

    memset(&surface, 0, sizeof surface); /* flat water at Y = 0 */
    memset(&body, 0, sizeof body);
    pixelica_quat_identity(&body.orientation);
    body.position.y = -draft;
    body.mass = 100.0f;
    body.inertia.x = 20.0f;
    body.inertia.y = 30.0f;
    body.inertia.z = 20.0f;
    if (pixelica_water_height(&surface, 0.0f, 0.0f, 0.0, &flat_height) != PIXELICA_OK ||
        flat_height != 0.0f) {
        return 6;
    }
    if (pixelica_water_load(&surface, &rig, &body, 0.0, &load) != PIXELICA_OK) {
        return 7;
    }
    if (load.submerged_probes != 4u || fabs((double)load.force.y) > 1.0e-2 ||
        fabs((double)load.torque.x) > 1.0e-3 || fabs((double)load.torque.z) > 1.0e-3) {
        return 8;
    }
    /* A step at equilibrium must not move it anywhere. */
    if (pixelica_water_body_step(&body, &load, 1.0f / 60.0f) != PIXELICA_OK) {
        return 9;
    }
    if (fabs((double)body.position.y + (double)draft) > 1.0e-3) {
        return 10;
    }
    return 0;
}
