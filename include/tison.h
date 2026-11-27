/**
 * Deserializes JSON directly into C objects described by typeinfo.c reflection data.
 *
 *   - `dst` must be zero-initialized. Missing fields keep their zero value.
 *   - On failure `dst` may be partially written, and nothing allocated so far is released.
 *   - Bool and char detection compare against `typeinfo_bool` and `typeinfo_char`, so at least
 *     one generated typeinfo file containing built-in types should be linked (this is usually the
 *     default, use `-no-builtin-types` if you need to generate more than one typeinfo files to
 *     avoid multiple redefinitions).
 *   - Floats are converted with strtof/strtod/strtold, so LC_NUMERIC must be "C".
 *
 * Default handling:
 *   integer   JSON integer, range checked. Fractions and exponents are rejected.
 *   float     any JSON number.
 *   bool      true/false, only for `_Bool`.
 *   enum      enumerator name string. Integers are rejected, even declared ones.
 *   JsonEnumInt
 *             annotation making an enum a JSON integer instead. Any value that fits the enum's
 *             storage is accepted, which is signed when some enumerator is negative.
 *   JsonFlags annotation making an enum a bit set, written as an array of enumerator names:
 *             `["PERM_READ", "PERM_WRITE"]`. Parsing ORs the names, and [] gives 0. Writing
 *             picks enumerators in declaration order, skipping those whose bits are already
 *             covered, so declare composites like PERM_RW before their parts to prefer them.
 *             Bits no enumerator covers are an error. Enums with negative enumerators and
 *             JsonTag fields cannot be flags.
 *   Both enum annotations go on the enum type, or on a member to override the type for that
 *   field. A member annotation only reaches the member itself, so arrays and pointers of enums
 *   need the annotation on the type.
 *   struct    object. Any key order, duplicates rejected, anonymous structs flattened.
 *             Unknown keys are rejected unless TISON_FLAG_IGNORE_UNKNOWN_FIELDS is set.
 *   T[N]      array with exactly N elements.
 *   char *    NUL-terminated string, allocated. null gives NULL.
 *   JsonCStr  member annotation making char[N] a string (needs len + 1 <= N).
 *   T *       one owned object, allocated and zeroed. null gives NULL.
 *   JsonArray=count
 *             member annotation making `T *` an owned array. The element count is written to
 *             the sibling integer field `count`, which must not appear in the JSON.
 *             [] and null give NULL and 0.
 *   Pointees and JsonArray elements are parsed with the pointer's member as hook context.
 *   union     only as a struct field annotated `JsonTag=kind`, where `kind` is a sibling enum.
 *             Each union member names its enumerator with `JsonWhen=ENUMERATOR`. The JSON is
 *             adjacently tagged: `{"kind": "SHAPE_SPHERE", "shape": {...}}`, keys in any order.
 *             A tag value with no member is a payload-less variant: the union key must then be
 *             absent or null.
 *   JsonSkip  member annotation hiding a field from JSON. Its key is an unknown field, and
 *             tison_free leaves it alone.
 *   null elsewhere, untagged or anonymous unions, void and function pointers: rejected unless a
 *   hook handles them.
 *
 * Limits of v0.1.0:
 *   TISON_MAX_KEY_LEN (256)         decoded bytes of a key or enumerator name with escapes. Longer
 *                                   escaped keys match no field. Keys without escapes are compared
 *                                   in place and have no limit.
 *   TISON_MAX_STRUCT_MEMBERS (256)  members tracked by the duplicate-key bitset. Members past it
 *                                   are still checked, by rescanning the earlier keys.
 *   TISON_MAX_NUMBER_LEN (256)      characters in a number literal. Longer numbers are an error.
 *   TISON_DEFAULT_MAX_DEPTH         nesting depth when TisonOptions.max_depth is 0.
 *   TISON_PATH_MAX_SEGMENTS         path segments kept for TisonError.path.
 *
 * Serialization mirrors these rules. Every field is written in declaration order except
 * JsonSkip fields and JsonArray counts. NULL pointers become null. An enum value that matches
 * no enumerator is an error, except with JsonEnumInt. Floats use %.9g / %.17g, so LC_NUMERIC
 * must be "C". Infinity, NaN, invalid UTF-8 and a JsonCStr char[N] without a NUL are errors.
 */
#ifndef TISON_H_
#define TISON_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "typeinfo.h"

#define TISON_DEFAULT_MAX_DEPTH 128
#define TISON_PATH_MAX_SEGMENTS 64

// TisonAllocator supplies the memory for everything parsing allocates: strings, owned objects
// and JsonArray elements. `ctx` is passed back to both callbacks.
typedef struct {
    void *ctx;
    // Returns NULL on failure. Never called with size 0.
    void *(*alloc)(void *ctx, size_t size);
    // Optional. Receives the size that was allocated. Lets tison_free release parsed output.
    void (*free)(void *ctx, void *ptr, size_t size);
} TisonAllocator;

// TisonTypeContext describes the value being parsed, written or freed. Hooks match on it, and
// read annotations from `member`.
typedef struct {
    const Type_Info *type;
    // Member metadata when parsing a struct field. NULL at the root and for array elements.
    const Type_Info_Member *member;
} TisonTypeContext;

typedef struct TisonParser TisonParser;
typedef struct TisonWriter TisonWriter;
typedef struct TisonOptions TisonOptions;

// TisonTypeHook overrides the default handling of the types it matches. Hooks are tried in
// order before the default rules, for the root value and for every nested one.
//
// `match` selects the hook in both read/write directions. `parse` must consume exactly one JSON
// value and `write` must produce exactly one. Returning false without failing the parser or writer
// reports TISON_ERR_HOOK. Calling `tison_parse_value` (or `tison_write_value`) on the same context
// from inside the hook recurses forever; use the `_default` variants to delegate. A matched
// hook without `write` makes serialization fail with TISON_ERR_UNSUPPORTED. `free` releases what
// `parse` allocated and is called by tison_free. Without it the value is left alone. Use
// `tison_free_value` to release nested values.
typedef struct {
    bool (*match)(const TisonTypeContext *ctx, void *user_data);
    bool (*parse)(TisonParser *p, void *dst, const TisonTypeContext *ctx, void *user_data);
    bool (*write)(TisonWriter *w, const void *src, const TisonTypeContext *ctx, void *user_data);
    void (*free)(const TisonOptions *options, void *dst, const TisonTypeContext *ctx,
                 void *user_data);
    void *user_data;
} TisonTypeHook;

typedef enum {
    TISON_FLAG_NONE = 0,
    // Parsing skips object keys that match no field instead of failing.
    TISON_FLAG_IGNORE_UNKNOWN_FIELDS = 1 << 0,
    // Serialization writes one field or element per line, indented by TisonOptions.indent.
    TISON_FLAG_PRETTY = 1 << 1,
} TisonFlags;

// TisonOptions configures parsing, serialization and freeing. A zeroed struct, or a NULL
// pointer wherever options are taken, selects the defaults. Pass the same options to tison_free
// that were used for parsing, since it needs the same allocator and hooks.
struct TisonOptions {
    TisonAllocator allocator;    // alloc == NULL means malloc and free. See tison_free.
    const TisonTypeHook *hooks;  // First match wins
    size_t hook_count;           // Number of hooks
    size_t max_depth;            // 0 means TISON_DEFAULT_MAX_DEPTH
    uint32_t flags;              // Bitmask of TisonFlags
    size_t indent;               // Spaces per level with TISON_FLAG_PRETTY, 0 means 4
};

typedef enum {
    TISON_OK,
    TISON_ERR_SYNTAX,
    TISON_ERR_TYPE,
    TISON_ERR_UNSUPPORTED,
    TISON_ERR_RANGE,
    TISON_ERR_STRING,
    TISON_ERR_ALLOC,
    TISON_ERR_DEPTH,
    TISON_ERR_DUPLICATE,
    TISON_ERR_UNKNOWN_FIELD,
    TISON_ERR_LENGTH,
    TISON_ERR_HOOK,
    TISON_ERR_IO,
} TisonErrorCode;

// TisonError reports where and why parsing or serialization failed. `path` locates the failing
// value inside the root object, and `message` describes the problem in more detail than
// tison_strerror(code).
typedef struct {
    TisonErrorCode code;
    size_t offset;        // Byte offset into the source, or into the output when serializing
    size_t line, column;  // 1-based, 0 when serializing
    char path[256];       // e.g. ".objects[3].material", empty at the root
    char message[128];
} TisonError;

// tison_strerror returns a short static description of `code`, such as "syntax error".
const char *tison_strerror(TisonErrorCode code);

// tison_free frees what parsing allocated inside `dst`, an object of type `type`, including the
// partial output of a failed parse, and resets the freed pointers to NULL. `dst` itself is not
// freed. Pass the options used for parsing. Memory from an allocator without `free` is left
// alone, but hook `free` callbacks still run.
//
// Sizes come from the parsed data: sizeof(T) for owned objects, count * sizeof(T) for
// JsonArray and strlen + 1 for strings, so do not change those after parsing.
void tison_free(void *dst, const Type_Info *type, const TisonOptions *options);

// tison_free_value is tison_free for a value nested inside another, described by `ctx`. Hook
// `free` callbacks use it to release the values they parsed.
void tison_free_value(const TisonOptions *options, void *dst, const TisonTypeContext *ctx);

// tison_deserialize parses the JSON in `source`, which needs no NUL terminator, into `dst`, a
// zero-initialized object of type `type`. The whole input must be one JSON value, optionally
// surrounded by whitespace.
//
// It returns TISON_OK on success. Otherwise it returns the error code and, if `err` is not NULL,
// fills it in. Either way release the output with tison_free. `options` may be NULL.
TisonErrorCode tison_deserialize(const char *source, size_t source_size, void *dst,
                                 const Type_Info *type, const TisonOptions *options,
                                 TisonError *err);

// -----------------------------------------------------------------------------
// Parser and scanner API, for hooks and standalone scanning
// -----------------------------------------------------------------------------

// TisonSpan is a byte range [begin, end) into the source.
typedef struct {
    const char *begin, *end;
} TisonSpan;

typedef struct {
    size_t decoded_len;  // Bytes after decoding escapes, without terminator
    bool has_escapes;    // False means the raw span is the decoded string
    bool has_nul;        // Contains \u0000
} TisonStringInfo;

// TisonMark is a saved parser position, taken with tison_mark and restored with tison_reset.
typedef struct {
    const char *cur;
    size_t depth, path_len;
} TisonMark;

typedef struct {
    size_t index;
    bool started;
} TisonArrayIter;

typedef struct {
    bool started;
} TisonObjectIter;

typedef struct {
    bool is_index;
    size_t index;
    TisonSpan key;
} TisonPathSegment;

// TisonParser is a cursor over a JSON source, plus the state of the parse: options, nesting
// depth, the path to the current value and the first error. Hooks receive one, and it can also
// drive a standalone scan after tison_parser_init.
//
// Once an error is recorded the parser stays failed: later scanning and parsing calls return
// false (or NULL) without consuming input, and only the first error is kept.
struct TisonParser {
    const char *begin, *cur, *end;
    TisonOptions options;
    size_t depth;
    bool failed;
    TisonError error;
    size_t path_len;
    TisonPathSegment path[TISON_PATH_MAX_SEGMENTS];
};

// tison_parser_init sets up `p` to scan `source_size` bytes of `source`. `options` is copied and
// may be NULL.
void tison_parser_init(TisonParser *p, const char *source, size_t source_size,
                       const TisonOptions *options);

// tison_ok reports whether `p` has not failed yet.
bool tison_ok(const TisonParser *p);
const TisonError *tison_parser_error(const TisonParser *p);

// tison_fail records an error at the current position, with a printf-style message. Only the
// first error is kept. It always returns false, so hooks can write `return tison_fail(...)`.
bool tison_fail(TisonParser *p, TisonErrorCode code, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;

void tison_skip_ws(TisonParser *p);

// tison_peek skips whitespace and returns the next byte without consuming it, or -1 at the end
// of the input.
int tison_peek(TisonParser *p);

// tison_consume consumes the next non-whitespace byte if it is `ch`, and reports whether it did.
// tison_expect does the same but fails the parser when the byte is not `ch`.
bool tison_consume(TisonParser *p, char ch);
bool tison_expect(TisonParser *p, char ch);

// tison_at_end reports whether only whitespace is left.
bool tison_at_end(TisonParser *p);

TisonMark tison_mark(const TisonParser *p);
void tison_reset(TisonParser *p, TisonMark mark);

// The scanners consume one token and validate it without converting it. Their out parameters
// may be NULL.
//
// tison_scan_string scans a string. `out` gets the raw content between the quotes, escapes
// included, and `info` its decoded length and whether decoding is needed at all.
bool tison_scan_string(TisonParser *p, TisonSpan *out, TisonStringInfo *info);

// tison_decode_string decodes a span validated by tison_scan_string into `out`, which must hold
// `decoded_len` bytes. It returns the number of bytes written, with no terminator.
size_t tison_decode_string(TisonSpan raw, char *out);

// tison_scan_number scans a number. `is_integer` is set when it has no fraction or exponent.
bool tison_scan_number(TisonParser *p, TisonSpan *out, bool *is_integer);

// tison_scan_literal scans the exact keyword `literal`, such as "null".
bool tison_scan_literal(TisonParser *p, const char *literal);

// tison_skip_value consumes one complete value of any kind, validating it.
bool tison_skip_value(TisonParser *p);

// tison_count_array consumes an array and stores its number of elements in `out_count`. To parse
// the elements afterwards, take a tison_mark before and tison_reset to it.
bool tison_count_array(TisonParser *p, size_t *out_count);

// Iteration over arrays and objects. `begin` consumes the opening bracket. `next` returns true
// when positioned at a value, which the caller must then consume, and false at the end of the
// container or on error (check tison_ok). For objects it also consumes the key and the colon,
// and stores the raw key in `key`. Nested values report their index or key in TisonError.path.
// USAGE:
//     TisonObjectIter it;
//     TisonSpan key;
//     if(!tison_object_begin(p, &it)) return false;
//     while(tison_object_next(p, &it, &key, NULL)) {
//         if(!tison_skip_value(p)) return false;
//     }
//     return tison_ok(p);
bool tison_array_begin(TisonParser *p, TisonArrayIter *it);
bool tison_array_next(TisonParser *p, TisonArrayIter *it);
bool tison_object_begin(TisonParser *p, TisonObjectIter *it);
bool tison_object_next(TisonParser *p, TisonObjectIter *it, TisonSpan *key, TisonStringInfo *info);

// tison_alloc allocates `size` bytes from the parser's allocator. On failure it fails the parser
// with TISON_ERR_ALLOC and returns NULL.
void *tison_alloc(TisonParser *p, size_t size);

// tison_parse_value parses the next value into `dst` as described by `ctx`, through the first
// matching hook or the default rules. tison_parse_default always applies the default rules, so a
// hook can delegate to it for the type it matched. Both return false on failure.
bool tison_parse_value(TisonParser *p, void *dst, const TisonTypeContext *ctx);
bool tison_parse_default(TisonParser *p, void *dst, const TisonTypeContext *ctx);

// tison_has_annotation reports whether the NULL-terminated `annotations` list, which may itself be
// NULL, contains exactly `name`.
bool tison_has_annotation(char **annotations, const char *name);

// -----------------------------------------------------------------------------
// Serialization
// -----------------------------------------------------------------------------

#define TISON_WRITE_BUFFER_SIZE 512

// TisonSink receives the serialized output in chunks. `ctx` is passed back to `write`.
typedef struct {
    void *ctx;
    // Returns false on failure, which aborts with TISON_ERR_IO.
    bool (*write)(void *ctx, const char *data, size_t len);
} TisonSink;

// tison_serialize writes `src`, an object of type `type`, as JSON to `sink`. Output is buffered,
// so the sink sees chunks of up to TISON_WRITE_BUFFER_SIZE bytes. On failure some output may
// already have been written.
//
// It returns TISON_OK on success. Otherwise it returns the error code and, if `err` is not NULL,
// fills it in. `options` may be NULL, and its allocator settings are ignored.
TisonErrorCode tison_serialize(TisonSink sink, const void *src, const Type_Info *type,
                               const TisonOptions *options, TisonError *err);
// tison_serialize_to_buffer is tison_serialize into `buf`. It writes at most cap - 1 bytes plus a
// NUL, and `*out_len` receives the full length. Truncation is not an error: the output was cut if
// `*out_len >= cap`. Pass a NULL `buf` and a `cap` of 0 to measure the length first.
TisonErrorCode tison_serialize_to_buffer(char *buf, size_t cap, size_t *out_len, const void *src,
                                         const Type_Info *type, const TisonOptions *options,
                                         TisonError *err);
// tison_serialize_to_file is tison_serialize into `f`. A failed fwrite is TISON_ERR_IO.
TisonErrorCode tison_serialize_to_file(FILE *f, const void *src, const Type_Info *type,
                                       const TisonOptions *options, TisonError *err);

// TisonWriter is the serializer's state: the sink and its buffer, options, nesting depth, the
// path to the current value and the first error. Hooks receive one. Like TisonParser, it stays
// failed after the first error and later calls return false.
struct TisonWriter {
    TisonSink sink;
    TisonOptions options;
    size_t depth;
    size_t written;
    bool failed;
    TisonError error;
    size_t path_len;
    TisonPathSegment path[TISON_PATH_MAX_SEGMENTS];
    size_t buf_len;
    char buf[TISON_WRITE_BUFFER_SIZE];
};

typedef struct {
    size_t index;
    bool first;
} TisonWriteContainer;

// tison_write_fail records an error with a printf-style message. Only the first is kept. It
// always returns false.
bool tison_write_fail(TisonWriter *w, TisonErrorCode code, const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 3, 4)))
#endif
    ;
bool tison_write_ok(const TisonWriter *w);

// Containers. Each begin needs its matching end. Inside an object, call tison_write_key before
// each value. Inside an array, call tison_write_element before each one. They write separators
// and pretty-print indentation, and track the path for errors.
// USAGE:
//     TisonWriteContainer c;
//     if(!tison_write_object_begin(w, &c)) return false;
//     if(!tison_write_key(w, &c, "red", 3) || !tison_write_uint(w, color->red)) return false;
//     return tison_write_object_end(w, &c);
bool tison_write_object_begin(TisonWriter *w, TisonWriteContainer *c);
bool tison_write_key(TisonWriter *w, TisonWriteContainer *c, const char *key, size_t len);
bool tison_write_object_end(TisonWriter *w, TisonWriteContainer *c);
bool tison_write_array_begin(TisonWriter *w, TisonWriteContainer *c);
bool tison_write_element(TisonWriter *w, TisonWriteContainer *c);
bool tison_write_array_end(TisonWriter *w, TisonWriteContainer *c);

bool tison_write_null(TisonWriter *w);
bool tison_write_bool(TisonWriter *w, bool v);
bool tison_write_int(TisonWriter *w, int64_t v);
bool tison_write_uint(TisonWriter *w, uint64_t v);
bool tison_write_float(TisonWriter *w, float v);
bool tison_write_double(TisonWriter *w, double v);
// tison_write_string writes `len` bytes of `s` as a JSON string, escaping as needed. Invalid
// UTF-8 fails with TISON_ERR_STRING.
bool tison_write_string(TisonWriter *w, const char *s, size_t len);

// tison_write_value writes `src` as described by `ctx`, through the first matching hook or the
// default rules. tison_write_default always applies the default rules, for hooks to delegate to.
bool tison_write_value(TisonWriter *w, const void *src, const TisonTypeContext *ctx);
bool tison_write_default(TisonWriter *w, const void *src, const TisonTypeContext *ctx);

#endif  // TISON_H_
