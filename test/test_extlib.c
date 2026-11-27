#include "ctest.h"
#include "test_util.h"
#include "tison_extlib.h"

typedef struct {
    CountingAllocator alloc;
    TisonExtlibContext ctx;
    TisonTypeHook hooks[2];
    TisonOptions opt;
} Fixture;

static void fixture_init(Fixture *f) {
    memset(f, 0, sizeof(*f));
    f->alloc = counting_allocator();
    f->ctx.allocator = &f->alloc.base;
    f->hooks[0] = tison_extlib_array_hook(&f->ctx);
    f->hooks[1] = tison_extlib_string_slice_hook(&f->ctx);
    f->opt.allocator = tison_allocator_from_extlib(&f->ctx);
    f->opt.hooks = f->hooks;
    f->opt.hook_count = 2;
}

static void free_and_check(Fixture *f, Scene *s) {
    tison_free(s, TI(Scene), &f->opt);
    ASSERT_EQUAL(f->alloc.calls, f->alloc.frees);
    ASSERT_EQUAL(f->alloc.bytes, f->alloc.freed_bytes);
}

CTEST(extlib, matches_array_shape_only) {
    TisonTypeContext arr = {TI(MaterialArray), NULL};
    TisonTypeContext mat = {TI(Material), NULL};
    TisonTypeContext vec = {TI(Vec3), NULL};
    ASSERT_TRUE(tison_match_extlib_array(&arr, NULL));
    ASSERT_FALSE(tison_match_extlib_array(&mat, NULL));
    ASSERT_FALSE(tison_match_extlib_array(&vec, NULL));
    TisonTypeContext slice = {TI(Ext_StringSlice), NULL};
    ASSERT_TRUE(tison_match_extlib_string_slice(&slice, NULL));
    ASSERT_FALSE(tison_match_extlib_string_slice(&vec, NULL));
}

CTEST(extlib, full_scene) {
    Fixture f;
    fixture_init(&f);
    const char *src =
        "{\"name\":\"cornell\","
        " \"materials\":[{\"name\":\"red\",\"kind\":\"MATERIAL_DIFFUSE\",\"albedo\":{\"x\":1}},"
        "                {\"name\":\"mirror\",\"kind\":\"MATERIAL_METAL\",\"path\":\"m.png\"}],"
        " \"indices\":[0,1,2],"
        " \"nested\":[[1.5],[],[2,3]],"
        " \"tags\":[\"plain\",\"esc\\naped\"]}";
    Scene s = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_OK, PARSE(src, &s, Scene, &f.opt, &err));

    ASSERT_EQUAL(7, s.name.size);
    ASSERT_TRUE(s.name.data < src || s.name.data >= src + strlen(src));  // Copied
    ASSERT_TRUE(memcmp(s.name.data, "cornell", 7) == 0);

    ASSERT_EQUAL(2, s.materials.size);
    ASSERT_EQUAL(2, s.materials.capacity);
    ASSERT_TRUE(s.materials.allocator == &f.alloc.base);
    ASSERT_STR("mirror", s.materials.items[1].name);
    ASSERT_STR("m.png", s.materials.items[1].path);
    ASSERT_EQUAL(MATERIAL_METAL, s.materials.items[1].kind);

    ASSERT_EQUAL(3, s.indices.size);
    ASSERT_EQUAL(2, s.indices.items[2]);

    ASSERT_EQUAL(3, s.nested.size);
    ASSERT_EQUAL(1, s.nested.items[0].size);
    ASSERT_EQUAL(0, s.nested.items[1].size);
    ASSERT_NULL(s.nested.items[1].items);
    ASSERT_TRUE(s.nested.items[1].allocator == &f.alloc.base);
    ASSERT_DBL_NEAR(3.0, s.nested.items[2].items[1]);

    ASSERT_EQUAL(2, s.tags.size);
    ASSERT_EQUAL(8, s.tags.items[1].size);
    ASSERT_TRUE(memcmp(s.tags.items[1].data, "esc\naped", 8) == 0);

    ASSERT_EQUAL(10, f.alloc.calls);
    free_and_check(&f, &s);
    ASSERT_NULL(s.tags.items);
    ASSERT_NULL(s.name.data);
}

CTEST(extlib, exact_single_allocation) {
    Fixture f;
    fixture_init(&f);
    Scene s = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"indices\":[1, 2, 3]}", &s, Scene, &f.opt, NULL));
    ASSERT_EQUAL(1, f.alloc.calls);
    ASSERT_EQUAL(3 * sizeof(int), f.alloc.bytes);
    ASSERT_EQUAL(3, s.indices.capacity);
    free_and_check(&f, &s);
}

CTEST(extlib, empty_array_sets_allocator) {
    Fixture f;
    fixture_init(&f);
    Scene s = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"indices\":[ ]}", &s, Scene, &f.opt, NULL));
    ASSERT_EQUAL(0, f.alloc.calls);
    ASSERT_NULL(s.indices.items);
    ASSERT_EQUAL(0, s.indices.size);
    ASSERT_EQUAL(0, s.indices.capacity);
    ASSERT_TRUE(s.indices.allocator == &f.alloc.base);
    // Pushing after parsing uses the parse allocator.
    array_push(&s.indices, 42);
    ASSERT_EQUAL(1, f.alloc.calls);
    free_and_check(&f, &s);
}

CTEST(extlib, elements_start_zeroed) {
    Fixture f;
    fixture_init(&f);
    f.alloc.dirty = true;
    Scene s = {0};
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"materials\":[{\"roughness\":1},{}]}", &s, Scene, &f.opt, NULL));
    ASSERT_NULL(s.materials.items[0].path);
    ASSERT_STR("", s.materials.items[1].name);
    ASSERT_EQUAL(MATERIAL_DIFFUSE, s.materials.items[1].kind);
    ASSERT_DBL_NEAR(0.0, s.materials.items[1].roughness);
    free_and_check(&f, &s);
}

CTEST(extlib, large_array) {
    Fixture f;
    fixture_init(&f);
    StringBuffer sb = {0};
    sb_append_cstr(&sb, "{\"indices\":[");
    for(int i = 0; i < 10000; i++) {
        sb_appendf(&sb, "%s%d", i ? "," : "", i);
    }
    sb_append_cstr(&sb, "]}");
    Scene s = {0};
    ASSERT_EQUAL(TISON_OK, tison_deserialize(sb.items, sb.size, &s, TI(Scene), &f.opt, NULL));
    ASSERT_EQUAL(10000, s.indices.size);
    ASSERT_EQUAL(9999, s.indices.items[9999]);
    ASSERT_EQUAL(1, f.alloc.calls);
    free_and_check(&f, &s);
    sb_free(&sb);
}

CTEST(extlib, malformed_arrays_allocate_nothing) {
    Fixture f;
    fixture_init(&f);
    const char *cases[] = {"{\"indices\":[1,2,]}", "{\"indices\":[1 2]}", "{\"indices\":[1,"};
    for(size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        Scene s = {0};
        ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE(cases[i], &s, Scene, &f.opt, NULL));
        ASSERT_NULL(s.indices.items);
    }
    ASSERT_EQUAL(0, f.alloc.calls);
    Scene s = {0};
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"indices\":{}}", &s, Scene, &f.opt, NULL));
}

CTEST(extlib, partial_failure_is_freeable) {
    Fixture f;
    fixture_init(&f);
    Scene s = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"indices\":[1,2,\"x\"]}", &s, Scene, &f.opt, &err));
    ASSERT_STR(".indices[2]", err.path);
    ASSERT_NOT_NULL(s.indices.items);
    ASSERT_EQUAL(3, s.indices.capacity);
    ASSERT_EQUAL(3, s.indices.size);  // Includes the failed, zeroed element
    ASSERT_EQUAL(0, s.indices.items[2]);
    ASSERT_TRUE(s.indices.allocator == &f.alloc.base);
    free_and_check(&f, &s);

    Scene t = {0};
    ASSERT_EQUAL(TISON_ERR_TYPE,
                 PARSE("{\"name\":\"n\",\"tags\":[\"a\",\"b\",1]}", &t, Scene, &f.opt, NULL));
    free_and_check(&f, &t);
}

CTEST(extlib, allocation_failure) {
    Fixture f;
    fixture_init(&f);
    f.alloc.fail = true;
    Scene s = {0};
    ASSERT_EQUAL(TISON_ERR_ALLOC, PARSE("{\"indices\":[1]}", &s, Scene, &f.opt, NULL));
    ASSERT_EQUAL(TISON_ERR_ALLOC, PARSE("{\"name\":\"a\\tb\"}", &s, Scene, &f.opt, NULL));
}

CTEST(extlib, borrow_strings) {
    Fixture f;
    Ext_Arena arena;
    fixture_init(&f);
    arena = ext_make_arena(.page_allocator = &ext_default_allocator.base);
    f.ctx.allocator = &arena.base;
    f.ctx.borrow_strings = true;
    const char *src = "{\"name\":\"cornell\",\"tags\":[\"plain\",\"esc\\naped\",\"\"]}";
    Scene s = {0};
    ASSERT_EQUAL(TISON_OK, PARSE(src, &s, Scene, &f.opt, NULL));
    ASSERT_TRUE(s.name.data > src && s.name.data < src + strlen(src));
    ASSERT_TRUE(s.tags.items[0].data > src && s.tags.items[0].data < src + strlen(src));
    ASSERT_TRUE(s.tags.items[1].data < src || s.tags.items[1].data >= src + strlen(src));
    ASSERT_NULL(s.tags.items[2].data);

    // Borrowed and owned slices cannot be told apart, so none is freed.
    tison_free(&s, TI(Scene), &f.opt);
    ASSERT_NULL(s.tags.items);
    ext_arena_destroy(&arena);
}

// -----------------------------------------------------------------------------
// Serialization
// -----------------------------------------------------------------------------

static void arena_fixture_init(Fixture *f, Ext_Arena *arena) {
    fixture_init(f);
    *arena = ext_make_arena(.page_allocator = &ext_default_allocator.base);
    f->ctx.allocator = &arena->base;
}

static Ext_StringBuffer write_scene(const Scene *s, const TisonOptions *opt, TisonError *err,
                                    TisonErrorCode *code) {
    Ext_StringBuffer sb = {0};
    *code = tison_serialize(tison_sink_from_extlib(&sb), s, TI(Scene), opt, err);
    return sb;
}

CTEST(extlib, serialize_round_trip) {
    Fixture f;
    Ext_Arena arena;
    arena_fixture_init(&f, &arena);
    const char *src =
        "{\"name\":\"cor\\nnell\",\"materials\":[{\"name\":\"red\",\"path\":\"r.png\"},{}],"
        "\"indices\":[0,1,2],\"nested\":[[1.5],[],[2,3]],\"tags\":[\"plain\",\"esc\\taped\"]}";
    Scene a = {0}, b = {0};
    ASSERT_EQUAL(TISON_OK, PARSE(src, &a, Scene, &f.opt, NULL));

    TisonErrorCode code;
    Ext_StringBuffer first = write_scene(&a, &f.opt, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_EQUAL(TISON_OK, tison_deserialize(first.items, first.size, &b, TI(Scene), &f.opt, NULL));
    Ext_StringBuffer second = write_scene(&b, &f.opt, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_EQUAL(first.size, second.size);
    ASSERT_TRUE(memcmp(first.items, second.items, first.size) == 0);

    sb_append_char(&first, '\0');
    ASSERT_STRSTR(first.items, "\"name\":\"cor\\nnell\"");
    ASSERT_STRSTR(first.items, "\"indices\":[0,1,2],\"nested\":[[1.5],[],[2,3]]");
    ASSERT_STRSTR(first.items, "\"tags\":[\"plain\",\"esc\\taped\"]");
    sb_free(&first);
    sb_free(&second);
    ext_arena_destroy(&arena);
}

CTEST(extlib, serialize_empty_and_invalid) {
    Fixture f;
    fixture_init(&f);
    Scene s = {0};
    TisonErrorCode code;
    Ext_StringBuffer out = write_scene(&s, &f.opt, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    sb_append_char(&out, '\0');
    ASSERT_STR("{\"name\":\"\",\"materials\":[],\"indices\":[],\"nested\":[],\"tags\":[]}",
               out.items);
    sb_free(&out);

    TisonError err;
    s.indices.size = 2;
    out = write_scene(&s, &f.opt, &err, &code);
    ASSERT_EQUAL(TISON_ERR_RANGE, code);
    ASSERT_STR(".indices", err.path);
    sb_free(&out);

    Scene t = {0};
    t.name.size = 3;
    out = write_scene(&t, &f.opt, &err, &code);
    ASSERT_EQUAL(TISON_ERR_RANGE, code);
    ASSERT_STR(".name", err.path);
    sb_free(&out);

    Scene u = {0};
    u.name = ext_ss_from_cstr("bad\xff");
    out = write_scene(&u, &f.opt, &err, &code);
    ASSERT_EQUAL(TISON_ERR_STRING, code);
    sb_free(&out);
}

CTEST(extlib, serialize_pretty_into_string_buffer) {
    Fixture f;
    fixture_init(&f);
    f.opt.flags = TISON_FLAG_PRETTY;
    f.opt.indent = 2;
    int items[] = {4, 5};
    Scene s = {0};
    s.indices.items = items;
    s.indices.size = 2;
    s.indices.capacity = 2;
    TisonErrorCode code;
    Ext_StringBuffer out = write_scene(&s, &f.opt, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    sb_append_char(&out, '\0');
    ASSERT_STRSTR(out.items, "  \"indices\": [\n    4,\n    5\n  ],\n");
    ASSERT_STRSTR(out.items, "  \"materials\": [],\n");
    sb_free(&out);
}
