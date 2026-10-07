#include "audio_kernel.h"
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 1u) return 0;
    const uint8_t operation = data[0];
    data++;
    size--;
    if (size > DTL_AUDIO_PCM_BYTES_MAX) size = DTL_AUDIO_PCM_BYTES_MAX;
    memcpy((void *)input_ptr(), data, size);
    if (operation & 1u) {
        const uint32_t count = decode_pcm((uint32_t)size);
        if (count > DTL_AUDIO_PCM_BYTES_MAX / 2u) abort();
        const float *samples = (const float *)playback_ptr();
        for (uint32_t i = 0u; i < count; i++)
            if (!(samples[i] >= -1.0f && samples[i] < 1.0f)) abort();
    } else {
        const float energy = process((uint32_t)(size / sizeof(float)));
        if (!(energy == -1.0f || (energy >= 0.0f && energy <= 1.0f))) abort();
    }
    clear_audio();
    return 0;
}
