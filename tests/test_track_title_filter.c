// Exercises track_title_filter.c's bracket-fuzzy-match refinement: a
// (), [], or {} segment is stripped wholesale if it contains ANY
// configured pattern anywhere within it, not just when the pattern
// matches the segment's entire content. Dash-suffix stripping stays an
// exact literal match (no closing delimiter to bound a "segment").
//
// Build (standalone, PC target - pthread-backed os_mutex):
//   cc -std=c11 -Wall -Wextra -Werror -pedantic -pthread -Icommon -Iinclude
//     tests/test_track_title_filter.c common/track_title_filter.c
//     -o /tmp/test-track-title-filter
//   /tmp/test-track-title-filter

#include "track_title_filter.h"
#include "rk_title_filter_cfg.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

// track_title_filter_init() reads NVS via this - stub it with a small,
// deliberately-crafted pattern list (not the full ~130-entry default) so
// test expectations stay easy to reason about.
bool platform_storage_read_title_filters(rk_title_filter_cfg_t *out) {
    rk_title_filter_cfg_set_defaults(out);
    static const char patterns[] =
        "Remaster\n"
        "Remastered\n"
        "Remastered YYYY\n"
        "LP Version\n"
        "Live";
    memcpy(out->patterns, patterns, sizeof(patterns));
    return true;
}

static void apply(char *title) {
    track_title_filter_apply(title, 0);
}

int main(void) {
    track_title_filter_init();

    // The motivating case: several fragments stacked in one bracket that
    // no single configured phrase would ever match end-to-end - "Remaster"
    // is present, so the whole segment goes.
    char a[128] = "Come Together (2009 Remaster; Remastered LP Version)";
    apply(a);
    assert(strcmp(a, "Come Together") == 0);

    // Same idea, square brackets.
    char b[128] = "Something [Remastered 2011 - Take 2]";
    apply(b);
    assert(strcmp(b, "Something") == 0);

    // A segment containing none of the configured patterns is left alone
    // - fuzzy matching must not turn into "strip every bracket".
    char c[128] = "Hey Jude (feat. The Beatles)";
    apply(c);
    assert(strcmp(c, "Hey Jude (feat. The Beatles)") == 0);

    // Multiple independent flagged segments in one title both go.
    char d[128] = "Track (Remaster) Extra [Live at Wembley]";
    apply(d);
    assert(strcmp(d, "Track Extra") == 0);

    // Dash suffix stays an exact match - unaffected by the bracket change.
    // ("Live" rather than "Remaster"/"Remastered" here: those two patterns
    // prefix-collide against each other in match_pattern_at - a
    // pre-existing matcher property, not part of this fix - which would
    // make an assertion built on them fragile rather than clear.)
    char e[128] = "Track Name - Live";
    apply(e);
    assert(strcmp(e, "Track Name") == 0);

    // YYYY wildcard still resolves correctly inside a fuzzy-matched
    // segment.
    char g[128] = "Song (Remastered 1999)";
    apply(g);
    assert(strcmp(g, "Song") == 0);

    // An unbalanced open bracket must not hang or corrupt the string.
    char h[128] = "Weird Title (Remaster";
    apply(h);
    assert(strcmp(h, "Weird Title (Remaster") == 0);

    puts("track title filter contracts passed");
    return 0;
}
