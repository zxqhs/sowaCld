#include "scapi.h"
#include "log.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HTML_CAP  (128 * 1024)
#define JSON_CAP  (96 * 1024)

static char g_client_id[64];
static int64_t g_client_ms;
static char g_cache_key[300];
static char g_cache_url[512];
static char g_cache_art[512];
static int64_t g_cache_ms;
static int g_cache_ok;

static int extract_client_id(const char *html, char *out, size_t cap)
{
    const char *p;
    size_t i;

    p = strstr(html, "\"hydratable\":\"apiClient\"");
    if (!p)
        p = strstr(html, "\"hydratable\": \"apiClient\"");
    if (!p)
        return -1;
    if (sc_json_string(p, "id", out, cap) != 0)
        return -1;
    if (strlen(out) < 16 || strlen(out) > 40)
        return -1;
    for (i = 0; out[i]; i++) {
        if (!isalnum((unsigned char)out[i]))
            return -1;
    }
    return 0;
}

static int refresh_client_id(void)
{
    char *html;
    int rc = -1;

    html = (char *)malloc(HTML_CAP);
    if (!html)
        return -1;
    if (sc_http_get("https://soundcloud.com", html, HTML_CAP) == 0 &&
        extract_client_id(html, g_client_id, sizeof g_client_id) == 0) {
        g_client_ms = sc_now_ms();
        LOG_I("SoundCloud client_id acquired");
        rc = 0;
    } else {
        LOG_W("failed to get SoundCloud client_id (need curl + network)");
        g_client_id[0] = '\0';
    }
    free(html);
    return rc;
}

static int ensure_client_id(void)
{
    int64_t now = sc_now_ms();

    if (g_client_id[0] && now - g_client_ms < 12 * 3600 * 1000LL)
        return 0;
    return refresh_client_id();
}

static int json_str_range(const char *beg, const char *end, const char *key, int want_last,
                         char *out, size_t cap)
{
    char pat[80];
    const char *p, *found = NULL;

    out[0] = '\0';
    if (!beg || beg >= end)
        return -1;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = beg;
    while ((p = strstr(p, pat)) != NULL && p < end) {
        found = p;
        p += strlen(pat);
        if (!want_last)
            break;
    }
    if (!found)
        return -1;
    return sc_json_string(found, key, out, cap);
}

static void extract_nearby(const char *base, const char *p, char *title, size_t tcap,
                           char *user, size_t ucap,
                           char *art, size_t acap)
{
    const char *limit = base + strlen(base);
    const char *back = p - 10000;
    const char *fwd = p + 4000;

    title[0] = user[0] = art[0] = '\0';
    if (back < base)
        back = base;
    if (fwd > limit)
        fwd = limit;
    json_str_range(p, fwd, "title", 0, title, tcap);
    if (json_str_range(p, fwd, "username", 0, user, ucap) != 0)
        json_str_range(p, fwd, "full_name", 0, user, ucap);
    json_str_range(back, p, "artwork_url", 1, art, acap);
    if (!art[0] || strcmp(art, "null") == 0)
        json_str_range(p, fwd, "avatar_url", 0, art, acap);
}

static int score_match(const char *want_title, const char *want_artist,
                       const char *got_title, const char *got_user)
{
    int s = 0;

    if (!got_title[0])
        return 0;
    if (sc_strcasecmp(want_title, got_title) == 0)
        s += 100;
    else if (sc_strcasestr(got_title, want_title) ||
             sc_strcasestr(want_title, got_title))
        s += 35;
    if (want_artist[0] && got_user[0]) {
        if (sc_strcasecmp(want_artist, got_user) == 0)
            s += 50;
        else if (sc_strcasestr(got_user, want_artist) ||
                 sc_strcasestr(want_artist, got_user))
            s += 25;
    }
    return s;
}

static int search_tracks(const char *title, const char *artist,
                         char *url_out, size_t url_cap,
                         char *art_out, size_t art_cap)
{
    char q[300], enc[900], api[1100], *json;
    const char *p;
    int best = -1;
    char best_url[512], best_art[512];

    url_out[0] = art_out[0] = '\0';
    best_url[0] = best_art[0] = '\0';

    if (artist && artist[0])
        snprintf(q, sizeof q, "%s %s", title, artist);
    else
        sc_strlcpy(q, title, sizeof q);

    if (ensure_client_id() != 0)
        return -1;

    sc_urlenc(enc, sizeof enc, q);
    snprintf(api, sizeof api,
             "https://api-v2.soundcloud.com/search/tracks?q=%s&limit=8&client_id=%s",
             enc, g_client_id);

    json = (char *)malloc(JSON_CAP);
    if (!json)
        return -1;
    if (sc_http_get(api, json, JSON_CAP) != 0) {
        /* client_id expired */
        g_client_id[0] = '\0';
        if (refresh_client_id() == 0) {
            snprintf(api, sizeof api,
                     "https://api-v2.soundcloud.com/search/tracks?q=%s&limit=8&client_id=%s",
                     enc, g_client_id);
            if (sc_http_get(api, json, JSON_CAP) != 0) {
                free(json);
                return -1;
            }
        } else {
            free(json);
            return -1;
        }
    }

    p = json;
    while ((p = strstr(p, "\"permalink_url\"")) != NULL) {
        char permalink[512], got_title[256], got_user[128], got_art[512];
        int sc;

        if (sc_json_string(p, "permalink_url", permalink, sizeof permalink) != 0) {
            p += 15;
            continue;
        }
        extract_nearby(json, p, got_title, sizeof got_title, got_user, sizeof got_user,
                       got_art, sizeof got_art);
        sc = score_match(title, artist ? artist : "", got_title, got_user);
        LOG_D("search hit score=%d title=\"%s\" user=\"%s\"", sc, got_title, got_user);
        if (sc > best && sc_is_track_url(permalink)) {
            best = sc;
            sc_strlcpy(best_url, permalink, sizeof best_url);
            sc_strlcpy(best_art, got_art, sizeof best_art);
        }
        p += 15;
    }
    free(json);

    if (best < 50 || !best_url[0])
        return -1;
    sc_strlcpy(url_out, best_url, url_cap);
    if (sc_is_http_url(best_art)) {
        sc_upgrade_art_url(best_art);
        sc_strlcpy(art_out, best_art, art_cap);
    }
    return 0;
}

void scapi_lookup(TrackInfo *t)
{
    char key[300], url[512], art[512];
    int64_t now;

    if (!t || t->kind != TRACK_PLAYING || !t->title[0])
        return;
    if (sc_is_track_url(t->url) && sc_is_http_url(t->art_url))
        return;

    snprintf(key, sizeof key, "%s\t%s", t->title, t->artist);
    now = sc_now_ms();
    if (strcmp(g_cache_key, key) == 0 && now - g_cache_ms < 10 * 60 * 1000LL) {
        if (g_cache_ok) {
            if (!sc_is_track_url(t->url) && g_cache_url[0])
                sc_strlcpy(t->url, g_cache_url, sizeof t->url);
            if (!sc_is_http_url(t->art_url) && g_cache_art[0])
                sc_strlcpy(t->art_url, g_cache_art, sizeof t->art_url);
        }
        return;
    }

    sc_strlcpy(g_cache_key, key, sizeof g_cache_key);
    g_cache_ms = now;
    g_cache_ok = 0;
    g_cache_url[0] = g_cache_art[0] = '\0';

    if (search_tracks(t->title, t->artist, url, sizeof url, art, sizeof art) != 0) {
        LOG_D("SoundCloud search: no match for \"%s\" / \"%s\"", t->title, t->artist);
        return;
    }

    if (!sc_is_track_url(t->url))
        sc_strlcpy(t->url, url, sizeof t->url);
    if (!sc_is_http_url(t->art_url) && art[0])
        sc_strlcpy(t->art_url, art, sizeof t->art_url);
    sc_strlcpy(g_cache_url, t->url, sizeof g_cache_url);
    sc_strlcpy(g_cache_art, t->art_url, sizeof g_cache_art);
    g_cache_ok = 1;
    LOG_I("resolved url=%s art=%s", t->url, t->art_url[0] ? "yes" : "no");
}
