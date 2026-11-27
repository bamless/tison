#ifndef TEST_UTIL_H_
#define TEST_UTIL_H_

#include <string.h>

#include "extlib.h"
#include "test_types.h"
#include "test_types_typeinfo.h"
#include "tison.h"

#define PARSE(src, dst, T, opts, err) \
    tison_deserialize((src), strlen(src), (dst), TI(T), (opts), (err))

// Plain TisonAllocator backed by malloc. Blocks still allocated are freed by test_heap_free.
typedef struct {
    size_t calls, bytes;
    size_t frees, freed_bytes;
    size_t fail_at;  // 1-based call number that fails, 0 for never
    bool dirty;      // Fill new blocks with garbage
    void *blocks[256];
    size_t block_count;
} TestHeap;

void test_heap_free(TestHeap *h);
TisonAllocator test_heap_allocator(TestHeap *h);
// Same heap without a free callback, like an arena.
TisonAllocator test_heap_allocator_no_free(TestHeap *h);

// Ext_Allocator wrapper over the default allocator that counts calls.
typedef struct {
    Ext_Allocator base;
    size_t calls, bytes;
    size_t frees, freed_bytes;
    bool fail;
    bool dirty;  // Fill new blocks with garbage
} CountingAllocator;

CountingAllocator counting_allocator(void);

#endif  // TEST_UTIL_H_
