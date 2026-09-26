#include "json_string.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_plain(void) {
    char out[32];
    const char *end = json_string_copy("hello\" rest", out, sizeof(out));
    assert(end && strcmp(out, "hello") == 0);
    assert(strcmp(end, " rest") == 0);
}

// The reported bug: a title containing escaped quotes was cut at the first \".
static void test_escaped_quotes(void) {
    char out[64];
    const char *end = json_string_copy("Down to Earth (From \\\"WALL-E\\\")\",\"line2\":\"x\"", out, sizeof(out));
    assert(end);
    assert(strcmp(out, "Down to Earth (From \"WALL-E\")") == 0);
    assert(strcmp(end, ",\"line2\":\"x\"") == 0);
}

static void test_other_escapes(void) {
    char out[64];
    assert(json_string_copy("a\\\\b\\/c\\nd\\te\"", out, sizeof(out)));
    assert(strcmp(out, "a\\b/c\nd\te") == 0);
    // A string ending in an escaped backslash still terminates at the following quote.
    const char *end = json_string_copy("x\\\\\" tail", out, sizeof(out));
    assert(end && strcmp(out, "x\\") == 0 && strcmp(end, " tail") == 0);
}

static void test_unicode(void) {
    char out[64];
    assert(json_string_copy("caf\\u00e9\"", out, sizeof(out)));
    assert(strcmp(out, "caf\xC3\xA9") == 0);
    assert(json_string_copy("\\u20ac\"", out, sizeof(out)));
    assert(strcmp(out, "\xE2\x82\xAC") == 0);
    // Surrogate pair: U+1F3B5
    assert(json_string_copy("\\ud83c\\udfb5\"", out, sizeof(out)));
    assert(strcmp(out, "\xF0\x9F\x8E\xB5") == 0);
    // Lone surrogate and malformed digits do not crash or overrun.
    assert(json_string_copy("\\ud83c!\"", out, sizeof(out)));
    assert(strcmp(out, "?!") == 0);
    assert(json_string_copy("\\u12g4\"", out, sizeof(out)));
    assert(strcmp(out, "u12g4") == 0);
}

static void test_truncation(void) {
    char out[6];
    const char *end = json_string_copy("abcdefghij\" more", out, sizeof(out));
    assert(end && strcmp(out, "abcde") == 0);
    assert(strcmp(end, " more") == 0);
    // A multi-byte character that does not fit is dropped whole, never split.
    char small[4];
    assert(json_string_copy("ab\\u20ac\"", small, sizeof(small)));
    assert(strcmp(small, "ab") == 0);
}

static void test_unterminated_and_bad_args(void) {
    char out[16];
    assert(json_string_copy("no end", out, sizeof(out)) == NULL);
    assert(json_string_copy("ends in escape\\", out, sizeof(out)) == NULL);
    assert(json_string_copy("x\"", out, 0) == NULL);
    assert(json_string_copy(NULL, out, sizeof(out)) == NULL);
    assert(json_string_copy("x\"", NULL, 4) == NULL);
    assert(json_string_copy("\"", out, sizeof(out)) && out[0] == '\0');
}

int main(void) {
    test_plain();
    test_escaped_quotes();
    test_other_escapes();
    test_unicode();
    test_truncation();
    test_unterminated_and_bad_args();
    printf("json_string ok\n");
    return 0;
}
