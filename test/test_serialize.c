#include <float.h>
#include <math.h>
#include <stdlib.h>

#include "ctest.h"
#include "test_util.h"

// Serializes into an exact-size heap string, using the buffer helper twice.
static char *to_json(const void *src, const Type_Info *type, const TisonOptions *opt,
                     TisonError *err, TisonErrorCode *code) {
    size_t len = 0;
    *code = tison_serialize_to_buffer(NULL, 0, &len, src, type, opt, err);
    if(*code != TISON_OK) return NULL;
    char *buf = malloc(len + 1);
    *code = tison_serialize_to_buffer(buf, len + 1, &len, src, type, opt, err);
    return buf;
}

static TisonErrorCode write_code(const void *src, const Type_Info *type, const TisonOptions *opt,
                                 TisonError *err) {
    size_t len;
    return tison_serialize_to_buffer(NULL, 0, &len, src, type, opt, err);
}

#define TO_JSON(src, T, opt, code) to_json((src), TI(T), (opt), NULL, (code))

// Parses `doc`, serializes it, parses that again and checks the second serialization is
// identical. Returns the first serialization.
static char *round_trip(const char *doc, const Type_Info *type, size_t size,
                        const TisonOptions *opt) {
    void *a = calloc(1, size), *b = calloc(1, size);
    TisonErrorCode code;
    ASSERT_EQUAL(TISON_OK, tison_deserialize(doc, strlen(doc), a, type, opt, NULL));
    char *first = to_json(a, type, opt, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_EQUAL(TISON_OK, tison_deserialize(first, strlen(first), b, type, opt, NULL));
    char *second = to_json(b, type, opt, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_STR(first, second);
    free(second);
    tison_free(a, type, opt);
    tison_free(b, type, opt);
    free(a);
    free(b);
    return first;
}

// -----------------------------------------------------------------------------
// Output format
// -----------------------------------------------------------------------------

CTEST(serialize, compact_output) {
    Material m = {0};
    strcpy(m.name, "red");
    m.kind = MATERIAL_METAL;
    m.albedo.x = 1.0f;
    m.albedo.y = 0.5f;
    m.roughness = 0.25f;
    TisonErrorCode code;
    char *s = TO_JSON(&m, Material, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_STR(
        "{\"name\":\"red\",\"path\":null,\"kind\":\"MATERIAL_METAL\","
        "\"albedo\":{\"x\":1,\"y\":0.5,\"z\":0},\"roughness\":0.25}",
        s);
    free(s);
}

CTEST(serialize, pretty_output) {
    Node tail = {2, NULL};
    Node head = {1, &tail};
    TisonOptions opt = {0};
    opt.flags = TISON_FLAG_PRETTY;
    opt.indent = 2;
    TisonErrorCode code;
    char *s = TO_JSON(&head, Node, &opt, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_STR(
        "{\n"
        "  \"value\": 1,\n"
        "  \"next\": {\n"
        "    \"value\": 2,\n"
        "    \"next\": null\n"
        "  }\n"
        "}",
        s);
    free(s);

    Model empty = {0};
    int value = 3, *boxed = &value;
    empty.boxed = &boxed;
    opt.indent = 0;
    s = TO_JSON(&empty, Model, &opt, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_STRSTR(s, "\n    \"boxed\": 3,\n");
    ASSERT_STRSTR(s, "\"vertices\": null");
    free(s);
}

// -----------------------------------------------------------------------------
// Round trips
// -----------------------------------------------------------------------------

CTEST(serialize, round_trips) {
    char *s = round_trip(
        "{\"name\":\"tex\",\"path\":\"a\\\"b\\u00e9\",\"kind\":\"MATERIAL_GLASS\","
        "\"albedo\":{\"x\":0.1,\"y\":-3e-7,\"z\":12345.678},\"roughness\":1}",
        TI(Material), sizeof(Material), NULL);
    ASSERT_STRSTR(s, "\"kind\":\"MATERIAL_GLASS\"");
    free(s);

    s = round_trip(
        "{\"vertices\":[{\"x\":1},{\"y\":2}],\"names\":[\"a\",\"b\\nc\"],"
        "\"origin\":{\"z\":4},\"boxed\":7,"
        "\"shapes\":[{\"kind\":\"SHAPE_GROUP\",\"shape\":{\"kind\":\"SHAPE_BOX\","
        "\"shape\":{\"size\":{\"x\":1}}}},{\"kind\":\"SHAPE_NONE\"}]}",
        TI(Model), sizeof(Model), NULL);
    ASSERT_NULL(strstr(s, "vertex_count"));
    ASSERT_NULL(strstr(s, "name_count"));
    ASSERT_STRSTR(s, "{\"kind\":\"SHAPE_NONE\",\"shape\":null,\"id\":0}");
    free(s);

    s = round_trip(
        "{\"i8\":-128,\"u8\":255,\"i16\":-32768,\"u16\":65535,\"i32\":-2147483648,"
        "\"u32\":4294967295,\"i64\":-9223372036854775808,\"u64\":18446744073709551615}",
        TI(Integers), sizeof(Integers), NULL);
    ASSERT_STRSTR(s, "\"i64\":-9223372036854775808,\"u64\":18446744073709551615");
    free(s);

    s = round_trip(
        "{\"perms\":[\"PERM_READ\",\"PERM_WRITE\"],\"history\":[[\"PERM_EXEC\"],[]],"
        "\"raw\":6,\"code\":-2,\"shapes\":[\"SHAPE_BOX\"]}",
        TI(EnumModes), sizeof(EnumModes), NULL);
    ASSERT_STRSTR(s, "\"perms\":[\"PERM_RW\"]");
    free(s);

    s = round_trip("{\"kind\":9,\"shape\":null}", TI(EnumIntTag), sizeof(EnumIntTag), NULL);
    ASSERT_STR("{\"kind\":9,\"shape\":null}", s);
    free(s);

    s = round_trip("{\"e333\":5,\"a000\":1,\"kind\":\"SHAPE_SPHERE\",\"shape\":{\"radius\":2}}",
                   TI(Big), sizeof(Big), NULL);
    ASSERT_STRSTR(s, "\"e333\":5,\"kind\"");
    free(s);
}

CTEST(serialize, floats_round_trip_exactly) {
    float floats[] = {0.1f, FLT_MAX, 0x1p-149f, -0.0f, 1.0f / 3.0f, FLT_MIN};
    double doubles[] = {0.1, 1e300, DBL_MIN / 4, -0.0, DBL_MAX, 1.0 / 3.0};
    for(size_t i = 0; i < sizeof(floats) / sizeof(*floats); i++) {
        Scalars in = {0}, out = {0};
        in.f = floats[i];
        in.d = doubles[i];
        TisonErrorCode code;
        char *s = TO_JSON(&in, Scalars, NULL, &code);
        ASSERT_EQUAL(TISON_OK, code);
        ASSERT_EQUAL(TISON_OK, PARSE(s, &out, Scalars, NULL, NULL));
        if(memcmp(&in.f, &out.f, sizeof(float)) || memcmp(&in.d, &out.d, sizeof(double))) {
            CTEST_LOG("case %zu did not round-trip: %s", i, s);
        }
        ASSERT_TRUE(memcmp(&in.f, &out.f, sizeof(float)) == 0);
        ASSERT_TRUE(memcmp(&in.d, &out.d, sizeof(double)) == 0);
        free(s);
    }

    Wide w = {0};
    w.wide = 1.0L / 3.0L;
    TisonErrorCode code;
    char *s = TO_JSON(&w, Wide, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    Wide back = {0};
    ASSERT_EQUAL(TISON_OK, PARSE(s, &back, Wide, NULL, NULL));
    ASSERT_TRUE(back.wide == w.wide);
    tison_free(&back, TI(Wide), NULL);
    free(s);
}

CTEST(serialize, non_finite_floats_fail) {
    Scalars s = {0};
    TisonError err;
    s.f = INFINITY;
    ASSERT_EQUAL(TISON_ERR_RANGE, write_code(&s, TI(Scalars), NULL, &err));
    ASSERT_STR(".f", err.path);
    s.f = 0;
    s.d = NAN;
    ASSERT_EQUAL(TISON_ERR_RANGE, write_code(&s, TI(Scalars), NULL, &err));

    Misc m = {0};
    m.points[1].y = NAN;
    ASSERT_EQUAL(TISON_ERR_RANGE, write_code(&m, TI(Misc), NULL, &err));
    ASSERT_STR(".points[1].y", err.path);
}

// -----------------------------------------------------------------------------
// Strings and enums
// -----------------------------------------------------------------------------

CTEST(serialize, string_escapes) {
    Misc m = {0};
    m.label = "q\"b\\s/\b\f\n\r\t\x01\x1f \xc3\xa9\xe2\x82\xac";
    m.bytes = (unsigned char *)"raw";
    TisonErrorCode code;
    char *s = TO_JSON(&m, Misc, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_STRSTR(s,
                  "\"label\":\"q\\\"b\\\\s/\\b\\f\\n\\r\\t\\u0001\\u001f \xc3\xa9\xe2\x82\xac\"");
    ASSERT_STRSTR(s, "\"bytes\":\"raw\"");
    ASSERT_STRSTR(s, "\"legacy\":[0,0]");
    free(s);

    Misc bad = {0};
    TisonError err;
    bad.label = "ok\xff";
    ASSERT_EQUAL(TISON_ERR_STRING, write_code(&bad, TI(Misc), NULL, &err));
    ASSERT_STR(".label", err.path);
    bad.label = "\xed\xa0\x80";  // Encoded surrogate
    ASSERT_EQUAL(TISON_ERR_STRING, write_code(&bad, TI(Misc), NULL, NULL));

    Material full = {0};
    memset(full.name, 'a', sizeof(full.name));
    ASSERT_EQUAL(TISON_ERR_STRING, write_code(&full, TI(Material), NULL, &err));
    ASSERT_STR(".name", err.path);
}

CTEST(serialize, enums) {
    Material m = {0};
    TisonError err;
    m.kind = (MaterialKind)3;
    ASSERT_EQUAL(TISON_ERR_RANGE, write_code(&m, TI(Material), NULL, &err));
    ASSERT_STR(".kind", err.path);
    ASSERT_STR("3 is not a declared value of MaterialKind", err.message);
}

CTEST(serialize, enum_int_mode) {
    EnumModes e = {0};
    TisonErrorCode code;
    e.code = (MaterialKind)-7;
    e.raw = PERM_READ | PERM_EXEC;
    char *s = TO_JSON(&e, EnumModes, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_STRSTR(s, "\"raw\":5,\"code\":-7");
    free(s);
}

CTEST(serialize, enum_flags) {
    EnumModes e = {0};
    TisonError err;
    TisonErrorCode code;
    char *s = TO_JSON(&e, EnumModes, NULL, &code);
    ASSERT_STRSTR(s, "\"perms\":[],\"history\":[[],[]]");
    free(s);

    // PERM_RW is declared before its parts, so it covers both bits.
    e.perms = PERM_READ | PERM_WRITE | PERM_EXEC;
    e.history[0] = PERM_WRITE;
    e.shapes = (ShapeKind)(SHAPE_SPHERE | SHAPE_BOX);
    s = TO_JSON(&e, EnumModes, NULL, &code);
    ASSERT_STRSTR(s, "\"perms\":[\"PERM_RW\",\"PERM_EXEC\"],\"history\":[[\"PERM_WRITE\"],[]]");
    // SHAPE_GROUP is 3, both bits, but declared last: SPHERE and BOX cover it first.
    ASSERT_STRSTR(s, "\"shapes\":[\"SHAPE_SPHERE\",\"SHAPE_BOX\"]");
    free(s);

    e.perms = (Permissions)(PERM_READ | 8);
    ASSERT_EQUAL(TISON_ERR_RANGE, write_code(&e, TI(EnumModes), NULL, &err));
    ASSERT_STR(".perms", err.path);
    ASSERT_STR("undeclared bits 0x8 in Permissions", err.message);

    TisonOptions pretty = {0};
    pretty.flags = TISON_FLAG_PRETTY;
    pretty.indent = 2;
    e.perms = PERM_EXEC;
    s = TO_JSON(&e, EnumModes, &pretty, &code);
    ASSERT_STRSTR(s, "\"perms\": [\n    \"PERM_EXEC\"\n  ],");
    free(s);
}

CTEST(serialize, enum_annotation_errors) {
    TisonError err;
    EnumConflict conflict = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&conflict, TI(EnumConflict), NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "mutually exclusive"));
    EnumOnInt on_int = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&on_int, TI(EnumOnInt), NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "require an enum"));
    EnumSignedFlags neg = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&neg, TI(EnumSignedFlags), NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "negative enumerator"));
    EnumFlagsTag tag = {0};
    tag.kind = PERM_READ;
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&tag, TI(EnumFlagsTag), NULL, &err));
    ASSERT_NOT_NULL(strstr(err.message, "cannot be a tag"));
}

// -----------------------------------------------------------------------------
// Field rules
// -----------------------------------------------------------------------------

CTEST(serialize, field_rules) {
    Model m = {0};
    m.vertex_count = 2;
    TisonError err;
    ASSERT_EQUAL(TISON_ERR_RANGE, write_code(&m, TI(Model), NULL, &err));
    ASSERT_STR(".vertices", err.path);

    int row = 1;
    int *rows[] = {&row};
    Mesh grid = {0};
    grid.rows = rows;
    grid.row_count = -1;
    ASSERT_EQUAL(TISON_ERR_RANGE, write_code(&grid, TI(Mesh), NULL, &err));
    ASSERT_STR(".rows", err.path);
    ASSERT_NOT_NULL(strstr(err.message, "negative count"));

    int cached = 1;
    Skipped k = {0};
    k.visible = 4;
    k.cache = &cached;
    k.hidden_a = 9;
    TisonErrorCode code;
    char *s = TO_JSON(&k, Skipped, NULL, &code);
    ASSERT_EQUAL(TISON_OK, code);
    ASSERT_STR("{\"visible\":4}", s);
    free(s);

    Layout l = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&l, TI(Layout), NULL, NULL));
    Mesh mesh = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&mesh, TI(Mesh), NULL, &err));
    ASSERT_STR(".opaque", err.path);
    UnionCases u = {0};
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&u, TI(UnionCases), NULL, &err));
}

// -----------------------------------------------------------------------------
// Hooks
// -----------------------------------------------------------------------------

static bool match_vec3(const TisonTypeContext *ctx, void *user_data) {
    (void)user_data;
    return ctx->type == TI(Vec3);
}

static bool parse_vec3(TisonParser *p, void *dst, const TisonTypeContext *ctx, void *user_data) {
    (void)ctx;
    (void)user_data;
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

static bool write_vec3(TisonWriter *w, const void *src, const TisonTypeContext *ctx,
                       void *user_data) {
    (void)ctx;
    (void)user_data;
    const Vec3 *v = src;
    TisonWriteContainer c;
    return tison_write_array_begin(w, &c) && tison_write_element(w, &c) &&
           tison_write_float(w, v->x) && tison_write_element(w, &c) && tison_write_float(w, v->y) &&
           tison_write_element(w, &c) && tison_write_float(w, v->z) && tison_write_array_end(w, &c);
}

CTEST(serialize, hooks) {
    TisonTypeHook hooks[] = {{match_vec3, parse_vec3, write_vec3, NULL, NULL}};
    TisonOptions opt = {0};
    opt.hooks = hooks;
    opt.hook_count = 1;
    char *s = round_trip("{\"albedo\":[0.5,1,2],\"name\":\"m\"}", TI(Material), sizeof(Material),
                         &opt);
    ASSERT_STRSTR(s, "\"albedo\":[0.5,1,2]");
    free(s);

    TisonTypeHook read_only[] = {{match_vec3, parse_vec3, NULL, NULL, NULL}};
    opt.hooks = read_only;
    Material m = {0};
    TisonError err;
    ASSERT_EQUAL(TISON_ERR_UNSUPPORTED, write_code(&m, TI(Material), &opt, &err));
    ASSERT_STR(".albedo", err.path);
}

// -----------------------------------------------------------------------------
// Output targets
// -----------------------------------------------------------------------------

CTEST(serialize, buffer_helper) {
    Vec3 v = {1, 2, 3};
    const char *expected = "{\"x\":1,\"y\":2,\"z\":3}";
    size_t full = strlen(expected), len = 0;
    char buf[64];

    ASSERT_EQUAL(TISON_OK, tison_serialize_to_buffer(NULL, 0, &len, &v, TI(Vec3), NULL, NULL));
    ASSERT_EQUAL(full, len);

    memset(buf, '#', sizeof(buf));
    ASSERT_EQUAL(TISON_OK,
                 tison_serialize_to_buffer(buf, full + 1, &len, &v, TI(Vec3), NULL, NULL));
    ASSERT_STR(expected, buf);

    memset(buf, '#', sizeof(buf));
    ASSERT_EQUAL(TISON_OK, tison_serialize_to_buffer(buf, 5, &len, &v, TI(Vec3), NULL, NULL));
    ASSERT_EQUAL(full, len);
    ASSERT_STR("{\"x\"", buf);
    ASSERT_EQUAL('#', buf[5]);
}

CTEST(serialize, file_helper) {
    FILE *f = tmpfile();
    ASSERT_NOT_NULL(f);
    Vec3 v = {1, 2, 3};
    ASSERT_EQUAL(TISON_OK, tison_serialize_to_file(f, &v, TI(Vec3), NULL, NULL));
    rewind(f);
    char buf[64] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    ASSERT_EQUAL(strlen("{\"x\":1,\"y\":2,\"z\":3}"), n);
    ASSERT_STR("{\"x\":1,\"y\":2,\"z\":3}", buf);
}

static bool failing_write(void *ctx, const char *data, size_t len) {
    (void)ctx;
    (void)data;
    (void)len;
    return false;
}

CTEST(serialize, sink_failure_and_cycles) {
    Vec3 v = {0};
    TisonSink sink = {NULL, failing_write};
    ASSERT_EQUAL(TISON_ERR_IO, tison_serialize(sink, &v, TI(Vec3), NULL, NULL));

    Node loop = {1, NULL};
    loop.next = &loop;
    TisonError err;
    ASSERT_EQUAL(TISON_ERR_DEPTH, write_code(&loop, TI(Node), NULL, &err));
}
