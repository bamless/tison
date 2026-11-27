#include <stdlib.h>
#include <string.h>

#undef __STRICT_ANSI__
#define CTEST_MAIN
#define CTEST_SEGFAULT
#define CTEST_COLOR_OK
#include "ctest.h"
#include "test_util.h"

static void *test_heap_alloc(void *ctx, size_t size) {
    TestHeap *h = ctx;
    h->calls++;
    if(h->fail_at && h->calls == h->fail_at) return NULL;
    if(h->block_count == sizeof(h->blocks) / sizeof(h->blocks[0])) return NULL;
    void *p = malloc(size);
    if(!p) return NULL;
    h->bytes += size;
    if(h->dirty) memset(p, 0xAB, size);
    h->blocks[h->block_count++] = p;
    return p;
}

static void test_heap_release(void *ctx, void *ptr, size_t size) {
    TestHeap *h = ctx;
    for(size_t i = 0; i < h->block_count; i++) {
        if(h->blocks[i] != ptr) continue;
        h->blocks[i] = h->blocks[--h->block_count];
        h->frees++;
        h->freed_bytes += size;
        free(ptr);
        return;
    }
    abort();  // Not one of ours
}

void test_heap_free(TestHeap *h) {
    for(size_t i = 0; i < h->block_count; i++) free(h->blocks[i]);
    h->block_count = 0;
}

TisonAllocator test_heap_allocator(TestHeap *h) {
    TisonAllocator a = {h, test_heap_alloc, test_heap_release};
    return a;
}

TisonAllocator test_heap_allocator_no_free(TestHeap *h) {
    TisonAllocator a = {h, test_heap_alloc, NULL};
    return a;
}

static void *counting_alloc(Ext_Allocator *a, size_t size) {
    CountingAllocator *c = (CountingAllocator *)a;
    if(c->fail) return NULL;
    c->calls++;
    c->bytes += size;
    void *p = ext_allocator_alloc(&ext_default_allocator.base, size);
    if(p && c->dirty) memset(p, 0xAB, size);
    return p;
}

static void *counting_realloc(Ext_Allocator *a, void *ptr, size_t old_size, size_t new_size) {
    (void)a;
    return ext_allocator_realloc(&ext_default_allocator.base, ptr, old_size, new_size);
}

static void counting_free(Ext_Allocator *a, void *ptr, size_t size) {
    CountingAllocator *c = (CountingAllocator *)a;
    c->frees++;
    c->freed_bytes += size;
    ext_allocator_free(&ext_default_allocator.base, ptr, size);
}

CountingAllocator counting_allocator(void) {
    CountingAllocator c = {
        {counting_alloc, counting_realloc, counting_free}, 0, 0, 0, 0, false, false};
    return c;
}

int main(int argc, const char **argv) {
    return ctest_main(argc, argv);
}
