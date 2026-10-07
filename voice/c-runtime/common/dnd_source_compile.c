#include "dnd_source_compile.h"
#include "dnd_source_spell.h"
#include "utf8.h"
#include <openssl/sha.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    char lower[DND_SOURCE_PAGE_CAP + 1u], compact[DND_SOURCE_PAGE_CAP + 1u];
    uint32_t offsets[DND_SOURCE_PAGE_CAP];
    size_t length, record_count;
    const dnd_source_record *records;
    dnd_source_compiled_page compiled;
} workspace;
typedef struct { uint32_t begin, end; char name[DND_SOURCE_NAME_CAP]; } location;

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c; }
static int letter(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }
static int hex64(const char s[65]) {
    if (s[64]) return 0;
    for (size_t i = 0; i < 64; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}
static int hash_matches(const dnd_source_record *r) {
    static const char hex[] = "0123456789abcdef";
    unsigned char digest[32];
    if (!hex64(r->record_id) || !hex64(r->content_hash) ||
        !SHA256((const unsigned char *)r->content, r->content_len, digest)) return 0;
    for (size_t i = 0; i < 32; ++i)
        if (r->content_hash[2u * i] != hex[digest[i] >> 4] ||
            r->content_hash[2u * i + 1u] != hex[digest[i] & 15]) return 0;
    return 1;
}
static size_t normalize(const char *s, size_t length, char *out, int compact) {
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = lower(s[i]);
        if (letter(c) || (!compact && c >= '0' && c <= '9')) out[used++] = c;
        else if (!compact && used && out[used - 1u] != ' ') out[used++] = ' ';
    }
    if (used && out[used - 1u] == ' ') --used;
    out[used] = 0;
    return used;
}
static int cover(const workspace *w, size_t begin, size_t end, size_t maximum,
    dnd_source_group *out) {
    dnd_source_group g = {0};
    if (begin >= end || end > w->length || end - begin > 1024u) return 0;
    size_t cursor = begin, previous = w->record_count;
    while (cursor < end) {
        size_t best = w->record_count, best_end = cursor;
        for (size_t i = 0; i < w->record_count; ++i) {
            size_t at = w->compiled.alignment.begin[i], stop = at + w->records[i].content_len;
            if (previous != w->record_count && i != previous + 1u) continue;
            if (at <= cursor && stop > best_end) { best = i; best_end = stop; }
        }
        if (best == w->record_count || g.count == maximum) return 0;
        size_t stop = best_end < end ? best_end : end;
        g.spans[g.count++] = (dnd_source_span){(uint32_t)best, (uint32_t)cursor, (uint32_t)stop};
        cursor = stop; previous = best;
    }
    g.begin = (uint32_t)begin; g.end = (uint32_t)end;
    *out = g;
    return 1;
}
static int title_shape(const workspace *w, size_t begin, size_t end) {
    size_t letters = 0, caps = 0, singles = 0, words = 0;
    if ((begin && letter(w->text[begin - 1u])) || (end < w->length && letter(w->text[end]))) return 0;
    for (size_t i = begin; i < end;) {
        if (!letter(w->text[i])) { ++i; continue; }
        size_t n = 0;
        while (i < end && letter(w->text[i])) {
            ++letters; ++n; caps += w->text[i] >= 'A' && w->text[i] <= 'Z'; ++i;
        }
        ++words; singles += n == 1u;
    }
    return letters >= 4u && (caps * 4u >= letters * 3u || (caps && singles * 4u >= words * 3u));
}
static int modal(const workspace *w, size_t begin, size_t end) {
    static const char *const words[] = {"can", "must", "cannot"};
    for (size_t i = begin; i < end; ++i) {
        if (i > begin && letter(w->lower[i - 1u])) continue;
        for (size_t j = 0; j < sizeof(words) / sizeof(words[0]); ++j) {
            size_t n = strlen(words[j]);
            if (n <= end - i && !memcmp(w->lower + i, words[j], n) &&
                (i + n == end || !letter(w->lower[i + n]))) return 1;
        }
    }
    return 0;
}
static uint32_t flags(const workspace *w, size_t begin, size_t end) {
    char normalized[1025], compact[1025];
    uint32_t result = 0;
    if (dnd_source_spell_header(w->text + begin, end - begin)) result |= DND_SOURCE_SPELL_HEADER;
    normalize(w->text + begin, end - begin, normalized, 0);
    normalize(w->text + begin, end - begin, compact, 1);
    if (!strncmp(normalized, "once per turn ", 14)) result |= DND_SOURCE_ONCE_PER_TURN;
    if (strstr(compact, "reaction") && strstr(compact, "canoccur") && strstr(compact, "yourturn") &&
        (strstr(compact, "someoneelse") || strstr(compact, "anothercreaturesturn")))
        result |= DND_SOURCE_OTHER_TURN_REACTION;
    return result;
}
static int sentence_end(const workspace *w, size_t at, size_t limit) {
    char c = w->text[at];
    return (c == '.' || c == '!' || c == '?') && (at + 1u == limit || w->text[at + 1u] == ' ');
}

static int section(workspace *w, const dnd_source_section_hint *h, uint32_t document, uint32_t page) {
    dnd_source_entry e = {.document = document, .page = page, .kind = DND_SOURCE_SECTION};
    if (h->parent_begin >= h->parent_end || h->parent_end >= h->heading_begin ||
        h->heading_begin >= h->heading_end || h->heading_end >= h->body_end || h->body_end > w->length ||
        h->parent_end - h->parent_begin >= DND_SOURCE_NAME_CAP ||
        h->heading_end - h->heading_begin >= DND_SOURCE_NAME_CAP ||
        !title_shape(w, h->parent_begin, h->parent_end) ||
        w->text[h->heading_begin] < 'A' || w->text[h->heading_begin] > 'Z' ||
        (h->heading_begin && w->text[h->heading_begin - 1u] != ' ') ||
        w->text[h->heading_end - 1u] != '.' || w->text[h->heading_end] != ' ' ||
        !sentence_end(w, h->body_end - 1u, w->length) ||
        !utf8_validate_v1((const uint8_t *)w->text + h->parent_begin, h->parent_end - h->parent_begin) ||
        !utf8_validate_v1((const uint8_t *)w->text + h->heading_begin, h->body_end - h->heading_begin))
        return DND_SOURCE_PAGE_INVALID;
    size_t begin = h->heading_end;
    while (begin < h->body_end && w->text[begin] == ' ') ++begin;
    if (!normalize(w->text + h->heading_begin, h->heading_end - h->heading_begin, e.name, 1) ||
        !normalize(w->text + h->parent_begin, h->parent_end - h->parent_begin, e.alias, 1) ||
        !cover(w, h->parent_begin, h->parent_end, 2, &e.fields[0]) ||
        !cover(w, h->heading_begin, h->heading_end, 2, &e.heading) ||
        !cover(w, begin, h->body_end, 2, &e.body)) return DND_SOURCE_PAGE_INVALID;
    size_t end = begin;
    while (end < h->body_end && !sentence_end(w, end, h->body_end)) ++end;
    if (end == h->body_end || !cover(w, begin, end + 1u, 2, &e.opening)) return DND_SOURCE_PAGE_INVALID;
    e.flags = flags(w, begin, end + 1u);
    if (w->compiled.count == DND_SOURCE_HEADINGS_MAX) return DND_SOURCE_PAGE_CAPACITY;
    w->compiled.entries[w->compiled.count++] = e;
    return DND_SOURCE_PAGE_OK;
}
static int rules(workspace *w, const dnd_source_heading *headings, size_t count,
    uint32_t document, uint32_t page) {
    location locations[DND_SOURCE_HEADINGS_MAX];
    size_t located = 0, compact_length = 0;
    for (size_t i = 0; i < w->length; ++i) if (letter(w->text[i])) {
        w->offsets[compact_length] = (uint32_t)i;
        w->compact[compact_length++] = w->lower[i];
    }
    w->compact[compact_length] = 0;
    for (size_t i = 0; i < count; ++i) {
        char name[DND_SOURCE_NAME_CAP];
        size_t n = normalize(headings[i].text, strlen(headings[i].text), name, 1);
        if (n < 4u) continue;
        const char *p = w->compact;
        while ((p = strstr(p, name))) {
            size_t at = (size_t)(p++ - w->compact);
            uint32_t begin = w->offsets[at], end = w->offsets[at + n - 1u] + 1u;
            if (!title_shape(w, begin, end)) continue;
            size_t j = 0;
            for (; j < located; ++j) if (locations[j].begin == begin && locations[j].end == end) break;
            if (j < located) continue;
            if (located == DND_SOURCE_HEADINGS_MAX) return DND_SOURCE_PAGE_CAPACITY;
            locations[located] = (location){.begin = begin, .end = end};
            memcpy(locations[located++].name, name, n + 1u);
        }
    }
    for (size_t i = 1; i < located; ++i) {
        location value = locations[i]; size_t j = i;
        while (j && (locations[j - 1u].begin > value.begin ||
            (locations[j - 1u].begin == value.begin && locations[j - 1u].end < value.end))) {
            locations[j] = locations[j - 1u]; --j;
        }
        locations[j] = value;
    }
    size_t previous_end = 0;
    for (size_t i = 0; i < located; ++i) {
        const location *h = &locations[i];
        if (h->begin < previous_end) continue;
        previous_end = h->end;
        size_t limit = w->length;
        for (size_t j = i + 1u; j < located; ++j)
            if (locations[j].begin >= h->end) { limit = locations[j].begin; break; }
        size_t begin = h->end;
        while (begin < limit && w->text[begin] == ' ') ++begin;
        int spell = dnd_source_spell_header(w->text + begin, limit - begin);
        for (size_t end = begin; end < limit; ++end) {
            if (!sentence_end(w, end, limit)) continue;
            if (end + 1u - begin <= 1024u && (spell || modal(w, begin, end + 1u))) {
                dnd_source_entry e = {.document = document, .page = page, .kind = DND_SOURCE_RULE};
                memcpy(e.name, h->name, sizeof(e.name));
                if (cover(w, h->begin, h->end, 2, &e.heading) && cover(w, begin, end + 1u, 2, &e.opening)) {
                    e.flags = flags(w, begin, end + 1u);
                    size_t tail = end + 1u;
                    for (size_t j = tail; j < limit; ++j) if (sentence_end(w, j, limit)) tail = j + 1u;
                    size_t trimmed = limit;
                    while (trimmed > begin && w->text[trimmed - 1u] == ' ') --trimmed;
                    /* A page edge cannot establish a complete spell. A later
                     * heading and complete final sentence must bound the body. */
                    if (!spell || (limit < w->length && tail == trimmed))
                        (void)cover(w, begin, tail, 2, &e.body);
                    w->compiled.entries[w->compiled.count++] = e;
                }
                break;
            }
            if (spell) break;
            begin = end + 1u;
            while (begin < limit && w->text[begin] == ' ') ++begin;
        }
    }
    return DND_SOURCE_PAGE_OK;
}
static size_t number_end(const workspace *w, size_t at, size_t limit) {
    size_t start = at;
    while (at < limit && w->text[at] >= '0' && w->text[at] <= '9') ++at;
    if (at == start || at - start > 4u) return 0;
    if (at < limit && w->text[at] == '/') {
        start = ++at;
        while (at < limit && w->text[at] >= '0' && w->text[at] <= '9') ++at;
        if (at == start || at - start > 2u) return 0;
    }
    return at < limit && w->text[at] != ' ' && w->text[at] != '(' && w->text[at] != '.' ? 0 : at;
}
static size_t number_field(const workspace *w, const char *marker, size_t begin, size_t limit, size_t *end) {
    const char *p = w->lower + begin;
    size_t n = strlen(marker);
    while ((p = strstr(p, marker))) {
        size_t at = (size_t)(p++ - w->lower);
        if (at + n >= limit) break;
        if (at && letter(w->lower[at - 1u])) continue;
        *end = number_end(w, at + n, limit);
        if (*end) return at;
    }
    *end = 0; return w->length;
}
static int uppercase_before(const workspace *w, size_t at) {
    size_t letters = 0, caps = 0;
    while (at && w->text[at - 1u] == ' ') --at;
    while (at && letter(w->text[at - 1u])) {
        --at; ++letters; caps += w->text[at] >= 'A' && w->text[at] <= 'Z';
    }
    return letters >= 2u && caps == letters;
}
static int type_follows(const workspace *w, size_t at) {
    static const char *const sizes[] = {"tiny ", "small ", "medium ", "large ", "huge ", "gargantuan "};
    static const char *const types[] = {"aberration", "beast", "celestial", "construct", "dragon", "elemental", "fey", "fiend", "giant", "humanoid", "monstrosity", "ooze", "plant", "undead"};
    while (at < w->length && w->text[at] == ' ') ++at;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        size_t n = strlen(sizes[i]);
        if (n > w->length - at || memcmp(w->lower + at, sizes[i], n)) continue;
        for (size_t j = 0; j < sizeof(types) / sizeof(types[0]); ++j) {
            size_t m = strlen(types[j]);
            if (m <= w->length - at - n && !memcmp(w->lower + at + n, types[j], m) &&
                !letter(w->lower[at + n + m])) return 1;
        }
    }
    return 0;
}
static int repeated_name(const workspace *w, const char *key, size_t end, int one_block) {
    size_t n = strlen(key), count = 0, letters = 0, caps = 0;
    if (!one_block || !n || strchr(key, ' ') || key[n - 1u] == 's') return 0;
    while (end < w->length && w->text[end] == ' ') ++end;
    while (end < w->length && letter(w->text[end])) {
        ++letters; caps += w->text[end] >= 'A' && w->text[end] <= 'Z'; ++end;
    }
    if (letters >= 2u && caps == letters) return 0;
    const char *p = w->lower;
    while ((p = strstr(p, key))) {
        if ((p == w->lower || !letter(p[-1])) && !letter(p[n])) ++count;
        p += n;
    }
    return count >= 3u;
}
static void creature(workspace *w, const dnd_source_heading *heading, uint32_t document, uint32_t page) {
    static const char *const markers[] = {"armor class ", "hit points ", "speed ", "challenge ", "damage immunities "};
    dnd_source_entry e = {.document = document, .page = page, .kind = DND_SOURCE_CREATURE};
    size_t ac_end, next_end, used = 0;
    if (!normalize(heading->text, strlen(heading->text), e.name, 0)) return;
    for (size_t i = 0; e.name[i]; ++i)
        if (!i || e.name[i] != e.name[i - 1u] || !letter(e.name[i])) e.alias[used++] = e.name[i];
    e.alias[used] = 0;
    size_t ac = number_field(w, markers[0], 0, w->length, &ac_end);
    const char *name = strstr(w->lower, e.name);
    if (!ac_end || !name) return;
    size_t at = (size_t)(name - w->lower), end = at + strlen(e.name);
    size_t next_ac = number_field(w, markers[0], ac_end, w->length, &next_end);
    if (at > 256u || at >= ac || (at && letter(w->text[at - 1u])) || letter(w->lower[end]) ||
        uppercase_before(w, at) || (!type_follows(w, end) && !repeated_name(w, e.alias, end, next_ac == w->length)) ||
        !cover(w, at, end, 1, &e.heading) || !cover(w, ac, ac_end, 1, &e.fields[0])) return;
    for (size_t i = 1; i < 4; ++i) {
        size_t stop, start = number_field(w, markers[i], ac_end, next_ac, &stop);
        if (!stop || !cover(w, start, stop, 1, &e.fields[i])) return;
    }
    if (e.fields[1].begin - ac > 512u) return;
    const char *immunity = strstr(w->lower + ac_end, markers[4]);
    if (immunity && (size_t)(immunity - w->lower) < next_ac) {
        static const char *const ends[] = {"senses ", "languages ", "challenge ", "condition immunities ", "damage resistances ", "damage vulnerabilities "};
        size_t start = (size_t)(immunity - w->lower), stop = next_ac, value = start + strlen(markers[4]);
        for (size_t i = 0; i < sizeof(ends) / sizeof(ends[0]); ++i) {
            const char *p = strstr(w->lower + value, ends[i]);
            if (p && (size_t)(p - w->lower) < stop) stop = (size_t)(p - w->lower);
        }
        while (stop > value && w->text[stop - 1u] == ' ') --stop;
        if (stop > value && stop - start <= 512u) (void)cover(w, start, stop, 1, &e.fields[4]);
    }
    w->compiled.entries[w->compiled.count++] = e;
}

int dnd_source_compile_page_sections(const char *text, size_t length,
    uint32_t document, uint32_t page, uint32_t kind,
    const dnd_source_record *records, size_t record_count,
    const dnd_source_heading *headings, size_t heading_count,
    const dnd_source_section_hint *sections, size_t section_count,
    dnd_source_compiled_page *out) {
    if (!out) return DND_SOURCE_PAGE_INVALID;
    memset(out, 0, sizeof(*out));
    if (!records || !record_count || record_count > DND_SOURCE_PAGE_CHUNKS_MAX ||
        document >= DND_SOURCE_DOCUMENTS_MAX || !page || page > INT32_MAX ||
        (kind != DND_SOURCE_RULE && kind != DND_SOURCE_CREATURE) ||
        heading_count > DND_SOURCE_HEADINGS_MAX || (heading_count && !headings) ||
        section_count > DND_SOURCE_HEADINGS_MAX || (section_count && (!sections || kind != DND_SOURCE_RULE)))
        return DND_SOURCE_PAGE_INVALID;
    for (size_t i = 0; i < section_count; ++i)
        for (size_t j = 0; j < i; ++j)
            if (sections[i].heading_begin == sections[j].heading_begin) return DND_SOURCE_PAGE_INVALID;
    dnd_source_page_chunk chunks[DND_SOURCE_PAGE_CHUNKS_MAX];
    for (size_t i = 0; i < record_count; ++i) {
        if (records[i].document != document || records[i].page != page) return DND_SOURCE_PAGE_INVALID;
        chunks[i] = (dnd_source_page_chunk){records[i].content, records[i].content_len, records[i].chunk};
    }
    for (size_t i = 0; i < heading_count; ++i) {
        if (!memchr(headings[i].text, 0, sizeof(headings[i].text))) return DND_SOURCE_PAGE_INVALID;
        for (size_t j = 0; headings[i].text[j]; ++j)
            if ((unsigned char)headings[i].text[j] < 32u || (unsigned char)headings[i].text[j] > 126u) return DND_SOURCE_PAGE_INVALID;
    }
    workspace *w = calloc(1, sizeof(*w));
    if (!w) return DND_SOURCE_PAGE_CAPACITY;
    int result = dnd_source_page_align(text, length, chunks, record_count, &w->compiled.alignment);
    if (result) goto done;
    for (size_t i = 0; i < record_count; ++i) {
        if (!hash_matches(&records[i])) { result = DND_SOURCE_PAGE_INVALID; goto done; }
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(records[j].record_id, records[i].record_id)) { result = DND_SOURCE_PAGE_INVALID; goto done; }
    }
    w->text = text; w->length = length; w->records = records; w->record_count = record_count;
    for (size_t i = 0; i < length; ++i) w->lower[i] = lower(text[i]);
    if (kind == DND_SOURCE_RULE) result = rules(w, headings, heading_count, document, page);
    else if (heading_count) creature(w, &headings[0], document, page);
    for (size_t i = 0; !result && i < section_count; ++i)
        result = section(w, &sections[i], document, page);
    if (!result) *out = w->compiled;
done:
    free(w);
    return result;
}

int dnd_source_compile_page(const char *text, size_t length,
    uint32_t document, uint32_t page, uint32_t kind,
    const dnd_source_record *records, size_t record_count,
    const dnd_source_heading *headings, size_t heading_count,
    dnd_source_compiled_page *out) {
    return dnd_source_compile_page_sections(text, length, document, page, kind,
        records, record_count, headings, heading_count, NULL, 0, out);
}
