#include "stt_vocabulary.h"
#include "cmp_json.h"

#include <string.h>

int stt_vocabulary_evaluator_ready(const char *health) {
    cmp_json_object object;
    char status[16], backend[32], device[16], fingerprint[65];
    int evaluation = 0, supported = 0, loaded = 0;
    if (!health || !cmp_json_object_parse(health, &object) ||
        !cmp_json_object_str(&object, "status", status, sizeof(status)) ||
        !cmp_json_object_str(&object, "backend", backend, sizeof(backend)) ||
        !cmp_json_object_str(&object, "device", device, sizeof(device)) ||
        !cmp_json_object_str(&object, "model_artifacts_sha256", fingerprint, sizeof(fingerprint)) ||
        !cmp_json_object_bool(&object, "evaluation_only", &evaluation) ||
        !cmp_json_object_bool(&object, "spelling_hints_supported", &supported) ||
        !cmp_json_object_bool(&object, "loaded", &loaded) ||
        strcmp(status, "ready") || strcmp(backend, "mlx-whisper") ||
        strcmp(device, "METAL") || !evaluation || !supported || !loaded ||
        strlen(fingerprint) != 64u) return 0;
    for (size_t i = 0; i < 64u; ++i) {
        char c = fingerprint[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}
