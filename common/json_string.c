#include "json_string.h"

#include <stdint.h>

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Reads exactly four hex digits at `p`; returns -1 if any is not one.
static int32_t read_hex4(const char *p) {
    int32_t value = 0;
    for (int i = 0; i < 4; i++) {
        int d = hex_digit(p[i]);
        if (d < 0) return -1;
        value = (value << 4) | d;
    }
    return value;
}

// Appends the UTF-8 encoding of `cp`, but only if all of it fits (never splits a character).
static void put_utf8(char *out, size_t cap, size_t *pos, uint32_t cp) {
    char enc[4];
    size_t n;
    if (cp < 0x80) {
        enc[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        enc[0] = (char)(0xC0 | (cp >> 6));
        enc[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else if (cp < 0x10000) {
        enc[0] = (char)(0xE0 | (cp >> 12));
        enc[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        enc[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    } else {
        enc[0] = (char)(0xF0 | (cp >> 18));
        enc[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        enc[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        enc[3] = (char)(0x80 | (cp & 0x3F));
        n = 4;
    }
    if (*pos + n > cap) return;
    for (size_t i = 0; i < n; i++) out[(*pos)++] = enc[i];
}

const char *json_string_copy(const char *after_open_quote, char *out, size_t out_len) {
    if (!after_open_quote || !out || out_len == 0) {
        return NULL;
    }
    const size_t cap = out_len - 1;
    size_t pos = 0;
    const char *p = after_open_quote;
    while (*p) {
        char c = *p;
        if (c == '"') {
            out[pos] = '\0';
            return p + 1;
        }
        if (c != '\\') {
            if (pos < cap) out[pos++] = c;
            p++;
            continue;
        }
        // Escape sequence.
        p++;
        char e = *p;
        if (e == '\0') break;
        p++;
        switch (e) {
            case 'b': e = '\b'; break;
            case 'f': e = '\f'; break;
            case 'n': e = '\n'; break;
            case 'r': e = '\r'; break;
            case 't': e = '\t'; break;
            case 'u': {
                int32_t cp = read_hex4(p);
                if (cp < 0) {
                    // Malformed: keep the letter, let the digits (if any) follow as text.
                    e = 'u';
                    break;
                }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    int32_t low = read_hex4(p + 2);
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        p += 6;
                    }
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = '?';  // lone surrogate
                put_utf8(out, cap, &pos, (uint32_t)cp);
                continue;
            }
            default:
                // '"', '\\', '/' and anything unknown stand for themselves.
                break;
        }
        if (pos < cap) out[pos++] = e;
    }
    out[pos] = '\0';
    return NULL;
}
