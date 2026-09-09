#include "browser.h"
#include "log.h"
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct {
    TrackInfo playing;
    TrackInfo browsing;
    int has_playing;
    int has_browsing;
} ScanAcc;

static ScanAcc g_acc;

int browser_init(void)
{
    return 0;
}

void browser_shutdown(void)
{
}

static BOOL CALLBACK enum_proc(HWND hwnd, LPARAM lp)
{
    wchar_t wbuf[512];
    char utf8[1024];
    int n;
    TrackInfo parsed;

    (void)lp;
    if (!IsWindowVisible(hwnd))
        return TRUE;
    n = GetWindowTextW(hwnd, wbuf, 512);
    if (n <= 0)
        return TRUE;
    n = WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, utf8, (int)sizeof utf8, NULL, NULL);
    if (n <= 0)
        return TRUE;
    if (!track_is_soundcloud_title(utf8))
        return TRUE;
    LOG_D("window: %s", utf8);
    track_parse(utf8, &parsed);
    if (parsed.kind == TRACK_PLAYING && !g_acc.has_playing) {
        g_acc.playing = parsed;
        g_acc.has_playing = 1;
    } else if (parsed.kind != TRACK_NONE && !g_acc.has_browsing) {
        g_acc.browsing = parsed;
        g_acc.has_browsing = 1;
    }
    return TRUE;
}

int browser_scan(TrackInfo *out)
{
    track_clear(out);
    memset(&g_acc, 0, sizeof g_acc);
    EnumWindows(enum_proc, 0);
    if (g_acc.has_playing)
        *out = g_acc.playing;
    else if (g_acc.has_browsing)
        *out = g_acc.browsing;
    return out->kind != TRACK_NONE;
}

void browser_enrich(TrackInfo *t)
{
    (void)t;
}
