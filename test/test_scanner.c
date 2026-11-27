#include "ctest.h"
#include "test_util.h"

static TisonErrorCode skip(const char *src, size_t max_depth) {
    TisonOptions opt = {0};
    opt.max_depth = max_depth;
    TisonParser p;
    tison_parser_init(&p, src, strlen(src), &opt);
    if(!tison_skip_value(&p)) return tison_parser_error(&p)->code;
    if(!tison_at_end(&p)) return TISON_ERR_SYNTAX;
    return TISON_OK;
}

CTEST(scanner, valid_values) {
    const char *valid[] = {
        "{}",          "[]",
        " \t\r\n[ ] ", "\"\"",
        "\"x\"",       "0",
        "-0",          "1.5e10",
        "-1E-2",       "2e+3",
        "true",        "false",
        "null",        "{\"a\":[1,{\"b\":\"],}\"}],\"c\":null}",
        "[[[]]]",      "\"\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80\"",
    };
    for(size_t i = 0; i < sizeof(valid) / sizeof(*valid); i++) {
        ASSERT_EQUAL(TISON_OK, skip(valid[i], 0));
    }
}

CTEST(scanner, invalid_syntax) {
    const char *invalid[] = {
        "",    "   ",  "01",         "-01",   "1.",        ".5",    "1e",   "1e+",    "+1",
        "-",   "[1,]", "{\"a\":1,}", "[1 2]", "{\"a\" 1}", "{1:2}", "[,1]", "[1,,2]", "tru",
        "nul", "fals", "[",          "{",     "{\"a\":",   "]",     "x",    "[1]]",   "{} {}",
    };
    for(size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        TisonErrorCode code = skip(invalid[i], 0);
        if(code == TISON_OK) CTEST_LOG("accepted invalid input '%s'", invalid[i]);
        ASSERT_NOT_EQUAL(TISON_OK, code);
    }
}

CTEST(scanner, invalid_strings) {
    const char *invalid[] = {
        "\"abc",
        "\"\\x\"",
        "\"\\u12G4\"",
        "\"\\u12\"",
        "\"\\uD800\"",
        "\"\\uDC00\"",
        "\"\\uD800\\u0041\"",
        "\"a\nb\"",
        "\"\t\"",
        "\"\xc0\xaf\"",
        "\"\xed\xa0\x80\"",
        "\"\xf5\x80\x80\x80\"",
        "\"\xe2\x82\"",
        "\"\x80\"",
        "\"\\",
    };
    for(size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++) {
        TisonErrorCode code = skip(invalid[i], 0);
        if(code != TISON_ERR_STRING) CTEST_LOG("string case %zu gave %d", i, (int)code);
        ASSERT_EQUAL(TISON_ERR_STRING, code);
    }
}

CTEST(scanner, string_decoding) {
    const char *src = "\"a\\\"b\\\\c\\/\\n\\u00e9\\ud83d\\ude00\\u0000\"";
    const char expected[] = "a\"b\\c/\n\xc3\xa9\xf0\x9f\x98\x80";
    TisonParser p;
    tison_parser_init(&p, src, strlen(src), NULL);
    TisonSpan raw;
    TisonStringInfo info;
    ASSERT_TRUE(tison_scan_string(&p, &raw, &info));
    ASSERT_TRUE(info.has_escapes);
    ASSERT_TRUE(info.has_nul);
    ASSERT_EQUAL(sizeof(expected), info.decoded_len);  // Includes the decoded \u0000
    char buf[64];
    ASSERT_EQUAL(info.decoded_len, tison_decode_string(raw, buf));
    ASSERT_DATA((const unsigned char *)expected, sizeof(expected), (const unsigned char *)buf,
                info.decoded_len);
}

CTEST(scanner, unescaped_string_is_raw_span) {
    const char *src = "  \"hello\"";
    TisonParser p;
    tison_parser_init(&p, src, strlen(src), NULL);
    TisonSpan raw;
    TisonStringInfo info;
    ASSERT_TRUE(tison_scan_string(&p, &raw, &info));
    ASSERT_FALSE(info.has_escapes);
    ASSERT_EQUAL(5, info.decoded_len);
    ASSERT_TRUE(raw.begin == src + 3);
}

CTEST(scanner, no_nul_termination_needed) {
    const char src[] = {'[', '1', ',', '2', ']', '9'};
    TisonParser p;
    tison_parser_init(&p, src, 5, NULL);
    ASSERT_TRUE(tison_skip_value(&p));
    ASSERT_TRUE(tison_at_end(&p));

    tison_parser_init(&p, src, 2, NULL);
    ASSERT_FALSE(tison_skip_value(&p));
    ASSERT_EQUAL(TISON_ERR_SYNTAX, tison_parser_error(&p)->code);
}

CTEST(scanner, depth_limit) {
    char deep[512];
    memset(deep, '[', 200);
    memset(deep + 200, ']', 200);
    deep[400] = '\0';
    ASSERT_EQUAL(TISON_ERR_DEPTH, skip(deep, 0));
    ASSERT_EQUAL(TISON_OK, skip(deep, 200));
    ASSERT_EQUAL(TISON_ERR_DEPTH, skip("[[[1]]]", 2));
    ASSERT_EQUAL(TISON_OK, skip("[[[1]]]", 3));
    ASSERT_EQUAL(TISON_ERR_DEPTH, skip("{\"a\":{\"b\":{}}}", 2));
}

CTEST(scanner, count_array) {
    const char *src = "[1, [2,3], {\"a\":[4]}, \"x,]\"] tail";
    TisonParser p;
    tison_parser_init(&p, src, strlen(src), NULL);
    size_t count = 0;
    ASSERT_TRUE(tison_count_array(&p, &count));
    ASSERT_EQUAL(4, count);
    ASSERT_EQUAL(0, p.depth);

    tison_parser_init(&p, " [ ] ", 5, NULL);
    ASSERT_TRUE(tison_count_array(&p, &count));
    ASSERT_EQUAL(0, count);

    tison_parser_init(&p, "[1,,2]", 6, NULL);
    ASSERT_FALSE(tison_count_array(&p, &count));

    tison_parser_init(&p, "{}", 2, NULL);
    ASSERT_FALSE(tison_count_array(&p, &count));
    ASSERT_EQUAL(TISON_ERR_TYPE, tison_parser_error(&p)->code);
}

CTEST(scanner, error_position) {
    const char *src = "{\n  \"a\": tru\n}";
    TisonParser p;
    tison_parser_init(&p, src, strlen(src), NULL);
    ASSERT_FALSE(tison_skip_value(&p));
    const TisonError *e = tison_parser_error(&p);
    ASSERT_EQUAL(TISON_ERR_SYNTAX, e->code);
    ASSERT_EQUAL(2, e->line);
    ASSERT_EQUAL(8, e->column);
    ASSERT_EQUAL(9, e->offset);
    ASSERT_STR(".a", e->path);
}

CTEST(scanner, mark_and_reset) {
    const char *src = "[1, 2]";
    TisonParser p;
    tison_parser_init(&p, src, strlen(src), NULL);
    TisonMark m = tison_mark(&p);
    ASSERT_TRUE(tison_skip_value(&p));
    tison_reset(&p, m);
    ASSERT_TRUE(p.cur == src);
    ASSERT_TRUE(tison_skip_value(&p));
    ASSERT_TRUE(tison_at_end(&p));
}
