#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include "ctest.h"
#include "test_util.h"

// -----------------------------------------------------------------------------
// Scalars
// -----------------------------------------------------------------------------

CTEST(scalars, integer_limits) {
    Integers v = {0};
    const char *src =
        "{\"i8\":-128,\"u8\":255,\"i16\":-32768,\"u16\":65535,\"i32\":-2147483648,"
        "\"u32\":4294967295,\"i64\":-9223372036854775808,\"u64\":18446744073709551615}";
    ASSERT_EQUAL(TISON_OK, PARSE(src, &v, Integers, NULL, NULL));
    ASSERT_EQUAL(INT8_MIN, v.i8);
    ASSERT_EQUAL(UINT8_MAX, v.u8);
    ASSERT_EQUAL(INT16_MIN, v.i16);
    ASSERT_EQUAL(UINT16_MAX, v.u16);
    ASSERT_EQUAL(INT32_MIN, v.i32);
    ASSERT_EQUAL_U(UINT32_MAX, v.u32);
    ASSERT_EQUAL(INT64_MIN, v.i64);
    ASSERT_EQUAL_U(UINT64_MAX, v.u64);

    Integers w = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"i8\":127,\"i64\":9223372036854775807,\"u8\":-0}", &w, Integers,
                                 NULL, NULL));
    ASSERT_EQUAL(INT8_MAX, w.i8);
    ASSERT_EQUAL(INT64_MAX, w.i64);
    ASSERT_EQUAL(0, w.u8);
}

CTEST(scalars, integer_errors) {
    struct {
        const char *src;
        TisonErrorCode code;
    } cases[] = {
        {"{\"i8\":128}", TISON_ERR_RANGE},
        {"{\"i8\":-129}", TISON_ERR_RANGE},
        {"{\"u8\":256}", TISON_ERR_RANGE},
        {"{\"u8\":-1}", TISON_ERR_RANGE},
        {"{\"i64\":9223372036854775808}", TISON_ERR_RANGE},
        {"{\"u64\":18446744073709551616}", TISON_ERR_RANGE},
        {"{\"u64\":99999999999999999999999}", TISON_ERR_RANGE},
        {"{\"i32\":1.0}", TISON_ERR_TYPE},
        {"{\"i32\":1e2}", TISON_ERR_TYPE},
        {"{\"i32\":\"1\"}", TISON_ERR_TYPE},
        {"{\"i32\":true}", TISON_ERR_TYPE},
        {"{\"i32\":null}", TISON_ERR_TYPE},
        {"{\"i32\":01}", TISON_ERR_SYNTAX},
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        Integers v = {0};
        TisonErrorCode code = PARSE(cases[i].src, &v, Integers, NULL, NULL);
        if(code != cases[i].code) CTEST_LOG("'%s' gave %d", cases[i].src, (int)code);
        ASSERT_EQUAL(cases[i].code, code);
    }
}

CTEST(scalars, floats_and_bools) {
    Scalars v = {0};
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"f\":1.5,\"d\":1e-300,\"flag\":true}", &v, Scalars, NULL, NULL));
    ASSERT_DBL_NEAR(1.5, v.f);
    ASSERT_DBL_NEAR(1e-300, v.d);
    ASSERT_TRUE(v.flag);

    Scalars w = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"f\":3,\"d\":-0.0,\"flag\":false}", &w, Scalars, NULL, NULL));
    ASSERT_DBL_NEAR(3.0, w.f);
    ASSERT_TRUE(signbit(w.d));
    ASSERT_FALSE(w.flag);

    Scalars x = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"f\":0.1,\"d\":0.1}", &x, Scalars, NULL, NULL));
    ASSERT_TRUE(x.f == 0.1f);
    ASSERT_TRUE(x.d == 0.1);

    Scalars e = {0};
    ASSERT_EQUAL(TISON_ERR_RANGE, PARSE("{\"f\":1e39}", &e, Scalars, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_RANGE, PARSE("{\"d\":1e400}", &e, Scalars, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"f\":\"1\"}", &e, Scalars, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"flag\":1}", &e, Scalars, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"flag\":\"true\"}", &e, Scalars, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{\"flag\":tru}", &e, Scalars, NULL, NULL));
}

CTEST(scalars, enums) {
    MaterialKind k = MATERIAL_DIFFUSE;
    ASSERT_EQUAL(TISON_OK, PARSE("\"MATERIAL_METAL\"", &k, MaterialKind, NULL, NULL));
    ASSERT_EQUAL(MATERIAL_METAL, k);
    ASSERT_EQUAL(TISON_OK, PARSE("\"MATERIAL_\\u0044IFFUSE\"", &k, MaterialKind, NULL, NULL));
    ASSERT_EQUAL(MATERIAL_DIFFUSE, k);
    ASSERT_EQUAL(TISON_ERR_RANGE, PARSE("\"NOPE\"", &k, MaterialKind, NULL, NULL));
    // Names only: even declared values are rejected as integers.
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("-2", &k, MaterialKind, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("3", &k, MaterialKind, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("5.0", &k, MaterialKind, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("true", &k, MaterialKind, NULL, NULL));
}

CTEST(enums, int_mode) {
    EnumModes e = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_OK, PARSE("{\"code\":-2,\"raw\":7}", &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(MATERIAL_GLASS, e.code);
    ASSERT_EQUAL(7, (int)e.raw);
    // Any value that fits, declared or not. MaterialKind has a negative enumerator, so it is
    // read as signed and Permissions as unsigned.
    ASSERT_EQUAL(TISON_OK, PARSE("{\"code\":-7}", &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(-7, (int)e.code);
    ASSERT_EQUAL(TISON_ERR_RANGE, PARSE("{\"raw\":-1}", &e, EnumModes, NULL, &err));
    ASSERT_STR(".raw", err.path);
    ASSERT_EQUAL(TISON_ERR_RANGE, PARSE("{\"code\":2147483648}", &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"code\":\"MATERIAL_METAL\"}", &e, EnumModes, NULL, NULL));
}

CTEST(enums, flags) {
    EnumModes e = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_OK, PARSE("{\"perms\":[\"PERM_READ\",\"PERM_EXEC\",\"PERM_READ\"]}", &e,
                                 EnumModes, NULL, NULL));
    ASSERT_EQUAL(PERM_READ | PERM_EXEC, (int)e.perms);
    ASSERT_EQUAL(TISON_OK, PARSE("{\"perms\":[]}", &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(0, (int)e.perms);
    // The type annotation reaches array elements, and a member annotation applies to a plain enum.
    ASSERT_EQUAL(TISON_OK, PARSE("{\"history\":[[\"PERM_RW\"],[\"PERM_WRITE\"]],"
                                 "\"shapes\":[\"SHAPE_SPHERE\",\"SHAPE_BOX\"]}",
                                 &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(PERM_RW, (int)e.history[0]);
    ASSERT_EQUAL(PERM_WRITE, (int)e.history[1]);
    ASSERT_EQUAL(SHAPE_SPHERE | SHAPE_BOX, (int)e.shapes);

    ASSERT_EQUAL(TISON_ERR_RANGE,
                 PARSE("{\"perms\":[\"PERM_READ\",\"PERM_FLY\"]}", &e, EnumModes, NULL, &err));
    ASSERT_STR(".perms[1]", err.path);
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"perms\":[1]}", &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"perms\":\"PERM_READ\"}", &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"perms\":null}", &e, EnumModes, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"perms\":3}", &e, EnumModes, NULL, NULL));
}

CTEST(enums, annotation_errors) {
    TisonError err;
    EnumConflict conflict = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"both\":1}", &conflict, EnumConflict, NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "mutually exclusive"));
    EnumOnInt on_int = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"count\":1}", &on_int, EnumOnInt, NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "require an enum"));
    EnumSignedFlags neg = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"flags\":[]}", &neg, EnumSignedFlags, NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "negative enumerator"));
    EnumFlagsTag tag = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"kind\":[\"PERM_READ\"],\"shape\":{\"radius\":1}}",
                                              &tag, EnumFlagsTag, NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "cannot be a tag"));
}

CTEST(enums, int_tag) {
    EnumIntTag t = {0};
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"kind\":1,\"shape\":{\"radius\":2}}", &t, EnumIntTag, NULL, NULL));
    ASSERT_EQUAL(SHAPE_SPHERE, t.kind);
    ASSERT_EQUAL(2.0f, t.shape.sphere.radius);
    // An undeclared tag value is a variant without payload.
    ASSERT_EQUAL(TISON_OK, PARSE("{\"kind\":9,\"shape\":null}", &t, EnumIntTag, NULL, NULL));
    ASSERT_EQUAL(9, (int)t.kind);
}

// -----------------------------------------------------------------------------
// Structs and arrays
// -----------------------------------------------------------------------------

CTEST(structs, out_of_order_and_missing) {
    TestHeap heap = {0};
    TisonOptions opt = {0};
    opt.allocator = test_heap_allocator(&heap);
    Material m = {0};
    const char *src =
        "{ \"roughness\": 0.25, \"albedo\": {\"z\": 3, \"x\": 1}, \"kind\": \"MATERIAL_GLASS\","
        "  \"name\": \"glass\", \"path\": \"tex/\\u00e9.png\" }";
    ASSERT_EQUAL(TISON_OK, PARSE(src, &m, Material, &opt, NULL));
    ASSERT_STR("glass", m.name);
    ASSERT_STR("tex/\xc3\xa9.png", m.path);
    ASSERT_EQUAL(MATERIAL_GLASS, m.kind);
    ASSERT_DBL_NEAR(1.0, m.albedo.x);
    ASSERT_DBL_NEAR(0.0, m.albedo.y);
    ASSERT_DBL_NEAR(3.0, m.albedo.z);
    ASSERT_DBL_NEAR(0.25, m.roughness);
    ASSERT_EQUAL(1, heap.calls);
    ASSERT_EQUAL(strlen(m.path) + 1, heap.bytes);
    test_heap_free(&heap);
}

CTEST(structs, cstr_bounds) {
    Material m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"name\":\"1234567\"}", &m, Material, NULL, NULL));
    ASSERT_STR("1234567", m.name);
    Material n = {0};
    ASSERT_EQUAL(TISON_ERR_LENGTH, PARSE("{\"name\":\"12345678\"}", &n, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_STRING, PARSE("{\"name\":\"a\\u0000\"}", &n, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"name\":1}", &n, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_OK, PARSE("{\"path\":\"x\"}", &n, Material, NULL, NULL));
    ASSERT_STR("x", n.path);
    tison_free(&n, TI(Material), NULL);
    ASSERT_NULL(n.path);

    TestHeap heap = {0};
    heap.fail_at = 1;
    TisonOptions opt = {0};
    opt.allocator = test_heap_allocator(&heap);
    ASSERT_EQUAL(TISON_ERR_ALLOC, PARSE("{\"path\":\"x\"}", &n, Material, &opt, NULL));
    test_heap_free(&heap);
}

CTEST(structs, unknown_and_duplicate_fields) {
    Material m = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_ERR_UNKNOWN_FIELD, PARSE("{\"colour\":1}", &m, Material, NULL, &err));
    ASSERT_STR(".colour", err.path);

    TisonOptions opt = {0};
    opt.flags = TISON_FLAG_IGNORE_UNKNOWN_FIELDS;
    const char *src = "{\"extra\":{\"deep\":[1,[2,{\"x\":\"]\"}]]},\"roughness\":2}";
    ASSERT_EQUAL(TISON_OK, PARSE(src, &m, Material, &opt, NULL));
    ASSERT_DBL_NEAR(2.0, m.roughness);
    ASSERT_EQUAL(TISON_ERR_SYNTAX,
                 PARSE("{\"extra\":[1,],\"roughness\":2}", &m, Material, &opt, NULL));

    Material d = {0};
    ASSERT_EQUAL(TISON_ERR_DUPLICATE,
                 PARSE("{\"roughness\":1,\"roughness\":2}", &d, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_DUPLICATE,
                 PARSE("{\"width\":1,\"id\":0,\"width\":2}", &(Layout){0}, Layout, NULL, NULL));
}

CTEST(structs, syntax_errors) {
    Material m = {0};
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{\"roughness\":1,}", &m, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{\"roughness\" 1}", &m, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{\"roughness\":1", &m, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{} x", &m, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("[]", &m, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("null", &m, Material, NULL, NULL));
    ASSERT_EQUAL(TISON_OK, PARSE(" {} \n", &m, Material, NULL, NULL));
}

CTEST(structs, anonymous_members_and_nested_arrays) {
    Layout l = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"width\":640,\"height\":480,\"id\":7,"
                                 "\"grid\":[[1,2,3],[4,5,6]]}",
                                 &l, Layout, NULL, NULL));
    ASSERT_EQUAL(640, l.width);
    ASSERT_EQUAL(480, l.height);
    ASSERT_EQUAL(7, l.id);
    ASSERT_EQUAL(6, l.grid[1][2]);

    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"as_int\":1}", &l, Layout, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_UNKNOWN_FIELD, PARSE("{\"\":1}", &l, Layout, NULL, NULL));

    TisonError err;
    ASSERT_EQUAL(TISON_ERR_LENGTH, PARSE("{\"grid\":[[1,2],[3,4,5]]}", &l, Layout, NULL, &err));
    ASSERT_STR(".grid[0]", err.path);
    ASSERT_EQUAL(TISON_ERR_LENGTH,
                 PARSE("{\"grid\":[[1,2,3],[4,5,6],[7,8,9]]}", &l, Layout, NULL, &err));
    ASSERT_STR(".grid[2]", err.path);
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{\"grid\":[[1,2,3],[4,5,6],]}", &l, Layout, NULL, NULL));
}

CTEST(structs, error_paths_and_unsupported) {
    Misc m = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_ERR_TYPE,
                 PARSE("{\"points\":[{\"x\":1},{\"y\":\"a\"}]}", &m, Misc, NULL, &err));
    ASSERT_STR(".points[1].y", err.path);
    ASSERT_EQUAL(TISON_OK, PARSE("{\"ptr\":1}", &m, Misc, NULL, NULL));
    ASSERT_EQUAL(1, *m.ptr);
    tison_free(&m, TI(Misc), NULL);

    Misc r = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"raw\":[1,2,3,-4]}", &r, Misc, NULL, NULL));
    ASSERT_EQUAL(-4, r.raw[3]);
}

// Every proper prefix must fail cleanly. Exact-size heap copies let ASan catch reads past the end.
static void check_prefixes_fail(const char *src, const Type_Info *type, size_t dst_size) {
    size_t len = strlen(src);
    TestHeap heap = {0};
    TisonOptions opt = {0};
    opt.allocator = test_heap_allocator(&heap);
    void *dst = calloc(1, dst_size);
    for(size_t n = 0; n < len; n++) {
        char *copy = malloc(n ? n : 1);
        memcpy(copy, src, n);
        memset(dst, 0, dst_size);
        TisonErrorCode code = tison_deserialize(copy, n, dst, type, &opt, NULL);
        if(code == TISON_OK) CTEST_LOG("prefix of length %zu accepted", n);
        ASSERT_NOT_EQUAL(TISON_OK, code);
        free(copy);
        test_heap_free(&heap);
    }
    memset(dst, 0, dst_size);
    ASSERT_EQUAL(TISON_OK, tison_deserialize(src, len, dst, type, &opt, NULL));
    test_heap_free(&heap);
    free(dst);
}

CTEST(structs, truncated_inputs_fail) {
    check_prefixes_fail(
        "{\"name\":\"ab\\n\",\"kind\":\"MATERIAL_METAL\",\"albedo\":{\"x\":1.5e1,\"y\":-2,\"z\":0},"
        "\"roughness\":0.5}",
        TI(Material), sizeof(Material));
    check_prefixes_fail("{\"vertices\":[{\"x\":1}],\"names\":[\"a\"],\"origin\":{}}", TI(Mesh),
                        sizeof(Mesh));
    check_prefixes_fail(
        "{\"shape\":{\"kind\":\"SHAPE_SPHERE\",\"shape\":{\"radius\":1}},"
        "\"kind\":\"SHAPE_GROUP\"}",
        TI(Shape), sizeof(Shape));
}

CTEST(strings, char_pointer_is_string) {
    TestHeap heap = {0};
    TisonOptions opt = {0};
    opt.allocator = test_heap_allocator(&heap);

    Material m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"path\":\"a\\tb\"}", &m, Material, &opt, NULL));
    ASSERT_STR("a\tb", m.path);
    ASSERT_EQUAL(1, heap.calls);
    ASSERT_EQUAL(4, heap.bytes);

    Material n = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"path\":null}", &n, Material, &opt, NULL));
    ASSERT_NULL(n.path);
    ASSERT_EQUAL(1, heap.calls);
    ASSERT_EQUAL(TISON_ERR_STRING, PARSE("{\"path\":\"\\u0000\"}", &n, Material, &opt, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"path\":[1]}", &n, Material, &opt, NULL));
    test_heap_free(&heap);
}

CTEST(strings, cstr_annotations) {
    TestHeap heap = {0};
    TisonOptions opt = {0};
    opt.allocator = test_heap_allocator(&heap);
    Misc m = {0};
    // The pre-rename CStr annotation is ignored, so this is an integer array.
    ASSERT_EQUAL(TISON_OK, PARSE("{\"legacy\":[1,2],\"label\":\"x\",\"bytes\":\"ab\"}", &m, Misc,
                                 &opt, NULL));
    ASSERT_EQUAL(2, m.legacy[1]);
    ASSERT_STR("x", m.label);
    ASSERT_STR("ab", (const char *)m.bytes);
    Misc n = {0};
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"legacy\":\"a\"}", &n, Misc, &opt, NULL));
    test_heap_free(&heap);
}

// -----------------------------------------------------------------------------
// Pointers
// -----------------------------------------------------------------------------

static TisonOptions heap_options(TestHeap *heap) {
    TisonOptions opt = {0};
    opt.allocator = test_heap_allocator(heap);
    return opt;
}

CTEST(pointers, single_objects) {
    TestHeap heap = {0};
    heap.dirty = true;
    TisonOptions opt = heap_options(&heap);

    Mesh m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"origin\":{\"y\":2},\"boxed\":5}", &m, Mesh, &opt, NULL));
    ASSERT_NOT_NULL(m.origin);
    ASSERT_DBL_NEAR(0.0, m.origin->x);
    ASSERT_DBL_NEAR(2.0, m.origin->y);
    ASSERT_EQUAL(5, **m.boxed);
    ASSERT_EQUAL(3, heap.calls);

    Mesh n = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"origin\":null,\"boxed\":null}", &n, Mesh, &opt, NULL));
    ASSERT_NULL(n.origin);
    ASSERT_NULL(n.boxed);
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"opaque\":{}}", &n, Mesh, &opt, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"origin\":[1,2,3]}", &n, Mesh, &opt, NULL));

    Node list = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"value\":1,\"next\":{\"value\":2,\"next\":null}}", &list, Node,
                                 &opt, NULL));
    ASSERT_EQUAL(2, list.next->value);
    ASSERT_NULL(list.next->next);
    test_heap_free(&heap);
}

CTEST(pointers, owned_arrays) {
    TestHeap heap = {0};
    heap.dirty = true;
    TisonOptions opt = heap_options(&heap);

    Mesh m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"vertices\":[{\"x\":1},{\"z\":3}]}", &m, Mesh, &opt, NULL));
    ASSERT_EQUAL(2, m.vertex_count);
    ASSERT_DBL_NEAR(0.0, m.vertices[0].y);
    ASSERT_DBL_NEAR(3.0, m.vertices[1].z);
    ASSERT_EQUAL(1, heap.calls);
    ASSERT_EQUAL(2 * sizeof(Vec3), heap.bytes);

    Mesh e = {0};
    e.vertex_count = 7;
    ASSERT_EQUAL(TISON_OK, PARSE("{\"vertices\":[],\"small\":null}", &e, Mesh, &opt, NULL));
    ASSERT_NULL(e.vertices);
    ASSERT_EQUAL(0, e.vertex_count);
    ASSERT_NULL(e.small);
    ASSERT_EQUAL(1, heap.calls);

    Mesh s = {0};
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"names\":[\"a\",\"bc\"],\"rows\":[1,2]}", &s, Mesh, &opt, NULL));
    ASSERT_EQUAL(2, s.name_count);
    ASSERT_STR("bc", s.names[1]);
    ASSERT_EQUAL(2, s.row_count);
    ASSERT_EQUAL(2, *s.rows[1]);

    // Annotation written without spaces around '='.
    Mesh t = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"tight\":[7]}", &t, Mesh, &opt, NULL));
    ASSERT_EQUAL(1, t.tight_count);
    ASSERT_EQUAL(7, t.tight[0]);
    test_heap_free(&heap);
}

CTEST(pointers, owned_array_errors) {
    TestHeap heap = {0};
    TisonOptions opt = heap_options(&heap);

    StringBuffer sb = {0};
    sb_append_cstr(&sb, "{\"small\":[");
    for(int i = 0; i < 300; i++) sb_appendf(&sb, "%s0", i ? "," : "");
    sb_append_cstr(&sb, "]}");
    Mesh m = {0};
    ASSERT_EQUAL(TISON_ERR_RANGE, tison_deserialize(sb.items, sb.size, &m, TI(Mesh), &opt, NULL));
    ASSERT_EQUAL(0, heap.calls);
    sb_free(&sb);

    TisonError err;
    ASSERT_EQUAL(TISON_ERR_DUPLICATE,
                 PARSE("{\"vertex_count\":1,\"vertices\":[]}", &m, Mesh, &opt, &err));
    ASSERT_STR(".vertex_count", err.path);
    ASSERT_EQUAL(TISON_ERR_DUPLICATE,
                 PARSE("{\"vertices\":[],\"vertex_count\":0}", &m, Mesh, &opt, NULL));
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"missing\":[]}", &m, Mesh, &opt, NULL));
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, PARSE("{\"bad_count\":[]}", &m, Mesh, &opt, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"vertices\":{}}", &m, Mesh, &opt, NULL));
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{\"vertices\":[{},]}", &m, Mesh, &opt, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"vertices\":[{},1]}", &m, Mesh, &opt, &err));
    ASSERT_STR(".vertices[1]", err.path);
    test_heap_free(&heap);
}

CTEST(structs, more_members_than_the_bitset) {
    Big *b = calloc(1, sizeof(*b));
    ASSERT_EQUAL(TISON_OK, PARSE("{\"e333\":1,\"a000\":2,\"d321\":3}", b, Big, NULL, NULL));
    ASSERT_EQUAL(1, b->e333);
    ASSERT_EQUAL(2, b->a000);
    ASSERT_EQUAL(3, b->d321);

    TisonError err;
    ASSERT_EQUAL(TISON_ERR_DUPLICATE,
                 PARSE("{\"e333\":1,\"a000\":2,\"b111\":0,\"e333\":3}", b, Big, NULL, &err));
    ASSERT_STR(".e333", err.path);
    ASSERT_EQUAL(TISON_ERR_DUPLICATE,
                 PARSE("{\"e312\":1,\"e\\u0033\\u00312\":2}", b, Big, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_DUPLICATE, PARSE("{\"a001\":1,\"a001\":2}", b, Big, NULL, NULL));

    // The tag sits past the bitset, so deciding whether to defer uses the rescan too.
    Big *c = calloc(1, sizeof(*c));
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"shape\":{\"radius\":4},\"kind\":\"SHAPE_SPHERE\"}", c, Big, NULL, NULL));
    ASSERT_DBL_NEAR(4.0, c->shape.sphere.radius);
    memset(c, 0, sizeof(*c));
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"kind\":\"SHAPE_SPHERE\",\"shape\":{\"radius\":6}}", c, Big, NULL, NULL));
    ASSERT_DBL_NEAR(6.0, c->shape.sphere.radius);
    free(b);
    free(c);
}

// -----------------------------------------------------------------------------
// Default allocator and tison_free
// -----------------------------------------------------------------------------

CTEST(malloc_default, parse_and_free_everything) {
    Mesh m = {0};
    const char *src =
        "{\"vertices\":[{\"x\":1}],\"names\":[\"a\",\"bc\"],\"rows\":[1,2],"
        "\"origin\":{\"z\":1},\"boxed\":3}";
    ASSERT_EQUAL(TISON_OK, PARSE(src, &m, Mesh, NULL, NULL));
    ASSERT_STR("bc", m.names[1]);
    tison_free(&m, TI(Mesh), NULL);
    ASSERT_NULL(m.vertices);
    ASSERT_EQUAL(0, m.vertex_count);
    ASSERT_NULL(m.names);
    ASSERT_EQUAL(0, m.name_count);
    ASSERT_NULL(m.rows);
    ASSERT_NULL(m.origin);
    ASSERT_NULL(m.boxed);
    tison_free(&m, TI(Mesh), NULL);  // Safe to call twice

    Shape s = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"kind\":\"SHAPE_GROUP\",\"shape\":{\"kind\":\"SHAPE_GROUP\","
                                 "\"shape\":{\"kind\":\"SHAPE_SPHERE\",\"shape\":{\"radius\":1}}}}",
                                 &s, Shape, NULL, NULL));
    ASSERT_DBL_NEAR(1.0, s.shape.child->shape.child->shape.sphere.radius);
    tison_free(&s, TI(Shape), NULL);
    ASSERT_NULL(s.shape.child);
}

CTEST(malloc_default, free_after_failed_parse) {
    Mesh m = {0};
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"names\":[\"a\",\"b\",1]}", &m, Mesh, NULL, NULL));
    ASSERT_EQUAL(3, m.name_count);
    ASSERT_NULL(m.names[2]);
    tison_free(&m, TI(Mesh), NULL);

    Shape s = {0};
    ASSERT_EQUAL(TISON_ERR_TYPE,
                 PARSE("{\"kind\":\"SHAPE_GROUP\",\"shape\":{\"kind\":\"SHAPE_SPHERE\","
                       "\"shape\":{\"radius\":\"x\"}}}",
                       &s, Shape, NULL, NULL));
    tison_free(&s, TI(Shape), NULL);

    Misc c = {0};
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"label\":\"ok\",\"raw\":true}", &c, Misc, NULL, NULL));
    tison_free(&c, TI(Misc), NULL);
    ASSERT_NULL(c.label);
}

static bool match_borrowed_origin(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    return ctx->member && strcmp(ctx->member->name, "origin") == 0 &&
           ctx->type->tag == TYPE_TAG_POINTER;
}

static bool parse_borrowed_origin(TisonParser *p, void *dst, const TisonTypeContext *ctx,
                                  void *user_data) {
    (void)ctx;
    Vec3 *borrowed = user_data;
    memcpy(dst, &borrowed, sizeof(borrowed));
    return tison_skip_value(p);
}

CTEST(malloc_default, free_skips_hooks_and_uses_custom_free) {
    static Vec3 borrowed;
    TisonTypeHook hooks[] = {{match_borrowed_origin, parse_borrowed_origin, NULL, NULL, &borrowed}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 1;
    Mesh m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"origin\":{},\"boxed\":1}", &m, Mesh, &opt, NULL));
    ASSERT_TRUE(m.origin == &borrowed);
    tison_free(&m, TI(Mesh), &opt);
    ASSERT_TRUE(m.origin == &borrowed);
    ASSERT_NULL(m.boxed);

    // A custom allocator with free is released through it.
    TestHeap heap = {0};
    TisonOptions custom = heap_options(&heap);
    Mesh n = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"boxed\":1}", &n, Mesh, &custom, NULL));
    tison_free(&n, TI(Mesh), &custom);
    ASSERT_NULL(n.boxed);
    ASSERT_EQUAL(2, heap.frees);
    test_heap_free(&heap);
}

CTEST(structs, json_skip) {
    Skipped s = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_OK, PARSE("{\"visible\":1}", &s, Skipped, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_UNKNOWN_FIELD, PARSE("{\"cache\":1}", &s, Skipped, NULL, &err));
    ASSERT_STR(".cache", err.path);
    ASSERT_EQUAL(TISON_ERR_UNKNOWN_FIELD, PARSE("{\"hidden_a\":1}", &s, Skipped, NULL, NULL));

    TisonOptions opt = {0};
    opt.flags = TISON_FLAG_IGNORE_UNKNOWN_FIELDS;
    int cached = 42;
    Skipped t = {0};
    t.cache = &cached;
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"cache\":7,\"hidden_b\":2,\"visible\":3}", &t, Skipped, &opt, NULL));
    ASSERT_TRUE(t.cache == &cached);
    ASSERT_EQUAL(0, t.hidden_b);
    ASSERT_EQUAL(3, t.visible);

    // Never parsed, so never freed: freeing a stack address would crash under ASan.
    tison_free(&t, TI(Skipped), &opt);
    ASSERT_TRUE(t.cache == &cached);
}

CTEST(custom_free, exact_sizes) {
    TestHeap heap = {0};
    TisonOptions opt = heap_options(&heap);
    Model m = {0};
    const char *src = "{\"vertices\":[{},{},{}],\"names\":[\"abc\"],\"origin\":{},\"boxed\":2}";
    ASSERT_EQUAL(TISON_OK, PARSE(src, &m, Model, &opt, NULL));
    // vertices, names, "abc", origin, boxed and *boxed.
    ASSERT_EQUAL(6, heap.calls);
    tison_free(&m, TI(Model), &opt);
    ASSERT_EQUAL(heap.calls, heap.frees);
    ASSERT_EQUAL(heap.bytes, heap.freed_bytes);
    ASSERT_EQUAL(0, heap.block_count);
    ASSERT_NULL(m.vertices);
    ASSERT_EQUAL(0, m.vertex_count);
    ASSERT_NULL(m.origin);

    Mesh partial = {0};
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"names\":[\"a\",\"bb\",1]}", &partial, Mesh, &opt, NULL));
    tison_free(&partial, TI(Mesh), &opt);
    ASSERT_EQUAL(heap.calls, heap.frees);
    ASSERT_EQUAL(heap.bytes, heap.freed_bytes);
    test_heap_free(&heap);
}

CTEST(custom_free, allocator_without_free) {
    TestHeap heap = {0};
    TisonOptions opt = {0};
    opt.allocator = test_heap_allocator_no_free(&heap);
    Model m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"origin\":{},\"names\":[\"x\"]}", &m, Model, &opt, NULL));
    tison_free(&m, TI(Model), &opt);
    ASSERT_NOT_NULL(m.origin);
    ASSERT_NOT_NULL(m.names);
    ASSERT_EQUAL(1, m.name_count);
    ASSERT_EQUAL(0, heap.frees);
    test_heap_free(&heap);
}

typedef struct {
    size_t frees;
    const Type_Info_Member *member;
} FreeHookState;

static bool match_origin_vec3(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    return ctx->type == TI(Vec3) && ctx->member && strcmp(ctx->member->name, "origin") == 0;
}

static void free_origin_vec3(const TisonOptions *options, void *dst, const TisonTypeContext *ctx,
                             void *user_data) {
    (void)options;
    FreeHookState *s = user_data;
    s->frees++;
    s->member = ctx->member;
    memset(dst, 0, sizeof(Vec3));
}

CTEST(custom_free, hook_free_callback) {
    FreeHookState state = {0};
    // No parse callback is needed to exercise free: build the value by hand.
    TisonTypeHook hooks[] = {{match_origin_vec3, NULL, NULL, free_origin_vec3, &state}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 1;
    // The hook releases the pointee's contents, the core then frees the pointer itself.
    Model m = {0};
    m.origin = calloc(1, sizeof(Vec3));
    tison_free(&m, TI(Model), &opt);
    ASSERT_EQUAL(1, state.frees);
    ASSERT_STR("origin", state.member->name);
    ASSERT_NULL(m.origin);
}

// -----------------------------------------------------------------------------
// Tagged unions
// -----------------------------------------------------------------------------

CTEST(unions, tag_before_and_after_payload) {
    Shape a = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"kind\":\"SHAPE_SPHERE\",\"shape\":{\"radius\":2}}", &a, Shape,
                                 NULL, NULL));
    ASSERT_EQUAL(SHAPE_SPHERE, a.kind);
    ASSERT_DBL_NEAR(2.0, a.shape.sphere.radius);

    Shape b = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"shape\":{\"size\":{\"x\":1}},\"kind\":\"SHAPE_BOX\",\"id\":3}",
                                 &b, Shape, NULL, NULL));
    ASSERT_EQUAL(SHAPE_BOX, b.kind);
    ASSERT_DBL_NEAR(1.0, b.shape.box.size.x);
    ASSERT_EQUAL(3, b.id);

    UnionCases c = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"list\":[{\"kind\":\"SHAPE_SPHERE\",\"shape\":{\"radius\":1}},"
                                 "{\"shape\":{\"radius\":5},\"kind\":\"SHAPE_SPHERE\"}]}",
                                 &c, UnionCases, NULL, NULL));
    ASSERT_DBL_NEAR(5.0, c.list[1].shape.sphere.radius);
}

CTEST(unions, payload_less_variants) {
    Shape s = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"kind\":\"SHAPE_NONE\"}", &s, Shape, NULL, NULL));
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"kind\":\"SHAPE_NONE\",\"shape\":null}", &s, Shape, NULL, NULL));
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"shape\":null,\"kind\":\"SHAPE_NONE\"}", &s, Shape, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE,
                 PARSE("{\"kind\":\"SHAPE_NONE\",\"shape\":{}}", &s, Shape, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE,
                 PARSE("{\"shape\":{},\"kind\":\"SHAPE_NONE\"}", &s, Shape, NULL, NULL));
}

CTEST(unions, errors) {
    Shape s = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"shape\":{\"radius\":1}}", &s, Shape, NULL, &err));
    ASSERT_STR(".shape", err.path);
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"shape\":null}", &s, Shape, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_SYNTAX, PARSE("{\"shape\":{\"radius\":1,},\"kind\":\"SHAPE_SPHERE\"}",
                                         &s, Shape, NULL, NULL));
    ASSERT_EQUAL(TISON_ERR_TYPE, PARSE("{\"shape\":{\"radius\":\"x\"},\"kind\":\"SHAPE_SPHERE\"}",
                                       &s, Shape, NULL, &err));
    ASSERT_STR(".shape.radius", err.path);
    ASSERT_EQUAL(TISON_ERR_UNKNOWN_FIELD,
                 PARSE("{\"kind\":\"SHAPE_SPHERE\",\"shape\":{\"size\":{}}}", &s, Shape, NULL,
                       NULL));
    ASSERT_EQUAL(TISON_ERR_DUPLICATE, PARSE("{\"kind\":\"SHAPE_SPHERE\",\"shape\":{},\"shape\":{}}",
                                            &s, Shape, NULL, NULL));

    const char *bad[] = {"bad_when", "twice", "not_enum", "no_field", "untagged"};
    for(size_t i = 0; i < sizeof(bad) / sizeof(*bad); i++) {
        char src[128];
        snprintf(src, sizeof(src), "{\"kind\":\"SHAPE_SPHERE\",\"%s\":{}}", bad[i]);
        UnionCases c = {0};
        TisonErrorCode code = PARSE(src, &c, UnionCases, NULL, NULL);
        if(code != TISON_ERR_UNSUPPORTED) CTEST_LOG("%s gave %d", bad[i], (int)code);
        ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, code);
    }
}

CTEST(unions, nested_and_deferred_in_deferred) {
    TestHeap heap = {0};
    TisonOptions opt = heap_options(&heap);
    Shape s = {0};
    ASSERT_EQUAL(TISON_OK,
                 PARSE("{\"shape\":{\"shape\":{\"size\":{\"z\":4}},\"kind\":\"SHAPE_BOX\"},"
                       "\"kind\":\"SHAPE_GROUP\"}",
                       &s, Shape, &opt, NULL));
    ASSERT_EQUAL(SHAPE_GROUP, s.kind);
    ASSERT_NOT_NULL(s.shape.child);
    ASSERT_EQUAL(SHAPE_BOX, s.shape.child->kind);
    ASSERT_DBL_NEAR(4.0, s.shape.child->shape.box.size.z);
    ASSERT_EQUAL(1, heap.calls);
    test_heap_free(&heap);
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------

typedef struct {
    size_t calls;
    const Type_Info_Member *last_member;
    bool fail_silently;
} Vec3HookState;

static bool match_vec3(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    return ctx->type == TI(Vec3);
}

// Accepts [x, y, z] and delegates objects to the default struct handling.
static bool parse_vec3(TisonParser *p, void *dst, const TisonTypeContext *ctx, void *user_data) {
    Vec3HookState *s = user_data;
    s->calls++;
    s->last_member = ctx->member;
    if(s->fail_silently) return false;
    if(tison_peek(p) == '{') return tison_parse_default(p, dst, ctx);
    float *v = dst;
    TisonArrayIter it;
    if(!tison_array_begin(p, &it)) return false;
    while(tison_array_next(p, &it)) {
        if(it.index >= 3) return tison_fail(p, TISON_ERR_LENGTH, "vec3 takes 3 components");
        TisonTypeContext fctx = {TI(float), NULL};
        if(!tison_parse_value(p, &v[it.index], &fctx)) return false;
    }
    return tison_ok(p);
}

static bool match_cstr_member(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    return ctx->member && tison_has_annotation(ctx->member->annotations, "JsonCStr") &&
           ctx->type->tag == TYPE_TAG_ARRAY;
}

static bool parse_upper_cstr(TisonParser *p, void *dst, const TisonTypeContext *ctx,
                             void *user_data) {
    (void)user_data;
    if(!tison_parse_default(p, dst, ctx)) return false;
    for(char *c = dst; *c; c++) {
        if(*c >= 'a' && *c <= 'z') *c = (char)(*c - 'a' + 'A');
    }
    return true;
}

CTEST(hooks, type_match_and_member_context) {
    Vec3HookState state = {0};
    TisonTypeHook hooks[] = {{match_vec3, parse_vec3, NULL, NULL, &state}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 1;

    Material m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"albedo\":[0.5, 0.25, 1]}", &m, Material, &opt, NULL));
    ASSERT_DBL_NEAR(0.25, m.albedo.y);
    ASSERT_EQUAL(1, state.calls);
    ASSERT_NOT_NULL(state.last_member);
    ASSERT_STR("albedo", state.last_member->name);

    Misc misc = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"points\":[[1,2,3],{\"y\":9}]}", &misc, Misc, &opt, NULL));
    ASSERT_EQUAL(3, state.calls);
    ASSERT_NULL(state.last_member);
    ASSERT_DBL_NEAR(3.0, misc.points[0].z);
    ASSERT_DBL_NEAR(9.0, misc.points[1].y);

    TisonError err;
    ASSERT_EQUAL(TISON_ERR_LENGTH, PARSE("{\"albedo\":[1,2,3,4]}", &m, Material, &opt, &err));
    ASSERT_STR(".albedo[3]", err.path);
}

CTEST(hooks, pointee_sees_pointer_member) {
    TestHeap heap = {0};
    Vec3HookState state = {0};
    TisonTypeHook hooks[] = {{match_vec3, parse_vec3, NULL, NULL, &state}};
    TisonOptions opt = heap_options(&heap);
    opt.hooks = hooks;
    opt.hook_count = 1;

    Mesh m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"origin\":[1,2,3]}", &m, Mesh, &opt, NULL));
    ASSERT_STR("origin", state.last_member->name);
    ASSERT_DBL_NEAR(3.0, m.origin->z);
    ASSERT_EQUAL(TISON_OK, PARSE("{\"vertices\":[[4,5,6]]}", &m, Mesh, &opt, NULL));
    ASSERT_STR("vertices", state.last_member->name);
    ASSERT_DBL_NEAR(6.0, m.vertices[0].z);
    test_heap_free(&heap);
}

CTEST(hooks, first_match_wins) {
    Vec3HookState first = {0}, second = {0};
    TisonTypeHook hooks[] = {{match_vec3, parse_vec3, NULL, NULL, &first},
                             {match_vec3, parse_vec3, NULL, NULL, &second}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 2;
    Vec3 v = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("[1,2,3]", &v, Vec3, &opt, NULL));
    ASSERT_EQUAL(1, first.calls);
    ASSERT_EQUAL(0, second.calls);
}

CTEST(hooks, annotation_match) {
    TisonTypeHook hooks[] = {{match_cstr_member, parse_upper_cstr, NULL, NULL, NULL}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 1;
    Material m = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("{\"name\":\"metal\"}", &m, Material, &opt, NULL));
    ASSERT_STR("METAL", m.name);
}

CTEST(hooks, silent_failure_reports_hook_error) {
    Vec3HookState state = {0};
    state.fail_silently = true;
    TisonTypeHook hooks[] = {{match_vec3, parse_vec3, NULL, NULL, &state}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 1;
    Vec3 v = {0};
    ASSERT_EQUAL(TISON_ERR_HOOK, PARSE("[1,2,3]", &v, Vec3, &opt, NULL));
}

CTEST(hooks, root_hook_sees_null_member) {
    Vec3HookState state = {0};
    state.last_member = (const Type_Info_Member *)&state;
    TisonTypeHook hooks[] = {{match_vec3, parse_vec3, NULL, NULL, &state}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 1;
    Vec3 v = {0};
    ASSERT_EQUAL(TISON_OK, PARSE("[1,2,3]", &v, Vec3, &opt, NULL));
    ASSERT_NULL(state.last_member);
}
