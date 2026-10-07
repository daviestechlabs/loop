#ifndef DTL_TURN_PROVENANCE_H
#define DTL_TURN_PROVENANCE_H

#include <stdint.h>

#define DTL_PROVENANCE_INPUT_CAPACITY 131072u
#define DTL_PROVENANCE_REQUEST_CAPACITY 128u
enum { DTL_PROVENANCE_TOOL, DTL_PROVENANCE_CITATION, DTL_PROVENANCE_PROMPT, DTL_PROVENANCE_WITNESS };

uint8_t *dtl_provenance_input(void);
char *dtl_provenance_request(void);
uint32_t dtl_provenance_project(uint32_t length, uint32_t request_length);
uint32_t dtl_provenance_count(uint32_t kind);
const char *dtl_provenance_string(uint32_t kind, uint32_t index, uint32_t field);
double dtl_provenance_number(uint32_t kind, uint32_t index, uint32_t field);
void dtl_provenance_clear(void);

#endif
