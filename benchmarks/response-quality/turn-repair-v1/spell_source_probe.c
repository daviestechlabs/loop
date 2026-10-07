#include "common/dnd_source_index.h"
#include "common/dnd_source_spell.h"
#include <stdio.h>

/* Offline length-prefixed adapter. IO stays outside the parsing kernel. */
int main(void) {
    unsigned char size[4];
    char text[DND_SOURCE_PAGE_CAP];
    for (;;) {
        size_t count = fread(size, 1, sizeof(size), stdin);
        if (!count) return ferror(stdin) ? 2 : 0;
        if (count != sizeof(size)) return 2;
        size_t length = ((size_t)size[0] << 24u) | ((size_t)size[1] << 16u) |
                        ((size_t)size[2] << 8u) | (size_t)size[3];
        if (length > sizeof(text) || fread(text, 1, length, stdin) != length) return 2;
        dnd_spell_header_facts f;
        int valid = dnd_source_spell_facts(text, length, &f);
        printf("%d %u %u %u %u %zu %zu %zu %zu\n", valid,
               f.level, f.standard_cost, f.ritual_tag, f.conditional_reaction,
               f.classification_begin, f.classification_end, f.time_begin, f.time_end);
    }
}
