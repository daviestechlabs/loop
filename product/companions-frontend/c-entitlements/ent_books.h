#ifndef COMPANIONS_ENT_BOOKS_H
#define COMPANIONS_ENT_BOOKS_H
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *slug;
    const char *title;
    const char *category;
    int core;
} ent_book;

const ent_book *ent_books(size_t *count);
/* premium is server-derived. Unknown values produce no access. */
uint64_t ent_book_access_mask(int premium);
int ent_book_mask_valid(uint64_t mask);
int ent_book_allowed(uint64_t mask, const char *slug);
#endif
