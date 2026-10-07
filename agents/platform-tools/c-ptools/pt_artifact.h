#ifndef PT_ARTIFACT_H
#define PT_ARTIFACT_H

#include "pt_store.h"

/* Publish one immutable output before marking its call complete. */
int pt_artifact_write(const char *directory, pt_call *call);
/* Verify retained bytes before returning an authenticated completed result. */
int pt_artifact_verify(const char *directory, const pt_call *call);
/* At startup, restrict an intact, singly linked, same-owner/group 0660 file.
 * Other invalid files remain unchanged and unavailable to authenticated reads. */
int pt_artifact_restore_private(const char *directory, const pt_call *call);

#endif
