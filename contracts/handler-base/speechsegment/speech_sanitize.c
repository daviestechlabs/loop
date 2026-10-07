/* speech_sanitize.c — pure C Orpheus tag allowlist + fail-closed strip.
 *
 * Mirrors contracts/handler-base/speechsegment Go policy:
 *   allowlist: laugh, chuckle, cough, sniffle, groan, yawn, gasp
 *   MaxEmotiveTagsPerSegment = 2
 *   maxTagSpanRunes = 24
 */

#include "speech_sanitize.h"

#include <string.h>

#if defined(__x86_64__) && (defined(__GNUC__) || defined(__clang__))
#include <cpuid.h>
#include <immintrin.h>
#define SPEECH_CANONICAL_ASCII_SSE2 1
#else
#define SPEECH_CANONICAL_ASCII_SSE2 0
#endif

#if defined(__GNUC__) || defined(__clang__)
#define SPEECH_ALWAYS_INLINE static inline __attribute__((always_inline))
#else
#define SPEECH_ALWAYS_INLINE static inline
#endif

enum {
    CANONICAL_ASCII_VALID = 1,
    CANONICAL_ASCII_JSON_ESCAPE = 2
};

/* Production allowlist (bare names, lowercase a-z only). */
static const char *const k_allowed[] = {
    "laugh",
    "chuckle",
    "cough",
    "sniffle",
    "groan",
    "yawn",
    "gasp",
};
static const size_t k_allowed_n = sizeof(k_allowed) / sizeof(k_allowed[0]);

/* Decode one UTF-8 code point. Returns 0 on success; sets *cp and *adv. */
static int utf8_decode(const char *s, size_t len, size_t i, uint32_t *cp, size_t *adv)
{
    unsigned char c0;
    if (i >= len) {
        return -1;
    }
    c0 = (unsigned char)s[i];
    if (c0 < 0x80u) {
        *cp = c0;
        *adv = 1;
        return 0;
    }
    if ((c0 & 0xe0u) == 0xc0u && i + 1 < len) {
        unsigned char c1 = (unsigned char)s[i + 1];
        if ((c1 & 0xc0u) != 0x80u) {
            *cp = 0xfffdu;
            *adv = 1;
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x1fu) << 6) | (uint32_t)(c1 & 0x3fu);
        *adv = 2;
        return 0;
    }
    if ((c0 & 0xf0u) == 0xe0u && i + 2 < len) {
        unsigned char c1 = (unsigned char)s[i + 1];
        unsigned char c2 = (unsigned char)s[i + 2];
        if ((c1 & 0xc0u) != 0x80u || (c2 & 0xc0u) != 0x80u) {
            *cp = 0xfffdu;
            *adv = 1;
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x0fu) << 12) |
              ((uint32_t)(c1 & 0x3fu) << 6) |
              (uint32_t)(c2 & 0x3fu);
        *adv = 3;
        return 0;
    }
    if ((c0 & 0xf8u) == 0xf0u && i + 3 < len) {
        unsigned char c1 = (unsigned char)s[i + 1];
        unsigned char c2 = (unsigned char)s[i + 2];
        unsigned char c3 = (unsigned char)s[i + 3];
        if ((c1 & 0xc0u) != 0x80u || (c2 & 0xc0u) != 0x80u || (c3 & 0xc0u) != 0x80u) {
            *cp = 0xfffdu;
            *adv = 1;
            return 0;
        }
        *cp = ((uint32_t)(c0 & 0x07u) << 18) |
              ((uint32_t)(c1 & 0x3fu) << 12) |
              ((uint32_t)(c2 & 0x3fu) << 6) |
              (uint32_t)(c3 & 0x3fu);
        *adv = 4;
        return 0;
    }
    /* Invalid / truncated: consume one byte as replacement. */
    *cp = 0xfffdu;
    *adv = 1;
    return 0;
}

static int is_ascii_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\v' || cp == '\f';
}

/* Subset matching Go unicode.IsSpace for common cases used on the voice path. */
static int is_space_cp(uint32_t cp)
{
    if (is_ascii_space(cp)) {
        return 1;
    }
    /* NEL, NBSP, and common Unicode spaces Go's unicode.IsSpace covers. */
    if (cp == 0x0085u || cp == 0x00a0u || cp == 0x1680u ||
        (cp >= 0x2000u && cp <= 0x200au) ||
        cp == 0x2028u || cp == 0x2029u || cp == 0x202fu ||
        cp == 0x205fu || cp == 0x3000u) {
        return 1;
    }
    return 0;
}

static int is_speech_punct(uint32_t cp)
{
    switch (cp) {
    case '.':
    case '!':
    case '?':
    case ',':
    case ';':
    case ':':
    case 0x2026u: /* … */
        return 1;
    default:
        return 0;
    }
}

#if SPEECH_CANONICAL_ASCII_SSE2
static int canonical_ascii_use_avx2;

__attribute__((constructor))
static void canonical_ascii_select(void) {
    unsigned int eax;
    unsigned int ebx;
    unsigned int ecx;
    unsigned int edx;
    unsigned int xcr0_lo;
    unsigned int xcr0_hi;

    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0 ||
        (ecx & ((1u << 27) | (1u << 28))) !=
            ((1u << 27) | (1u << 28))) {
        return;
    }
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    if ((xcr0_lo & 0x6u) != 0x6u ||
        __get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx) == 0) {
        return;
    }
    canonical_ascii_use_avx2 = (ebx & (1u << 5)) != 0u;
}

__attribute__((target("sse2")))
static int canonical_ascii_sse2_block(
    const char *p,
    const char *begin,
    const char *end,
    int classify_json
) {
    const __m128i bytes = _mm_loadu_si128((const __m128i_u *)p);
    const __m128i printable =
        _mm_cmpgt_epi8(bytes, _mm_set1_epi8((char)0x1f));
    const __m128i invalid = _mm_or_si128(
        _mm_andnot_si128(printable, _mm_cmpeq_epi8(bytes, bytes)),
        _mm_or_si128(
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8('<')),
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8((char)0x7f))));
    unsigned int spaces;
    int classification = CANONICAL_ASCII_VALID;

    if (!classify_json) {
        if (_mm_movemask_epi8(invalid) != 0) return 0;
    } else {
        const __m128i json_escape = _mm_or_si128(
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8('"')),
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8('\\')));
        unsigned int trouble = (unsigned int)_mm_movemask_epi8(
            _mm_or_si128(invalid, json_escape));
        if (trouble != 0u) {
            if (_mm_movemask_epi8(invalid) != 0) return 0;
            classification |= CANONICAL_ASCII_JSON_ESCAPE;
        }
    }
    spaces = (unsigned int)_mm_movemask_epi8(
        _mm_cmpeq_epi8(bytes, _mm_set1_epi8(' ')));
    if (spaces == 0u) {
        return classification;
    }
    if ((spaces & (spaces >> 1)) != 0u ||
        ((spaces & 1u) != 0u && p != begin && p[-1] == ' ')) {
        return 0;
    }
    /* OR groups comma/period. Clearing bit zero groups colon/semicolon. */
    {
        const __m128i punctuation = _mm_or_si128(
            _mm_or_si128(
                _mm_cmpeq_epi8(bytes, _mm_set1_epi8('!')),
                _mm_cmpeq_epi8(
                    _mm_or_si128(bytes, _mm_set1_epi8(2)),
                    _mm_set1_epi8('.'))),
            _mm_or_si128(
                _mm_cmpeq_epi8(
                    _mm_and_si128(bytes, _mm_set1_epi8((char)-2)),
                    _mm_set1_epi8(':')),
                _mm_cmpeq_epi8(bytes, _mm_set1_epi8('?'))));
        unsigned int punctuation_mask =
            (unsigned int)_mm_movemask_epi8(punctuation);
        if ((spaces & (punctuation_mask >> 1)) != 0u) {
            return 0;
        }
    }
    if ((spaces & UINT32_C(0x8000)) != 0u && p + 16 != end &&
        is_speech_punct((unsigned char)p[16])) {
        return 0;
    }
    return classification;
}

__attribute__((target("sse2")))
static int canonical_ascii_sse2_half(
    const char *p,
    const char *begin,
    const char *end,
    int classify_json
) {
    const __m128i bytes = _mm_loadl_epi64((const __m128i_u *)p);
    const __m128i printable =
        _mm_cmpgt_epi8(bytes, _mm_set1_epi8((char)0x1f));
    const __m128i invalid = _mm_or_si128(
        _mm_andnot_si128(printable, _mm_cmpeq_epi8(bytes, bytes)),
        _mm_or_si128(
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8('<')),
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8((char)0x7f))));
    unsigned int spaces;
    int classification = CANONICAL_ASCII_VALID;

    if (!classify_json) {
        if (((unsigned int)_mm_movemask_epi8(invalid) &
             UINT32_C(0xff)) != 0u) return 0;
    } else {
        const __m128i json_escape = _mm_or_si128(
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8('"')),
            _mm_cmpeq_epi8(bytes, _mm_set1_epi8('\\')));
        unsigned int trouble = (unsigned int)_mm_movemask_epi8(
            _mm_or_si128(invalid, json_escape)) & UINT32_C(0xff);
        if (trouble != 0u) {
            if (((unsigned int)_mm_movemask_epi8(invalid) &
                 UINT32_C(0xff)) != 0u) return 0;
            classification |= CANONICAL_ASCII_JSON_ESCAPE;
        }
    }
    spaces = (unsigned int)_mm_movemask_epi8(
        _mm_cmpeq_epi8(bytes, _mm_set1_epi8(' '))) & UINT32_C(0xff);
    if (spaces == 0u) {
        return classification;
    }
    if ((spaces & (spaces >> 1)) != 0u ||
        ((spaces & 1u) != 0u && p != begin && p[-1] == ' ')) {
        return 0;
    }
    {
        const __m128i punctuation = _mm_or_si128(
            _mm_or_si128(
                _mm_cmpeq_epi8(bytes, _mm_set1_epi8('!')),
                _mm_cmpeq_epi8(
                    _mm_or_si128(bytes, _mm_set1_epi8(2)),
                    _mm_set1_epi8('.'))),
            _mm_or_si128(
                _mm_cmpeq_epi8(
                    _mm_and_si128(bytes, _mm_set1_epi8((char)-2)),
                    _mm_set1_epi8(':')),
                _mm_cmpeq_epi8(bytes, _mm_set1_epi8('?'))));
        unsigned int punctuation_mask =
            (unsigned int)_mm_movemask_epi8(punctuation);
        if ((spaces & (punctuation_mask >> 1)) != 0u) {
            return 0;
        }
    }
    if ((spaces & UINT32_C(0x80)) != 0u && p + 8 != end &&
        is_speech_punct((unsigned char)p[8])) {
        return 0;
    }
    return classification;
}

__attribute__((target("avx2")))
static int canonical_ascii_avx2_span(
    const char *begin,
    const char *end
) {
    const char *p = begin;

    for (;;) {
        const __m256i bytes =
            _mm256_loadu_si256((const __m256i_u *)p);
        const __m256i printable =
            _mm256_cmpgt_epi8(bytes, _mm256_set1_epi8((char)0x1f));
        const __m256i invalid = _mm256_or_si256(
            _mm256_andnot_si256(
                printable, _mm256_cmpeq_epi8(bytes, bytes)),
            _mm256_or_si256(
                _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('<')),
                _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8((char)0x7f))));
        unsigned int spaces;

        if (_mm256_movemask_epi8(invalid) != 0) {
            return 0;
        }
        spaces = (unsigned int)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(' ')));
        if (spaces != 0u) {
            const __m256i punctuation = _mm256_or_si256(
                _mm256_or_si256(
                    _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('!')),
                    _mm256_cmpeq_epi8(
                        _mm256_or_si256(bytes, _mm256_set1_epi8(2)),
                        _mm256_set1_epi8('.'))),
                _mm256_or_si256(
                    _mm256_cmpeq_epi8(
                        _mm256_and_si256(bytes, _mm256_set1_epi8((char)-2)),
                        _mm256_set1_epi8(':')),
                    _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('?'))));
            unsigned int punctuation_mask =
                (unsigned int)_mm256_movemask_epi8(punctuation);
            if ((spaces & (spaces >> 1)) != 0u ||
                ((spaces & 1u) != 0u && p != begin && p[-1] == ' ') ||
                (spaces & (punctuation_mask >> 1)) != 0u ||
                ((spaces & UINT32_C(0x80000000)) != 0u && p + 32 != end &&
                 is_speech_punct((unsigned char)p[32]))) {
                return 0;
            }
        }
        if (p + 32 == end) {
            return 1;
        }
        p += 32;
        if ((size_t)(end - p) < 32u) {
            p = end - 32;
        }
    }
}

__attribute__((target("avx2")))
static int canonical_ascii_json_avx2_span(
    const char *begin,
    const char *end
) {
    const char *p = begin;
    int classification = CANONICAL_ASCII_VALID;

    for (;;) {
        const __m256i bytes =
            _mm256_loadu_si256((const __m256i_u *)p);
        const __m256i printable =
            _mm256_cmpgt_epi8(bytes, _mm256_set1_epi8((char)0x1f));
        const __m256i invalid = _mm256_or_si256(
            _mm256_andnot_si256(
                printable, _mm256_cmpeq_epi8(bytes, bytes)),
            _mm256_or_si256(
                _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('<')),
                _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8((char)0x7f))));
        unsigned int spaces;

        {
            const __m256i json_escape = _mm256_or_si256(
                _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('"')),
                _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('\\')));
            unsigned int trouble = (unsigned int)_mm256_movemask_epi8(
                _mm256_or_si256(invalid, json_escape));
            if (trouble != 0u) {
                if (_mm256_movemask_epi8(invalid) != 0) return 0;
                classification |= CANONICAL_ASCII_JSON_ESCAPE;
            }
        }
        spaces = (unsigned int)_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8(' ')));
        if (spaces != 0u) {
            const __m256i punctuation = _mm256_or_si256(
                _mm256_or_si256(
                    _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('!')),
                    _mm256_cmpeq_epi8(
                        _mm256_or_si256(bytes, _mm256_set1_epi8(2)),
                        _mm256_set1_epi8('.'))),
                _mm256_or_si256(
                    _mm256_cmpeq_epi8(
                        _mm256_and_si256(bytes, _mm256_set1_epi8((char)-2)),
                        _mm256_set1_epi8(':')),
                    _mm256_cmpeq_epi8(bytes, _mm256_set1_epi8('?'))));
            unsigned int punctuation_mask =
                (unsigned int)_mm256_movemask_epi8(punctuation);
            if ((spaces & (spaces >> 1)) != 0u ||
                ((spaces & 1u) != 0u && p != begin && p[-1] == ' ') ||
                (spaces & (punctuation_mask >> 1)) != 0u ||
                ((spaces & UINT32_C(0x80000000)) != 0u && p + 32 != end &&
                 is_speech_punct((unsigned char)p[32]))) {
                return 0;
            }
        }
        if (p + 32 == end) {
            return classification;
        }
        p += 32;
        if ((size_t)(end - p) < 32u) {
            p = end - 32;
        }
    }
}
#endif

/* Return true only when sanitization must preserve every input byte. */
SPEECH_ALWAYS_INLINE int classify_canonical_ascii(
    const char *in,
    size_t in_len,
    int classify_json
)
{
#if !SPEECH_CANONICAL_ASCII_SSE2
    const size_t ones = SIZE_MAX / 0xffu;
    const size_t highs = ones << 7;
#endif
    const char *p;
    const char *end;
    int classification = CANONICAL_ASCII_VALID;

    if (!in || in_len == 0 || in[0] == ' ' || in[in_len - 1] == ' ') {
        return 0;
    }
    p = in;
    end = in + in_len;
#if SPEECH_CANONICAL_ASCII_SSE2
    if (in_len >= 32u && canonical_ascii_use_avx2) {
        return classify_json ?
            canonical_ascii_json_avx2_span(in, end) :
            canonical_ascii_avx2_span(in, end);
    }
    if (in_len >= 16u) {
        while ((size_t)(end - p) >= 16u) {
            int block = canonical_ascii_sse2_block(
                p, in, end, classify_json);
            if ((block & CANONICAL_ASCII_VALID) == 0) {
                return 0;
            }
            classification |= block;
            p += 16;
        }
        /* Recheck the final vector without loading past the bounded span. */
        if (p != end) {
            int block = canonical_ascii_sse2_block(
                end - 16, in, end, classify_json);
            if ((block & CANONICAL_ASCII_VALID) == 0) {
                return 0;
            }
            classification |= block;
        }
        return classification;
    }
    if (in_len >= 8u) {
        int block = canonical_ascii_sse2_half(p, in, end, classify_json);
        if ((block & CANONICAL_ASCII_VALID) == 0) {
            return 0;
        }
        classification |= block;
        p += 8;
    }
#else
    /* memcpy keeps unaligned loads defined. Zero-byte tests classify each lane. */
    while ((size_t)(end - p) >= sizeof(size_t)) {
        size_t word;
        size_t controls;
        size_t angles;
        size_t deletes;
        size_t quotes;
        size_t backslashes;
        size_t spaces;
        size_t i;

        memcpy(&word, p, sizeof(word));
        controls = word & (ones * 0xe0u);
        angles = word ^ (ones * (size_t)'<');
        deletes = word ^ (ones * 0x7fu);
        if ((word & highs) != 0u ||
            ((controls - ones) & ~controls & highs) != 0u ||
            ((angles - ones) & ~angles & highs) != 0u ||
            ((deletes - ones) & ~deletes & highs) != 0u) {
            return 0;
        }
        if (classify_json) {
            quotes = word ^ (ones * (size_t)'"');
            backslashes = word ^ (ones * (size_t)'\\');
            if (((quotes - ones) & ~quotes & highs) != 0u ||
                ((backslashes - ones) & ~backslashes & highs) != 0u) {
                classification |= CANONICAL_ASCII_JSON_ESCAPE;
            }
        }
        spaces = word ^ (ones * (size_t)' ');
        if (((spaces - ones) & ~spaces & highs) != 0u) {
            for (i = 0; i < sizeof(word); ++i) {
                const char *space = p + i;
                if (*space == ' ' &&
                    (space[-1] == ' ' ||
                     is_speech_punct((unsigned char)space[1]))) {
                    return 0;
                }
            }
        }
        p += sizeof(word);
    }
#endif
    while (p < end) {
        unsigned char c = (unsigned char)*p;

        if (c > ' ' && c < 0x7fu && c != '<') {
            if (classify_json && (c == '"' || c == '\\')) {
                classification |= CANONICAL_ASCII_JSON_ESCAPE;
            }
            p++;
            continue;
        }
        if (c != ' ' || p[-1] == ' ' ||
            is_speech_punct((unsigned char)p[1])) {
            return 0;
        }
        p++;
    }
    return classification;
}

static int is_canonical_ascii(const char *in, size_t in_len)
{
    return (classify_canonical_ascii(in, in_len, 0) &
            CANONICAL_ASCII_VALID) != 0;
}

int speech_is_canonical_ascii_v1(const char *in, size_t in_len)
{
    return is_canonical_ascii(in, in_len);
}

enum {
    TAG_KIND_NONE = 0,
    TAG_KIND_ALLOWED = 1,
    TAG_KIND_DROP = 2
};

static int is_allowed_name(const char *name, size_t n)
{
    size_t i;
    for (i = 0; i < k_allowed_n; ++i) {
        size_t ln = strlen(k_allowed[i]);
        if (ln == n && memcmp(k_allowed[i], name, n) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Trim ASCII space from both ends of [p, p+n); updates *p and *n. */
static void trim_ascii_space(const char **p, size_t *n)
{
    const char *s = *p;
    size_t len = *n;
    while (len > 0 && is_ascii_space((unsigned char)s[0])) {
        s++;
        len--;
    }
    while (len > 0 && is_ascii_space((unsigned char)s[len - 1])) {
        len--;
    }
    *p = s;
    *n = len;
}

/*
 * Classify tag inner (between '<' and '>').
 * *name_out / *name_len only valid when kind == TAG_KIND_ALLOWED.
 */
static int classify_tag_inner(const char *inner, size_t inner_len, const char **name_out, size_t *name_len)
{
    const char *p = inner;
    size_t n = inner_len;
    size_t i;
    trim_ascii_space(&p, &n);
    if (n == 0) {
        return TAG_KIND_DROP;
    }
    if (p[0] == '/') {
        return TAG_KIND_DROP;
    }
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (c < 'a' || c > 'z') {
            return TAG_KIND_DROP;
        }
    }
    if (is_allowed_name(p, n)) {
        *name_out = p;
        *name_len = n;
        return TAG_KIND_ALLOWED;
    }
    return TAG_KIND_DROP;
}

/*
 * Scan tag starting at '<' (start). Returns:
 *   end byte after '>' on closed tag, kind set
 *   (size_t)-1 if no closing '>' within max span (kind ignored)
 * Nested '<' before '>' also returns (size_t)-1 (caller advances one byte).
 */
static size_t scan_tag_span(
    const char *text,
    size_t len,
    size_t start,
    int *kind,
    const char **name_out,
    size_t *name_len
)
{
    size_t j;
    int runes = 1; /* count '<' */
    uint32_t cp;
    size_t adv;

    *kind = TAG_KIND_NONE;
    *name_out = NULL;
    *name_len = 0;

    if (start >= len || text[start] != '<') {
        return (size_t)-1;
    }
    j = start + 1;
    while (j < len) {
        if (utf8_decode(text, len, j, &cp, &adv) != 0) {
            break;
        }
        runes++;
        if (cp == '>') {
            const char *inner = text + start + 1;
            size_t inner_len = j - (start + 1);
            *kind = classify_tag_inner(inner, inner_len, name_out, name_len);
            return j + adv;
        }
        if (runes >= SPEECH_SANITIZE_MAX_TAG_SPAN) {
            return (size_t)-1;
        }
        if (cp == '<') {
            return (size_t)-1;
        }
        j += adv;
    }
    return (size_t)-1;
}

/* Drop incomplete open starting at '<' — mirrors Go skipIncompleteOpen. */
static size_t skip_incomplete_open(const char *text, size_t len, size_t start)
{
    size_t j = start + 1;
    int runes = 1;
    uint32_t cp;
    size_t adv;

    while (j < len) {
        if (utf8_decode(text, len, j, &cp, &adv) != 0) {
            break;
        }
        if (cp == '>') {
            return j + adv;
        }
        if (cp == '<') {
            return j;
        }
        runes++;
        if (runes >= SPEECH_SANITIZE_MAX_TAG_SPAN) {
            return j;
        }
        if (is_space_cp(cp)) {
            return start + 1; /* drop only '<' */
        }
        j += adv;
    }
    return len;
}

/* Collapse whitespace + fix space-before-punct after tag removal. */
static size_t collapse_speech_spaces(const char *in, size_t in_len, char *out, size_t out_cap)
{
    size_t i = 0;
    size_t o = 0;
    int prev_space = 0;
    uint32_t cp;
    size_t adv;

    /* Trim leading space. */
    while (i < in_len) {
        if (utf8_decode(in, in_len, i, &cp, &adv) != 0) {
            break;
        }
        if (!is_space_cp(cp)) {
            break;
        }
        i += adv;
    }

    while (i < in_len) {
        if (utf8_decode(in, in_len, i, &cp, &adv) != 0) {
            break;
        }
        if (is_space_cp(cp)) {
            if (!prev_space) {
                if (o + 1 > out_cap) {
                    return (size_t)-1;
                }
                out[o++] = ' ';
                prev_space = 1;
            }
            i += adv;
            continue;
        }
        if (prev_space && is_speech_punct(cp)) {
            /* Drop trailing space before punctuation. */
            if (o > 0 && out[o - 1] == ' ') {
                o--;
            }
        }
        prev_space = 0;
        if (o + adv > out_cap) {
            return (size_t)-1;
        }
        memcpy(out + o, in + i, adv);
        o += adv;
        i += adv;
    }

    /* Trim trailing space. */
    while (o > 0 && out[o - 1] == ' ') {
        o--;
    }
    return o;
}

static int speech_sanitize_slow_v1(
    const char *in,
    size_t in_len,
    int keep_allowed,
    char *out,
    size_t out_cap,
    size_t *out_len
)
{
    /* Scratch holds pre-collapse rewrite; same bound as input. */
    char scratch_local[4096];
    char *scratch;
    size_t scratch_cap;
    size_t i;
    size_t o;
    int kept_tags;
    char stack_fallback[1]; /* unused when heap not available — use VLA-free path */

    (void)stack_fallback;

    /* Prefer stack for typical TTS segments; fall back to out as dual buffer
     * via a second pass using out for scratch only when in_len is large —
     * but collapse needs separate dest. Allocate scratch on stack up to 4k;
     * for larger, write raw rewrite into out then collapse in-place via memmove
     * through a temp of size in_len using out then re-collapse into itself is
     * hard. Simpler: require out_cap >= in_len and use a two-phase with
     * scratch = out for phase1 only when in_len <= out_cap, then collapse into
     * a reversed buffer... Easiest ruthless path: VLA forbidden under -Werror
     * pedantic; use out as scratch and a second buffer on stack for collapse
     * result when in_len <= 4096, else collapse in place carefully.
     */
    if (in_len <= sizeof(scratch_local)) {
        scratch = scratch_local;
        scratch_cap = sizeof(scratch_local);
    } else {
        /* Phase-1 rewrite into out; collapse into scratch_local only if fits
         * after rewrite (always <= in_len). For in_len > 4096, collapse in
         * place: write collapse to front via separate small logic using out
         * as both — use out for rewrite, then collapse from out into out
         * with a memmove-safe single pass reading from a copy window.
         * For large inputs, reuse out as scratch and collapse into a
         * temporary trailing region — not possible without 2x. Write rewrite
         * into out, then if in_len > 4096, run collapse reading from out
         * into scratch_local in chunks... Actually: collapse can run
         * in-place if we only shrink. Implement shrink-in-place collapse.
         */
        scratch = out;
        scratch_cap = out_cap;
    }

    o = 0;
    kept_tags = 0;
    i = 0;
    while (i < in_len) {
        if (in[i] != '<') {
            if (o + 1 > scratch_cap) {
                return SPEECH_SANITIZE_ERR_CAPACITY;
            }
            scratch[o++] = in[i];
            i++;
            continue;
        }
        {
            int kind = TAG_KIND_NONE;
            const char *name = NULL;
            size_t name_len = 0;
            size_t end = scan_tag_span(in, in_len, i, &kind, &name, &name_len);
            if (end == (size_t)-1) {
                i = skip_incomplete_open(in, in_len, i);
                continue;
            }
            if (kind == TAG_KIND_ALLOWED && keep_allowed &&
                kept_tags < SPEECH_SANITIZE_MAX_EMOTIVE) {
                /* Write exact lowercase <name> */
                if (o + 2 + name_len > scratch_cap) {
                    return SPEECH_SANITIZE_ERR_CAPACITY;
                }
                scratch[o++] = '<';
                memcpy(scratch + o, name, name_len);
                o += name_len;
                scratch[o++] = '>';
                kept_tags++;
            }
            /* else drop */
            i = end;
        }
    }

    if (scratch == out) {
        /* Collapse in-place: need a second buffer. For large in_len use
         * stack 4k only when rewrite o <= 4096; else allocate nothing and
         * collapse into a growing prefix (read cursor ahead of write). */
        if (o <= sizeof(scratch_local)) {
            size_t collapsed = collapse_speech_spaces(scratch, o, scratch_local, sizeof(scratch_local));
            if (collapsed == (size_t)-1) {
                return SPEECH_SANITIZE_ERR_CAPACITY;
            }
            memcpy(out, scratch_local, collapsed);
            *out_len = collapsed;
            return SPEECH_SANITIZE_OK;
        }
        /* In-place shrink collapse: read from scratch[0..o), write to out[0..). */
        {
            size_t ri = 0;
            size_t wo = 0;
            int prev_space = 0;
            uint32_t cp;
            size_t adv;
            while (ri < o) {
                if (utf8_decode(scratch, o, ri, &cp, &adv) != 0) {
                    break;
                }
                if (is_space_cp(cp)) {
                    ri += adv;
                    continue; /* trim leading via first non-space pass below */
                }
                break;
            }
            /* Restart: first find start */
            ri = 0;
            while (ri < o) {
                if (utf8_decode(scratch, o, ri, &cp, &adv) != 0) {
                    break;
                }
                if (!is_space_cp(cp)) {
                    break;
                }
                ri += adv;
            }
            while (ri < o) {
                if (utf8_decode(scratch, o, ri, &cp, &adv) != 0) {
                    break;
                }
                if (is_space_cp(cp)) {
                    if (!prev_space) {
                        out[wo++] = ' ';
                        prev_space = 1;
                    }
                    ri += adv;
                    continue;
                }
                if (prev_space && is_speech_punct(cp)) {
                    if (wo > 0 && out[wo - 1] == ' ') {
                        wo--;
                    }
                }
                prev_space = 0;
                /* Copy UTF-8 bytes; safe because wo <= ri always when shrinking. */
                memmove(out + wo, scratch + ri, adv);
                wo += adv;
                ri += adv;
            }
            while (wo > 0 && out[wo - 1] == ' ') {
                wo--;
            }
            *out_len = wo;
            return SPEECH_SANITIZE_OK;
        }
    }

    {
        size_t collapsed = collapse_speech_spaces(scratch, o, out, out_cap);
        if (collapsed == (size_t)-1) {
            return SPEECH_SANITIZE_ERR_CAPACITY;
        }
        *out_len = collapsed;
        return SPEECH_SANITIZE_OK;
    }
}

int speech_sanitize_v1(
    const char *in,
    size_t in_len,
    int keep_allowed,
    char *out,
    size_t out_cap,
    size_t *out_len
)
{
    if (!out_len) {
        return SPEECH_SANITIZE_ERR_ARGUMENT;
    }
    *out_len = 0;
    if (in_len == 0u) {
        return SPEECH_SANITIZE_OK;
    }
    if (!in || !out) {
        return SPEECH_SANITIZE_ERR_ARGUMENT;
    }
    if (out_cap < in_len) {
        return SPEECH_SANITIZE_ERR_CAPACITY;
    }
    if (is_canonical_ascii(in, in_len)) {
        if (out != in) {
            memmove(out, in, in_len);
        }
        *out_len = in_len;
        return SPEECH_SANITIZE_OK;
    }
    return speech_sanitize_slow_v1(
        in, in_len, keep_allowed, out, out_cap, out_len);
}

int speech_sanitize_json_plain_v1(
    const char *in,
    size_t in_len,
    int keep_allowed,
    char *out,
    size_t out_cap,
    size_t *out_len,
    int *json_plain
)
{
    int classification;
    if (!json_plain) {
        return SPEECH_SANITIZE_ERR_ARGUMENT;
    }
    *json_plain = 0;
    if (!out_len) {
        return SPEECH_SANITIZE_ERR_ARGUMENT;
    }
    *out_len = 0;
    if (in_len == 0u) {
        return SPEECH_SANITIZE_OK;
    }
    if (!in || !out) {
        return SPEECH_SANITIZE_ERR_ARGUMENT;
    }
    if (out_cap < in_len) {
        return SPEECH_SANITIZE_ERR_CAPACITY;
    }
    classification = classify_canonical_ascii(in, in_len, 1);
    if ((classification & CANONICAL_ASCII_VALID) != 0) {
        if (out != in) {
            memmove(out, in, in_len);
        }
        *out_len = in_len;
        if ((classification & CANONICAL_ASCII_JSON_ESCAPE) == 0) {
            *json_plain = 1;
        }
        return SPEECH_SANITIZE_OK;
    }
    return speech_sanitize_slow_v1(
        in, in_len, keep_allowed, out, out_cap, out_len);
}

int speech_strip_tags_preserve_space_v1(
    const char *in,
    size_t in_len,
    char *out,
    size_t out_cap,
    size_t *out_len
)
{
    size_t i;
    size_t o;

    if (!out_len) {
        return SPEECH_SANITIZE_ERR_ARGUMENT;
    }
    *out_len = 0;
    if (in_len == 0) {
        return SPEECH_SANITIZE_OK;
    }
    if (!in || !out) {
        return SPEECH_SANITIZE_ERR_ARGUMENT;
    }
    if (out_cap < in_len) {
        return SPEECH_SANITIZE_ERR_CAPACITY;
    }

    o = 0;
    i = 0;
    while (i < in_len) {
        if (in[i] != '<') {
            out[o++] = in[i];
            i++;
            continue;
        }
        {
            int kind = TAG_KIND_NONE;
            const char *name = NULL;
            size_t name_len = 0;
            size_t end = scan_tag_span(in, in_len, i, &kind, &name, &name_len);
            (void)kind;
            (void)name;
            (void)name_len;
            if (end == (size_t)-1) {
                i = skip_incomplete_open(in, in_len, i);
                continue;
            }
            i = end; /* drop tag span entirely */
        }
    }
    *out_len = o;
    return SPEECH_SANITIZE_OK;
}
