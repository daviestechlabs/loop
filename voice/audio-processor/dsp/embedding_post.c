#include "embedding_post.h"

#include <math.h>
#include <stddef.h>

int embedding_cls_pool(const float* hidden, int batch, int seq, int dim,
                       int normalize, float* out)
{
    if (!hidden || !out || batch <= 0 || seq < 1 || dim <= 0)
        return -1;

    for (int b = 0; b < batch; b++) {
        const float* cls = hidden + (size_t)b * (size_t)seq * (size_t)dim;
        float* row = out + (size_t)b * (size_t)dim;
        int any = 0;
        double sum = 0.0;
        for (int d = 0; d < dim; d++) {
            float v = cls[d];
            if (!isfinite(v))
                return -1;
            if (v != 0.0f)
                any = 1;
            row[d] = v;
            if (normalize)
                sum += (double)v * (double)v;
        }
        if (!any)
            return -1;
        if (!normalize)
            continue;
        double nrm = sqrt(sum);
        if (nrm < 1e-12)
            nrm = 1e-12;
        double inv = 1.0 / nrm;
        for (int d = 0; d < dim; d++)
            row[d] = (float)((double)row[d] * inv);
    }
    return 0;
}
