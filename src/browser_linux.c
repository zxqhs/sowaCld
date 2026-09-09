#include "browser.h"
#include "log.h"
#include "scapi.h"
#include "util.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static Display *g_dpy;
static Atom g_net_client_list;
static Atom g_net_client_stack;
static Atom g_net_wm_name;
static Atom g_utf8;

typedef struct {
    TrackInfo live;      /* playerctl / MPRIS — actual playback */
    TrackInfo playing;   /* window title parse */
    TrackInfo browsing;
    TrackInfo media;     /* playerctl without a SoundCloud url */
    int has_live;
    int has_playing;
    int has_browsing;
    int has_media;
} Acc;

static void enrich_cancel(void);

static int x11_quiet(Display *d, XErrorEvent *e)
{
    (void)d;
    (void)e;
    return 0;
}

int browser_init(void)
{
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy) {
        LOG_W("X11 unavailable (pure Wayland?). Falling back to hyprctl/sway/playerctl");
    } else {
        XSetErrorHandler(x11_quiet);
        g_net_client_list = XInternAtom(g_dpy, "_NET_CLIENT_LIST", False);
        g_net_client_stack = XInternAtom(g_dpy, "_NET_CLIENT_LIST_STACKING", False);
        g_net_wm_name = XInternAtom(g_dpy, "_NET_WM_NAME", False);
        g_utf8 = XInternAtom(g_dpy, "UTF8_STRING", False);
        LOG_I("X11 display opened");
    }
#ifdef HAVE_MPRIS
    if (mpris_init() != 0)
        LOG_D("MPRIS/dbus unavailable");
#endif
    LOG_I("scanners: X11=%s + hyprctl/sway/niri/wmctrl/playerctl",
          g_dpy ? "on" : "off");
    return 0;
}

void browser_shutdown(void)
{
    enrich_cancel();
#ifdef HAVE_MPRIS
    mpris_shutdown();
#endif
    if (g_dpy) {
        XCloseDisplay(g_dpy);
        g_dpy = NULL;
    }
}

static void acc_add(Acc *a, const TrackInfo *parsed)
{
    if (parsed->kind == TRACK_PLAYING && !a->has_playing) {
        a->playing = *parsed;
        a->has_playing = 1;
    } else if (parsed->kind != TRACK_NONE && !a->has_browsing) {
        a->browsing = *parsed;
        a->has_browsing = 1;
    }
}

static void acc_title(Acc *a, const char *title)
{
    TrackInfo parsed;

    if (!title || !title[0])
        return;
    LOG_D("title: %s", title);
    if (!track_is_soundcloud_title(title))
        return;
    track_parse(title, &parsed);
    acc_add(a, &parsed);
}

static int get_title(Window w, char *out, size_t cap)
{
    Atom actual;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    int ok = 0;

    if (XGetWindowProperty(g_dpy, w, g_net_wm_name, 0, 1024, False, g_utf8,
                           &actual, &fmt, &nitems, &after, &data) == Success &&
        data && nitems > 0) {
        sc_strlcpy(out, (const char *)data, cap);
        ok = 1;
    }
    if (data)
        XFree(data);
    if (!ok) {
        char *name = NULL;
        if (XFetchName(g_dpy, w, &name) && name) {
            sc_strlcpy(out, name, cap);
            ok = 1;
        }
        if (name)
            XFree(name);
    }
    return ok;
}

static int get_list(Atom atom, Window **wins, unsigned long *count)
{
    Atom actual;
    int fmt;
    unsigned long nitems, after;
    unsigned char *data = NULL;
    Window root = DefaultRootWindow(g_dpy);

    *wins = NULL;
    *count = 0;
    if (XGetWindowProperty(g_dpy, root, atom, 0, 4096, False, XA_WINDOW,
                           &actual, &fmt, &nitems, &after, &data) != Success ||
        !data || nitems == 0) {
        if (data)
            XFree(data);
        return -1;
    }
    *wins = (Window *)data;
    *count = nitems;
    return 0;
}

static void scan_x11(Acc *a)
{
    Window *wins = NULL;
    unsigned long n = 0, i;

    if (!g_dpy)
        return;
    XSync(g_dpy, False);
    if (get_list(g_net_client_list, &wins, &n) != 0) {
        if (get_list(g_net_client_stack, &wins, &n) != 0)
            return;
    }
    for (i = 0; i < n; i++) {
        char title[512];
        if (!get_title(wins[i], title, sizeof title) || !title[0])
            continue;
        acc_title(a, title);
    }
    if (wins)
        XFree(wins);
}

static char *run_cmd(const char *cmd, size_t cap)
{
    FILE *fp;
    char *buf;
    size_t n;
    char wrapped[768];

    /* playerctl -a and missing hyprctl/sway can block the whole poll. */
    snprintf(wrapped, sizeof wrapped, "timeout -k 0.2 0.4 %s", cmd);
    fp = popen(wrapped, "r");
    if (!fp)
        return NULL;
    buf = (char *)malloc(cap);
    if (!buf) {
        pclose(fp);
        return NULL;
    }
    n = fread(buf, 1, cap - 1, fp);
    buf[n] = '\0';
    pclose(fp);
    if (!n) {
        free(buf);
        return NULL;
    }
    return buf;
}

static void scan_json_titles(const char *json, Acc *a)
{
    const char *p = json;

    while ((p = strstr(p, "\"title\"")) != NULL) {
        char title[512];
        size_t i = 0;

        p += 7;
        while (*p && *p != ':' && *p != '"')
            p++;
        if (*p == ':')
            p++;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p != '"')
            continue;
        p++;
        while (*p && *p != '"' && i + 1 < sizeof title) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n')
                    title[i++] = ' ';
                else
                    title[i++] = *p;
                p++;
                continue;
            }
            title[i++] = *p++;
        }
        title[i] = '\0';
        if (*p == '"')
            p++;
        acc_title(a, title);
    }
}

static void scan_cmd_json(const char *cmd, Acc *a)
{
    char *buf = run_cmd(cmd, 256 * 1024);
    if (!buf)
        return;
    scan_json_titles(buf, a);
    free(buf);
}

static void scan_wmctrl(Acc *a)
{
    char *buf, *line, *save;

    buf = run_cmd("wmctrl -l 2>/dev/null", 64 * 1024);
    if (!buf)
        return;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *p = line;
        int tok;
        for (tok = 0; tok < 3 && *p; tok++) {
            while (*p && !isspace((unsigned char)*p))
                p++;
            while (*p && isspace((unsigned char)*p))
                p++;
        }
        if (*p)
            acc_title(a, p);
    }
    free(buf);
}

static void urlenc(char *dst, size_t cap, const char *src)
{
    static const char *hex = "0123456789ABCDEF";
    size_t o = 0;

    if (!dst || cap == 0)
        return;
    while (src && *src && o + 4 < cap) {
        unsigned char c = (unsigned char)*src++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            dst[o++] = (char)c;
        else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 15];
        }
    }
    dst[o] = '\0';
}

static void enrich_oembed(TrackInfo *t)
{
    static char cache_key[512];
    static char cache_art[512];
    static int64_t cache_ms;
    static int cache_ok;
    char enc[768], oembed[1024], json[8192], thumb[512];
    int64_t now;

    if (!t || !sc_is_track_url(t->url))
        return;
    if (sc_is_http_url(t->art_url))
        return;

    now = sc_now_ms();
    if (strcmp(cache_key, t->url) == 0 && now - cache_ms < 300000) {
        if (cache_ok && cache_art[0])
            sc_strlcpy(t->art_url, cache_art, sizeof t->art_url);
        return;
    }

    urlenc(enc, sizeof enc, t->url);
    snprintf(oembed, sizeof oembed,
             "https://soundcloud.com/oembed?format=json&url=%s", enc);
    sc_strlcpy(cache_key, t->url, sizeof cache_key);
    cache_ms = now;
    cache_ok = 0;
    cache_art[0] = '\0';

    if (sc_http_get(oembed, json, sizeof json) != 0) {
        LOG_D("oembed failed");
        return;
    }
    if (sc_json_string(json, "thumbnail_url", thumb, sizeof thumb) != 0 ||
        !sc_is_http_url(thumb))
        return;
    sc_upgrade_art_url(thumb);
    sc_strlcpy(t->art_url, thumb, sizeof t->art_url);
    sc_strlcpy(cache_art, thumb, sizeof cache_art);
    cache_ok = 1;
    LOG_D("oembed art: %s", thumb);
}

static int url_foreign_media(const char *url)
{
    if (!url || !url[0])
        return 0;
    return sc_strcasestr(url, "youtube") ||
           sc_strcasestr(url, "youtu.be") ||
           sc_strcasestr(url, "spotify") ||
           sc_strcasestr(url, "music.youtube") ||
           sc_strcasestr(url, "twitch.tv") ||
           sc_strcasestr(url, "deezer");
}

static void fill_playing(TrackInfo *t, const char *title, const char *artist,
                         const char *url, const char *art, int paused)
{
    track_clear(t);
    t->kind = TRACK_PLAYING;
    t->paused = paused;
    sc_strlcpy(t->title, title && title[0] ? title : "Unknown track", sizeof t->title);
    sc_strlcpy(t->artist, artist ? artist : "", sizeof t->artist);
    sc_utf8_clamp(t->title, 128);
    sc_utf8_clamp(t->artist, 128);
    sc_strlcpy(t->details, t->title, sizeof t->details);
    if (sc_is_track_url(url))
        sc_strlcpy(t->url, url, sizeof t->url);
    if (sc_is_http_url(art) && !sc_istartswith(art, "file:")) {
        sc_strlcpy(t->art_url, art, sizeof t->art_url);
        sc_upgrade_art_url(t->art_url);
    }
}

static void scan_playerctl(Acc *a)
{
    char *buf, *line, *save;

    buf = run_cmd("playerctl -a metadata --format '{{status}}\t{{title}}\t{{artist}}\t{{xesam:url}}\t{{mpris:artUrl}}' 2>/dev/null",
                  64 * 1024);
    if (!buf)
        return;
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *status, *title, *artist, *url, *art, *p;
        int is_sc, paused;

        status = line;
        p = strchr(status, '\t');
        if (!p)
            continue;
        *p = '\0';
        title = p + 1;
        p = strchr(title, '\t');
        if (!p)
            continue;
        *p = '\0';
        artist = p + 1;
        p = strchr(artist, '\t');
        if (!p)
            continue;
        *p = '\0';
        url = p + 1;
        art = "";
        p = strchr(url, '\t');
        if (p) {
            *p = '\0';
            art = p + 1;
        }

        if (sc_strcasecmp(status, "Playing") != 0 &&
            sc_strcasecmp(status, "Paused") != 0)
            continue;

        is_sc = (url && sc_strcasestr(url, "soundcloud")) ||
                (art && sc_strcasestr(art, "sndcdn.com"));
        paused = sc_strcasecmp(status, "Paused") == 0;

        if (is_sc) {
            if (!a->has_live || (a->live.paused && !paused)) {
                fill_playing(&a->live, title, artist, url, art, paused);
                a->has_live = 1;
                LOG_D("playerctl sc: %s — %s (%s)", a->live.artist, a->live.title, status);
            }
        }

        if (!paused && title[0] && !url_foreign_media(url)) {
            if (!a->has_media) {
                fill_playing(&a->media, title, artist, url, art, 0);
                a->has_media = 1;
            }
        }

        if (!is_sc)
            acc_title(a, title);
    }
    free(buf);
}

typedef struct {
    pid_t pid;
    int   fd;
    char  title[129];
    char  artist[129];
} EnrichJob;

static EnrichJob g_job = { 0, -1, {0}, {0} };

static char g_res_title[129];
static char g_res_artist[129];
static char g_res_url[512];
static char g_res_art[512];

static int res_apply(TrackInfo *t)
{
    if (!t || t->kind != TRACK_PLAYING)
        return 0;
    if (strcmp(t->title, g_res_title) != 0 || strcmp(t->artist, g_res_artist) != 0)
        return 0;
    if (!t->url[0] && g_res_url[0])
        sc_strlcpy(t->url, g_res_url, sizeof t->url);
    if (!t->art_url[0] && g_res_art[0])
        sc_strlcpy(t->art_url, g_res_art, sizeof t->art_url);
    return (t->url[0] || t->art_url[0]);
}

static void res_store(const TrackInfo *t)
{
    if (!t || t->kind != TRACK_PLAYING || !t->title[0])
        return;
    if (strcmp(g_res_title, t->title) != 0 || strcmp(g_res_artist, t->artist) != 0) {
        g_res_url[0] = g_res_art[0] = '\0';
        sc_strlcpy(g_res_title, t->title, sizeof g_res_title);
        sc_strlcpy(g_res_artist, t->artist, sizeof g_res_artist);
    }
    if (t->url[0])
        sc_strlcpy(g_res_url, t->url, sizeof g_res_url);
    if (t->art_url[0])
        sc_strlcpy(g_res_art, t->art_url, sizeof g_res_art);
}

static void enrich_cancel(void)
{
    if (g_job.pid > 0) {
        kill(g_job.pid, SIGKILL);
        waitpid(g_job.pid, NULL, 0);
        g_job.pid = 0;
    }
    if (g_job.fd >= 0) {
        close(g_job.fd);
        g_job.fd = -1;
    }
    g_job.title[0] = g_job.artist[0] = '\0';
}

static void enrich_start(const TrackInfo *t)
{
    int fds[2];
    pid_t pid;

    if (!t || t->kind != TRACK_PLAYING || !t->title[0])
        return;
    if (sc_is_track_url(t->url) && sc_is_http_url(t->art_url))
        return;
    if (g_job.pid > 0)
        return;

    if (pipe(fds) != 0)
        return;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        TrackInfo tmp;
        char buf[1100];
        int n;

        close(fds[0]);
        tmp = *t;
        enrich_oembed(&tmp);
        scapi_lookup(&tmp);
        n = snprintf(buf, sizeof buf, "%s\n%s\n", tmp.url, tmp.art_url);
        if (n > 0)
            (void)write(fds[1], buf, (size_t)n);
        close(fds[1]);
        _exit(0);
    }
    close(fds[1]);
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    g_job.pid = pid;
    g_job.fd = fds[0];
    sc_strlcpy(g_job.title, t->title, sizeof g_job.title);
    sc_strlcpy(g_job.artist, t->artist, sizeof g_job.artist);
}

static void enrich_take(TrackInfo *t)
{
    char buf[1100];
    ssize_t n;
    char *nl, *art;

    if (g_job.pid <= 0)
        return;

    if (!t || t->kind != TRACK_PLAYING ||
        strcmp(t->title, g_job.title) != 0 ||
        strcmp(t->artist, g_job.artist) != 0) {
        enrich_cancel();
        return;
    }

    n = read(g_job.fd, buf, sizeof buf - 1);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;
    close(g_job.fd);
    g_job.fd = -1;
    waitpid(g_job.pid, NULL, 0);
    g_job.pid = 0;
    if (n <= 0)
        return;
    buf[n] = '\0';
    nl = strchr(buf, '\n');
    if (!nl)
        return;
    *nl = '\0';
    if (buf[0] && !sc_is_track_url(t->url) && sc_is_track_url(buf))
        sc_strlcpy(t->url, buf, sizeof t->url);
    art = nl + 1;
    nl = strchr(art, '\n');
    if (nl)
        *nl = '\0';
    if (art[0] && !sc_is_http_url(t->art_url) && sc_is_http_url(art))
        sc_strlcpy(t->art_url, art, sizeof t->art_url);
    res_store(t);
}

#ifdef HAVE_MPRIS
static void merge_mpris(Acc *a)
{
    TrackInfo mpris;

    track_clear(&mpris);
    if (mpris_query(&mpris) != 0 || mpris.kind != TRACK_PLAYING)
        return;
    if (!a->has_live) {
        a->live = mpris;
        a->has_live = 1;
        return;
    }
    if (!mpris.paused && mpris.title[0] &&
        (strcmp(a->live.title, mpris.title) != 0 ||
         strcmp(a->live.artist, mpris.artist) != 0)) {
        if (!mpris.url[0] && a->live.url[0])
            sc_strlcpy(mpris.url, a->live.url, sizeof mpris.url);
        if (!mpris.art_url[0] && a->live.art_url[0])
            sc_strlcpy(mpris.art_url, a->live.art_url, sizeof mpris.art_url);
        a->live = mpris;
    } else {
        if (!a->live.url[0] && mpris.url[0])
            sc_strlcpy(a->live.url, mpris.url, sizeof a->live.url);
        if (!a->live.art_url[0] && mpris.art_url[0])
            sc_strlcpy(a->live.art_url, mpris.art_url, sizeof a->live.art_url);
        a->live.paused = mpris.paused;
    }
}
#endif

int browser_scan(TrackInfo *out)
{
    Acc acc;

    memset(&acc, 0, sizeof acc);
    track_clear(out);

#ifdef HAVE_MPRIS
    merge_mpris(&acc);
#endif
    scan_playerctl(&acc);

    if (!(acc.has_live && !acc.live.paused && acc.live.title[0])) {
        scan_x11(&acc);
        scan_cmd_json("hyprctl clients -j 2>/dev/null", &acc);
        scan_cmd_json("swaymsg -t get_tree 2>/dev/null", &acc);
        scan_cmd_json("niri msg --json windows 2>/dev/null", &acc);
        scan_wmctrl(&acc);
    }

    /* PLAYING only if a player actually reports Playing/Paused.
       A tab title like "Track by Artist | SoundCloud" is not playback. */
    if (acc.has_live && !acc.live.paused)
        *out = acc.live;
    else if ((acc.has_live || acc.has_playing || acc.has_browsing) && acc.has_media)
        *out = acc.media;
    else if (acc.has_live)
        *out = acc.live;
    else if (acc.has_browsing && acc.browsing.searching)
        *out = acc.browsing;
    else if (acc.has_browsing)
        *out = acc.browsing;
    else if (acc.has_playing) {
        *out = acc.playing;
        out->kind = TRACK_BROWSING;
        out->paused = 0;
        out->searching = 0;
        if (!out->details[0])
            sc_strlcpy(out->details, "Browsing", sizeof out->details);
    }

    if (out->kind == TRACK_PLAYING) {
        res_apply(out);
        enrich_take(out);
        res_store(out);
        if (!sc_is_track_url(out->url) || !sc_is_http_url(out->art_url))
            enrich_start(out);
    }
    return out->kind != TRACK_NONE;
}

void browser_enrich(TrackInfo *t)
{
    /* Cover lookup is forked from browser_scan so skips stay non-blocking. */
    (void)t;
}
