#include "pixelica/shaders.h"

int main(void)
{
    uint16_t pixels[4] = {0xffffu, 0x0000u, 0xf800u, 0x07e0u};
    PIXELICA_ShaderSurface surface = {pixels, 2u, 2u, 2u};
    PIXELICA_Shader shader;
    if (pixelica_shader_default(PIXELICA_SHADER_INVERT, &shader) != PIXELICA_OK)
        return 1;
    if (pixelica_shader_apply(&surface, &shader, NULL, 0u) != PIXELICA_OK)
        return 2;
    return pixels[0] == 0u && pixels[1] == 0xffffu &&
           pixels[2] == 0x07ffu && pixels[3] == 0xf81fu ? 0 : 3;
}
