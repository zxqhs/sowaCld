#include "track.h"
#include "log.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

void track_clear(TrackInfo *t)
{
    memset(t, 0, sizeof *t);
}

#define EM_DASH "\xE2\x80\x94"
#define EN_DASH "\xE2\x80\x93"

static void strip_browser_suffix(char *s)
{
    static const char *suf[] = {
        " " EM_DASH " Zen Browser", " - Zen Browser",
        " " EM_DASH " Firefox", " - Firefox",
        " " EM_DASH " Mozilla Firefox", " - Mozilla Firefox",
        " " EM_DASH " Google Chrome", " - Google Chrome",
        " " EM_DASH " Chromium", " - Chromium",
        " " EM_DASH " Brave", " - Brave",
        " " EM_DASH " Vivaldi", " - Vivaldi",
        " " EM_DASH " Microsoft Edge", " - Microsoft Edge",
        " " EM_DASH " Librewolf", " - Librewolf",
        " " EM_DASH " Floorp", " - Floorp",
        " " EN_DASH " Zen Browser",
        NULL
    };
    size_t n, sl, i;

    if (!s || !(n = strlen(s)))
        return;
    for (i = 0; suf[i]; i++) {
        sl = strlen(suf[i]);
        if (n > sl && sc_strcasecmp(s + n - sl, suf[i]) == 0) {
            s[n - sl] = '\0';
            sc_trim(s);
            return;
        }
    }
}

static int is_foreign_site(const char *s)
{
    static const char *bad[] = {
        "youtube", "youtu.be", "google search", "google.com",
        "duckduckgo", "bing.com", "bing search", "yahoo",
        "reddit", "twitter", "tiktok", "instagram", "twitch",
        "spotify", "wikipedia", "wikihow", "facebook",
        "soundcloud-rpc",
        NULL
    };
    int i;

    for (i = 0; bad[i]; i++) {
        if (sc_strcasestr(s, bad[i]))
            return 1;
    }
    return 0;
}

static int has_sc_site_marker(const char *s)
{
    size_t n;

    if (sc_strcasestr(s, "| soundcloud"))
        return 1;
    if (sc_strcasestr(s, "with soundcloud"))
        return 1;
    if (sc_strcasestr(s, "free listening on soundcloud"))
        return 1;
    if (sc_strcasestr(s, "hear the world") && sc_strcasestr(s, "soundcloud"))
        return 1;
    if (sc_istartswith(s, "soundcloud"))
        return 1;
    n = strlen(s);
    if (n >= 14 && sc_strcasecmp(s + n - 14, " on soundcloud") == 0)
        return 1;
    return 0;
}

int track_is_soundcloud_title(const char *title)
{
    char buf[512];

    if (!title || !title[0])
        return 0;
    sc_strlcpy(buf, title, sizeof buf);
    sc_trim(buf);
    strip_browser_suffix(buf);
    if (is_foreign_site(buf))
        return 0;
    return has_sc_site_marker(buf);
}

static void strip_soundcloud_suffix(char *s)
{
    char *p, *last = NULL;

    for (p = s; ; ) {
        char *bar = strstr(p, " | ");
        if (!bar)
            break;
        if (sc_strcasestr(bar + 3, "soundcloud"))
            last = bar;
        p = bar + 3;
    }
    if (last)
        *last = '\0';

    p = sc_strcasestr(s, " - hear the");
    if (p)
        *p = '\0';

    sc_trim(s);
}

static int try_search_title(const char *s, char *details, size_t cap)
{
    static const char *markers[] = {
        " results on soundcloud",
        " results | soundcloud",
        " search | soundcloud",
        NULL
    };
    int i;

    for (i = 0; markers[i]; i++) {
        const char *p = sc_strcasestr(s, markers[i]);
        if (p && p != s) {
            char q[101];
            size_t n = (size_t)(p - s);
            if (n >= sizeof q)
                n = sizeof q - 1;
            memcpy(q, s, n);
            q[n] = '\0';
            sc_trim(q);
            if (!q[0])
                continue;
            snprintf(details, cap, "Searching: '%s'", q);
            return 1;
        }
    }
    return 0;
}

static int is_generic_browsing(const char *s)
{
    if (!s || !s[0])
        return 1;
    if (sc_strcasecmp(s, "soundcloud") == 0)
        return 1;
    if (sc_istartswith(s, "stream and listen"))
        return 1;
    if (sc_istartswith(s, "hear the world"))
        return 1;
    if (sc_istartswith(s, "discover the top"))
        return 1;
    return 0;
}

void track_parse(const char *raw_title, TrackInfo *out)
{
    char work[512];
    char *by, *last_by = NULL, *p;

    track_clear(out);
    if (!raw_title)
        return;
    sc_strlcpy(out->raw, raw_title, sizeof out->raw);
    if (!track_is_soundcloud_title(raw_title))
        return;

    sc_strlcpy(work, raw_title, sizeof work);
    sc_trim(work);
    strip_browser_suffix(work);

    if (try_search_title(work, out->details, sizeof out->details)) {
        out->kind = TRACK_BROWSING;
        out->searching = 1;
        return;
    }

    strip_soundcloud_suffix(work);

    if (is_generic_browsing(work)) {
        out->kind = TRACK_BROWSING;
        sc_strlcpy(out->details, "Browsing", sizeof out->details);
        return;
    }

    for (p = work; (by = sc_strcasestr(p, " by ")) != NULL; p = by + 4)
        last_by = by;

    if (last_by) {
        *last_by = '\0';
        sc_trim(work);
        sc_trim(last_by + 4);
        if (work[0] && last_by[4]) {
            out->kind = TRACK_PLAYING;
            sc_strlcpy(out->title, work, sizeof out->title);
            sc_strlcpy(out->artist, last_by + 4, sizeof out->artist);
            sc_utf8_clamp(out->title, 128);
            sc_utf8_clamp(out->artist, 128);
            sc_strlcpy(out->details, out->title, sizeof out->details);
            return;
        }
        *last_by = ' '; /* restore, fall through — shouldn't happen often */
    }

    out->kind = TRACK_BROWSING;
    sc_strlcpy(out->details, work, sizeof out->details);
    sc_utf8_clamp(out->details, 128);
}

int track_same_song(const TrackInfo *a, const TrackInfo *b)
{
    if (a->kind != b->kind)
        return 0;
    if (a->paused != b->paused)
        return 0;
    if (a->kind == TRACK_NONE)
        return 1;
    if (a->kind == TRACK_PLAYING)
        return strcmp(a->title, b->title) == 0 &&
               strcmp(a->artist, b->artist) == 0;
    return strcmp(a->details, b->details) == 0;
}

int track_equal(const TrackInfo *a, const TrackInfo *b)
{
    if (!track_same_song(a, b))
        return 0;
    if (a->kind == TRACK_NONE)
        return 1;
    if (a->kind == TRACK_PLAYING)
        return strcmp(a->url, b->url) == 0 &&
               strcmp(a->art_url, b->art_url) == 0;
    return strcmp(a->url, b->url) == 0;
}

void track_log(const TrackInfo *t, const char *prefix)
{
    const char *p = prefix ? prefix : "";

    switch (t->kind) {
    case TRACK_NONE:
        LOG_I("%s[NONE]", p);
        break;
    case TRACK_BROWSING:
        LOG_I("%s[BROWSING] details=\"%s\"", p, t->details);
        break;
    case TRACK_PLAYING:
        LOG_I("%s[PLAYING] title=\"%s\" artist=\"%s\" paused=%d art=%s url=%s",
              p, t->title, t->artist, t->paused,
              t->art_url[0] ? "yes" : "no",
              t->url[0] ? t->url : "-");
        break;
    }
}

static int expect(int cond, const char *name)
{
    if (cond) {
        printf("  OK   %s\n", name);
        return 0;
    }
    printf("  FAIL %s\n", name);
    return 1;
}

int track_self_test(void)
{
    TrackInfo t;
    int fails = 0;
    char long_title[400];

    printf("track_self_test\n");

    track_parse("Never Gonna Give You Up by Rick Astley | SoundCloud", &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strcmp(t.title, "Never Gonna Give You Up") == 0 &&
                    strcmp(t.artist, "Rick Astley") == 0,
                    "basic title by artist | SoundCloud");

    track_parse("Song by Artist | Free Listening on SoundCloud", &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strcmp(t.title, "Song") == 0 &&
                    strcmp(t.artist, "Artist") == 0,
                    "Free Listening suffix");

    track_parse("Mix by DJ | Stream Mix playlist on SoundCloud", &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strcmp(t.title, "Mix") == 0 &&
                    strcmp(t.artist, "DJ") == 0,
                    "playlist suffix");

    track_parse("Stream and listen to music online for free with SoundCloud", &t);
    fails += expect(t.kind == TRACK_BROWSING &&
                    strcmp(t.details, "Browsing") == 0,
                    "homepage stream-and-listen");

    track_parse("SoundCloud - Hear the world's sounds", &t);
    fails += expect(t.kind == TRACK_BROWSING &&
                    strcmp(t.details, "Browsing") == 0,
                    "classic homepage title");

    track_parse("someuser | SoundCloud", &t);
    fails += expect(t.kind == TRACK_BROWSING &&
                    strcmp(t.details, "someuser") == 0,
                    "profile page");

    track_parse("foo by bar by baz | SoundCloud", &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strcmp(t.title, "foo by bar") == 0 &&
                    strcmp(t.artist, "baz") == 0,
                    "last ' by ' wins");

    track_parse("Say \"Hello\" by Artist | SoundCloud", &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strcmp(t.title, "Say \"Hello\"") == 0 &&
                    strcmp(t.artist, "Artist") == 0,
                    "quotes in title");

    track_parse("SOUNDCLOUD", &t);
    fails += expect(t.kind == TRACK_BROWSING &&
                    strcmp(t.details, "Browsing") == 0,
                    "bare SOUNDCLOUD");

    track_parse("", &t);
    fails += expect(t.kind == TRACK_NONE, "empty → NONE");

    track_parse("YouTube - never gonna", &t);
    fails += expect(t.kind == TRACK_NONE, "non-SoundCloud → NONE");

    track_parse("~/Документы/C/soundcloud-rpc: make rebuild && ./soundcloud-rpc - make", &t);
    fails += expect(t.kind == TRACK_NONE, "soundcloud-rpc terminal → NONE");

    track_parse("\xd0\x9f\xd0\xbb\xd0\xb5\xd0\xb9\xd0\xbb\xd0\xb8\xd1\x81\xd1\x82 \xd0\xbf\xd0\xbe soundcloud 2025 #1 - YouTube", &t);
    fails += expect(t.kind == TRACK_NONE, "YouTube mentioning soundcloud → NONE");

    track_parse("playlist about soundcloud 2025 #1", &t);
    fails += expect(t.kind == TRACK_NONE, "title with word soundcloud only → NONE");

    track_parse("Kurayami no naka by DJ UNIVXRSEL | SoundCloud " EM_DASH " Zen Browser", &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strcmp(t.title, "Kurayami no naka") == 0 &&
                    strcmp(t.artist, "DJ UNIVXRSEL") == 0,
                    "Zen Browser real SoundCloud tab");

    track_parse("FATTMACK - CRAZY STORY | THE BOOTH by SoundCloud", &t);
    fails += expect(t.kind == TRACK_NONE, "YouTube-style 'by SoundCloud' → NONE");

    track_parse("soundcloud - Google Search " EM_DASH " Zen Browser", &t);
    fails += expect(t.kind == TRACK_NONE, "Google search → NONE");

    track_parse("Discover the top streamed music and songs online on Soundcloud", &t);
    fails += expect(t.kind == TRACK_BROWSING &&
                    strcmp(t.details, "Browsing") == 0,
                    "SoundCloud homepage (discover) → BROWSING");

    track_parse("addiction results on SoundCloud", &t);
    fails += expect(t.kind == TRACK_BROWSING && t.searching &&
                    strcmp(t.details, "Searching: 'addiction'") == 0,
                    "search results → Searching");

    track_parse("hello world results on SoundCloud — Zen Browser", &t);
    fails += expect(t.kind == TRACK_BROWSING && t.searching &&
                    strcmp(t.details, "Searching: 'hello world'") == 0,
                    "search + Zen suffix");

    track_parse("  Chill Track by Cool Artist | SOUNDCLOUD  ", &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strcmp(t.title, "Chill Track") == 0 &&
                    strcmp(t.artist, "Cool Artist") == 0,
                    "case + surrounding space");

    memset(long_title, 'A', 200);
    memcpy(long_title + 200, " by Bob | SoundCloud", 21);
    long_title[221] = '\0';
    track_parse(long_title, &t);
    fails += expect(t.kind == TRACK_PLAYING &&
                    strlen(t.title) <= 128 &&
                    strcmp(t.artist, "Bob") == 0,
                    "long title clamped to 128");

    /* equal / paused */
    {
        TrackInfo a, b;
        track_parse("T by A | SoundCloud", &a);
        track_parse("T by A | SoundCloud", &b);
        fails += expect(track_equal(&a, &b), "equal same track");
        b.paused = 1;
        fails += expect(!track_equal(&a, &b), "paused breaks equality");
        track_parse("T by B | SoundCloud", &b);
        fails += expect(!track_equal(&a, &b), "different artist");
        track_parse("T by A | SoundCloud", &b);
        sc_strlcpy(b.art_url, "https://i1.sndcdn.com/art.jpg", sizeof b.art_url);
        fails += expect(track_same_song(&a, &b), "same song ignores art");
        fails += expect(!track_equal(&a, &b), "art change is not equal");
    }

    /* utf-8 clamp shouldn't split a 2-byte char: é is C3 A9 */
    {
        char u[8];
        u[0] = (char)0xC3;
        u[1] = (char)0xA9;
        u[2] = 'x';
        u[3] = '\0';
        sc_utf8_clamp(u, 1);
        fails += expect(u[0] == '\0', "utf8 clamp doesn't split lead byte");
    }

    if (fails)
        printf("%d test(s) failed\n", fails);
    else
        printf("all tests passed\n");
    return fails ? 1 : 0;
}
