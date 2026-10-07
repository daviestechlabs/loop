#ifndef DTL_AUDIO_KERNEL_H
#define DTL_AUDIO_KERNEL_H
#include <stdint.h>

#define DTL_AUDIO_INPUT_MAX 4096u
#define DTL_AUDIO_OUTPUT_SAMPLES 320u
#define DTL_AUDIO_PCM_BYTES_MAX (DTL_AUDIO_INPUT_MAX * sizeof(float))

uintptr_t input_ptr(void);
uintptr_t output_ptr(void);
uint32_t output_sample_count(void);
float process(uint32_t input_length);
uintptr_t playback_ptr(void);
uint32_t decode_pcm(uint32_t byte_length);
void clear_audio(void);
#endif
