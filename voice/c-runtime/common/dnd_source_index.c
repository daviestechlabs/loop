#include "dnd_source_index.h"
#include "dnd_source_select_internal.h"
#include "dnd_source_spell.h"
#include "ent_books.h"
#include "utf8.h"
#include <openssl/sha.h>
#include <string.h>

static size_t string_length(const char *s, size_t capacity) {
    for (size_t i = 0; i < capacity; ++i) if (!s[i]) return i;
    return capacity;
}

static int hash_valid(const char *s) {
    if (string_length(s, 65) != 64) return 0;
    for (size_t i = 0; i < 64; ++i)
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
    return 1;
}

static int identifier(const char *s, size_t capacity) {
    size_t n = string_length(s, capacity);
    if (!n || n == capacity) return 0;
    for (size_t i = 0; i < n; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z') ||
            (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_' || s[i] == '.')) return 0;
    return 1;
}

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 'a' - 'A') : c; }
static int letter(char c) { c = lower(c); return c >= 'a' && c <= 'z'; }

static int normalize(const char *s, size_t length, char *out, size_t capacity, int compact) {
    size_t used = 0;
    for (size_t i = 0; i < length; ++i) {
        char c = lower(s[i]);
        if (letter(c) || (!compact && c >= '0' && c <= '9')) {
            if (used + 1 >= capacity) return -1;
            out[used++] = c;
        } else if (!compact && used && out[used - 1] != ' ') {
            if (used + 1 >= capacity) return -1;
            out[used++] = ' ';
        }
    }
    if (used && out[used - 1] == ' ') --used;
    out[used] = '\0';
    return used ? 0 : -1;
}

static int phrase(const char *text, const char *key) {
    size_t length = strlen(key);
    if (!length) return 0;
    const char *p = text;
    while ((p = strstr(p, key)) != NULL) {
        if ((p == text || p[-1] == ' ') && (!p[length] || p[length] == ' ')) return 1;
        ++p;
    }
    return 0;
}

static int record_valid(const dnd_source_data *d, size_t i) {
    const dnd_source_record *r = &d->records[i];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    static const char hex[] = "0123456789abcdef";
    if (r->document >= d->document_count || !r->page || r->page > INT32_MAX ||
        r->chunk > INT32_MAX || r->begin >= DND_SOURCE_PAGE_CAP || !r->content ||
        !r->content_len || r->content_len >= DND_RAG_CONTENT_CAP ||
        r->content_len > DND_SOURCE_PAGE_CAP - r->begin ||
        !hash_valid(r->record_id) || !hash_valid(r->content_hash) ||
        (i && strcmp(d->records[i - 1].record_id, r->record_id) >= 0) ||
        memchr(r->content, '\0', r->content_len) ||
        !utf8_validate_v1((const uint8_t *)r->content, r->content_len) ||
        !SHA256((const unsigned char *)r->content, r->content_len, digest)) return 0;
    for (size_t j = 0; j < sizeof(digest); ++j)
        if (r->content_hash[j * 2] != hex[digest[j] >> 4] ||
            r->content_hash[j * 2 + 1] != hex[digest[j] & 15]) return 0;
    return 1;
}

static int group_valid(const dnd_source_data *d, const dnd_source_entry *e,
    const dnd_source_group *g, char *text, size_t capacity) {
    size_t used = 0;
    uint32_t cursor = g->begin;
    if (g->count > DND_SOURCE_SPANS_MAX) return 0;
    if (!g->count) {
        if (g->begin || g->end) return 0;
    } else if (g->begin >= g->end || g->end > DND_SOURCE_PAGE_CAP || g->end - g->begin >= capacity) return 0;
    for (size_t i = 0; i < DND_SOURCE_SPANS_MAX; ++i) {
        const dnd_source_span *s = &g->spans[i];
        if (i >= g->count) {
            if (s->record || s->begin || s->end) return 0;
            continue;
        }
        if (s->record >= d->record_count || s->begin != cursor || s->begin >= s->end || s->end > g->end) return 0;
        const dnd_source_record *r = &d->records[s->record];
        if (r->document != e->document || r->page != e->page || s->begin < r->begin ||
            s->end - r->begin > r->content_len) return 0;
        if (i) {
            const dnd_source_record *previous = &d->records[g->spans[i - 1].record];
            size_t previous_end = previous->begin + previous->content_len;
            if (r->chunk != previous->chunk + 1u || r->begin < previous->begin || r->begin > previous_end) return 0;
            size_t overlap = previous_end - r->begin;
            if (overlap > r->content_len || memcmp(previous->content + r->begin - previous->begin,
                r->content, overlap)) return 0;
        }
        size_t length = s->end - s->begin;
        memcpy(text + used, r->content + s->begin - r->begin, length);
        used += length;
        cursor = s->end;
    }
    text[used] = '\0';
    return cursor == g->end && (!used || utf8_validate_v1((const uint8_t *)text, used));
}

static uint32_t source_flags(const char *text) {
    char normalized[2048], compact[1025];
    uint32_t flags = 0;
    size_t n = strlen(text);
    if (dnd_source_spell_header(text, n)) flags |= DND_SOURCE_SPELL_HEADER;
    if (normalize(text, n, normalized, sizeof(normalized), 0) ||
        normalize(text, n, compact, sizeof(compact), 1)) return 0;
    if (!strncmp(normalized, "once per turn ", 14)) flags |= DND_SOURCE_ONCE_PER_TURN;
    if (strstr(compact, "reaction") && strstr(compact, "canoccur") && strstr(compact, "yourturn") &&
        (strstr(compact, "someoneelse") || strstr(compact, "anothercreaturesturn"))) flags |= DND_SOURCE_OTHER_TURN_REACTION;
    return flags;
}

static int sentence_ends(const char *text) {
    size_t n = strlen(text);
    return n && (text[n - 1] == '.' || text[n - 1] == '!' || text[n - 1] == '?');
}

static int flags_match(uint32_t declared, const char *text) {
    uint32_t derived = source_flags(text);
    /* Older pinned artifacts omit the spell marker. They retain their old
     * selection behavior; only a newly declared marker enables that path. */
    return (declared & ~(uint32_t)DND_SOURCE_SPELL_HEADER) ==
        (derived & ~(uint32_t)DND_SOURCE_SPELL_HEADER) &&
        (!(declared & DND_SOURCE_SPELL_HEADER) || (derived & DND_SOURCE_SPELL_HEADER));
}

static int field_text_valid(const char *text, size_t field) {
    static const char *const prefixes[] = {"armor class ", "hit points ", "speed ", "challenge ", "damage immunities "};
    size_t prefix = strlen(prefixes[field]);
    if (strlen(text) <= prefix) return 0;
    for (size_t i = 0; i < prefix; ++i) if (lower(text[i]) != prefixes[field][i]) return 0;
    if (field == 4) return 1;
    /* The compiled numeric spans stop at the number, before prose or units.
     * Inspect original bytes so a slash remains distinguishable from a space. */
    const char *number = text + prefix;
    size_t digits = 0;
    while (*number >= '0' && *number <= '9') { ++number; ++digits; }
    if (!digits || digits > 4) return 0;
    if (*number == '/') {
        ++number; digits = 0;
        while (*number >= '0' && *number <= '9') { ++number; ++digits; }
        if (!digits || digits > 2) return 0;
    }
    return !*number;
}

static int entry_valid(const dnd_source_data *d, const dnd_source_entry *e) {
    char text[1025], name[DND_SOURCE_NAME_CAP], alias[DND_SOURCE_NAME_CAP];
    if (e->document >= d->document_count || !e->page || e->page > INT32_MAX ||
        (e->kind != DND_SOURCE_RULE && e->kind != DND_SOURCE_CREATURE && e->kind != DND_SOURCE_SECTION) ||
        string_length(e->name, sizeof(e->name)) == sizeof(e->name) ||
        string_length(e->alias, sizeof(e->alias)) == sizeof(e->alias) ||
        !e->heading.count || !group_valid(d, e, &e->heading, text, sizeof(text)) ||
        normalize(text, strlen(text), name, sizeof(name), e->kind != DND_SOURCE_CREATURE) || strcmp(name, e->name)) return 0;
    if (e->kind == DND_SOURCE_SECTION && !sentence_ends(text)) return 0;
    size_t used = 0;
    for (size_t i = 0; name[i]; ++i)
        if (!i || name[i] != name[i - 1] || !letter(name[i])) alias[used++] = name[i];
    alias[used] = '\0';
    if (e->kind != DND_SOURCE_CREATURE) {
        if ((e->kind == DND_SOURCE_RULE && e->alias[0]) || !e->opening.count ||
            (e->kind == DND_SOURCE_SECTION && !e->body.count) ||
            !group_valid(d, e, &e->opening, text, sizeof(text)) || !sentence_ends(text) ||
            e->heading.end > e->opening.begin || !flags_match(e->flags, text) ||
            !group_valid(d, e, &e->body, text, sizeof(text)) ||
            (e->body.count && (!sentence_ends(text) || e->body.begin != e->opening.begin || e->body.end < e->opening.end))) return 0;
    } else if ((e->alias[0] && strcmp(alias, e->alias)) || e->flags || e->body.count || e->opening.count ||
        !group_valid(d, e, &e->body, text, sizeof(text)) || !group_valid(d, e, &e->opening, text, sizeof(text))) return 0;
    for (size_t i = 0; i < DND_SOURCE_FIELDS; ++i) {
        if (!group_valid(d, e, &e->fields[i], text, sizeof(text))) return 0;
        if (e->kind == DND_SOURCE_SECTION && i == 0) {
            if (!e->fields[i].count || e->fields[i].end >= e->heading.begin ||
                normalize(text, strlen(text), alias, sizeof(alias), 1) || strcmp(alias, e->alias)) return 0;
        } else if (e->kind != DND_SOURCE_CREATURE) {
            if (e->fields[i].count) return 0;
        } else if (e->fields[i].count && (e->fields[i].begin < e->heading.end || !field_text_valid(text, i))) return 0;
    }
    return 1;
}

int dnd_source_index_admit(const dnd_source_data *d, dnd_source_index *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!d || !identifier(d->collection, sizeof(d->collection)) ||
        string_length(d->corpus_version, sizeof(d->corpus_version)) != 71 ||
        memcmp(d->corpus_version, "sha256:", 7) || !hash_valid(d->corpus_version + 7) ||
        !identifier(d->ruleset, sizeof(d->ruleset)) ||
        !d->documents || !d->document_count || d->document_count > DND_SOURCE_DOCUMENTS_MAX ||
        !d->records || !d->record_count || d->record_count > DND_SOURCE_RECORDS_MAX ||
        !d->entries || !d->entry_count || d->entry_count > DND_SOURCE_ENTRIES_MAX) return -1;
    for (size_t i = 0; i < d->document_count; ++i) {
        const dnd_source_document *doc = &d->documents[i];
        if (!identifier(doc->document_id, sizeof(doc->document_id)) ||
            !identifier(doc->book_slug, sizeof(doc->book_slug)) ||
            !ent_book_allowed(ent_book_access_mask(1), doc->book_slug) || !hash_valid(doc->source_sha256)) return -1;
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(doc->document_id, d->documents[j].document_id)) return -1;
    }
    for (size_t i = 0; i < d->record_count; ++i) if (!record_valid(d, i)) return -1;
    for (size_t i = 0; i < d->entry_count; ++i) if (!entry_valid(d, &d->entries[i])) return -1;
    out->data = d;
    return 0;
}

static unsigned requested_fields(const char *q) {
    unsigned fields = 0;
    if (phrase(q, "armor class") || phrase(q, "ac")) fields |= 1u;
    if (phrase(q, "hit points") || phrase(q, "hp")) fields |= 2u;
    if (phrase(q, "speed") || phrase(q, "movement")) fields |= 4u;
    if (phrase(q, "challenge") || phrase(q, "cr")) fields |= 8u;
    if (phrase(q, "immune") || phrase(q, "immunity") || phrase(q, "immunities")) fields |= 16u;
    return fields;
}

static int single_spell_context(const char *q, size_t start, size_t end) {
    const char *after = q + end;
    if (*after == ' ') ++after;
    if (!strncmp(after, "spell", 5) && (!after[5] || after[5] == ' ')) return 1;
    static const char *const verbs[] = {"cast ", "casting ", "casts "};
    int casting = 0;
    for (size_t i = 0; i < sizeof(verbs) / sizeof(verbs[0]); ++i) {
        size_t n = strlen(verbs[i]);
        if (start >= n && !memcmp(q + start - n, verbs[i], n) &&
            (start == n || q[start - n - 1u] == ' ')) casting = 1;
    }
    if (!casting) return 0;
    if (!*after) return 1;
    static const char *const continuations[] = {
        "on", "at", "when", "if", "to", "for", "against", "as", "protect", "help", "work", "and", "or"
    };
    for (size_t i = 0; i < sizeof(continuations) / sizeof(continuations[0]); ++i) {
        size_t n = strlen(continuations[i]);
        if (!strncmp(after, continuations[i], n) && (!after[n] || after[n] == ' ')) return 1;
    }
    return 0;
}

static int named_rule_matches(const char *q, const char *name, int spell) {
    size_t length = strlen(name);
    for (size_t start = 0; q[start]; ++start) {
        if ((start && q[start - 1] != ' ') || q[start] != name[0]) continue;
        size_t used = 0, words = 0;
        for (size_t end = start; q[end]; ++end) {
            if (q[end] == ' ') continue;
            if (used == length || q[end] != name[used]) break;
            ++used;
            if (q[end + 1] && q[end + 1] != ' ') continue;
            ++words;
            if (words < 2 && !(spell && used == length && single_spell_context(q, start, end + 1u))) continue;
            if (used == length) return 1;
            if (length >= 2 && used + 1 == length && name[length - 1] == 's' && name[length - 2] != 's') return 1;
        }
    }
    return 0;
}

int dnd_source_spell_name_matches(const char *normalized, const char *name) {
    return named_rule_matches(normalized, name, 1);
}

static int rule_name_matches(const char *q, const char *name) {
    return named_rule_matches(q, name, 0);
}

static int add_group(const dnd_source_group *g, dnd_source_selection *out) {
    if (!g->count) return DND_SOURCE_INCOMPLETE;
    for (size_t i = 0; i < g->count; ++i) {
        size_t j = 0;
        for (; j < out->count; ++j) if (out->records[j] == g->spans[i].record) break;
        if (j < out->count) {
            /* A requested full-record context supersedes any narrower excerpt. */
            out->excerpts[j].count = 0;
            continue;
        }
        if (out->count == DND_RAG_HITS_MAX) return DND_SOURCE_CAPACITY;
        out->records[out->count++] = g->spans[i].record;
    }
    return DND_SOURCE_SELECTED;
}

static int add_excerpt(dnd_rag_excerpt *out, uint32_t begin, uint32_t end) {
    size_t first = 0, last;
    while (first < out->count && out->spans[first].end < begin) ++first;
    last = first;
    while (last < out->count && out->spans[last].begin <= end) {
        if (out->spans[last].begin < begin) begin = out->spans[last].begin;
        if (out->spans[last].end > end) end = out->spans[last].end;
        ++last;
    }
    if (first == last && out->count == DND_RAG_EXCERPT_SPANS_MAX) return DND_SOURCE_CAPACITY;
    memmove(out->spans + first + 1u, out->spans + last,
        (out->count - last) * sizeof(out->spans[0]));
    out->spans[first] = (dnd_rag_byte_span){begin, end};
    out->count = out->count - (last - first) + 1u;
    return DND_SOURCE_SELECTED;
}

static int spell_excerpts(const dnd_source_data *d, const dnd_source_entry *e,
    dnd_source_selection *out) {
    const dnd_source_group *groups[] = {&e->heading, &e->body};
    for (size_t g = 0; g < 2; ++g) for (size_t s = 0; s < groups[g]->count; ++s) {
        uint32_t record = groups[g]->spans[s].record;
        const dnd_source_record *r = &d->records[record];
        uint32_t begin = e->heading.begin > r->begin ? e->heading.begin : r->begin;
        uint32_t end = e->body.end;
        if (end - r->begin > r->content_len) end = r->begin + (uint32_t)r->content_len;
        for (size_t i = 0; i < out->count; ++i) if (out->records[i] == record) {
            int reason = add_excerpt(&out->excerpts[i], begin - r->begin, end - r->begin);
            if (reason != DND_SOURCE_SELECTED) return reason;
            break;
        }
    }
    return DND_SOURCE_SELECTED;
}

static int choose_creature(const dnd_source_data *d, const uint8_t *allowed_documents,
    const char *q, unsigned fields, dnd_source_selection *out) {
    size_t selected = d->entry_count, length = 0;
    const char *key = NULL;
    for (size_t i = 0; i < d->entry_count; ++i) {
        const dnd_source_entry *e = &d->entries[i];
        if (e->kind != DND_SOURCE_CREATURE || !allowed_documents[e->document]) continue;
        const char *candidate = phrase(q, e->name) ? e->name : phrase(q, e->alias) ? e->alias : NULL;
        if (candidate && strlen(candidate) > length) { selected = i; key = candidate; length = strlen(candidate); }
    }
    if (!key) return DND_SOURCE_NO_MATCH;
    for (size_t i = 0; i < d->entry_count; ++i) {
        const dnd_source_entry *e = &d->entries[i];
        if (i == selected || e->kind != DND_SOURCE_CREATURE || !allowed_documents[e->document]) continue;
        const char *candidate = phrase(q, e->name) ? e->name : phrase(q, e->alias) ? e->alias : NULL;
        if (candidate && (!strcmp(key, candidate) || !phrase(key, candidate))) return DND_SOURCE_AMBIGUOUS;
    }
    const dnd_source_entry *e = &d->entries[selected];
    int reason = add_group(&e->heading, out);
    for (size_t i = 0; i < DND_SOURCE_FIELDS && reason == DND_SOURCE_SELECTED; ++i)
        if (fields & (1u << i)) reason = add_group(&e->fields[i], out);
    if (reason == DND_SOURCE_SELECTED) out->replace_baseline = 1;
    return reason;
}

enum { SOURCE_RANK_NONE = 2 * DND_RAG_HITS_MAX };

static size_t source_rank(const dnd_source_entry *e, const dnd_source_anchor *anchors, size_t count) {
    size_t rank = SOURCE_RANK_NONE;
    for (size_t i = 0; i < count; ++i) {
        if (anchors[i].document != e->document) continue;
        /* Any exact page precedes document fallback. Preserve hit order within
         * each tier; an unrelated or unindexed document supplies no anchor. */
        if (anchors[i].page == e->page) return i;
        if (rank == SOURCE_RANK_NONE) rank = DND_RAG_HITS_MAX + i;
    }
    return rank;
}

static int same_group(const dnd_source_group *a, const dnd_source_group *b) {
    if (a->begin != b->begin || a->end != b->end || a->count != b->count) return 0;
    for (size_t i = 0; i < a->count; ++i)
        if (a->spans[i].record != b->spans[i].record || a->spans[i].begin != b->spans[i].begin ||
            a->spans[i].end != b->spans[i].end) return 0;
    return 1;
}

static int section_matches(const dnd_source_entry *e, const char *q) {
    if (!strcmp(e->alias, "concentration") && !strcmp(e->name, "takingdamage")) {
        if (phrase(q, "feat") || phrase(q, "advantage") || phrase(q, "disadvantage") ||
            phrase(q, "proficiency") || phrase(q, "war caster")) return 0;
        if (!(phrase(q, "concentration") || phrase(q, "concentrating")) ||
            !(phrase(q, "damage") || phrase(q, "damaged"))) return 0;
        static const char *const cues[] = {"check", "checks", "save", "saving throw", "dc",
            "taking damage", "take damage", "after damage", "when damaged"};
        for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i)
            if (phrase(q, cues[i])) return 1;
        return 0;
    }
    if (!strcmp(e->alias, "grappling") && !strcmp(e->name, "escapingagrapple")) {
        if (phrase(q, "grappler") || phrase(q, "feat") || phrase(q, "spell") ||
            phrase(q, "teleport") || phrase(q, "teleportation")) return 0;
        static const char *const cues[] = {
            "escape a grapple", "escape the grapple", "escape from a grapple", "escape from the grapple",
            "escaping a grapple", "escaping the grapple", "get out of a grapple", "get out of the grapple",
            "break free of a grapple", "break free from a grapple", "escape being grappled"
        };
        for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i)
            if (phrase(q, cues[i])) return 1;
        return 0;
    }
    return (phrase(q, e->alias) || rule_name_matches(q, e->alias)) && rule_name_matches(q, e->name);
}

static int needs_turn_context(const char *q) {
    static const char *const cues[] = {
        "how often", "frequency", "round", "rounds", "turn", "turns",
        "more than once", "twice", "two times", "multiple times",
        "reaction", "reactions", "opportunity attack", "opportunity attacks"
    };
    for (size_t i = 0; i < sizeof(cues) / sizeof(cues[0]); ++i)
        if (phrase(q, cues[i])) return 1;
    return 0;
}


static int add_turn_context(const dnd_source_data *d, const dnd_source_entry *e,
    dnd_source_selection *out) {
    if (!(e->flags & DND_SOURCE_ONCE_PER_TURN)) return DND_SOURCE_SELECTED;
    size_t context = d->entry_count;
    for (size_t i = 0; i < d->entry_count; ++i) {
        const dnd_source_entry *candidate = &d->entries[i];
        if (candidate->document != e->document || candidate->kind != DND_SOURCE_RULE ||
            !(candidate->flags & DND_SOURCE_OTHER_TURN_REACTION) || strcmp(candidate->name, "reactions")) continue;
        if (context != d->entry_count) return DND_SOURCE_AMBIGUOUS;
        context = i;
    }
    /* The opening identifies reactions; the body supplies examples and limits. */
    return context == d->entry_count ? DND_SOURCE_INCOMPLETE : add_group(&d->entries[context].body, out);
}

static int choose_sections(const dnd_source_data *d, const uint8_t *allowed, const char *q,
    dnd_source_selection *out) {
    size_t picks[DND_RAG_HITS_MAX], count = 0;
    for (size_t i = 0; i < d->entry_count; ++i) {
        const dnd_source_entry *e = &d->entries[i];
        if (e->kind != DND_SOURCE_SECTION || !allowed[e->document] || !section_matches(e, q)) continue;
        size_t j = 0;
        for (; j < count; ++j) {
            const dnd_source_entry *old = &d->entries[picks[j]];
            if (strcmp(old->name, e->name) || strcmp(old->alias, e->alias)) return DND_SOURCE_AMBIGUOUS;
            if (old->document != e->document) continue;
            if (old->page != e->page || old->flags != e->flags || !same_group(&old->heading, &e->heading) ||
                !same_group(&old->body, &e->body) || !same_group(&old->opening, &e->opening) ||
                !same_group(&old->fields[0], &e->fields[0])) return DND_SOURCE_AMBIGUOUS;
            break;
        }
        if (j < count) continue;
        if (count == DND_RAG_HITS_MAX) return DND_SOURCE_CAPACITY;
        picks[count++] = i;
    }
    if (!count) return DND_SOURCE_NO_MATCH;
    /* Preserve every admitted printing, without text equivalence or a hidden
     * edition preference. Document ID orders groups; spans retain source order. */
    for (size_t i = 1; i < count; ++i) {
        size_t value = picks[i], j = i;
        while (j && strcmp(d->documents[d->entries[picks[j - 1u]].document].document_id,
            d->documents[d->entries[value].document].document_id) > 0) {
            picks[j] = picks[j - 1u]; --j;
        }
        picks[j] = value;
    }
    for (size_t i = 0; i < count; ++i) {
        int reason = add_group(&d->entries[picks[i]].body, out);
        if (reason != DND_SOURCE_SELECTED) return reason;
    }
    if (needs_turn_context(q)) for (size_t i = 0; i < count; ++i) {
        int reason = add_turn_context(d, &d->entries[picks[i]], out);
        if (reason != DND_SOURCE_SELECTED) return reason;
    }
    out->replace_baseline = 1;
    return DND_SOURCE_SELECTED;
}


static int choose_rules(const dnd_source_data *d, const uint8_t *allowed_documents, const char *q,
    const dnd_source_anchor *anchors, size_t count, int spells_only, dnd_source_selection *out) {
    size_t picks[DND_RAG_HITS_MAX], ranks[DND_RAG_HITS_MAX], picked = 0;
    int ambiguous[DND_RAG_HITS_MAX] = {0};
    /* Document recovery runs only when no named rule matches an anchored page.
     * Incidental names elsewhere in a book cannot expand existing page matches. */
    for (size_t tier = 0; tier < 2 && !picked; ++tier) {
        for (size_t i = 0; i < d->entry_count; ++i) {
            const dnd_source_entry *e = &d->entries[i];
            if (e->kind != DND_SOURCE_RULE || !allowed_documents[e->document]) continue;
            if (spells_only && !(e->flags & DND_SOURCE_SPELL_HEADER)) continue;
            size_t rank = source_rank(e, anchors, count), found = 0;
            if (rank == SOURCE_RANK_NONE || rank / DND_RAG_HITS_MAX != tier ||
                !named_rule_matches(q, e->name, (e->flags & DND_SOURCE_SPELL_HEADER) != 0)) continue;
            for (; found < picked; ++found) if (!strcmp(e->name, d->entries[picks[found]].name)) break;
            if (found == picked) {
                if (picked == DND_RAG_HITS_MAX) return DND_SOURCE_CAPACITY;
                picks[picked] = i;
                ranks[picked++] = rank;
            } else if (rank < ranks[found]) {
                picks[found] = i; ranks[found] = rank; ambiguous[found] = 0;
            } else if (rank == ranks[found]) {
                const dnd_source_entry *old = &d->entries[picks[found]];
                if (old->document != e->document || old->page != e->page || old->flags != e->flags ||
                    !same_group(&old->heading, &e->heading) || !same_group(&old->opening, &e->opening) ||
                    !same_group(&old->body, &e->body))
                    ambiguous[found] = 1;
            }
        }
    }
    if (!picked) return DND_SOURCE_NO_MATCH;
    int only_spells = picked != 0;
    for (size_t i = 0; i < picked; ++i)
        if (!(d->entries[picks[i]].flags & DND_SOURCE_SPELL_HEADER)) only_spells = 0;
    for (size_t i = 0; i < picked; ++i) {
        if (ambiguous[i]) return DND_SOURCE_AMBIGUOUS;
        if (only_spells) {
            int reason = add_group(&d->entries[picks[i]].heading, out);
            if (reason != DND_SOURCE_SELECTED) return reason;
        }
        int reason = add_group(&d->entries[picks[i]].body, out);
        if (reason != DND_SOURCE_SELECTED) return reason;
    }
    if (only_spells) {
        for (size_t i = 0; i < picked; ++i) {
            int reason = spell_excerpts(d, &d->entries[picks[i]], out);
            if (reason != DND_SOURCE_SELECTED) return reason;
        }
        out->replace_baseline = 1;
    }
    if (needs_turn_context(q)) for (size_t i = 0; i < picked; ++i) {
        int reason = add_turn_context(d, &d->entries[picks[i]], out);
        if (reason != DND_SOURCE_SELECTED) return reason;
    }
    return DND_SOURCE_SELECTED;
}

int dnd_source_query_prepare(const dnd_source_index *index, const dnd_rag_scope *scope,
    const char *corpus, const char *query, size_t query_len,
    const dnd_source_anchor *anchors, size_t count,
    char normalized[DND_SOURCE_QUERY_CAP], uint8_t allowed[DND_SOURCE_DOCUMENTS_MAX]) {
    if (!normalized || !allowed) return -1;
    normalized[0] = 0; memset(allowed, 0, DND_SOURCE_DOCUMENTS_MAX);
    if (!index || !index->data || !scope || !corpus || !query || !query_len ||
        query_len >= DND_SOURCE_QUERY_CAP || memchr(query, '\0', query_len) ||
        !utf8_validate_v1((const uint8_t *)query, query_len) ||
        normalize(query, query_len, normalized, DND_SOURCE_QUERY_CAP, 0) ||
        scope->kind != DND_RAG_SHARED_RULEBOOK || !identifier(scope->authenticated_user_id, sizeof(scope->authenticated_user_id)) ||
        scope->owner_user_id[0] || scope->campaign_id[0] || scope->session_id[0] || scope->character_id[0] ||
        !identifier(scope->collection, sizeof(scope->collection)) || !identifier(scope->ruleset, sizeof(scope->ruleset)) ||
        string_length(corpus, 128) != 71 || strcmp(corpus, index->data->corpus_version) ||
        strcmp(scope->collection, index->data->collection) || strcmp(scope->ruleset, index->data->ruleset) ||
        count > DND_RAG_HITS_MAX || (count && !anchors)) return -1;
    for (size_t i = 0; i < count; ++i)
        if ((anchors[i].document != UINT32_MAX && anchors[i].document >= index->data->document_count) ||
            !anchors[i].page || anchors[i].page > INT32_MAX) return -1;
    for (size_t i = 0; i < index->data->document_count; ++i)
        allowed[i] = (uint8_t)ent_book_allowed(scope->book_mask, index->data->documents[i].book_slug);
    return 0;
}

int dnd_source_index_select(
    const dnd_source_index *index, const dnd_rag_scope *scope, const char *corpus,
    const char *query, size_t query_len, const dnd_source_anchor *anchors,
    size_t count, dnd_source_selection *out) {
    char normalized[DND_SOURCE_QUERY_CAP];
    uint8_t allowed_documents[DND_SOURCE_DOCUMENTS_MAX] = {0};
    dnd_source_selection selected = {0};
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (dnd_source_query_prepare(index, scope, corpus, query, query_len, anchors, count,
        normalized, allowed_documents)) return -1;
    /* A source-backed spell name identifies the subject. Generic mechanics
     * such as saving throws or bonus actions must not displace that subject.
     * Scope, anchors, completeness and ambiguity still apply. */
    int reason = choose_rules(index->data, allowed_documents, normalized, anchors, count, 1, &selected);
    if (reason == DND_SOURCE_NO_MATCH)
        reason = choose_sections(index->data, allowed_documents, normalized, &selected);
    unsigned fields = requested_fields(normalized);
    if (reason == DND_SOURCE_NO_MATCH && fields)
        reason = choose_creature(index->data, allowed_documents, normalized, fields, &selected);
    if (reason == DND_SOURCE_NO_MATCH)
        reason = choose_rules(index->data, allowed_documents, normalized, anchors, count, 0, &selected);
    if (reason == DND_SOURCE_SELECTED) *out = selected;
    out->reason = reason;
    return 0;
}
