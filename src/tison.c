#include "tison.h"

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TISON_MAX_KEY_LEN        256
#define TISON_MAX_STRUCT_MEMBERS 256
#define TISON_MAX_NUMBER_LEN     256

#ifndef NDEBUG
#define TISON_UNREACHABLE() \
    (fprintf(stderr, "%s:%d: error: reached unreachable code\n", __FILE__, __LINE__), abort())
#elif defined(__GNUC__) || defined(__clang__)
#define TISON_UNREACHABLE() __builtin_unreachable()
#elif defined(_MSC_VER)
#define TISON_UNREACHABLE() __assume(0)
#else
#define TISON_UNREACHABLE() abort()
#endif

// Forward declare typeinfos needed to distinguish different 1-byte types.
extern Type_Info_Integer typeinfo_bool;
extern Type_Info_Integer typeinfo_char;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static bool span_eq(TisonSpan s, const char *cstr) {
    size_t n = strlen(cstr);
    return (size_t)(s.end - s.begin) == n && memcmp(s.begin, cstr, n) == 0;
}

static bool is_bool_type(const Type_Info *t) {
    return t == &typeinfo_bool.base;
}

// Any 1-byte integer except bool. JsonCStr accepts these.
static bool is_byte_type(const Type_Info *t) {
    return t && t->tag == TYPE_TAG_INTEGER && t->size == 1 && !is_bool_type(t);
}

// Annotations of the member that owns this exact type. A pointee inherits its pointer's member
// for hooks, but must not reapply the member's annotations to itself.
static char **own_annotations(const TisonTypeContext *ctx) {
    if(!ctx->member || ctx->member->type != ctx->type) return NULL;
    return ctx->member->annotations;
}

static const char *annotation_value(char **annotations, const char *name) {
    if(!annotations) return NULL;
    size_t n = strlen(name);
    for(char **a = annotations; *a; a++) {
        const char *s = *a;
        if(strncmp(s, name, n) != 0) continue;
        s += n;
        while(*s == ' ') s++;
        if(*s != '=') continue;
        s++;
        while(*s == ' ') s++;
        return s;
    }
    return NULL;
}

static bool is_object_type(const Type_Info *t) {
    return t && t->tag != TYPE_TAG_VOID && t->size > 0;
}

static bool is_supported_int_size(size_t size) {
    return size == 1 || size == 2 || size == 4 || size == 8;
}

static bool is_count_type(const Type_Info *t) {
    return t->tag == TYPE_TAG_INTEGER && !is_bool_type(t) && is_supported_int_size(t->size);
}

static uint64_t load_uint(const void *src, size_t size) {
    switch(size) {
    case 1: {
        uint8_t v;
        memcpy(&v, src, 1);
        return v;
    }
    case 2: {
        uint16_t v;
        memcpy(&v, src, 2);
        return v;
    }
    case 4: {
        uint32_t v;
        memcpy(&v, src, 4);
        return v;
    }
    case 8: {
        uint64_t v;
        memcpy(&v, src, 8);
        return v;
    }
    default:
        TISON_UNREACHABLE();
    }
}

static int64_t load_int(const void *src, size_t size) {
    switch(size) {
    case 1: {
        int8_t v;
        memcpy(&v, src, 1);
        return v;
    }
    case 2: {
        int16_t v;
        memcpy(&v, src, 2);
        return v;
    }
    case 4: {
        int32_t v;
        memcpy(&v, src, 4);
        return v;
    }
    case 8: {
        int64_t v;
        memcpy(&v, src, 8);
        return v;
    }
    default:
        TISON_UNREACHABLE();
    }
}

static void *load_pointer(const void *src) {
    void *ptr;
    memcpy(&ptr, src, sizeof(ptr));
    return ptr;
}

static bool is_skipped(const Type_Info_Member *m) {
    if(tison_has_annotation(m->annotations, "JsonSkip")) return true;
    const Type_Info *t = m->type;
    return m->name[0] == '\0' && (t->tag == TYPE_TAG_STRUCT || t->tag == TYPE_TAG_UNION) &&
           tison_has_annotation(((const Type_Info_Struct *)t)->annotations, "JsonSkip");
}

typedef struct {
    const Type_Info_Member *member;
    size_t offset;
    size_t index;  // Position among the flattened members, for duplicate tracking
    bool in_union;
} MemberLookup;

// Searches `members`, descending into anonymous structs and unions. `index` counts every
// flattened member visited so far.
static bool find_member(const Type_Info_Member *members, size_t count, TisonSpan key,
                        size_t base_offset, bool in_union, size_t *index, MemberLookup *out) {
    for(size_t i = 0; i < count; i++) {
        const Type_Info_Member *m = &members[i];
        const Type_Info *t = m->type;
        if(is_skipped(m)) continue;
        if(m->name[0] == '\0' && (t->tag == TYPE_TAG_STRUCT || t->tag == TYPE_TAG_UNION)) {
            const Type_Info_Struct *s = (const Type_Info_Struct *)t;
            bool u = in_union || t->tag == TYPE_TAG_UNION;
            if(find_member(s->members, s->members_count, key, base_offset + m->offset, u, index,
                           out)) {
                return true;
            }
            continue;
        }
        if(m->name[0] != '\0' && span_eq(key, m->name)) {
            out->member = m;
            out->offset = base_offset + m->offset;
            out->index = *index;
            out->in_union = in_union;
            return true;
        }
        (*index)++;
    }
    return false;
}

static bool find_member_by_name(const Type_Info_Struct *s, const char *name, MemberLookup *out) {
    TisonSpan key = {name, name + strlen(name)};
    size_t index = 0;
    return find_member(s->members, s->members_count, key, 0, false, &index, out);
}

// True if some member, flattened members included, is annotated `JsonArray=<name>`.
static bool is_derived_count(const Type_Info_Member *members, size_t count, const char *name) {
    for(size_t i = 0; i < count; i++) {
        const Type_Info_Member *m = &members[i];
        const Type_Info *t = m->type;
        if(m->name[0] == '\0' && (t->tag == TYPE_TAG_STRUCT || t->tag == TYPE_TAG_UNION)) {
            const Type_Info_Struct *sub = (const Type_Info_Struct *)t;
            if(is_derived_count(sub->members, sub->members_count, name)) return true;
            continue;
        }
        const char *target = annotation_value(m->annotations, "JsonArray");
        if(target && strcmp(target, name) == 0) return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Enums
// -----------------------------------------------------------------------------

// An enum is read and written as one enumerator name by default. JsonEnumInt makes it a plain
// integer, and JsonFlags an array of enumerator names OR-ed together. Either can annotate the
// enum type or a single member, and the member wins.
typedef enum {
    ENUM_NAMES,
    ENUM_INT,
    ENUM_FLAGS,
} EnumMode;

static const char *enum_label(const Type_Info_Enum *e) {
    return e->name[0] ? e->name : "enum";
}

static uint64_t size_mask(size_t size) {
    return size == 8 ? UINT64_MAX : (((uint64_t)1 << (size * 8)) - 1);
}

static bool enum_is_signed(const Type_Info_Enum *e) {
    for(size_t i = 0; i < e->values_count; i++) {
        if(e->values[i].value < 0) return true;
    }
    return false;
}

static const char MISPLACED_ENUM_ANNOTATION[] =
    "JsonEnumInt and JsonFlags require an enum field, annotate the enum type for arrays";

static bool has_enum_annotation(char **annotations) {
    return tison_has_annotation(annotations, "JsonEnumInt") ||
           tison_has_annotation(annotations, "JsonFlags");
}

static const char *annotations_mode(char **annotations, EnumMode *mode) {
    bool as_int = tison_has_annotation(annotations, "JsonEnumInt");
    bool flags = tison_has_annotation(annotations, "JsonFlags");
    if(as_int && flags) return "JsonEnumInt and JsonFlags are mutually exclusive";
    *mode = as_int ? ENUM_INT : flags ? ENUM_FLAGS : ENUM_NAMES;
    return NULL;
}

static const char *enum_mode(const Type_Info_Enum *e, char **member_annotations, EnumMode *mode) {
    const char *msg;
    if(has_enum_annotation(member_annotations)) {
        msg = annotations_mode(member_annotations, mode);
    } else {
        msg = annotations_mode(e->annotations, mode);
    }
    if(msg) return msg;
    if(*mode == ENUM_FLAGS && enum_is_signed(e)) return "JsonFlags enum has a negative enumerator";
    return NULL;
}

static bool is_flags_tag(const Type_Info_Member *tag) {
    EnumMode mode;
    const Type_Info_Enum *e = (const Type_Info_Enum *)tag->type;
    return !enum_mode(e, tag->annotations, &mode) && mode == ENUM_FLAGS;
}

static const char *tag_variant(const Type_Info_Enum *e, const void *src) {
    size_t size = e->base.size;
    uint64_t bits = load_uint(src, size);
    uint64_t mask = size_mask(size);
    for(size_t i = 0; i < e->values_count; i++) {
        if(((uint64_t)e->values[i].value & mask) == bits) return e->values[i].name;
    }
    return NULL;
}

static const Type_Info_Member *variant_member(const Type_Info_Union *u, const char *variant) {
    if(!variant) return NULL;
    for(size_t i = 0; i < u->members_count; i++) {
        const char *when = annotation_value(u->members[i].annotations, "JsonWhen");
        if(when && strcmp(when, variant) == 0) return &u->members[i];
    }
    return NULL;
}

static const TisonTypeHook *find_hook_in(const TisonOptions *opt, const TisonTypeContext *ctx) {
    for(size_t i = 0; i < opt->hook_count; i++) {
        const TisonTypeHook *h = &opt->hooks[i];
        if(h->match(ctx, h->user_data)) return h;
    }
    return NULL;
}

// Returns the length of a valid UTF-8 sequence at `s`, or 0. Rejects overlongs, surrogates and
// code points above U+10FFFF.
static size_t utf8_valid_seq(const unsigned char *s, const unsigned char *end) {
    unsigned char c = s[0];
    size_t n;
    unsigned char lo = 0x80, hi = 0xBF;
    if(c < 0x80) return 1;
    if(c >= 0xC2 && c <= 0xDF) {
        n = 2;
    } else if(c >= 0xE0 && c <= 0xEF) {
        n = 3;
        if(c == 0xE0) lo = 0xA0;
        if(c == 0xED) hi = 0x9F;
    } else if(c >= 0xF0 && c <= 0xF4) {
        n = 4;
        if(c == 0xF0) lo = 0x90;
        if(c == 0xF4) hi = 0x8F;
    } else {
        return 0;
    }
    if((size_t)(end - s) < n) return 0;
    if(s[1] < lo || s[1] > hi) return 0;
    for(size_t i = 2; i < n; i++) {
        if(s[i] < 0x80 || s[i] > 0xBF) return 0;
    }
    return n;
}

static void tison_render_path(char *out, size_t cap, const TisonPathSegment *path,
                              size_t path_len) {
    size_t len = 0;
    size_t n = path_len < TISON_PATH_MAX_SEGMENTS ? path_len : TISON_PATH_MAX_SEGMENTS;
    out[0] = '\0';
    for(size_t i = 0; i < n && len + 1 < cap; i++) {
        const TisonPathSegment *s = &path[i];
        int w;
        if(s->is_index) {
            w = snprintf(out + len, cap - len, "[%zu]", s->index);
        } else {
            w = snprintf(out + len, cap - len, ".%.*s", (int)(s->key.end - s->key.begin),
                         s->key.begin);
        }
        if(w < 0) break;
        len += (size_t)w;
    }
    if(len >= cap) out[cap - 1] = '\0';
}

void *tison_alloc(TisonParser *p, size_t size) {
    if(p->failed) return NULL;
    if(size == 0) size = 1;
    const TisonAllocator *a = &p->options.allocator;
    void *ptr = a->alloc ? a->alloc(a->ctx, size) : malloc(size);
    if(!ptr) tison_fail(p, TISON_ERR_ALLOC, "failed to allocate %zu bytes", size);
    return ptr;
}

const char *tison_strerror(TisonErrorCode code) {
    switch(code) {
    case TISON_OK:
        return "ok";
    case TISON_ERR_SYNTAX:
        return "syntax error";
    case TISON_ERR_TYPE:
        return "type mismatch";
    case TISON_ERR_UNSUPPORTED:
        return "unsupported type";
    case TISON_ERR_RANGE:
        return "value out of range";
    case TISON_ERR_STRING:
        return "invalid string";
    case TISON_ERR_ALLOC:
        return "allocation failure";
    case TISON_ERR_DEPTH:
        return "maximum depth exceeded";
    case TISON_ERR_DUPLICATE:
        return "duplicate field";
    case TISON_ERR_UNKNOWN_FIELD:
        return "unknown field";
    case TISON_ERR_LENGTH:
        return "array length mismatch";
    case TISON_ERR_HOOK:
        return "hook failure";
    case TISON_ERR_IO:
        return "output error";
    }
    return "unknown error";
}

// -----------------------------------------------------------------------------
// Parser state and errors
// -----------------------------------------------------------------------------

void tison_parser_init(TisonParser *p, const char *source, size_t source_size,
                       const TisonOptions *options) {
    memset(p, 0, sizeof(*p));
    p->begin = source;
    p->cur = source;
    p->end = source + source_size;
    if(options) p->options = *options;
    if(p->options.max_depth == 0) p->options.max_depth = TISON_DEFAULT_MAX_DEPTH;
}

bool tison_ok(const TisonParser *p) {
    return !p->failed;
}

const TisonError *tison_parser_error(const TisonParser *p) {
    return &p->error;
}

static void render_path(TisonParser *p) {
    tison_render_path(p->error.path, sizeof(p->error.path), p->path, p->path_len);
}

bool tison_fail(TisonParser *p, TisonErrorCode code, const char *fmt, ...) {
    if(p->failed) return false;
    p->failed = true;
    p->error.code = code;
    p->error.offset = (size_t)(p->cur - p->begin);
    p->error.line = 1;
    p->error.column = 1;
    for(const char *c = p->begin; c < p->cur; c++) {
        if(*c == '\n') {
            p->error.line++;
            p->error.column = 1;
        } else {
            p->error.column++;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->error.message, sizeof(p->error.message), fmt, ap);
    va_end(ap);
    render_path(p);
    return false;
}

static bool fail_unexpected(TisonParser *p, const char *what) {
    if(p->cur >= p->end) return tison_fail(p, TISON_ERR_SYNTAX, "unexpected end of input");
    return tison_fail(p, TISON_ERR_SYNTAX, "expected %s", what);
}

static bool enter(TisonParser *p) {
    if(p->depth >= p->options.max_depth) {
        return tison_fail(p, TISON_ERR_DEPTH, "nesting deeper than %zu", p->options.max_depth);
    }
    p->depth++;
    return true;
}

static void path_push(TisonParser *p) {
    if(p->path_len < TISON_PATH_MAX_SEGMENTS) {
        memset(&p->path[p->path_len], 0, sizeof(p->path[0]));
    }
    p->path_len++;
}

static TisonPathSegment *path_top(TisonParser *p) {
    if(p->path_len == 0 || p->path_len > TISON_PATH_MAX_SEGMENTS) return NULL;
    return &p->path[p->path_len - 1];
}

TisonMark tison_mark(const TisonParser *p) {
    TisonMark m = {p->cur, p->depth, p->path_len};
    return m;
}

void tison_reset(TisonParser *p, TisonMark mark) {
    p->cur = mark.cur;
    p->depth = mark.depth;
    p->path_len = mark.path_len;
}

// -----------------------------------------------------------------------------
// Scanner
// -----------------------------------------------------------------------------

void tison_skip_ws(TisonParser *p) {
    while(p->cur < p->end &&
          (*p->cur == ' ' || *p->cur == '\t' || *p->cur == '\n' || *p->cur == '\r')) {
        p->cur++;
    }
}

// Peek the next non-white space character in the source.
int tison_peek(TisonParser *p) {
    tison_skip_ws(p);
    return p->cur < p->end ? (unsigned char)*p->cur : -1;
}

bool tison_consume(TisonParser *p, char ch) {
    if(p->failed) return false;
    if(tison_peek(p) == (unsigned char)ch) {
        p->cur++;
        return true;
    }
    return false;
}

bool tison_expect(TisonParser *p, char ch) {
    if(p->failed) return false;
    if(tison_consume(p, ch)) return true;
    char what[4] = {'\'', ch, '\'', '\0'};
    return fail_unexpected(p, what);
}

bool tison_at_end(TisonParser *p) {
    return tison_peek(p) == -1;
}

static int hex_digit(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool read_hex4(const char *s, const char *end, uint32_t *out) {
    if(end - s < 4) return false;
    uint32_t v = 0;
    for(int i = 0; i < 4; i++) {
        int d = hex_digit(s[i]);
        if(d < 0) return false;
        v = (v << 4) | (uint32_t)d;
    }
    *out = v;
    return true;
}

static size_t utf8_len(uint32_t cp) {
    if(cp < 0x80) return 1;
    if(cp < 0x800) return 2;
    if(cp < 0x10000) return 3;
    return 4;
}

bool tison_scan_string(TisonParser *p, TisonSpan *out, TisonStringInfo *info) {
    if(p->failed) return false;
    if(tison_peek(p) != '"') return fail_unexpected(p, "string");
    p->cur++;
    TisonStringInfo inf = {0, false, false};
    const char *start = p->cur;
    while(true) {
        if(p->cur >= p->end) return tison_fail(p, TISON_ERR_STRING, "unterminated string");
        unsigned char c = (unsigned char)*p->cur;
        if(c == '"') break;
        if(c < 0x20) return tison_fail(p, TISON_ERR_STRING, "unescaped control character");
        if(c == '\\') {
            inf.has_escapes = true;
            if(p->end - p->cur < 2) return tison_fail(p, TISON_ERR_STRING, "unterminated string");
            char e = p->cur[1];
            if(e == 'u') {
                uint32_t cp;
                if(!read_hex4(p->cur + 2, p->end, &cp)) {
                    return tison_fail(p, TISON_ERR_STRING, "invalid \\u escape");
                }
                const char *esc = p->cur;
                p->cur += 6;
                if(cp >= 0xDC00 && cp <= 0xDFFF) {
                    p->cur = esc;
                    return tison_fail(p, TISON_ERR_STRING, "unpaired low surrogate");
                }
                if(cp >= 0xD800 && cp <= 0xDBFF) {
                    uint32_t lo;
                    if(p->end - p->cur < 2 || p->cur[0] != '\\' || p->cur[1] != 'u' ||
                       !read_hex4(p->cur + 2, p->end, &lo) || lo < 0xDC00 || lo > 0xDFFF) {
                        p->cur = esc;
                        return tison_fail(p, TISON_ERR_STRING, "unpaired high surrogate");
                    }
                    p->cur += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                if(cp == 0) inf.has_nul = true;
                inf.decoded_len += utf8_len(cp);
                continue;
            }
            switch(e) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                break;
            default:
                return tison_fail(p, TISON_ERR_STRING, "invalid escape '\\%c'", e);
            }
            p->cur += 2;
            inf.decoded_len++;
            continue;
        }
        size_t n = utf8_valid_seq((const unsigned char *)p->cur, (const unsigned char *)p->end);
        if(n == 0) return tison_fail(p, TISON_ERR_STRING, "invalid UTF-8");
        p->cur += n;
        inf.decoded_len += n;
    }
    if(out) {
        out->begin = start;
        out->end = p->cur;
    }
    if(info) *info = inf;
    p->cur++;
    return true;
}

static size_t put_utf8(char *out, uint32_t cp) {
    if(cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if(cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if(cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t tison_decode_string(TisonSpan raw, char *out) {
    const char *s = raw.begin;
    size_t n = 0;
    while(s < raw.end) {
        if(*s != '\\') {
            out[n++] = *s++;
            continue;
        }
        char e = s[1];
        s += 2;
        switch(e) {
        case 'b':
            out[n++] = '\b';
            break;
        case 'f':
            out[n++] = '\f';
            break;
        case 'n':
            out[n++] = '\n';
            break;
        case 'r':
            out[n++] = '\r';
            break;
        case 't':
            out[n++] = '\t';
            break;
        case 'u': {
            uint32_t cp, lo;
            read_hex4(s, raw.end, &cp);
            s += 4;
            if(cp >= 0xD800 && cp <= 0xDBFF) {
                read_hex4(s + 2, raw.end, &lo);
                s += 6;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            }
            n += put_utf8(out + n, cp);
            break;
        }
        case '"':
        case '\\':
        case '/':
            out[n++] = e;
            break;
        default:
            TISON_UNREACHABLE();
        }
    }
    return n;
}

static bool is_number_start(int c) {
    return c == '-' || (c >= '0' && c <= '9');
}

static bool is_digit(const TisonParser *p) {
    return p->cur < p->end && *p->cur >= '0' && *p->cur <= '9';
}

bool tison_scan_number(TisonParser *p, TisonSpan *out, bool *is_integer) {
    if(p->failed) return false;
    int c = tison_peek(p);
    if(c != '-' && !(c >= '0' && c <= '9')) return fail_unexpected(p, "number");
    const char *start = p->cur;
    bool integer = true;
    if(*p->cur == '-') p->cur++;
    if(!is_digit(p)) return fail_unexpected(p, "digit");
    if(*p->cur == '0') {
        p->cur++;
        if(is_digit(p)) return tison_fail(p, TISON_ERR_SYNTAX, "leading zeros are not allowed");
    } else {
        while(is_digit(p)) p->cur++;
    }
    if(p->cur < p->end && *p->cur == '.') {
        integer = false;
        p->cur++;
        if(!is_digit(p)) return fail_unexpected(p, "digit after '.'");
        while(is_digit(p)) p->cur++;
    }
    if(p->cur < p->end && (*p->cur == 'e' || *p->cur == 'E')) {
        integer = false;
        p->cur++;
        if(p->cur < p->end && (*p->cur == '+' || *p->cur == '-')) p->cur++;
        if(!is_digit(p)) return fail_unexpected(p, "digit in exponent");
        while(is_digit(p)) p->cur++;
    }
    if(out) {
        out->begin = start;
        out->end = p->cur;
    }
    if(is_integer) *is_integer = integer;
    return true;
}

bool tison_scan_literal(TisonParser *p, const char *literal) {
    if(p->failed) return false;
    tison_skip_ws(p);
    size_t n = strlen(literal);
    if((size_t)(p->end - p->cur) < n || memcmp(p->cur, literal, n) != 0) {
        return fail_unexpected(p, literal);
    }
    p->cur += n;
    return true;
}

bool tison_array_begin(TisonParser *p, TisonArrayIter *it) {
    if(p->failed) return false;
    if(tison_peek(p) != '[') {
        if(p->cur >= p->end) return fail_unexpected(p, "array");
        return tison_fail(p, TISON_ERR_TYPE, "expected array");
    }
    if(!enter(p)) return false;
    p->cur++;
    it->index = 0;
    it->started = false;
    path_push(p);
    TisonPathSegment *s = path_top(p);
    if(s) s->is_index = true;
    return true;
}

bool tison_array_next(TisonParser *p, TisonArrayIter *it) {
    if(p->failed) return false;
    if(!it->started) {
        it->started = true;
        if(tison_consume(p, ']')) goto done;
    } else {
        if(tison_consume(p, ']')) goto done;
        if(!tison_expect(p, ',')) return false;
        it->index++;
        if(tison_peek(p) == ']') return fail_unexpected(p, "value");  // Trailing comma
    }
    TisonPathSegment *s = path_top(p);
    if(s) s->index = it->index;
    return true;

done:
    p->depth--;
    p->path_len--;
    return false;
}

bool tison_object_begin(TisonParser *p, TisonObjectIter *it) {
    if(p->failed) return false;
    if(tison_peek(p) != '{') {
        if(p->cur >= p->end) return fail_unexpected(p, "object");
        return tison_fail(p, TISON_ERR_TYPE, "expected object");
    }
    if(!enter(p)) return false;
    p->cur++;
    it->started = false;
    path_push(p);
    return true;
}

bool tison_object_next(TisonParser *p, TisonObjectIter *it, TisonSpan *key, TisonStringInfo *info) {
    if(p->failed) return false;
    if(!it->started) {
        it->started = true;
        if(tison_consume(p, '}')) goto done;
    } else {
        if(tison_consume(p, '}')) goto done;
        if(!tison_expect(p, ',')) return false;
    }
    if(tison_peek(p) != '"') return fail_unexpected(p, "string key");

    TisonSpan k;
    if(!tison_scan_string(p, &k, info)) return false;
    TisonPathSegment *s = path_top(p);

    if(s) s->key = k;
    if(key) *key = k;
    return tison_expect(p, ':');

done:
    p->depth--;
    p->path_len--;
    return false;
}

bool tison_skip_value(TisonParser *p) {
    if(p->failed) return false;
    switch(tison_peek(p)) {
    case '{': {
        TisonObjectIter it;
        if(!tison_object_begin(p, &it)) return false;
        while(tison_object_next(p, &it, NULL, NULL)) {
            if(!tison_skip_value(p)) return false;
        }
        return tison_ok(p);
    }
    case '[': {
        TisonArrayIter it;
        if(!tison_array_begin(p, &it)) return false;
        while(tison_array_next(p, &it)) {
            if(!tison_skip_value(p)) return false;
        }
        return tison_ok(p);
    }
    case '"':
        return tison_scan_string(p, NULL, NULL);
    case 't':
        return tison_scan_literal(p, "true");
    case 'f':
        return tison_scan_literal(p, "false");
    case 'n':
        return tison_scan_literal(p, "null");
    default:
        if(!is_number_start(tison_peek(p))) return fail_unexpected(p, "value");
        return tison_scan_number(p, NULL, NULL);
    }
}

bool tison_count_array(TisonParser *p, size_t *out_count) {
    TisonArrayIter it;
    size_t count = 0;
    if(!tison_array_begin(p, &it)) return false;
    while(tison_array_next(p, &it)) {
        if(!tison_skip_value(p)) return false;
        count++;
    }
    if(!tison_ok(p)) return false;
    *out_count = count;
    return true;
}

// -----------------------------------------------------------------------------
// Primitive values
// -----------------------------------------------------------------------------

bool tison_has_annotation(char **annotations, const char *name) {
    if(!annotations) return false;
    for(char **a = annotations; *a; a++) {
        if(strcmp(*a, name) == 0) return true;
    }
    return false;
}

static bool type_mismatch(TisonParser *p, const char *expected) {
    int c = tison_peek(p);
    if(c == -1) return fail_unexpected(p, expected);
    if(c == 'n') return tison_fail(p, TISON_ERR_TYPE, "expected %s, got null", expected);
    return tison_fail(p, TISON_ERR_TYPE, "expected %s", expected);
}

static bool scan_integer(TisonParser *p, bool *negative, uint64_t *magnitude) {
    TisonMark start = tison_mark(p);

    TisonSpan span;
    bool integer;
    if(!tison_scan_number(p, &span, &integer)) return false;
    if(!integer) {
        tison_reset(p, start);
        tison_skip_ws(p);
        return tison_fail(p, TISON_ERR_TYPE, "expected integer, got %.*s",
                          (int)(span.end - span.begin), span.begin);
    }

    const char *s = span.begin;
    *negative = *s == '-';
    if(*negative) s++;
    uint64_t v = 0;
    for(; s < span.end; s++) {
        unsigned d = (unsigned)(*s - '0');
        if(v > (UINT64_MAX - d) / 10) {
            tison_reset(p, start);
            tison_skip_ws(p);
            return tison_fail(p, TISON_ERR_RANGE, "integer %.*s out of range",
                              (int)(span.end - span.begin), span.begin);
        }
        v = v * 10 + d;
    }
    *magnitude = v;
    return true;
}

static bool to_int64(bool negative, uint64_t mag, int64_t *out) {
    if(negative) {
        if(mag > (uint64_t)INT64_MAX + 1) return false;
        *out = mag == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)mag;
    } else {
        if(mag > (uint64_t)INT64_MAX) return false;
        *out = (int64_t)mag;
    }
    return true;
}

static void store_int(void *dst, size_t size, uint64_t bits) {
    switch(size) {
    case 1: {
        uint8_t v = (uint8_t)bits;
        memcpy(dst, &v, 1);
    } break;
    case 2: {
        uint16_t v = (uint16_t)bits;
        memcpy(dst, &v, 2);
    } break;
    case 4: {
        uint32_t v = (uint32_t)bits;
        memcpy(dst, &v, 4);
    } break;
    case 8: {
        memcpy(dst, &bits, 8);
    } break;
    default:
        TISON_UNREACHABLE();
    }
}

static bool fit_bits(size_t size, bool is_signed, bool neg, uint64_t mag, uint64_t *out) {
    unsigned bits = (unsigned)size * 8;
    if(is_signed) {
        int64_t v;
        int64_t max = (int64_t)(UINT64_MAX >> (65 - bits));
        if(!to_int64(neg, mag, &v) || v > max || v < -max - 1) return false;
        *out = (uint64_t)v;
        return true;
    }
    uint64_t max = UINT64_MAX >> (64 - bits);
    if((neg && mag != 0) || mag > max) return false;
    *out = neg ? 0 : mag;
    return true;
}

static bool fit_int(const Type_Info_Integer *t, bool neg, uint64_t mag, uint64_t *out) {
    return fit_bits(t->base.size, t->is_signed, neg, mag, out);
}

static bool parse_integer(TisonParser *p, void *dst, const Type_Info_Integer *t) {
    size_t size = t->base.size;
    if(!is_supported_int_size(size)) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "integer of size %zu", size);
    }
    if(!is_number_start(tison_peek(p))) return type_mismatch(p, "integer");
    const char *start = p->cur;
    bool neg;
    uint64_t mag, bitsv;
    if(!scan_integer(p, &neg, &mag)) return false;
    if(!fit_int(t, neg, mag, &bitsv)) {
        const char *end = p->cur;
        p->cur = start;
        return tison_fail(p, TISON_ERR_RANGE, "%.*s does not fit in a %s %zu-bit integer",
                          (int)(end - start), start, t->is_signed ? "signed" : "unsigned",
                          size * 8);
    }
    store_int(dst, size, bitsv);
    return true;
}

static bool parse_float(TisonParser *p, void *dst, const Type_Info *t) {
    if(!is_number_start(tison_peek(p))) return type_mismatch(p, "number");
    const char *start = p->cur;
    TisonSpan span;
    if(!tison_scan_number(p, &span, NULL)) return false;
    size_t len = (size_t)(span.end - span.begin);
    if(len >= TISON_MAX_NUMBER_LEN) {
        // TODO: convert long literals without a fixed buffer.
        p->cur = start;
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "number literal longer than %d bytes",
                          TISON_MAX_NUMBER_LEN - 1);
    }

    char buf[TISON_MAX_NUMBER_LEN];
    memcpy(buf, span.begin, len);
    buf[len] = '\0';

    // TODO: strto* depends on LC_NUMERIC. Replace with a locale-free converter.
    bool finite;
    if(t->size == sizeof(float)) {
        float v = strtof(buf, NULL);
        finite = isfinite(v);
        memcpy(dst, &v, sizeof(v));
    } else if(t->size == sizeof(double)) {
        double v = strtod(buf, NULL);
        finite = isfinite(v);
        memcpy(dst, &v, sizeof(v));
    } else if(t->size == sizeof(long double)) {
        long double v = strtold(buf, NULL);
        finite = isfinite(v);
        memcpy(dst, &v, sizeof(v));
    } else {
        p->cur = start;
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "float of size %zu", t->size);
    }

    if(!finite) {
        p->cur = start;
        return tison_fail(p, TISON_ERR_RANGE, "%s overflows a %zu-byte float", buf, t->size);
    }
    return true;
}

static bool parse_bool(TisonParser *p, void *dst) {
    int c = tison_peek(p);
    bool v;
    if(c == 't') {
        if(!tison_scan_literal(p, "true")) return false;
        v = true;
    } else if(c == 'f') {
        if(!tison_scan_literal(p, "false")) return false;
        v = false;
    } else {
        return type_mismatch(p, "boolean");
    }
    memcpy(dst, &v, sizeof(v));
    return true;
}

static bool key_text(TisonSpan raw, const TisonStringInfo *info, char *buf, size_t cap,
                     TisonSpan *out) {
    if(!info->has_escapes) {
        *out = raw;
        return true;
    }
    if(info->decoded_len > cap) return false;
    size_t n = tison_decode_string(raw, buf);
    out->begin = buf;
    out->end = buf + n;
    return true;
}

// Parses one enumerator name into `*value`.
static bool parse_enumerator(TisonParser *p, const Type_Info_Enum *e, long long *value) {
    if(tison_peek(p) != '"') return type_mismatch(p, "enumerator name");
    const char *start = p->cur;
    TisonSpan raw, name;
    TisonStringInfo info;
    char buf[TISON_MAX_KEY_LEN];
    if(!tison_scan_string(p, &raw, &info)) return false;
    if(key_text(raw, &info, buf, sizeof(buf), &name)) {
        for(size_t i = 0; i < e->values_count; i++) {
            if(span_eq(name, e->values[i].name)) {
                *value = e->values[i].value;
                return true;
            }
        }
    }
    p->cur = start;
    return tison_fail(p, TISON_ERR_RANGE, "unknown enumerator \"%.*s\" for %s",
                      (int)(raw.end - raw.begin), raw.begin, enum_label(e));
}

static bool parse_enum_int(TisonParser *p, void *dst, const Type_Info_Enum *e) {
    if(!is_number_start(tison_peek(p))) return type_mismatch(p, "integer");
    const char *start = p->cur;
    bool neg;
    uint64_t mag, bits;
    if(!scan_integer(p, &neg, &mag)) return false;
    if(!fit_bits(e->base.size, enum_is_signed(e), neg, mag, &bits)) {
        const char *end = p->cur;
        p->cur = start;
        return tison_fail(p, TISON_ERR_RANGE, "%.*s does not fit in %s", (int)(end - start), start,
                          enum_label(e));
    }
    store_int(dst, e->base.size, bits);
    return true;
}

// Duplicate names are accepted, since OR-ing them again changes nothing.
static bool parse_enum_flags(TisonParser *p, void *dst, const Type_Info_Enum *e) {
    if(tison_peek(p) != '[') return type_mismatch(p, "array of enumerator names");
    uint64_t mask = size_mask(e->base.size);
    uint64_t bits = 0;
    TisonArrayIter it;
    if(!tison_array_begin(p, &it)) return false;
    while(tison_array_next(p, &it)) {
        long long value;
        if(!parse_enumerator(p, e, &value)) return false;
        bits |= (uint64_t)value & mask;
    }
    if(!tison_ok(p)) return false;
    store_int(dst, e->base.size, bits);
    return true;
}

static bool parse_enum(TisonParser *p, void *dst, const Type_Info_Enum *e, char **annotations) {
    size_t size = e->base.size;
    if(!is_supported_int_size(size)) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "enum of size %zu", size);
    }
    EnumMode mode;
    const char *msg = enum_mode(e, annotations, &mode);
    if(msg) return tison_fail(p, TISON_ERR_UNSUPPORTED, "%s: %s", enum_label(e), msg);

    switch(mode) {
    case ENUM_NAMES: {
        long long value;
        if(!parse_enumerator(p, e, &value)) return false;
        store_int(dst, size, (uint64_t)value);
        return true;
    }
    case ENUM_INT:
        return parse_enum_int(p, dst, e);
    case ENUM_FLAGS:
        return parse_enum_flags(p, dst, e);
    }
    TISON_UNREACHABLE();
}

static bool parse_cstr_array(TisonParser *p, char *dst, size_t capacity) {
    if(tison_peek(p) != '"') return type_mismatch(p, "string");
    const char *start = p->cur;
    TisonSpan raw;
    TisonStringInfo info;
    if(!tison_scan_string(p, &raw, &info)) return false;
    if(info.has_nul) {
        p->cur = start;
        return tison_fail(p, TISON_ERR_STRING, "C string contains \\u0000");
    }
    if(info.decoded_len >= capacity) {
        p->cur = start;
        return tison_fail(p, TISON_ERR_LENGTH, "string of %zu bytes does not fit in char[%zu]",
                          info.decoded_len, capacity);
    }
    size_t n = tison_decode_string(raw, dst);
    dst[n] = '\0';
    return true;
}

static bool parse_cstr_pointer(TisonParser *p, void *dst) {
    if(tison_peek(p) == 'n') {
        if(!tison_scan_literal(p, "null")) return false;
        char *null_ptr = NULL;
        memcpy(dst, &null_ptr, sizeof(null_ptr));
        return true;
    }
    if(tison_peek(p) != '"') return type_mismatch(p, "string");
    const char *start = p->cur;
    TisonSpan raw;
    TisonStringInfo info;
    if(!tison_scan_string(p, &raw, &info)) return false;
    if(info.has_nul) {
        p->cur = start;
        return tison_fail(p, TISON_ERR_STRING, "C string contains \\u0000");
    }
    char *s = tison_alloc(p, info.decoded_len + 1);
    if(!s) return false;
    size_t n = tison_decode_string(raw, s);
    s[n] = '\0';
    memcpy(dst, &s, sizeof(s));
    return true;
}

// -----------------------------------------------------------------------------
// Pointers
// -----------------------------------------------------------------------------

static void store_pointer(void *dst, void *ptr) {
    memcpy(dst, &ptr, sizeof(ptr));
}

static bool parse_owned_pointer(TisonParser *p, void *dst, const Type_Info *pointee,
                                const Type_Info_Member *member) {
    if(tison_peek(p) == 'n') {
        if(!tison_scan_literal(p, "null")) return false;
        store_pointer(dst, NULL);
        return true;
    }
    void *obj = tison_alloc(p, pointee->size);
    if(!obj) return false;
    memset(obj, 0, pointee->size);
    store_pointer(dst, obj);
    TisonTypeContext ctx = {pointee, member};
    return tison_parse_value(p, obj, &ctx);
}

// `T *items` annotated `JsonArray=count`. The count is checked against its field before
// allocating, and written before the elements so a partial result stays consistent.
static bool parse_owned_array(TisonParser *p, void *dst, const Type_Info_Member *member,
                              const Type_Info_Integer *count_type, void *count_dst) {
    const Type_Info *elem = ((const Type_Info_Pointer *)member->type)->pointer_to;
    if(!is_object_type(elem)) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonArray on a void or function pointer");
    }
    size_t count = 0;
    bool is_null = tison_peek(p) == 'n';
    if(is_null) {
        if(!tison_scan_literal(p, "null")) return false;
    } else {
        TisonMark start = tison_mark(p);
        if(!tison_count_array(p, &count)) return false;
        tison_reset(p, start);
        tison_skip_ws(p);
    }
    uint64_t bits;
    if(!fit_int(count_type, false, count, &bits)) {
        return tison_fail(p, TISON_ERR_RANGE, "%zu elements do not fit in the count field", count);
    }
    if(count > SIZE_MAX / elem->size) {
        return tison_fail(p, TISON_ERR_RANGE, "array of %zu elements overflows size_t", count);
    }
    void *items = NULL;
    if(count > 0) {
        items = tison_alloc(p, count * elem->size);
        if(!items) return false;
        memset(items, 0, count * elem->size);
    }
    store_pointer(dst, items);
    store_int(count_dst, count_type->base.size, bits);
    if(is_null) return true;

    TisonArrayIter it;
    if(!tison_array_begin(p, &it)) return false;
    while(tison_array_next(p, &it)) {
        // Only a hook consuming the wrong amount of input can desync the two passes.
        if(it.index >= count) {
            return tison_fail(
                p, TISON_ERR_LENGTH,
                "more elements than counted (a hook may have consumed too little input)");
        }
        TisonTypeContext ctx = {elem, member};
        if(!tison_parse_value(p, (unsigned char *)items + it.index * elem->size, &ctx)) {
            return false;
        }
    }
    return tison_ok(p);
}

// -----------------------------------------------------------------------------
// Aggregates
// -----------------------------------------------------------------------------

static bool parse_struct_array(TisonParser *p, unsigned char *dst, const Type_Info_Struct *s,
                               const MemberLookup *found, const char *count_name) {
    MemberLookup count;
    if(!find_member_by_name(s, count_name, &count) || count.in_union) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonArray=%s: no such field", count_name);
    }
    const Type_Info *ct = count.member->type;
    if(!is_count_type(ct)) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonArray=%s: count field is not an integer",
                          count_name);
    }
    return parse_owned_array(p, dst + found->offset, found->member, (const Type_Info_Integer *)ct,
                             dst + count.offset);
}

static const TisonTypeHook *find_hook(TisonParser *p, const TisonTypeContext *ctx);

static bool resolve_key(const Type_Info_Struct *s, TisonSpan raw, const TisonStringInfo *info,
                        MemberLookup *out) {
    char buf[TISON_MAX_KEY_LEN];
    TisonSpan key;
    size_t index = 0;
    return key_text(raw, info, buf, sizeof(buf), &key) &&
           find_member(s->members, s->members_count, key, 0, false, &index, out);
}

// True if a key resolving to flattened member `index` appears in the object at `start` before
// the key at `stop` (or anywhere, if `stop` is NULL). Scans a copy of the parser over source
// that was already validated, so `p` is left untouched.
static bool key_seen_earlier(const TisonParser *p, const Type_Info_Struct *s, TisonMark start,
                             const char *stop, size_t index) {
    TisonParser q = *p;
    tison_reset(&q, start);
    TisonObjectIter it;
    TisonSpan raw;
    TisonStringInfo info;
    if(!tison_object_begin(&q, &it)) return false;
    while(tison_object_next(&q, &it, &raw, &info)) {
        if(stop && raw.begin >= stop) return false;
        MemberLookup found;
        if(resolve_key(s, raw, &info, &found) && found.index == index) return true;
        if(!tison_skip_value(&q)) return false;
    }
    return false;
}

static bool seen_before(const TisonParser *p, const Type_Info_Struct *s, TisonMark start,
                        const uint64_t *seen, const char *key_pos, size_t index) {
    if(index < TISON_MAX_STRUCT_MEMBERS) return seen[index / 64] & ((uint64_t)1 << (index % 64));
    return key_seen_earlier(p, s, start, key_pos, index);
}

static void mark_seen(uint64_t *seen, size_t index) {
    if(index < TISON_MAX_STRUCT_MEMBERS) seen[index / 64] |= (uint64_t)1 << (index % 64);
}

// -----------------------------------------------------------------------------
// Tagged unions
// -----------------------------------------------------------------------------

// Adjacently tagged: `{"kind": "SHAPE_SPHERE", "shape": {...}}`, where the union field is
// annotated `JsonTag=kind` and each union member `JsonWhen=<ENUMERATOR>`.
// TODO: other layouts (externally and internally tagged), selectable per union.

typedef struct {
    MemberLookup tag;
    const char *variant;
} TagInfo;

static bool resolve_tag(TisonParser *p, const Type_Info_Struct *s, const char *tag_name,
                        TagInfo *out) {
    if(!find_member_by_name(s, tag_name, &out->tag) || out->tag.in_union) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonTag=%s: no such field", tag_name);
    }
    if(out->tag.member->type->tag != TYPE_TAG_ENUM) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonTag=%s: tag field is not an enum",
                          tag_name);
    }
    if(is_flags_tag(out->tag.member)) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonTag=%s: a JsonFlags enum cannot be a tag",
                          tag_name);
    }
    return true;
}

static const Type_Info_Enum_Value *find_enumerator(const Type_Info_Enum *e, const char *name) {
    for(size_t i = 0; i < e->values_count; i++) {
        if(strcmp(e->values[i].name, name) == 0) return &e->values[i];
    }
    return NULL;
}

static bool parse_tagged_union(TisonParser *p, unsigned char *dst, const Type_Info_Struct *s,
                               const MemberLookup *found, const char *tag_name) {
    TagInfo tag;
    if(!resolve_tag(p, s, tag_name, &tag)) return false;
    const Type_Info_Enum *e = (const Type_Info_Enum *)tag.tag.member->type;
    const Type_Info_Union *u = (const Type_Info_Union *)found->member->type;

    size_t size = e->base.size;
    if(!is_supported_int_size(size)) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "enum of size %zu", size);
    }
    tag.variant = tag_variant(e, dst + tag.tag.offset);

    for(size_t i = 0; i < u->members_count; i++) {
        const Type_Info_Member *m = &u->members[i];
        const char *when = annotation_value(m->annotations, "JsonWhen");
        if(!when) continue;
        if(!find_enumerator(e, when)) {
            return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonWhen=%s is not an enumerator of %s",
                              when, e->name[0] ? e->name : "the tag enum");
        }
        for(size_t j = 0; j < i; j++) {
            const char *other = annotation_value(u->members[j].annotations, "JsonWhen");
            if(other && strcmp(other, when) == 0) {
                return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonWhen=%s is used twice", when);
            }
        }
    }

    const Type_Info_Member *selected = variant_member(u, tag.variant);
    if(!selected) {
        if(tison_peek(p) == 'n') return tison_scan_literal(p, "null");
        return tison_fail(p, TISON_ERR_TYPE, "variant %s has no payload",
                          tag.variant ? tag.variant : "(undeclared)");
    }
    TisonTypeContext ctx = {selected->type, selected};
    return tison_parse_value(p, dst + found->offset + selected->offset, &ctx);
}

// -----------------------------------------------------------------------------
// Structs
// -----------------------------------------------------------------------------

typedef enum {
    FIELD_DEFAULT,
    FIELD_ARRAY,
    FIELD_TAGGED_UNION,
} FieldKind;

static FieldKind field_kind(TisonParser *p, const MemberLookup *found, const char **arg) {
    TisonTypeContext ctx = {found->member->type, found->member};
    if(find_hook(p, &ctx)) return FIELD_DEFAULT;
    char **ann = found->member->annotations;
    Type_Info_Tag tag = found->member->type->tag;
    if(tag == TYPE_TAG_POINTER && (*arg = annotation_value(ann, "JsonArray"))) return FIELD_ARRAY;
    if(tag == TYPE_TAG_UNION && (*arg = annotation_value(ann, "JsonTag"))) {
        return FIELD_TAGGED_UNION;
    }
    return FIELD_DEFAULT;
}

// Parses tagged unions whose key came before their tag, now that every tag is known.
static bool parse_deferred_unions(TisonParser *p, unsigned char *dst, const Type_Info_Struct *s,
                                  TisonMark start) {
    TisonMark end = tison_mark(p);
    tison_reset(p, start);
    TisonObjectIter it;
    TisonSpan raw;
    TisonStringInfo info;
    if(!tison_object_begin(p, &it)) return false;
    while(tison_object_next(p, &it, &raw, &info)) {
        MemberLookup found;
        const char *tag_name = NULL;
        if(resolve_key(s, raw, &info, &found) &&
           field_kind(p, &found, &tag_name) == FIELD_TAGGED_UNION) {
            TagInfo tag;
            if(!resolve_tag(p, s, tag_name, &tag)) return false;
            if(!key_seen_earlier(p, s, start, raw.begin, tag.tag.index)) {
                if(!key_seen_earlier(p, s, start, NULL, tag.tag.index)) {
                    return tison_fail(p, TISON_ERR_TYPE, "tag field \"%s\" is missing", tag_name);
                }
                if(!parse_tagged_union(p, dst, s, &found, tag_name)) return false;
                continue;
            }
        }
        if(!tison_skip_value(p)) return false;
    }
    if(!tison_ok(p)) return false;
    tison_reset(p, end);
    return true;
}

static bool parse_struct(TisonParser *p, void *dst, const Type_Info_Struct *s) {
    uint64_t seen[TISON_MAX_STRUCT_MEMBERS / 64] = {0};
    unsigned char *base = dst;
    bool deferred = false;
    TisonMark start = tison_mark(p);
    TisonObjectIter it;
    TisonSpan raw;
    TisonStringInfo info;
    if(!tison_object_begin(p, &it)) return false;

    while(tison_object_next(p, &it, &raw, &info)) {
        MemberLookup found = {0};
        if(!resolve_key(s, raw, &info, &found)) {
            if(p->options.flags & TISON_FLAG_IGNORE_UNKNOWN_FIELDS) {
                if(!tison_skip_value(p)) return false;
                continue;
            }
            return tison_fail(p, TISON_ERR_UNKNOWN_FIELD, "unknown field \"%.*s\" in %s",
                              (int)(raw.end - raw.begin), raw.begin,
                              s->name[0] ? s->name : "struct");
        }
        if(found.in_union) {
            return tison_fail(p, TISON_ERR_UNSUPPORTED, "field \"%s\" is inside an anonymous union",
                              found.member->name);
        }
        if(seen_before(p, s, start, seen, raw.begin, found.index)) {
            return tison_fail(p, TISON_ERR_DUPLICATE, "duplicate field \"%s\"", found.member->name);
        }
        mark_seen(seen, found.index);

        if(is_derived_count(s->members, s->members_count, found.member->name)) {
            return tison_fail(p, TISON_ERR_DUPLICATE,
                              "\"%s\" is derived from a JsonArray and cannot be set",
                              found.member->name);
        }

        bool ok;
        const char *arg = NULL;
        switch(field_kind(p, &found, &arg)) {
        case FIELD_ARRAY: {
            ok = parse_struct_array(p, base, s, &found, arg);
        } break;
        case FIELD_TAGGED_UNION: {
            TagInfo tag;
            if(!resolve_tag(p, s, arg, &tag)) return false;
            if(seen_before(p, s, start, seen, raw.begin, tag.tag.index)) {
                ok = parse_tagged_union(p, base, s, &found, arg);
            } else {
                deferred = true;
                ok = tison_skip_value(p);
            }
        } break;
        case FIELD_DEFAULT: {
            TisonTypeContext ctx = {found.member->type, found.member};
            ok = tison_parse_value(p, base + found.offset, &ctx);
        } break;
        }
        if(!ok) return false;
    }

    if(!tison_ok(p)) return false;
    return !deferred || parse_deferred_unions(p, base, s, start);
}

static bool parse_fixed_array(TisonParser *p, void *dst, const Type_Info_Array *a) {
    if(a->num_elements == 0) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "flexible or zero-length array");
    }

    const Type_Info *elem = a->element_type;

    TisonArrayIter it;
    if(!tison_array_begin(p, &it)) return false;

    size_t count = 0;
    while(tison_array_next(p, &it)) {
        if(it.index >= a->num_elements) {
            return tison_fail(p, TISON_ERR_LENGTH, "more than %zu elements", a->num_elements);
        }
        TisonTypeContext ctx = {elem, NULL};
        if(!tison_parse_value(p, (unsigned char *)dst + it.index * elem->size, &ctx)) {
            return false;
        }
        count++;
    }

    if(!tison_ok(p)) return false;

    if(count != a->num_elements) {
        p->cur--;  // Report at the closing bracket
        return tison_fail(p, TISON_ERR_LENGTH, "expected %zu elements, got %zu", a->num_elements,
                          count);
    }

    return true;
}

// -----------------------------------------------------------------------------
// Dispatch
// -----------------------------------------------------------------------------

bool tison_parse_default(TisonParser *p, void *dst, const TisonTypeContext *ctx) {
    if(p->failed) return false;
    const Type_Info *t = ctx->type;

    char **annotations = own_annotations(ctx);
    if(tison_has_annotation(annotations, "JsonCStr")) {
        if(t->tag == TYPE_TAG_ARRAY && is_byte_type(((const Type_Info_Array *)t)->element_type) &&
           ((const Type_Info_Array *)t)->num_elements > 0) {
            return parse_cstr_array(p, dst, ((const Type_Info_Array *)t)->num_elements);
        }
        if(t->tag == TYPE_TAG_POINTER && is_byte_type(((const Type_Info_Pointer *)t)->pointer_to)) {
            return parse_cstr_pointer(p, dst);
        }
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "JsonCStr requires char[N] or char *");
    }
    if(t->tag != TYPE_TAG_ENUM && has_enum_annotation(annotations)) {
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "%s", MISPLACED_ENUM_ANNOTATION);
    }

    switch(t->tag) {
    case TYPE_TAG_INTEGER:
        if(is_bool_type(t)) return parse_bool(p, dst);
        return parse_integer(p, dst, (const Type_Info_Integer *)t);
    case TYPE_TAG_FLOAT:
        return parse_float(p, dst, t);
    case TYPE_TAG_ENUM:
        return parse_enum(p, dst, (const Type_Info_Enum *)t, annotations);
    case TYPE_TAG_STRUCT:
        return parse_struct(p, dst, (const Type_Info_Struct *)t);
    case TYPE_TAG_ARRAY:
        return parse_fixed_array(p, dst, (const Type_Info_Array *)t);
    case TYPE_TAG_POINTER: {
        const Type_Info *pointee = ((const Type_Info_Pointer *)t)->pointer_to;
        if(annotation_value(annotations, "JsonArray")) {
            return tison_fail(p, TISON_ERR_UNSUPPORTED,
                              "JsonArray is only supported on struct fields");
        }
        // TODO: Escape hatch for a single heap-allocated char.
        if(pointee == &typeinfo_char.base) return parse_cstr_pointer(p, dst);
        if(!is_object_type(pointee)) {
            return tison_fail(p, TISON_ERR_UNSUPPORTED, "void or function pointer");
        }
        return parse_owned_pointer(p, dst, pointee, ctx->member);
    }
    case TYPE_TAG_UNION:
        if(annotation_value(annotations, "JsonTag")) {
            return tison_fail(p, TISON_ERR_UNSUPPORTED,
                              "JsonTag is only supported on struct fields");
        }
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "union without a hook or JsonTag");
    case TYPE_TAG_VOID:
        return tison_fail(p, TISON_ERR_UNSUPPORTED, "void type");
    }
    TISON_UNREACHABLE();
}

static const TisonTypeHook *find_hook(TisonParser *p, const TisonTypeContext *ctx) {
    return find_hook_in(&p->options, ctx);
}

bool tison_parse_value(TisonParser *p, void *dst, const TisonTypeContext *ctx) {
    if(p->failed) return false;
    const TisonTypeHook *h = find_hook(p, ctx);
    if(!h) return tison_parse_default(p, dst, ctx);
    if(h->parse(p, dst, ctx, h->user_data)) return tison_ok(p);
    if(!p->failed) {
        tison_fail(p, TISON_ERR_HOOK, "hook %zu failed", (size_t)(h - p->options.hooks));
    }
    return false;
}

TisonErrorCode tison_deserialize(const char *source, size_t source_size, void *dst,
                                 const Type_Info *type, const TisonOptions *options,
                                 TisonError *err) {
    TisonParser p;
    tison_parser_init(&p, source, source_size, options);
    TisonTypeContext ctx = {type, NULL};
    if(tison_parse_value(&p, dst, &ctx) && !tison_at_end(&p)) {
        tison_fail(&p, TISON_ERR_SYNTAX, "trailing data after root value");
    }
    if(err) *err = p.error;
    return p.failed ? p.error.code : TISON_OK;
}

// -----------------------------------------------------------------------------
// Freeing
// -----------------------------------------------------------------------------

static bool can_release(const TisonOptions *opt) {
    return !opt->allocator.alloc || opt->allocator.free;
}

static void release(const TisonOptions *opt, void *ptr, size_t size) {
    if(!opt->allocator.alloc) {
        free(ptr);
    } else if(opt->allocator.free) {
        opt->allocator.free(opt->allocator.ctx, ptr, size ? size : 1);
    }
}

static bool free_hooked(const TisonOptions *opt, void *dst, const TisonTypeContext *ctx) {
    const TisonTypeHook *h = find_hook_in(opt, ctx);
    if(!h) return false;
    if(h->free) h->free(opt, dst, ctx, h->user_data);
    return true;
}

static void free_struct(const TisonOptions *opt, unsigned char *dst, const Type_Info_Struct *s,
                        const Type_Info_Struct *root, size_t base) {
    for(size_t i = 0; i < s->members_count; i++) {
        const Type_Info_Member *m = &s->members[i];
        const Type_Info *t = m->type;

        unsigned char *field = dst + base + m->offset;
        if(is_skipped(m)) continue;

        if(m->name[0] == '\0') {
            // Anonymous unions are never parsed with the currently supported union layouts.
            if(t->tag == TYPE_TAG_STRUCT) {
                free_struct(opt, dst, (const Type_Info_Struct *)t, root, base + m->offset);
            }
            continue;
        }

        TisonTypeContext ctx = {t, m};
        if(free_hooked(opt, field, &ctx)) continue;

        const char *count_name = annotation_value(m->annotations, "JsonArray");
        if(t->tag == TYPE_TAG_POINTER && count_name) {
            MemberLookup count;
            if(!find_member_by_name(root, count_name, &count) ||
               !is_count_type(count.member->type)) {
                continue;
            }
            const Type_Info *elem = ((const Type_Info_Pointer *)t)->pointer_to;

            unsigned char *items = load_pointer(field);
            if(!items) continue;

            size_t n = load_uint(dst + count.offset, count.member->type->size);
            for(size_t j = 0; j < n; j++) {
                TisonTypeContext ectx = {elem, m};
                tison_free_value(opt, items + j * elem->size, &ectx);
            }

            if(can_release(opt)) {
                release(opt, items, n * elem->size);
                store_pointer(field, NULL);
                store_int(dst + count.offset, count.member->type->size, 0);
            }

            continue;
        }

        const char *tag_name = annotation_value(m->annotations, "JsonTag");
        if(t->tag == TYPE_TAG_UNION && tag_name) {
            MemberLookup tag;
            if(!find_member_by_name(root, tag_name, &tag) ||
               tag.member->type->tag != TYPE_TAG_ENUM ||
               !is_supported_int_size(tag.member->type->size)) {
                continue;
            }

            const Type_Info_Enum *e = (const Type_Info_Enum *)tag.member->type;
            const Type_Info_Member *selected = variant_member((const Type_Info_Union *)t,
                                                              tag_variant(e, dst + tag.offset));

            if(selected) {
                TisonTypeContext uctx = {selected->type, selected};
                tison_free_value(opt, field + selected->offset, &uctx);
            }

            continue;
        }
        tison_free_value(opt, field, &ctx);
    }
}

void tison_free_value(const TisonOptions *opt, void *dst, const TisonTypeContext *ctx) {
    if(free_hooked(opt, dst, ctx)) return;

    const Type_Info *t = ctx->type;
    switch(t->tag) {
    case TYPE_TAG_STRUCT: {
        const Type_Info_Struct *s = (const Type_Info_Struct *)t;
        free_struct(opt, dst, s, s, 0);
    } break;
    case TYPE_TAG_ARRAY: {
        const Type_Info_Array *a = (const Type_Info_Array *)t;
        for(size_t i = 0; i < a->num_elements; i++) {
            TisonTypeContext ectx = {a->element_type, NULL};
            tison_free_value(opt, (unsigned char *)dst + i * a->element_type->size, &ectx);
        }
    } break;
    case TYPE_TAG_POINTER: {
        const Type_Info *pointee = ((const Type_Info_Pointer *)t)->pointer_to;
        void *ptr = load_pointer(dst);
        if(!ptr) break;
        bool is_string = pointee == &typeinfo_char.base ||
                         (is_byte_type(pointee) &&
                          tison_has_annotation(own_annotations(ctx), "JsonCStr"));
        size_t size;
        if(is_string) {
            size = strlen(ptr) + 1;
        } else if(is_object_type(pointee)) {
            TisonTypeContext pctx = {pointee, ctx->member};
            tison_free_value(opt, ptr, &pctx);
            size = pointee->size;
        } else {
            break;  // void and function pointers are never allocated
        }

        if(can_release(opt)) {
            release(opt, ptr, size);
            store_pointer(dst, NULL);
        }
    } break;
    case TYPE_TAG_VOID:
    case TYPE_TAG_INTEGER:
    case TYPE_TAG_FLOAT:
    case TYPE_TAG_ENUM:
    // Tagged unions are freed by their struct, other unions by hooks.
    case TYPE_TAG_UNION:
        break;
    }
}

void tison_free(void *dst, const Type_Info *type, const TisonOptions *options) {
    TisonOptions none = {0};
    if(!options) options = &none;
    TisonTypeContext ctx = {type, NULL};
    tison_free_value(options, dst, &ctx);
}

// -----------------------------------------------------------------------------
// Writer output and errors
// -----------------------------------------------------------------------------

bool tison_write_fail(TisonWriter *w, TisonErrorCode code, const char *fmt, ...) {
    if(w->failed) return false;
    w->failed = true;
    w->error.code = code;
    w->error.offset = w->written;
    w->error.line = 0;
    w->error.column = 0;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w->error.message, sizeof(w->error.message), fmt, ap);
    va_end(ap);
    tison_render_path(w->error.path, sizeof(w->error.path), w->path, w->path_len);
    return false;
}

bool tison_write_ok(const TisonWriter *w) {
    return !w->failed;
}

static bool flush(TisonWriter *w) {
    if(w->buf_len == 0) return true;
    size_t n = w->buf_len;
    w->buf_len = 0;
    if(!w->sink.write(w->sink.ctx, w->buf, n)) {
        return tison_write_fail(w, TISON_ERR_IO, "sink failed to write %zu bytes", n);
    }
    return true;
}

static bool emit(TisonWriter *w, const char *data, size_t len) {
    if(w->failed) return false;
    while(len > 0) {
        if(w->buf_len == sizeof(w->buf) && !flush(w)) return false;
        size_t n = sizeof(w->buf) - w->buf_len;
        if(n > len) n = len;
        memcpy(w->buf + w->buf_len, data, n);
        w->buf_len += n;
        w->written += n;
        data += n;
        len -= n;
    }
    return true;
}

static bool emit_cstr(TisonWriter *w, const char *s) {
    return emit(w, s, strlen(s));
}

static bool newline(TisonWriter *w, size_t depth) {
    if(!(w->options.flags & TISON_FLAG_PRETTY)) return true;
    if(!emit(w, "\n", 1)) return false;
    size_t spaces = depth * (w->options.indent ? w->options.indent : 4);
    static const char blanks[] = "                                ";
    while(spaces > 0) {
        size_t n = spaces < sizeof(blanks) - 1 ? spaces : sizeof(blanks) - 1;
        if(!emit(w, blanks, n)) return false;
        spaces -= n;
    }
    return true;
}

// -----------------------------------------------------------------------------
// Writing containers
// -----------------------------------------------------------------------------

static bool begin(TisonWriter *w, TisonWriteContainer *c, const char *open, bool is_index) {
    if(w->failed) return false;
    if(w->depth >= w->options.max_depth) {
        return tison_write_fail(w, TISON_ERR_DEPTH, "nesting deeper than %zu",
                                w->options.max_depth);
    }
    if(!emit_cstr(w, open)) return false;
    w->depth++;
    if(w->path_len < TISON_PATH_MAX_SEGMENTS) {
        memset(&w->path[w->path_len], 0, sizeof(w->path[0]));
        w->path[w->path_len].is_index = is_index;
    }
    w->path_len++;
    c->index = 0;
    c->first = true;
    return true;
}

static bool end(TisonWriter *w, TisonWriteContainer *c, const char *close) {
    if(w->failed) return false;
    w->depth--;
    w->path_len--;
    if(!c->first && !newline(w, w->depth)) return false;
    return emit_cstr(w, close);
}

static TisonPathSegment *writer_path_top(TisonWriter *w) {
    if(w->path_len == 0 || w->path_len > TISON_PATH_MAX_SEGMENTS) return NULL;
    return &w->path[w->path_len - 1];
}

static bool separator(TisonWriter *w, TisonWriteContainer *c) {
    if(w->failed) return false;
    if(!c->first) {
        if(!emit(w, ",", 1)) return false;
        c->index++;
    }
    c->first = false;
    return newline(w, w->depth);
}

bool tison_write_object_begin(TisonWriter *w, TisonWriteContainer *c) {
    return begin(w, c, "{", false);
}

bool tison_write_object_end(TisonWriter *w, TisonWriteContainer *c) {
    return end(w, c, "}");
}

bool tison_write_array_begin(TisonWriter *w, TisonWriteContainer *c) {
    return begin(w, c, "[", true);
}

bool tison_write_array_end(TisonWriter *w, TisonWriteContainer *c) {
    return end(w, c, "]");
}

bool tison_write_key(TisonWriter *w, TisonWriteContainer *c, const char *key, size_t len) {
    if(!separator(w, c)) return false;
    TisonPathSegment *s = writer_path_top(w);
    if(s) {
        s->key.begin = key;
        s->key.end = key + len;
    }
    if(!tison_write_string(w, key, len)) return false;
    return emit_cstr(w, (w->options.flags & TISON_FLAG_PRETTY) ? ": " : ":");
}

bool tison_write_element(TisonWriter *w, TisonWriteContainer *c) {
    if(!separator(w, c)) return false;
    TisonPathSegment *s = writer_path_top(w);
    if(s) s->index = c->index;
    return true;
}

// -----------------------------------------------------------------------------
// Writing primitives
// -----------------------------------------------------------------------------

bool tison_write_null(TisonWriter *w) {
    return emit_cstr(w, "null");
}

bool tison_write_bool(TisonWriter *w, bool v) {
    return emit_cstr(w, v ? "true" : "false");
}

bool tison_write_int(TisonWriter *w, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return emit(w, buf, (size_t)n);
}

bool tison_write_uint(TisonWriter *w, uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return emit(w, buf, (size_t)n);
}

// TODO: snprintf follows LC_NUMERIC. Replace with a locale-free formatter.
bool tison_write_float(TisonWriter *w, float v) {
    if(!isfinite(v)) return tison_write_fail(w, TISON_ERR_RANGE, "%f has no JSON form", v);
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%.9g", (double)v);
    return emit(w, buf, (size_t)n);
}

bool tison_write_double(TisonWriter *w, double v) {
    if(!isfinite(v)) return tison_write_fail(w, TISON_ERR_RANGE, "%f has no JSON form", v);
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%.17g", v);
    return emit(w, buf, (size_t)n);
}

static bool write_long_double(TisonWriter *w, long double v) {
    if(!isfinite(v)) return tison_write_fail(w, TISON_ERR_RANGE, "%Lf has no JSON form", v);
    // Digits needed to round-trip: ceil(1 + mantissa_bits * log10(2)).
    int digits = sizeof(long double) == sizeof(double) ? 17 : LDBL_MANT_DIG * 30103 / 100000 + 2;
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.*Lg", digits, v);
    return emit(w, buf, (size_t)n);
}

bool tison_write_string(TisonWriter *w, const char *s, size_t len) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char *c = (const unsigned char *)s;
    const unsigned char *end = c + len;
    if(!emit(w, "\"", 1)) return false;
    const unsigned char *run = c;
    while(c < end) {
        char esc[7] = {'\\', 0, 0, 0, 0, 0, 0};
        size_t esc_len = 2;
        switch(*c) {
        case '"':
            esc[1] = '"';
            break;
        case '\\':
            esc[1] = '\\';
            break;
        case '\b':
            esc[1] = 'b';
            break;
        case '\f':
            esc[1] = 'f';
            break;
        case '\n':
            esc[1] = 'n';
            break;
        case '\r':
            esc[1] = 'r';
            break;
        case '\t':
            esc[1] = 't';
            break;
        default:
            if(*c < 0x20) {
                esc[1] = 'u';
                esc[2] = '0';
                esc[3] = '0';
                esc[4] = hex[*c >> 4];
                esc[5] = hex[*c & 0xF];
                esc_len = 6;
                break;
            }
            size_t n = utf8_valid_seq(c, end);
            if(n == 0) {
                return tison_write_fail(w, TISON_ERR_STRING, "invalid UTF-8 at byte %zu",
                                        (size_t)(c - (const unsigned char *)s));
            }
            c += n;
            continue;
        }
        if(!emit(w, (const char *)run, (size_t)(c - run))) return false;
        if(!emit(w, esc, esc_len)) return false;
        c++;
        run = c;
    }
    if(!emit(w, (const char *)run, (size_t)(c - run))) return false;
    return emit(w, "\"", 1);
}

static bool write_integer(TisonWriter *w, const void *src, const Type_Info_Integer *t) {
    size_t size = t->base.size;
    if(!is_supported_int_size(size)) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "integer of size %zu", size);
    }
    if(t->is_signed) return tison_write_int(w, load_int(src, size));
    return tison_write_uint(w, load_uint(src, size));
}

static bool flag_selected(uint64_t v, uint64_t bits, uint64_t covered) {
    return v != 0 && (bits & v) == v && (v & ~covered) != 0;
}

static bool write_enum_flags(TisonWriter *w, const void *src, const Type_Info_Enum *e) {
    uint64_t mask = size_mask(e->base.size);
    uint64_t bits = load_uint(src, e->base.size);

    // Check for undeclared bits first, so the error points at the field and nothing is written.
    uint64_t covered = 0;
    for(size_t i = 0; i < e->values_count; i++) {
        uint64_t v = (uint64_t)e->values[i].value & mask;
        if(flag_selected(v, bits, covered)) covered |= v;
    }
    if(bits & ~covered) {
        return tison_write_fail(w, TISON_ERR_RANGE, "undeclared bits 0x%llx in %s",
                                (unsigned long long)(bits & ~covered), enum_label(e));
    }

    covered = 0;
    TisonWriteContainer c;
    if(!tison_write_array_begin(w, &c)) return false;
    for(size_t i = 0; i < e->values_count; i++) {
        uint64_t v = (uint64_t)e->values[i].value & mask;
        if(!flag_selected(v, bits, covered)) continue;
        if(!tison_write_element(w, &c)) return false;
        if(!tison_write_string(w, e->values[i].name, strlen(e->values[i].name))) return false;
        covered |= v;
    }
    return tison_write_array_end(w, &c);
}

static bool write_enum(TisonWriter *w, const void *src, const Type_Info_Enum *e,
                       char **annotations) {
    size_t size = e->base.size;
    if(!is_supported_int_size(size)) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "enum of size %zu", size);
    }
    EnumMode mode;
    const char *msg = enum_mode(e, annotations, &mode);
    if(msg) return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "%s: %s", enum_label(e), msg);

    switch(mode) {
    case ENUM_NAMES: {
        const char *name = tag_variant(e, src);
        if(name) return tison_write_string(w, name, strlen(name));
        if(enum_is_signed(e)) {
            return tison_write_fail(w, TISON_ERR_RANGE, "%lld is not a declared value of %s",
                                    (long long)load_int(src, size), enum_label(e));
        }
        return tison_write_fail(w, TISON_ERR_RANGE, "%llu is not a declared value of %s",
                                (unsigned long long)load_uint(src, size), enum_label(e));
    }
    case ENUM_INT:
        if(enum_is_signed(e)) return tison_write_int(w, load_int(src, size));
        return tison_write_uint(w, load_uint(src, size));
    case ENUM_FLAGS:
        return write_enum_flags(w, src, e);
    }
    TISON_UNREACHABLE();
}

static bool write_float_value(TisonWriter *w, const void *src, const Type_Info *t) {
    if(t->size == sizeof(float)) {
        float v;
        memcpy(&v, src, sizeof(v));
        return tison_write_float(w, v);
    }
    if(t->size == sizeof(double)) {
        double v;
        memcpy(&v, src, sizeof(v));
        return tison_write_double(w, v);
    }
    if(t->size == sizeof(long double)) {
        long double v;
        memcpy(&v, src, sizeof(v));
        return write_long_double(w, v);
    }
    return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "float of size %zu", t->size);
}

static bool write_cstr_pointer(TisonWriter *w, const void *src) {
    const char *s = load_pointer(src);
    if(!s) return tison_write_null(w);
    return tison_write_string(w, s, strlen(s));
}

static bool write_cstr_array(TisonWriter *w, const char *src, size_t capacity) {
    const char *nul = memchr(src, '\0', capacity);
    if(!nul) {
        return tison_write_fail(w, TISON_ERR_STRING, "char[%zu] has no NUL terminator", capacity);
    }
    return tison_write_string(w, src, (size_t)(nul - src));
}

// -----------------------------------------------------------------------------
// Writing aggregates
// -----------------------------------------------------------------------------

static bool write_array_field(TisonWriter *w, const unsigned char *root, const Type_Info_Struct *s,
                              const Type_Info_Member *m, const void *field,
                              const char *count_name) {
    MemberLookup count;
    if(!find_member_by_name(s, count_name, &count) || count.in_union) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "JsonArray=%s: no such field",
                                count_name);
    }
    const Type_Info *ct = count.member->type;
    if(!is_count_type(ct)) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED,
                                "JsonArray=%s: count field is not an integer", count_name);
    }
    const Type_Info *elem = ((const Type_Info_Pointer *)m->type)->pointer_to;
    if(!is_object_type(elem)) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED,
                                "JsonArray on a void or function pointer");
    }
    const unsigned char *count_src = root + count.offset;
    if(((const Type_Info_Integer *)ct)->is_signed && load_int(count_src, ct->size) < 0) {
        return tison_write_fail(w, TISON_ERR_RANGE, "JsonArray=%s: negative count", count_name);
    }
    uint64_t bits = load_uint(count_src, ct->size);
    const unsigned char *items = load_pointer(field);
    if(!items) {
        if(bits != 0) {
            return tison_write_fail(w, TISON_ERR_RANGE,
                                    "JsonArray=%s: count %llu with a NULL pointer", count_name,
                                    (unsigned long long)bits);
        }
        return tison_write_null(w);
    }
    TisonWriteContainer c;
    if(!tison_write_array_begin(w, &c)) return false;
    for(uint64_t i = 0; i < bits; i++) {
        if(!tison_write_element(w, &c)) return false;
        TisonTypeContext ctx = {elem, m};
        if(!tison_write_value(w, items + i * elem->size, &ctx)) return false;
    }
    return tison_write_array_end(w, &c);
}

static bool write_union_field(TisonWriter *w, const unsigned char *root, const Type_Info_Struct *s,
                              const Type_Info_Member *m, const unsigned char *field,
                              const char *tag_name) {
    MemberLookup tag;
    if(!find_member_by_name(s, tag_name, &tag) || tag.in_union) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "JsonTag=%s: no such field", tag_name);
    }
    if(tag.member->type->tag != TYPE_TAG_ENUM) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "JsonTag=%s: tag field is not an enum",
                                tag_name);
    }
    if(is_flags_tag(tag.member)) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED,
                                "JsonTag=%s: a JsonFlags enum cannot be a tag", tag_name);
    }
    const Type_Info_Enum *e = (const Type_Info_Enum *)tag.member->type;
    if(!is_supported_int_size(e->base.size)) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "enum of size %zu", e->base.size);
    }
    const Type_Info_Member *selected = variant_member((const Type_Info_Union *)m->type,
                                                      tag_variant(e, root + tag.offset));
    if(!selected) return tison_write_null(w);
    TisonTypeContext ctx = {selected->type, selected};
    return tison_write_value(w, field + selected->offset, &ctx);
}

// Writes the members of `s` at `base` into the open object `c`, flattening anonymous structs.
// `root` is the outermost struct, which owns the JsonArray and JsonTag sibling fields.
static bool write_members(TisonWriter *w, TisonWriteContainer *c, const unsigned char *src,
                          const Type_Info_Struct *s, const Type_Info_Struct *root, size_t base) {
    for(size_t i = 0; i < s->members_count; i++) {
        const Type_Info_Member *m = &s->members[i];
        const Type_Info *t = m->type;
        const unsigned char *field = src + base + m->offset;
        if(is_skipped(m)) continue;
        if(m->name[0] == '\0') {
            if(t->tag == TYPE_TAG_STRUCT) {
                if(!write_members(w, c, src, (const Type_Info_Struct *)t, root, base + m->offset)) {
                    return false;
                }
                continue;
            }
            return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "anonymous union in %s",
                                    root->name[0] ? root->name : "struct");
        }
        if(is_derived_count(root->members, root->members_count, m->name)) continue;

        if(!tison_write_key(w, c, m->name, strlen(m->name))) return false;
        TisonTypeContext ctx = {t, m};
        bool hooked = find_hook_in(&w->options, &ctx) != NULL;
        const char *count_name = annotation_value(m->annotations, "JsonArray");
        const char *tag_name = annotation_value(m->annotations, "JsonTag");
        bool ok;
        if(!hooked && t->tag == TYPE_TAG_POINTER && count_name) {
            ok = write_array_field(w, src, root, m, field, count_name);
        } else if(!hooked && t->tag == TYPE_TAG_UNION && tag_name) {
            ok = write_union_field(w, src, root, m, field, tag_name);
        } else {
            ok = tison_write_value(w, field, &ctx);
        }
        if(!ok) return false;
    }
    return true;
}

static bool write_struct(TisonWriter *w, const void *src, const Type_Info_Struct *s) {
    TisonWriteContainer c;
    if(!tison_write_object_begin(w, &c)) return false;
    if(!write_members(w, &c, src, s, s, 0)) return false;
    return tison_write_object_end(w, &c);
}

static bool write_fixed_array(TisonWriter *w, const void *src, const Type_Info_Array *a) {
    if(a->num_elements == 0) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "flexible or zero-length array");
    }
    const Type_Info *elem = a->element_type;
    TisonWriteContainer c;
    if(!tison_write_array_begin(w, &c)) return false;
    for(size_t i = 0; i < a->num_elements; i++) {
        if(!tison_write_element(w, &c)) return false;
        TisonTypeContext ctx = {elem, NULL};
        if(!tison_write_value(w, (const unsigned char *)src + i * elem->size, &ctx)) return false;
    }
    return tison_write_array_end(w, &c);
}

// -----------------------------------------------------------------------------
// Writing dispatch
// -----------------------------------------------------------------------------

bool tison_write_default(TisonWriter *w, const void *src, const TisonTypeContext *ctx) {
    if(w->failed) return false;
    const Type_Info *t = ctx->type;
    char **annotations = own_annotations(ctx);
    if(tison_has_annotation(annotations, "JsonCStr")) {
        if(t->tag == TYPE_TAG_ARRAY && is_byte_type(((const Type_Info_Array *)t)->element_type) &&
           ((const Type_Info_Array *)t)->num_elements > 0) {
            return write_cstr_array(w, src, ((const Type_Info_Array *)t)->num_elements);
        }
        if(t->tag == TYPE_TAG_POINTER && is_byte_type(((const Type_Info_Pointer *)t)->pointer_to)) {
            return write_cstr_pointer(w, src);
        }
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "JsonCStr requires char[N] or char *");
    }
    if(t->tag != TYPE_TAG_ENUM && has_enum_annotation(annotations)) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "%s", MISPLACED_ENUM_ANNOTATION);
    }

    switch(t->tag) {
    case TYPE_TAG_INTEGER:
        if(is_bool_type(t)) {
            bool v;
            memcpy(&v, src, sizeof(v));
            return tison_write_bool(w, v);
        }
        return write_integer(w, src, (const Type_Info_Integer *)t);
    case TYPE_TAG_FLOAT:
        return write_float_value(w, src, t);
    case TYPE_TAG_ENUM:
        return write_enum(w, src, (const Type_Info_Enum *)t, annotations);
    case TYPE_TAG_STRUCT:
        return write_struct(w, src, (const Type_Info_Struct *)t);
    case TYPE_TAG_ARRAY:
        return write_fixed_array(w, src, (const Type_Info_Array *)t);
    case TYPE_TAG_POINTER: {
        const Type_Info *pointee = ((const Type_Info_Pointer *)t)->pointer_to;
        if(annotation_value(annotations, "JsonArray")) {
            return tison_write_fail(w, TISON_ERR_UNSUPPORTED,
                                    "JsonArray is only supported on struct fields");
        }
        if(pointee == &typeinfo_char.base) return write_cstr_pointer(w, src);
        if(!is_object_type(pointee)) {
            return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "void or function pointer");
        }
        const void *ptr = load_pointer(src);
        if(!ptr) return tison_write_null(w);
        TisonTypeContext pctx = {pointee, ctx->member};
        return tison_write_value(w, ptr, &pctx);
    }
    case TYPE_TAG_UNION:
        if(annotation_value(annotations, "JsonTag")) {
            return tison_write_fail(w, TISON_ERR_UNSUPPORTED,
                                    "JsonTag is only supported on struct fields");
        }
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "union without a hook or JsonTag");
    case TYPE_TAG_VOID:
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "void type");
    }

    TISON_UNREACHABLE();
}

bool tison_write_value(TisonWriter *w, const void *src, const TisonTypeContext *ctx) {
    if(w->failed) return false;
    const TisonTypeHook *h = find_hook_in(&w->options, ctx);
    if(!h) return tison_write_default(w, src, ctx);
    if(!h->write) {
        return tison_write_fail(w, TISON_ERR_UNSUPPORTED, "hook %zu has no write callback",
                                (size_t)(h - w->options.hooks));
    }
    if(h->write(w, src, ctx, h->user_data)) return tison_write_ok(w);
    if(!w->failed) {
        tison_write_fail(w, TISON_ERR_HOOK, "hook %zu failed", (size_t)(h - w->options.hooks));
    }
    return false;
}

// -----------------------------------------------------------------------------
// Writing entry points
// -----------------------------------------------------------------------------

TisonErrorCode tison_serialize(TisonSink sink, const void *src, const Type_Info *type,
                               const TisonOptions *options, TisonError *err) {
    TisonWriter w = {.sink = sink};
    if(options) w.options = *options;
    if(w.options.max_depth == 0) w.options.max_depth = TISON_DEFAULT_MAX_DEPTH;
    TisonTypeContext ctx = {type, NULL};
    if(tison_write_value(&w, src, &ctx)) flush(&w);
    if(err) *err = w.error;
    return w.failed ? w.error.code : TISON_OK;
}

typedef struct {
    char *buf;
    size_t cap, len;
} BufferSink;

static bool buffer_write(void *ctx, const char *data, size_t len) {
    BufferSink *b = ctx;
    if(b->cap > 0 && b->len < b->cap - 1) {
        size_t room = b->cap - 1 - b->len;
        memcpy(b->buf + b->len, data, len < room ? len : room);
    }
    b->len += len;
    return true;
}

TisonErrorCode tison_serialize_to_buffer(char *buf, size_t cap, size_t *out_len, const void *src,
                                         const Type_Info *type, const TisonOptions *options,
                                         TisonError *err) {
    BufferSink b = {buf, cap, 0};
    TisonSink sink = {&b, buffer_write};
    TisonErrorCode code = tison_serialize(sink, src, type, options, err);
    if(cap > 0) buf[b.len < cap - 1 ? b.len : cap - 1] = '\0';
    if(out_len) *out_len = b.len;
    return code;
}

static bool file_write(void *ctx, const char *data, size_t len) {
    return fwrite(data, 1, len, ctx) == len;
}

TisonErrorCode tison_serialize_to_file(FILE *f, const void *src, const Type_Info *type,
                                       const TisonOptions *options, TisonError *err) {
    TisonSink sink = {f, file_write};
    return tison_serialize(sink, src, type, options, err);
}
