#include "pixelica/particles.h"
#if defined(PARTICLES_COMPONENT_particles2d)
#include "pixelica/particles2d.h"
#elif defined(PARTICLES_COMPONENT_particles25d)
#include "pixelica/particles25d.h"
#endif

int main(void)
{
    PIXELICA_Particle storage[8];
    PIXELICA_ParticleEmitter emitter;
    PIXELICA_ParticleConfig config;
    uint32_t spawned = 0;
    pixelica_particles_config_default(&config);
    config.rate = 0.0f;
    if (pixelica_particles_init(&emitter, storage, 8, &config, 42) != PIXELICA_OK ||
        pixelica_particles_burst(&emitter, 4, &spawned) != PIXELICA_OK || spawned != 4 ||
        pixelica_particles_update(&emitter, 0.25f) != PIXELICA_OK || emitter.count != 4)
        return 1;
#if defined(PARTICLES_COMPONENT_particles2d) || defined(PARTICLES_COMPONENT_particles25d)
    {
        uint16_t pixels[64] = {0}, depth[64];
        PIXELICA2D_Context canvas;
        PIXELICA2D_Surface surface = {pixels, 8, 8, 8, depth, 8};
        unsigned i;
        for (i = 0; i < 64; ++i) depth[i] = UINT16_MAX;
        if (pixelica2d_init(&canvas, &surface) != PIXELICA_OK) return 2;
#if defined(PARTICLES_COMPONENT_particles2d)
        if (pixelica2d_particles_draw(&canvas, &emitter, 0) != PIXELICA_OK) return 3;
#else
        {
            float vp[16];
            PIXELICA25D_ParticleSortItem scratch[8];
            pixelica_mat4_identity(vp);
            if (pixelica25d_particles_draw(&canvas, vp, &emitter, 0, scratch, 8) != PIXELICA_OK)
                return 3;
        }
#endif
        if (canvas.stats.draw_calls != 4) return 4;
    }
#endif
    return 0;
}
