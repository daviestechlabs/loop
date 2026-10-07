#include "../common/dnd_source_page.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static size_t checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void rejected(const char *page, size_t length, const dnd_source_page_chunk *chunks,
    size_t count, int expected) {
    dnd_source_page_alignment out, zero = {0};
    memset(&out, 0xa5, sizeof(out));
    CHECK(dnd_source_page_align(page, length, chunks, count, &out) == expected);
    CHECK(!memcmp(&out, &zero, sizeof(out)));
}

int main(void) {
    dnd_source_page_alignment out;
    const char *page = "Start repeated token repeated token repeated token end";
    dnd_source_page_chunk chunks[] = {
        {"Start repeated token repeated token", 35, 0},
        {"repeated token repeated token end", 33, 1}
    };
    chunks[0].length = strlen(chunks[0].text); chunks[1].length = strlen(chunks[1].text);
    CHECK(!dnd_source_page_align(page, strlen(page), chunks, 2, &out));
    CHECK(out.count == 2 && out.begin[0] == 0 && out.begin[1] == 21);
    /* Longest suffix overlap chooses byte 6 and drops one repeated phrase. */
    CHECK(!memcmp(page + 6, chunks[0].text + 6, chunks[0].length - 6u));
    CHECK(strncmp(page + 6, chunks[1].text, chunks[1].length));

    dnd_source_page_chunk repeated[] = {{"aaaa",4,0},{"aa",2,1},{"aaaa",4,2}};
    CHECK(!dnd_source_page_align("aaaaaa", 6, repeated, 3, &out));
    CHECK(out.count == 3 && out.begin[0] == 0 && out.begin[1] == 2 && out.begin[2] == 2);
    dnd_source_page_chunk ambiguous[] = {{"aaaaa",5,0},{"aaaa",4,1},{"aaa",3,2}};
    rejected("aaaaaaaa",8,ambiguous,3,DND_SOURCE_PAGE_AMBIGUOUS);

    dnd_source_page_chunk unicode[] = {{"é猫",5,0},{"猫 fin",7,1}};
    CHECK(!dnd_source_page_align("é猫 fin",9,unicode,2,&out));
    CHECK(out.count == 2 && out.begin[1] == 2);
    dnd_source_page_chunk single = {"A complete page.",16,0};
    CHECK(!dnd_source_page_align(single.text,single.length,&single,1,&out));
    CHECK(out.count == 1 && out.begin[0] == 0);
    single.chunk = 1;
    rejected(single.text,single.length,&single,1,DND_SOURCE_PAGE_INCOMPLETE);
    single.chunk = 0;
    rejected("A complete page.x",17,&single,1,DND_SOURCE_PAGE_INCOMPLETE);
    rejected("xA complete page.",17,&single,1,DND_SOURCE_PAGE_INCOMPLETE);
    rejected("A complete",10,&single,1,DND_SOURCE_PAGE_INCOMPLETE);
    rejected("Unrelated text.",15,&single,1,DND_SOURCE_PAGE_INCOMPLETE);
    repeated[1].chunk = 2;
    rejected("aaaaaa",6,repeated,3,DND_SOURCE_PAGE_INCOMPLETE);
    repeated[1].chunk = 1;
    dnd_source_page_chunk gap[] = {{"abc",3,0},{"def",3,1}};
    rejected("abc def",7,gap,2,DND_SOURCE_PAGE_INCOMPLETE);
    dnd_source_page_chunk backwards[] = {{"abcde",5,0},{"bcd",3,1},{"ef",2,2}};
    rejected("abcdef",6,backwards,3,DND_SOURCE_PAGE_INCOMPLETE);

    rejected(NULL,1,&single,1,DND_SOURCE_PAGE_INVALID);
    rejected("",0,&single,1,DND_SOURCE_PAGE_INVALID);
    rejected("a\0b",3,&single,1,DND_SOURCE_PAGE_INVALID);
    rejected("a\xff",2,&single,1,DND_SOURCE_PAGE_INVALID);
    rejected("abc",3,NULL,1,DND_SOURCE_PAGE_INVALID);
    rejected("abc",3,&single,0,DND_SOURCE_PAGE_INVALID);
    rejected("abc",3,&single,129,DND_SOURCE_PAGE_INVALID);
    single.text = NULL;
    rejected("abc",3,&single,1,DND_SOURCE_PAGE_INVALID);
    single.text = "a\0b"; single.length = 3;
    rejected("abc",3,&single,1,DND_SOURCE_PAGE_INVALID);
    single.text = "a\xff"; single.length = 2;
    rejected("abc",3,&single,1,DND_SOURCE_PAGE_INVALID);
    single.text = "a"; single.length = 0;
    rejected("abc",3,&single,1,DND_SOURCE_PAGE_INVALID);
    CHECK(dnd_source_page_align("abc",3,&single,1,NULL) == DND_SOURCE_PAGE_INVALID);

    char many[129]; memset(many,'a',sizeof(many));
    single.text = "a"; single.length = 1;
    rejected(many,sizeof(many),&single,1,DND_SOURCE_PAGE_CAPACITY);
    rejected(many,sizeof(many)-1u,&single,1,DND_SOURCE_PAGE_INCOMPLETE);
    char large[8192]; memset(large,'z',sizeof(large));
    single.text = large; single.length = sizeof(large);
    rejected(large,sizeof(large),&single,1,DND_SOURCE_PAGE_INVALID);
    single.length = sizeof(large)-1u;
    CHECK(!dnd_source_page_align(large,single.length,&single,1,&out));
    printf("dnd_source_page: %zu checks passed\n",checks);
    return 0;
}
