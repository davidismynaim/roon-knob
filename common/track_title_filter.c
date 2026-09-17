#include "track_title_filter.h"

#include "os_mutex.h"
#include "platform/platform_storage.h"
#include "rk_title_filter_cfg.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// One pattern per configured line; kept short since these are short tag
// fragments like "Remastered YYYY", not arbitrary text. Sized for the
// ~130-entry built-in default list plus headroom for user additions.
#define MAX_PATTERNS 200
#define MAX_PATTERN_LEN 48

static os_mutex_t s_lock = OS_MUTEX_INITIALIZER;
static char s_patterns[MAX_PATTERNS][MAX_PATTERN_LEN];
static int s_pattern_count;

static void load_patterns(const char *blob) {
    s_pattern_count = 0;
    const char *p = blob;
    while (*p && s_pattern_count < MAX_PATTERNS) {
        const char *line_end = strchr(p, '\n');
        size_t len = line_end ? (size_t)(line_end - p) : strlen(p);

        // Trim a trailing \r (Windows-style line endings from the config
        // page's textarea) and surrounding whitespace.
        while (len > 0 && (p[len - 1] == '\r' || p[len - 1] == ' ' ||
                            p[len - 1] == '\t')) {
            len--;
        }
        const char *start = p;
        while (len > 0 && (*start == ' ' || *start == '\t')) {
            start++;
            len--;
        }

        if (len > 0 && len < MAX_PATTERN_LEN) {
            memcpy(s_patterns[s_pattern_count], start, len);
            s_patterns[s_pattern_count][len] = '\0';
            s_pattern_count++;
        }

        if (!line_end) {
            break;
        }
        p = line_end + 1;
    }
}

void track_title_filter_init(void) {
    // rk_title_filter_cfg_t is 4KB+ (RK_TITLE_FILTER_PATTERNS_MAX patterns
    // buffer) - heap, not a stack local. This runs deep in app_main()'s
    // call chain on the main task, whose entire stack is only 8KB; a 4KB+
    // local here silently overflowed it in practice, corrupting adjacent
    // memory in a way that only crashed later, elsewhere (WiFi startup).
    rk_title_filter_cfg_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        return;
    }
    if (!platform_storage_read_title_filters(cfg)) {
        free(cfg);
        return;
    }
    os_mutex_lock(&s_lock);
    load_patterns(cfg->patterns);
    os_mutex_unlock(&s_lock);
    free(cfg);
}

static bool char_eq_ci(char a, char b) {
    return tolower((unsigned char)a) == tolower((unsigned char)b);
}

// Attempts to match `pattern` starting at text[0]. Three wildcard tokens,
// checked longest-first so a genuine "YYYY" in the pattern is never
// misread as two "YY"s (which would then try to match literal 'Y'
// characters against the title's remaining digits and fail):
//   YYYY - exactly 4 consecutive digits
//   YY   - exactly 2 consecutive digits
//   NUM  - 1 or more consecutive digits (greedy; fine here since every
//          default pattern has a non-digit literal right after a NUM)
// Everything else matches literally, case-insensitively. Returns the
// number of characters in `text` consumed by a full match, or 0 if the
// pattern doesn't match here.
static size_t match_pattern_at(const char *text, const char *pattern) {
    size_t ti = 0;
    size_t pi = 0;
    size_t plen = strlen(pattern);
    while (pi < plen) {
        if (plen - pi >= 4 && memcmp(pattern + pi, "YYYY", 4) == 0) {
            for (int k = 0; k < 4; k++) {
                if (!isdigit((unsigned char)text[ti])) {
                    return 0;
                }
                ti++;
            }
            pi += 4;
        } else if (plen - pi >= 2 && memcmp(pattern + pi, "YY", 2) == 0) {
            for (int k = 0; k < 2; k++) {
                if (!isdigit((unsigned char)text[ti])) {
                    return 0;
                }
                ti++;
            }
            pi += 2;
        } else if (plen - pi >= 3 && memcmp(pattern + pi, "NUM", 3) == 0) {
            if (!isdigit((unsigned char)text[ti])) {
                return 0;
            }
            while (isdigit((unsigned char)text[ti])) {
                ti++;
            }
            pi += 3;
        } else {
            if (!text[ti] || !char_eq_ci(text[ti], pattern[pi])) {
                return 0;
            }
            ti++;
            pi++;
        }
    }
    return ti;
}

// Removes the first match of `pattern` found anywhere in `title`, in
// place. Returns true if a match was found and removed.
static bool strip_first_match(char *title, const char *pattern) {
    size_t title_len = strlen(title);
    for (size_t i = 0; i < title_len; i++) {
        size_t consumed = match_pattern_at(title + i, pattern);
        if (consumed > 0) {
            // +1 to also shift the NUL terminator.
            memmove(title + i, title + i + consumed,
                    title_len - i - consumed + 1);
            return true;
        }
    }
    return false;
}

// Builds `prefix` + `phrase` + `suffix` into `out` (sized MAX_PATTERN_LEN +
// 4, always enough for phrase's real MAX_PATTERN_LEN bound plus a 1-char
// prefix/suffix pair). Doing this with explicit lengths instead of
// snprintf("%s%s%s", ...) sidesteps a -Wformat-truncation false positive:
// GCC can't see through the pointer parameter that `phrase` is already
// bounded to MAX_PATTERN_LEN by load_patterns(), so it assumes the %s could
// be arbitrarily long and warns that `out` might be too small.
static void build_wrapped(char *out, const char *prefix, const char *phrase,
                          const char *suffix) {
    size_t pos = 0;
    size_t cap = MAX_PATTERN_LEN + 4;
    size_t n = strlen(prefix);
    if (n > cap - 1 - pos) n = cap - 1 - pos;
    memcpy(out + pos, prefix, n);
    pos += n;

    n = strlen(phrase);
    if (n > cap - 1 - pos) n = cap - 1 - pos;
    memcpy(out + pos, phrase, n);
    pos += n;

    n = strlen(suffix);
    if (n > cap - 1 - pos) n = cap - 1 - pos;
    memcpy(out + pos, suffix, n);
    pos += n;

    out[pos] = '\0';
}

// Configured patterns are bare inner phrases ("Remastered YYYY"), not
// "- Remastered YYYY" - a dash-suffix tag has no closing delimiter to
// bound a "segment" the way (), [], {} do (see strip_flagged_segment
// below, which handles those instead), so this stays an exact
// literal-phrase match.
static void strip_dash_suffix_everywhere(char *title, const char *phrase) {
    char wrapped[MAX_PATTERN_LEN + 4];
    build_wrapped(wrapped, "- ", phrase, "");
    while (strip_first_match(title, wrapped)) {
    }
}

// Finds the first `open...close` segment anywhere in `title` that contains
// ANY configured pattern anywhere within it - not necessarily spanning the
// whole segment - and strips the segment wholesale, delimiters included.
// Real-world tags stack several fragments in one bracket
// ("2009 Remaster; Remastered LP Version") that no single configured
// phrase would ever exactly match end-to-end; requiring only that one
// known fragment be *present* catches the whole combination without an
// ever-growing list of every phrase-combination anyone has ever used.
// Returns true (and stops at the first hit) if a segment was stripped, so
// the caller can loop for any further matching segments in the title.
static bool strip_flagged_segment(char *title, char open, char close) {
    size_t len = strlen(title);
    for (size_t i = 0; i < len; i++) {
        if (title[i] != open) {
            continue;
        }
        size_t j = i + 1;
        while (j < len && title[j] != close) {
            j++;
        }
        if (j >= len) {
            break;  // Unbalanced open with no matching close - nothing left to find
        }

        bool flagged = false;
        for (int p = 0; p < s_pattern_count && !flagged; p++) {
            for (size_t k = i + 1; k < j && !flagged; k++) {
                size_t consumed = match_pattern_at(title + k, s_patterns[p]);
                // Must land fully inside the segment - a match that runs
                // past the closing delimiter isn't really "in" it.
                if (consumed > 0 && k + consumed <= j) {
                    flagged = true;
                }
            }
        }

        if (flagged) {
            memmove(title + i, title + j + 1, len - j);  // len-j includes the NUL
            return true;
        }
        i = j;  // Not flagged - skip past this segment and keep scanning
    }
    return false;
}

static void strip_flagged_segments_everywhere(char *title, char open, char close) {
    while (strip_flagged_segment(title, open, close)) {
    }
}

// Collapses runs of spaces/tabs to a single space and trims both ends -
// stripping a pattern like " (Remastered 2011)" from the middle of a
// title (rare, but not impossible) can otherwise leave a double space
// behind.
static void collapse_whitespace(char *s) {
    size_t len = strlen(s);
    size_t w = 0;
    bool in_space = true;
    for (size_t r = 0; r < len; r++) {
        if (s[r] == ' ' || s[r] == '\t') {
            if (!in_space) {
                s[w++] = ' ';
                in_space = true;
            }
        } else {
            s[w++] = s[r];
            in_space = false;
        }
    }
    while (w > 0 && s[w - 1] == ' ') {
        w--;
    }
    s[w] = '\0';
}

void track_title_filter_apply(char *title, size_t title_buf_len) {
    (void)title_buf_len;
    if (!title || !title[0]) {
        return;
    }

    os_mutex_lock(&s_lock);
    // Bracketed/parenthesized/braced segments: fuzzy - stripped whole if
    // they contain any configured pattern anywhere within them.
    strip_flagged_segments_everywhere(title, '(', ')');
    strip_flagged_segments_everywhere(title, '[', ']');
    strip_flagged_segments_everywhere(title, '{', '}');
    // Dash suffixes: no bounding close delimiter, so still an exact
    // literal-phrase match per configured pattern.
    for (int i = 0; i < s_pattern_count; i++) {
        strip_dash_suffix_everywhere(title, s_patterns[i]);
    }
    os_mutex_unlock(&s_lock);

    collapse_whitespace(title);
}
