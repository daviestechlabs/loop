#include "dnd_source_passage.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static dnd_source_record records[32];
static dnd_source_passage_page pages[3];
static dnd_source_passage_input input;
static dnd_source_passage output;
static size_t checks;
static const char tail[] = "LAMPLIGHT Evocation cantrip Casting Time: 1 action Range: Self Components: V Duration: 1 minute A light appears.";
static char first[8192], second[8192], third[8192];

static void hash(const char *text, size_t length, char out[65]) {
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[32];
    CHECK(SHA256((const unsigned char *)text, length, bytes) != NULL);
    for (size_t i = 0; i < 32u; ++i) { out[i * 2u] = hex[bytes[i] >> 4]; out[i * 2u + 1u] = hex[bytes[i] & 15]; }
    out[64] = 0;
}
static void add_page(char *text, uint32_t page, const uint32_t (*cuts)[2], size_t count) {
    CHECK(input.page_count < 3 && input.record_count + count <= 32);
    dnd_source_passage_page *p = &pages[input.page_count++];
    p->text = text; p->length = strlen(text); p->page = page; p->count = count;
    for (size_t i = 0; i < count; ++i) {
        size_t index = input.record_count++;
        dnd_source_record *r = &records[index];
        p->records[i] = (uint32_t)index;
        r->document = 2; r->page = page; r->chunk = (uint32_t)i; r->begin = cuts[i][0];
        r->content = text + cuts[i][0]; r->content_len = cuts[i][1] - cuts[i][0];
        CHECK(snprintf(r->record_id, sizeof(r->record_id), "%064zu", index + 1u) == 64);
        hash(r->content, r->content_len, r->content_hash);
    }
}
static void reset(int cross_page) {
    memset(records, 0, sizeof(records)); memset(pages, 0, sizeof(pages));
    input = (dnd_source_passage_input){.document = 2, .records = records, .pages = pages, .heading_end = 5};
    CHECK(snprintf(first, sizeof(first), "AEGIS 8 th-level abjuration Casting Time: 1 action Range: Self Components: V Duration: 1 round A shell surrounds you.%s",
        cross_page ? " The target" : " It lasts. ") > 0);
    if (cross_page) {
        CHECK(snprintf(second, sizeof(second), " keeps the ward. %s", tail) > 0);
        const uint32_t a[][2] = {{0, 89}, {70, (uint32_t)strlen(first)}};
        const uint32_t b[][2] = {{0, (uint32_t)strlen(second)}};
        add_page(first, 12, a, 2); add_page(second, 13, b, 1);
        input.next_heading_begin = 17; input.next_heading_end = 26;
    } else {
        size_t length = strlen(first); CHECK(length + strlen(tail) < sizeof(first)); strcat(first, tail);
        const uint32_t a[][2] = {{0, 89}, {70, (uint32_t)strlen(first)}};
        add_page(first, 12, a, 2);
        input.next_heading_begin = (uint32_t)length; input.next_heading_end = (uint32_t)length + 9u;
    }
}
static void expect(int status) {
    memset(&output, 0xa5, sizeof(output));
    CHECK(dnd_source_compile_passage(&input, &output) == status);
    if (status) {
        const unsigned char *bytes = (const unsigned char *)&output;
        for (size_t i = 0; i < sizeof(output); ++i) CHECK(bytes[i] == 0);
    }
    ++checks;
}
static void witness_matches(void) {
    char joined[DND_SOURCE_PASSAGE_TEXT_CAP] = {0}, digest[65];
    size_t used = 0; uint32_t previous_page = 0;
    for (size_t i = 0; i < output.count; ++i) {
        const dnd_source_passage_span *s = &output.spans[i];
        CHECK(s->record < input.record_count && s->begin < s->end);
        const dnd_source_record *r = &records[s->record];
        CHECK(s->end <= r->content_len);
        if (i && previous_page != r->page) joined[used++] = '\n';
        memcpy(joined + used, r->content + s->begin, s->end - s->begin);
        used += s->end - s->begin; previous_page = r->page;
    }
    CHECK(used == output.length && !memcmp(joined, output.text, used));
    hash(joined, used, digest); CHECK(!strcmp(digest, output.text_sha256)); ++checks;
}
int main(void) {
    reset(0); expect(0); witness_matches();
    CHECK(!strcmp(output.name, "aegis") && output.page_start == 12 && output.page_end == 12 && output.count == 2);
    CHECK(strstr(output.text, "8 th-level") && !strstr(output.text, "LAMPLIGHT"));
    reset(1); expect(0); witness_matches(); CHECK(output.count == 3 && output.page_end == 13);
    CHECK(strstr(output.text, "The target\n keeps the ward.") != NULL);
    reset(1); records[0].content_hash[0] = records[0].content_hash[0] == 'a' ? 'b' : 'a'; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); records[2].document = 1; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); records[0].begin = 1; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); pages[1].page = 14; records[2].page = 14; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); memcpy(records[1].record_id, records[0].record_id, 65); expect(DND_SOURCE_PAGE_INVALID);
    reset(1); pages[0].records[0] = 31; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); input.heading_begin = 1; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); input.next_heading_end -= 1; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); input.page_count = 4; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); records[0].chunk = 1; expect(DND_SOURCE_PAGE_INVALID);
    reset(1); first[6] = '0'; hash(records[0].content, records[0].content_len, records[0].content_hash); expect(DND_SOURCE_PAGE_INCOMPLETE);
    reset(1); second[15] = ','; hash(records[2].content, records[2].content_len, records[2].content_hash); expect(DND_SOURCE_PAGE_INCOMPLETE);
    reset(1); second[strlen(second) - 1u] = '!'; expect(DND_SOURCE_PAGE_INVALID); /* excluded bytes still hashed */
    reset(0);
    size_t old = strlen(first); CHECK(old + strlen(tail) + 1u < sizeof(first)); strcat(first, " "); strcat(first, tail);
    pages[0].length = strlen(first); records[1].content_len = pages[0].length - records[1].begin;
    hash(records[1].content, records[1].content_len, records[1].content_hash);
    input.next_heading_begin = (uint32_t)old + 1u; input.next_heading_end = input.next_heading_begin + 9u;
    expect(DND_SOURCE_PAGE_AMBIGUOUS);
    reset(1);
    strcpy(second, tail); pages[1].length = strlen(second); records[2].content_len = pages[1].length;
    hash(records[2].content, records[2].content_len, records[2].content_hash);
    input.next_heading_begin = 0; input.next_heading_end = 9;
    expect(DND_SOURCE_PAGE_INCOMPLETE); /* previous page ends with an unfinished clause */
    reset(1);
    strcpy(first, "AEGIS 8 th-level abjuration Casting Time: 1 action Range: Self Components: V Duration: 1 round A ward remains.");
    pages[0].length = strlen(first); records[1].content_len = pages[0].length - records[1].begin;
    hash(records[0].content, records[0].content_len, records[0].content_hash); hash(records[1].content, records[1].content_len, records[1].content_hash);
    strcpy(second, tail); pages[1].length = strlen(second); records[2].content_len = pages[1].length;
    hash(records[2].content, records[2].content_len, records[2].content_hash);
    input.next_heading_begin = 0; input.next_heading_end = 9;
    expect(0); witness_matches(); CHECK(output.page_end == 12);
    reset(1);
    strcpy(third, second); strcpy(second, " continues into another page.");
    pages[1].length = strlen(second); records[2].content_len = pages[1].length;
    hash(records[2].content, records[2].content_len, records[2].content_hash);
    const uint32_t cut[][2] = {{0, (uint32_t)strlen(third)}};
    add_page(third, 14, cut, 1);
    expect(0); witness_matches(); CHECK(output.page_end == 14 && output.count == 4);
    for (size_t count = 16; count <= 17; ++count) {
        reset(0);
        strcpy(first, "AEGIS 8 th-level abjuration Casting Time: 1 action Range: Self Components: V Duration: 1 round A ward remains.");
        for (size_t i = 0; i < 40; ++i) {
            size_t length = strlen(first);
            CHECK(snprintf(first + length, sizeof(first) - length, " Mark%02zu remains.", i) > 0);
        }
        size_t end = strlen(first); strcat(first, " "); strcat(first, tail);
        memset(records, 0, sizeof(records)); memset(pages, 0, sizeof(pages));
        input.record_count = 0; input.page_count = 0;
        uint32_t cuts[17][2] = {{0}};
        size_t step = (end - 1u) / (count - 1u);
        for (size_t i = 0; i < count; ++i) {
            cuts[i][0] = (uint32_t)(i * step);
            cuts[i][1] = (uint32_t)(i + 1u == count ? strlen(first) : (i + 1u) * step);
        }
        add_page(first, 12, cuts, count);
        input.next_heading_begin = (uint32_t)end + 1u; input.next_heading_end = input.next_heading_begin + 9u;
        expect(count == 16u ? DND_SOURCE_PAGE_OK : DND_SOURCE_PAGE_CAPACITY);
        if (count == 16u) { witness_matches(); CHECK(output.count == 16); }
    }
    reset(0);
    strcpy(first, "AEGIS 8 th-level abjuration Casting Time: 1 action Range: Self Components: V Duration: 1 round A ward remains.");
    size_t prefix = strlen(first); memset(first + prefix, 'x', 4200u); first[prefix + 4200u] = '.';
    first[prefix + 4201u] = ' '; strcpy(first + prefix + 4202u, tail);
    pages[0].length = strlen(first); records[1].content_len = pages[0].length - records[1].begin;
    hash(records[0].content, records[0].content_len, records[0].content_hash); hash(records[1].content, records[1].content_len, records[1].content_hash);
    input.next_heading_begin = (uint32_t)prefix + 4202u; input.next_heading_end = input.next_heading_begin + 9u;
    expect(DND_SOURCE_PAGE_CAPACITY);
    reset(1); first[100] = (char)0xff; hash(records[1].content, records[1].content_len, records[1].content_hash);
    expect(DND_SOURCE_PAGE_INVALID);
    CHECK(dnd_source_compile_passage(&input, NULL) == DND_SOURCE_PAGE_INVALID);
    CHECK(dnd_source_compile_passage(NULL, &output) == DND_SOURCE_PAGE_INVALID);
    printf("OK %zu passage compiler checks\n", checks);
    return 0;
}
