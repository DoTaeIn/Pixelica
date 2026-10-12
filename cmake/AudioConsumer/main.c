#include "pixelica/audio.h"
#ifdef AUDIO_SDK_SDL
#include "pixelica/audio_sdl3.h"
#endif

int main(void)
{
    static const uint8_t samples[] = { 0xff, 0x7f, 0x00, 0x80 };
    PIXELICA_AudioClip clip = { samples, 2u, 48000u, 1u, PIXELICA_AUDIO_S16LE };
    PIXELICA_AudioMixer mixer;
    PIXELICA_AudioVoice voice;
    float out[4];
    if (pixelica_audio_init(&mixer, 48000u) != PIXELICA_OK ||
        pixelica_audio_play(&mixer, &clip, NULL, &voice) != PIXELICA_OK ||
        pixelica_audio_render(&mixer, out, 2u) != PIXELICA_OK ||
        out[0] < 0.99f || out[2] != -1.0f ||
        pixelica_audio_is_playing(&mixer, voice)) return 1;
#ifdef AUDIO_SDK_SDL
    {
        PIXELICA_AudioSDL3 device = {0};
        if (pixelica_audio_sdl3_open(&device, 48000u) != PIXELICA_OK) return 2;
        if (pixelica_audio_play(pixelica_audio_sdl3_mixer(&device), &clip, NULL, &voice) != PIXELICA_OK ||
            pixelica_audio_sdl3_update(&device) != PIXELICA_OK) {
            pixelica_audio_sdl3_close(&device);
            return 3;
        }
        pixelica_audio_sdl3_close(&device);
    }
#endif
    return 0;
}
