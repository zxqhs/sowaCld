#include "presence.h"
#include "json.h"
#include "log.h"
#include "platform.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

#if defined(SC_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static unsigned long current_pid(void)
{
    return (unsigned long)GetCurrentProcessId();
}
#else
#  include <unistd.h>
static unsigned long current_pid(void)
{
    return (unsigned long)getpid();
}
#endif

int presence_clear(DiscordIpc *ipc)
{
    char buf[256];
    JsonBuf jb;
    uint64_t nonce = discord_ipc_next_nonce(ipc);

    jb_init(&jb, buf, sizeof buf);
    jb_printf(&jb,
              "{\"nonce\":\"%llu\",\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":%lu,\"activity\":null}}",
              (unsigned long long)nonce, current_pid());
    if (!jb_finish(&jb)) {
        LOG_E("failed to build clear payload");
        return -1;
    }
    LOG_I("clearing presence");
    return discord_ipc_send_json(ipc, buf);
}

int presence_send(DiscordIpc *ipc, const Config *cfg, const TrackInfo *t)
{
    char buf[8192];
    JsonBuf jb;
    uint64_t nonce;
    const char *details;
    const char *state = NULL;
    char state_buf[160];
    const char *image;
    const char *btn_url;
    const char *btn_label;
    int playing;
    int has_state;
    int has_assets;
    int has_button;

    if (t->kind == TRACK_NONE)
        return presence_clear(ipc);

    playing = (t->kind == TRACK_PLAYING);
    details = playing ? t->title : t->details;
    if (!details || !details[0])
        details = playing ? "Unknown track" : "Browsing";

    has_state = 0;
    if (playing && t->artist[0]) {
        sc_strlcpy(state_buf, t->artist, sizeof state_buf);
        sc_utf8_clamp(state_buf, 128);
        state = state_buf;
        has_state = 1;
    }

    if (sc_is_http_url(t->art_url))
        image = t->art_url;
    else if (cfg->large_image_key[0])
        image = cfg->large_image_key;
    else
        image = NULL;

    btn_url = sc_is_track_url(t->url) ? t->url : NULL;
    btn_label = cfg->button_label;
    if (!btn_label[0] || sc_strcasestr(btn_label, "soundcloud"))
        btn_label = "Open Track";
    has_button = btn_url && btn_label[0];

    has_assets = image != NULL;

    nonce = discord_ipc_next_nonce(ipc);
    jb_init(&jb, buf, sizeof buf);

    jb_puts(&jb, "{\"nonce\":\"");
    jb_printf(&jb, "%llu", (unsigned long long)nonce);
    jb_puts(&jb, "\",\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":");
    jb_printf(&jb, "%lu", current_pid());
    jb_puts(&jb, ",\"activity\":{");

    jb_printf(&jb, "\"type\":%d", cfg->activity_type);
    if (cfg->activity_name[0]) {
        jb_puts(&jb, ",\"name\":");
        jb_str(&jb, cfg->activity_name);
    }
    jb_puts(&jb, ",\"details\":");
    jb_str(&jb, details);
    if (btn_url) {
        jb_puts(&jb, ",\"details_url\":");
        jb_str(&jb, btn_url);
    }
    if (has_state) {
        jb_puts(&jb, ",\"state\":");
        jb_str(&jb, state);
        if (btn_url) {
            jb_puts(&jb, ",\"state_url\":");
            jb_str(&jb, btn_url);
        }
    }

    if (playing && t->start_ms > 0) {
        jb_puts(&jb, ",\"timestamps\":{\"start\":");
        jb_printf(&jb, "%lld", (long long)(t->start_ms / 1000));
        jb_puts(&jb, "}");
    }

    if (has_assets) {
        jb_puts(&jb, ",\"assets\":{\"large_image\":");
        jb_str(&jb, image);
        if (btn_url) {
            jb_puts(&jb, ",\"large_url\":");
            jb_str(&jb, btn_url);
        }
        if (playing) {
            const char *key = t->paused ? cfg->small_image_pause : cfg->small_image_play;
            const char *txt = t->paused ? "Paused" : "Playing";
            if (key[0]) {
                jb_puts(&jb, ",\"small_image\":");
                jb_str(&jb, key);
                jb_puts(&jb, ",\"small_text\":");
                jb_str(&jb, txt);
            }
        }
        jb_puts(&jb, "}");
    }

    if (has_button) {
        jb_puts(&jb, ",\"buttons\":[{\"label\":");
        jb_str(&jb, btn_label);
        jb_puts(&jb, ",\"url\":");
        jb_str(&jb, btn_url);
        jb_puts(&jb, "}]");
    }

    jb_puts(&jb, "}}}");

    if (!jb_finish(&jb)) {
        LOG_E("failed to build SET_ACTIVITY payload");
        return -1;
    }

    track_log(t, "presence ");
    if (has_button)
        LOG_I("link \"%s\" -> %s  (button is for others; title/cover are clickable for you)",
              btn_label, btn_url);
    else if (t->url[0])
        LOG_W("url present but not a track permalink: %s", t->url);
    return discord_ipc_send_json(ipc, buf);
}
