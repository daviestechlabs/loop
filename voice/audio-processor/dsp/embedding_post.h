#ifndef EMBEDDING_POST_H
#define EMBEDDING_POST_H

/* CLS token select, finite/nonzero check, optional L2.
 * `hidden` is row-major [batch, seq, dim]. `out` is [batch, dim].
 * Caller owns both spans. Returns 0 on success, -1 on a bad span,
 * a nonfinite CLS row, or an all-zero CLS row. */
int embedding_cls_pool(const float* hidden, int batch, int seq, int dim,
                       int normalize, float* out);

#endif
