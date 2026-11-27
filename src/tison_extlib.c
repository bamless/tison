#include "tison_extlib.h"

#include <string.h>

typedef Ext_Array(char) RefArray;

static void *extlib_alloc(void *ctx, size_t size) {
    return ext_allocator_alloc(((TisonExtlibContext *)ctx)->allocator, size);
}

static void extlib_free(void *ctx, void *ptr, size_t size) {
    ext_allocator_free(((TisonExtlibContext *)ctx)->allocator, ptr, size);
}

TisonAllocator tison_allocator_from_extlib(TisonExtlibContext *ctx) {
    TisonAllocator a = {ctx, extlib_alloc, extlib_free};
    return a;
}

// -----------------------------------------------------------------------------
// Array(T)
// -----------------------------------------------------------------------------

static bool is_size_t(const Type_Info *t) {
    return t->tag == TYPE_TAG_INTEGER && t->size == sizeof(size_t) &&
           !((const Type_Info_Integer *)t)->is_signed;
}

static bool member_is(const Type_Info_Member *m, const char *name, size_t offset) {
    return strcmp(m->name, name) == 0 && m->offset == offset;
}

bool tison_match_extlib_array(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    const Type_Info *t = ctx->type;
    if(t->tag != TYPE_TAG_STRUCT || t->size != sizeof(RefArray)) return false;
    const Type_Info_Struct *s = (const Type_Info_Struct *)t;
    if(s->members_count != 4) return false;
    const Type_Info_Member *m = s->members;
    if(!member_is(&m[0], "items", offsetof(RefArray, items)) ||
       !member_is(&m[1], "capacity", offsetof(RefArray, capacity)) ||
       !member_is(&m[2], "size", offsetof(RefArray, size)) ||
       !member_is(&m[3], "allocator", offsetof(RefArray, allocator))) {
        return false;
    }
    if(m[0].type->tag != TYPE_TAG_POINTER) return false;
    const Type_Info *elem = ((const Type_Info_Pointer *)m[0].type)->pointer_to;
    if(!elem || elem->size == 0) return false;
    if(!is_size_t(m[1].type) || !is_size_t(m[2].type)) return false;
    if(m[3].type->tag != TYPE_TAG_POINTER) return false;
    const Type_Info *alloc = ((const Type_Info_Pointer *)m[3].type)->pointer_to;
    return alloc && alloc->tag == TYPE_TAG_STRUCT &&
           strcmp(((const Type_Info_Struct *)alloc)->name, "Ext_Allocator") == 0;
}

bool tison_parse_extlib_array(TisonParser *p, void *dst, const TisonTypeContext *ctx,
                              void *user_data) {
    TisonExtlibContext *ectx = user_data;
    const Type_Info_Struct *s = (const Type_Info_Struct *)ctx->type;
    const Type_Info *elem = ((const Type_Info_Pointer *)s->members[0].type)->pointer_to;
    unsigned char *base = dst;

    TisonMark start = tison_mark(p);
    size_t count;
    if(!tison_count_array(p, &count)) return false;
    tison_reset(p, start);
    tison_skip_ws(p);

    if(count > SIZE_MAX / elem->size) {
        return tison_fail(p, TISON_ERR_RANGE, "array of %zu elements overflows size_t", count);
    }
    void *items = NULL;
    if(count > 0) {
        items = ext_allocator_alloc(ectx->allocator, count * elem->size);
        if(!items) {
            return tison_fail(p, TISON_ERR_ALLOC, "failed to allocate %zu bytes",
                              count * elem->size);
        }
        // Elements are destinations too, and the core expects them zero-initialized.
        memset(items, 0, count * elem->size);
    }
    size_t zero = 0;
    memcpy(base + offsetof(RefArray, allocator), &ectx->allocator, sizeof(Ext_Allocator *));
    memcpy(base + offsetof(RefArray, items), &items, sizeof(void *));
    memcpy(base + offsetof(RefArray, capacity), &count, sizeof(size_t));
    memcpy(base + offsetof(RefArray, size), &zero, sizeof(size_t));

    TisonArrayIter it;
    if(!tison_array_begin(p, &it)) return false;
    while(tison_array_next(p, &it)) {
        // Only a hook consuming the wrong amount of input can desync the two passes.
        if(it.index >= count) {
            return tison_fail(
                p, TISON_ERR_LENGTH,
                "more elements than counted (a hook may have consumed too little input)");
        }
        // Counted before parsing, so tison_free also reaches a partially parsed element. The
        // storage is zeroed, so that is safe.
        size_t size = it.index + 1;
        memcpy(base + offsetof(RefArray, size), &size, sizeof(size_t));
        TisonTypeContext ectx_elem = {elem, NULL};
        if(!tison_parse_value(p, (unsigned char *)items + it.index * elem->size, &ectx_elem)) {
            return false;
        }
    }
    if(!tison_ok(p)) return false;
    memcpy(base + offsetof(RefArray, size), &count, sizeof(size_t));
    return true;
}

bool tison_write_extlib_array(TisonWriter *w, const void *src, const TisonTypeContext *ctx,
                              void *user_data) {
    (void)user_data;
    const Type_Info_Struct *s = (const Type_Info_Struct *)ctx->type;
    const Type_Info *elem = ((const Type_Info_Pointer *)s->members[0].type)->pointer_to;
    const unsigned char *base = src;
    const unsigned char *items;
    size_t size;
    memcpy(&items, base + offsetof(RefArray, items), sizeof(items));
    memcpy(&size, base + offsetof(RefArray, size), sizeof(size));
    if(size > 0 && !items) {
        return tison_write_fail(w, TISON_ERR_RANGE, "array of size %zu with NULL items", size);
    }
    TisonWriteContainer c;
    if(!tison_write_array_begin(w, &c)) return false;
    for(size_t i = 0; i < size; i++) {
        if(!tison_write_element(w, &c)) return false;
        TisonTypeContext ectx = {elem, NULL};
        if(!tison_write_value(w, items + i * elem->size, &ectx)) return false;
    }
    return tison_write_array_end(w, &c);
}

void tison_free_extlib_array(const TisonOptions *options, void *dst, const TisonTypeContext *ctx,
                             void *user_data) {
    (void)user_data;
    const Type_Info_Struct *s = (const Type_Info_Struct *)ctx->type;
    const Type_Info *elem = ((const Type_Info_Pointer *)s->members[0].type)->pointer_to;
    unsigned char *base = dst;
    unsigned char *items;
    size_t size, capacity;
    Ext_Allocator *allocator;
    memcpy(&items, base + offsetof(RefArray, items), sizeof(items));
    memcpy(&size, base + offsetof(RefArray, size), sizeof(size));
    memcpy(&capacity, base + offsetof(RefArray, capacity), sizeof(capacity));
    memcpy(&allocator, base + offsetof(RefArray, allocator), sizeof(Ext_Allocator *));
    if(items) {
        for(size_t i = 0; i < size; i++) {
            TisonTypeContext ectx = {elem, NULL};
            tison_free_value(options, items + i * elem->size, &ectx);
        }
        if(allocator) ext_allocator_free(allocator, items, capacity * elem->size);
    }
    memset(dst, 0, sizeof(RefArray));
}

TisonTypeHook tison_extlib_array_hook(TisonExtlibContext *ctx) {
    TisonTypeHook h = {tison_match_extlib_array, tison_parse_extlib_array, tison_write_extlib_array,
                       tison_free_extlib_array, ctx};
    return h;
}

// -----------------------------------------------------------------------------
// StringSlice
// -----------------------------------------------------------------------------

bool tison_match_extlib_string_slice(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    const Type_Info *t = ctx->type;
    return t->tag == TYPE_TAG_STRUCT && t->size == sizeof(Ext_StringSlice) &&
           strcmp(((const Type_Info_Struct *)t)->name, "Ext_StringSlice") == 0;
}

bool tison_parse_extlib_string_slice(TisonParser *p, void *dst, const TisonTypeContext *ctx,
                                     void *user_data) {
    (void)ctx;
    TisonExtlibContext *ectx = user_data;
    if(tison_peek(p) != '"') return tison_fail(p, TISON_ERR_TYPE, "expected string");
    TisonSpan raw;
    TisonStringInfo info;
    if(!tison_scan_string(p, &raw, &info)) return false;
    Ext_StringSlice ss;
    ss.size = info.decoded_len;
    if(info.decoded_len == 0) {
        ss.data = NULL;
    } else if(ectx->borrow_strings && !info.has_escapes) {
        ss.data = raw.begin;
    } else {
        char *buf = ext_allocator_alloc(ectx->allocator, info.decoded_len);
        if(!buf) {
            return tison_fail(p, TISON_ERR_ALLOC, "failed to allocate %zu bytes", info.decoded_len);
        }
        tison_decode_string(raw, buf);
        ss.data = buf;
    }
    memcpy(dst, &ss, sizeof(ss));
    return true;
}

bool tison_write_extlib_string_slice(TisonWriter *w, const void *src, const TisonTypeContext *ctx,
                                     void *user_data) {
    (void)ctx;
    (void)user_data;
    Ext_StringSlice ss;
    memcpy(&ss, src, sizeof(ss));
    if(ss.size > 0 && !ss.data) {
        return tison_write_fail(w, TISON_ERR_RANGE, "string slice of size %zu with NULL data",
                                ss.size);
    }
    return tison_write_string(w, ss.size ? ss.data : "", ss.size);
}

void tison_free_extlib_string_slice(const TisonOptions *options, void *dst,
                                    const TisonTypeContext *ctx, void *user_data) {
    (void)options;
    (void)ctx;
    TisonExtlibContext *ectx = user_data;
    if(ectx->borrow_strings) return;  // Cannot tell borrowed slices from owned ones
    Ext_StringSlice ss;
    memcpy(&ss, dst, sizeof(ss));
    if(ss.size > 0 && ss.data) ext_allocator_free(ectx->allocator, (void *)ss.data, ss.size);
    memset(dst, 0, sizeof(ss));
}

TisonTypeHook tison_extlib_string_slice_hook(TisonExtlibContext *ctx) {
    TisonTypeHook h = {tison_match_extlib_string_slice, tison_parse_extlib_string_slice,
                       tison_write_extlib_string_slice, tison_free_extlib_string_slice, ctx};
    return h;
}

// -----------------------------------------------------------------------------
// StringBuffer sink
// -----------------------------------------------------------------------------

static bool string_buffer_write(void *ctx, const char *data, size_t len) {
    ext_sb_append((Ext_StringBuffer *)ctx, data, len);
    return true;
}

TisonSink tison_sink_from_extlib(Ext_StringBuffer *sb) {
    TisonSink sink = {sb, string_buffer_write};
    return sink;
}
