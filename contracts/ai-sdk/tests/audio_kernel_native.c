#include "audio_kernel.h"
#include <stdio.h>
#include <string.h>

/* A binary oracle keeps the native float and PCM bytes independent of JS. */
int main(void) {
    uint32_t length;
    while (fread(&length, sizeof(length), 1u, stdin) == 1u) {
        float energy;
        if (length > DTL_AUDIO_INPUT_MAX ||
            fread((void *)input_ptr(), sizeof(float), length, stdin) != length) return 1;
        energy = process(length);
        if (fwrite(&energy, sizeof(energy), 1u, stdout) != 1u ||
            fwrite((void *)output_ptr(), sizeof(int16_t), DTL_AUDIO_OUTPUT_SAMPLES, stdout) != DTL_AUDIO_OUTPUT_SAMPLES) return 1;
        clear_audio();
        for (size_t i = 0; i < DTL_AUDIO_PCM_BYTES_MAX; i++)
            if (((const unsigned char *)input_ptr())[i] != 0u) return 1;
    }
    return ferror(stdin) ? 1 : 0;
}
