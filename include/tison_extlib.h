/**
 * Adapter between the tison core and extlib.
 *
 * Array(T) storage is allocated once, with exactly the element count, from `allocator`. The
 * array's `allocator` field is set even for `[]`.
 *
 * StringSlice values are copied into `allocator` by default (empty ones get NULL data). With
 * `borrow_strings`, unescaped slices point into the source buffer instead, which must then
 * outlive them. Escaped ones are still decoded into `allocator`, and since tison_free cannot
 * tell the two apart it frees no slices. Pair borrowing with an arena, or the escaped slices
 * leak.
 *
 * Both hooks also serialize: Array(T) as a JSON array of its `size` elements (`[]` when empty),
 * StringSlice as a string. A non-zero size with NULL storage is TISON_ERR_RANGE.
 *
 * tison_free releases arrays and slices too, given the options used to parse, including the
 * partial output of a failed parse.
 *
 * The context must outlive the parse, and `allocator` must outlive every array using it.
 */
#ifndef TISON_EXTLIB_H_
#define TISON_EXTLIB_H_

#include "extlib.h"
#include "tison.h"

typedef struct {
    Ext_Allocator *allocator;
    bool borrow_strings;  // Unescaped slices point into the source and are never freed
} TisonExtlibContext;

TisonAllocator tison_allocator_from_extlib(TisonExtlibContext *ctx);

bool tison_match_extlib_array(const TisonTypeContext *ctx, void *user_data);
bool tison_parse_extlib_array(TisonParser *p, void *dst, const TisonTypeContext *ctx,
                              void *user_data);
bool tison_write_extlib_array(TisonWriter *w, const void *src, const TisonTypeContext *ctx,
                              void *user_data);
void tison_free_extlib_array(const TisonOptions *options, void *dst, const TisonTypeContext *ctx,
                             void *user_data);
bool tison_match_extlib_string_slice(const TisonTypeContext *ctx, void *user_data);
bool tison_parse_extlib_string_slice(TisonParser *p, void *dst, const TisonTypeContext *ctx,
                                     void *user_data);

bool tison_write_extlib_string_slice(TisonWriter *w, const void *src, const TisonTypeContext *ctx,
                                     void *user_data);
void tison_free_extlib_string_slice(const TisonOptions *options, void *dst,
                                    const TisonTypeContext *ctx, void *user_data);

TisonTypeHook tison_extlib_array_hook(TisonExtlibContext *ctx);
TisonTypeHook tison_extlib_string_slice_hook(TisonExtlibContext *ctx);

// Appends serialized output to `sb` through its own allocator. No NUL terminator is added.
TisonSink tison_sink_from_extlib(Ext_StringBuffer *sb);

#endif  // TISON_EXTLIB_H_
