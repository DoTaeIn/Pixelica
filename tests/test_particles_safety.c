/* Audit regressions: validate every draw before writing, and protect inputs
 * from caller scratch. Checks remain enabled in Release. */
#include <float.h>
#include <string.h>
#include "pixelica/particles2d.h"
#ifdef PIXELICA_TEST_PARTICLES25D
#include "pixelica/particles25d.h"
#endif
#include "test_util.h"

static void test_rotation_overflow(void)
{
    uint16_t pixels[64] = {0}, depth[64], before[64];
    const uint16_t white = UINT16_MAX;
    const PIXELICA_Texture texture = {&white, 1, 1};
    PIXELICA2D_Surface surface = {pixels, 8, 8, 8, depth, 8};
    PIXELICA2D_Context canvas, old_canvas;
    PIXELICA2D_Sprite appearance;
    PIXELICA_ParticleConfig config;
    PIXELICA_ParticleEmitter emitter;
    PIXELICA_Particle particles[2];
    unsigned i;
    for (i = 0; i < 64; ++i) depth[i] = UINT16_MAX;
    pixelica_particles_config_default(&config);
    config.position.x = 4; config.position.y = 4;
    config.size_start = 4; config.size_end = 4;
    config.rate = 0;
    CHECK(pixelica_particles_init(&emitter, particles, 2, &config, 7) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&emitter, 2, NULL) == PIXELICA_OK);
    CHECK(pixelica2d_init(&canvas, &surface) == PIXELICA_OK);
    CHECK(pixelica2d_sprite_init(&appearance, &texture) == PIXELICA_OK);
    appearance.rotation = FLT_MAX;
    particles[1].rotation = FLT_MAX; /* Individually finite, combined value is not. */
    old_canvas = canvas;
    memcpy(before, pixels, sizeof pixels);
    CHECK(pixelica2d_particles_draw(&canvas, &emitter, &appearance) == PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, pixels, sizeof pixels) == 0);
    CHECK(memcmp(&old_canvas, &canvas, sizeof canvas) == 0);
#ifdef PIXELICA_TEST_PARTICLES25D
    {
        float vp[16];
        PIXELICA25D_ParticleSortItem scratch[2];
        PIXELICA25D_ParticleOptions options = {&appearance, false};
        particles[0].position.x = particles[0].position.y = 0;
        particles[1].position.x = particles[1].position.y = 0;
        pixelica_mat4_identity(vp);
        CHECK(pixelica25d_particles_draw(&canvas, vp, &emitter, &options, scratch, 2) ==
              PIXELICA_ERR_INVALID_ARG);
        CHECK(memcmp(before, pixels, sizeof pixels) == 0);
        CHECK(memcmp(&old_canvas, &canvas, sizeof canvas) == 0);
    }
#endif
}

#ifdef PIXELICA_TEST_PARTICLES25D
static void test_scratch_input_alias(void)
{
    union {
        PIXELICA_ParticleEmitter emitter;
        PIXELICA25D_ParticleSortItem scratch[32];
    } input;
    unsigned char before[sizeof input];
    uint16_t pixels[64] = {0}, depth[64];
    PIXELICA2D_Surface surface = {pixels, 8, 8, 8, depth, 8};
    PIXELICA2D_Context canvas;
    PIXELICA_Particle particles[2];
    PIXELICA_ParticleConfig config;
    float vp[16];
    unsigned i;
    for (i = 0; i < 64; ++i) depth[i] = UINT16_MAX;
    memset(&input, 0, sizeof input);
    pixelica_particles_config_default(&config);
    pixelica_mat4_identity(vp);
    CHECK(pixelica2d_init(&canvas, &surface) == PIXELICA_OK);
    CHECK(pixelica_particles_init(&input.emitter, particles, 2, &config, 7) == PIXELICA_OK);
    CHECK(pixelica_particles_burst(&input.emitter, 2, NULL) == PIXELICA_OK);
    memcpy(before, &input, sizeof input);
    CHECK(pixelica25d_particles_draw(&canvas, vp, &input.emitter, NULL, input.scratch, 32) ==
          PIXELICA_ERR_INVALID_ARG);
    CHECK(memcmp(before, &input, sizeof input) == 0);
    CHECK(canvas.stats.draw_calls == 0);
}
#endif

int main(void)
{
    test_rotation_overflow();
#ifdef PIXELICA_TEST_PARTICLES25D
    test_scratch_input_alias();
#endif
    return pixelica_t_finish("test_particles_safety");
}
