#include "browser.h"
#include "config.h"
#include "discord_ipc.h"
#include "log.h"
#include "platform.h"
#include "presence.h"
#include "track.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SC_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
static volatile long g_running = 1;
static BOOL WINAPI console_handler(DWORD type)
{
    (void)type;
    g_running = 0;
    return TRUE;
}
static int running(void) { return g_running != 0; }
static void install_signals(void)
{
    SetConsoleCtrlHandler(console_handler, TRUE);
}
#else
#  include <signal.h>
#  include <unistd.h>
static volatile sig_atomic_t g_running = 1;
static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}
static int running(void) { return g_running != 0; }
static void install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
}
#endif

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -c, --config PATH   Path to config.ini\n"
        "  --dry-run           Scan windows and print tracks, no Discord\n"
        "  --test-rpc          Set a dummy presence, wait for Enter, clear\n"
        "  --self-test         Run title-parser unit tests\n"
        "  -v, --verbose       Debug logging\n"
        "  -h, --help          This help\n",
        argv0);
}

static void sleep_interruptible(int ms)
{
    while (ms > 0 && running()) {
        int chunk = ms > 100 ? 100 : ms;
        sc_sleep_ms(chunk);
        ms -= chunk;
    }
}

static int run_test_rpc(const Config *cfg)
{
    DiscordIpc ipc;
    TrackInfo fake;

    discord_ipc_init(&ipc);
    LOG_I("connecting to Discord IPC...");
    if (discord_ipc_connect(&ipc, cfg->client_id) != 0) {
        LOG_E("cannot connect: is Discord desktop running?");
        return 1;
    }
    LOG_I("connected, setting dummy presence");
    track_clear(&fake);
    fake.kind = TRACK_PLAYING;
    sc_strlcpy(fake.title, "Test Track", sizeof fake.title);
    sc_strlcpy(fake.artist, "soundcloud-rpc", sizeof fake.artist);
    fake.start_ms = sc_now_ms();
    if (presence_send(&ipc, cfg, &fake) != 0) {
        LOG_E("SET_ACTIVITY failed");
        discord_ipc_close(&ipc);
        return 1;
    }
    fprintf(stderr, "Presence set. Press Enter to clear and exit...\n");
    if (isatty(0)) {
        int c;
        while (running() && (c = getchar()) != '\n' && c != EOF)
            ;
    } else {
        sleep_interruptible(15000);
    }
    presence_clear(&ipc);
    discord_ipc_close(&ipc);
    LOG_I("done");
    return 0;
}

static int run_loop(const Config *cfg, int dry)
{
    DiscordIpc ipc;
    TrackInfo last;     /* last payload actually sent */
    TrackInfo pending;
    int have_pending = 0;
    int misses = 0;
    int play_hold = 0;
    int64_t last_send_ms = 0;
    int64_t last_identity_ms = 0;
    const int64_t heartbeat_ms = 30000;

    discord_ipc_init(&ipc);
    track_clear(&last);
    track_clear(&pending);

    if (browser_init() != 0) {
        LOG_E("browser scanner init failed");
        return 1;
    }

    LOG_I("scanning every %d ms, 400 ms while playing/searching%s",
          cfg->poll_ms, dry ? " (dry-run)" : "");

    while (running()) {
        TrackInfo info;
        TrackInfo *shown;
        int64_t now;
        int same_song, quiet, rate_ok, waiting_art, identity_change, heartbeat, do_send;
        int fast;

        if (!dry && !discord_ipc_connected(&ipc)) {
            LOG_I("connecting to Discord IPC...");
            if (discord_ipc_connect(&ipc, cfg->client_id) != 0) {
                LOG_W("Discord not reachable, retry in 5s");
                sleep_interruptible(5000);
                continue;
            }
            LOG_I("connected to Discord");
            track_clear(&last);
            track_clear(&pending);
            have_pending = 0;
            last_send_ms = 0;
            play_hold = 0;
        }

        if (!dry && discord_ipc_pump(&ipc, 0) != 0) {
            LOG_W("IPC lost");
            continue;
        }

        track_clear(&info);
        browser_scan(&info);
        now = sc_now_ms();

        /* Skip gaps: playerctl/MPRIS go empty for a moment and the tab
           title falls back to Browsing. Keep the last playing track. */
        if (info.kind == TRACK_PLAYING) {
            play_hold = 0;
        } else if (last.kind == TRACK_PLAYING ||
                   (have_pending && pending.kind == TRACK_PLAYING)) {
            play_hold++;
            if (play_hold <= 6) {
                info = have_pending && pending.kind == TRACK_PLAYING ? pending : last;
            }
        } else {
            play_hold = 0;
        }

        if (info.kind == TRACK_NONE) {
            misses++;
            have_pending = 0;
            if (misses >= cfg->idle_scans_to_clear && last.kind != TRACK_NONE) {
                if (dry)
                    LOG_I("clear presence (no SoundCloud window)");
                else if (presence_clear(&ipc) != 0)
                    LOG_W("clear failed");
                track_clear(&last);
                last_send_ms = now;
            } else if (last.kind == TRACK_NONE &&
                       (misses == 1 || misses % 5 == 0)) {
                LOG_I("SoundCloud not seen yet — open soundcloud.com (try --dry-run -v)");
            } else {
                LOG_D("no SoundCloud (miss %d/%d)", misses, cfg->idle_scans_to_clear);
            }
        } else {
            misses = 0;
            shown = have_pending ? &pending : &last;
            same_song = track_same_song(&info, shown);

            if (info.kind == TRACK_PLAYING) {
                if (!same_song)
                    info.start_ms = now;
                else
                    info.start_ms = shown->start_ms ? shown->start_ms : now;
                if (same_song) {
                    if (!info.url[0] && shown->url[0])
                        sc_strlcpy(info.url, shown->url, sizeof info.url);
                    if (!info.art_url[0] && shown->art_url[0])
                        sc_strlcpy(info.art_url, shown->art_url, sizeof info.art_url);
                }
            }

            if (!same_song) {
                pending = info;
                have_pending = 1;
                last_identity_ms = now;
            } else if (!track_equal(&info, shown)) {
                pending = info;
                have_pending = 1;
            }

            /* One payload per settled track, with cover. Identity changes
               skip the long rate limit so the profile can catch up. */
            quiet = (now - last_identity_ms) >= 400;
            waiting_art = have_pending && pending.kind == TRACK_PLAYING &&
                          !sc_is_http_url(pending.art_url) &&
                          (now - last_identity_ms) < 1200;
            identity_change = have_pending && !track_same_song(&pending, &last);
            rate_ok = last_send_ms == 0 ||
                      (identity_change && (now - last_send_ms) >= 600) ||
                      (!identity_change && (now - last_send_ms) >= 4000);
            heartbeat = last.kind != TRACK_NONE &&
                        last_send_ms != 0 &&
                        (now - last_send_ms) >= heartbeat_ms;
            do_send = 0;
            if (heartbeat && !have_pending)
                do_send = 1;
            if (have_pending && rate_ok && quiet && !waiting_art)
                do_send = 1;

            if (do_send) {
                TrackInfo *send = have_pending ? &pending : &last;
                if (heartbeat && !have_pending)
                    send->start_ms = last.start_ms;
                if (dry) {
                    track_log(send, heartbeat && !have_pending ? "heartbeat " : "");
                } else {
                    if (identity_change && last.kind != TRACK_NONE) {
                        /* Bust a stale profile card: null, then the new track. */
                        (void)presence_clear(&ipc);
                        discord_ipc_pump(&ipc, 40);
                    }
                    if (presence_send(&ipc, cfg, send) != 0) {
                        LOG_W("SET_ACTIVITY failed, will reconnect");
                        discord_ipc_close(&ipc);
                        sleep_interruptible(cfg->poll_ms);
                        continue;
                    }
                    discord_ipc_pump(&ipc, 80);
                }
                last = *send;
                have_pending = 0;
                last_send_ms = now;
            }
        }

        fast = info.kind == TRACK_PLAYING || info.searching ||
               last.kind == TRACK_PLAYING || last.searching || have_pending;
        sleep_interruptible(fast ? 300 : cfg->poll_ms);
    }

    if (!dry && discord_ipc_connected(&ipc))
        presence_clear(&ipc);
    discord_ipc_close(&ipc);
    browser_shutdown();
    LOG_I("bye");
    return 0;
}

int main(int argc, char **argv)
{
    Config cfg;
    const char *cfg_path = NULL;
    char found[512];
    int dry = 0, test_rpc = 0, verbose = 0, i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            return track_self_test();
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            dry = 1;
        } else if (strcmp(argv[i], "--test-rpc") == 0) {
            test_rpc = 1;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if ((strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--config") == 0) &&
                   i + 1 < argc) {
            cfg_path = argv[++i];
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        }
    }

    config_defaults(&cfg);
    if (!cfg_path) {
        if (config_find_default(found, sizeof found) == 0)
            cfg_path = found;
        else
            cfg_path = "config.ini";
    }
    if (config_load(&cfg, cfg_path) != 0) {
        if (!dry && !test_rpc) {
            LOG_E("cannot read config '%s'", cfg_path);
            LOG_E("copy config.example.ini to config.ini and set client_id");
            LOG_E("create an application named SoundCloud at:");
            LOG_E("  https://discord.com/developers/applications");
            return 1;
        }
        LOG_W("no config at '%s', using defaults", cfg_path);
    } else {
        LOG_I("loaded %s", cfg_path);
    }

    if (verbose)
        cfg.log_level = LOG_DEBUG;
    log_set_level((LogLevel)cfg.log_level);

    if (!dry && !test_rpc && !config_client_id_ok(cfg.client_id)) {
        LOG_E("client_id missing or invalid in %s", cfg_path);
        LOG_E("it must be the Application ID (17-22 digits) from the Discord Developer Portal");
        return 1;
    }
    if ((test_rpc) && !config_client_id_ok(cfg.client_id)) {
        LOG_E("--test-rpc needs a valid client_id in config");
        return 1;
    }

    install_signals();

    if (test_rpc)
        return run_test_rpc(&cfg);
    return run_loop(&cfg, dry);
}
