/* Optional MPRIS backend. Compiled only with -DHAVE_MPRIS and libdbus-1. */

#include "browser.h"
#include "log.h"
#include "util.h"

#include <dbus/dbus.h>
#include <string.h>

static DBusConnection *g_bus;

int mpris_init(void)
{
    DBusError err;

    dbus_error_init(&err);
    g_bus = dbus_bus_get(DBUS_BUS_SESSION, &err);
    if (!g_bus) {
        LOG_D("dbus session: %s", err.message ? err.message : "fail");
        dbus_error_free(&err);
        return -1;
    }
    return 0;
}

void mpris_shutdown(void)
{
    if (g_bus) {
        dbus_connection_unref(g_bus);
        g_bus = NULL;
    }
}

static int get_player_prop(const char *bus_name, const char *prop, DBusMessage **reply_out)
{
    DBusMessage *msg, *reply;
    DBusError err;
    const char *iface = "org.mpris.MediaPlayer2.Player";

    *reply_out = NULL;
    msg = dbus_message_new_method_call(bus_name,
                                       "/org/mpris/MediaPlayer2",
                                       "org.freedesktop.DBus.Properties",
                                       "Get");
    if (!msg)
        return -1;
    if (!dbus_message_append_args(msg,
                                  DBUS_TYPE_STRING, &iface,
                                  DBUS_TYPE_STRING, &prop,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return -1;
    }
    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(g_bus, msg, 120, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return -1;
    }
    *reply_out = reply;
    return 0;
}

static int variant_string(DBusMessage *reply, char *out, size_t cap)
{
    DBusMessageIter it, var;

    dbus_message_iter_init(reply, &it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT)
        return -1;
    dbus_message_iter_recurse(&it, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_STRING)
        return -1;
    {
        const char *s = NULL;
        dbus_message_iter_get_basic(&var, &s);
        sc_strlcpy(out, s ? s : "", cap);
    }
    return 0;
}

static void dict_read_metadata(DBusMessage *reply, TrackInfo *out, int *is_sc)
{
    DBusMessageIter it, var, dict, entry, val;

    *is_sc = 0;
    dbus_message_iter_init(reply, &it);
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_VARIANT)
        return;
    dbus_message_iter_recurse(&it, &var);
    if (dbus_message_iter_get_arg_type(&var) != DBUS_TYPE_ARRAY)
        return;
    dbus_message_iter_recurse(&var, &dict);
    while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
        const char *key = NULL;
        dbus_message_iter_recurse(&dict, &entry);
        if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
            dbus_message_iter_get_basic(&entry, &key);
        dbus_message_iter_next(&entry);
        if (key && dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
            dbus_message_iter_recurse(&entry, &val);
            if (strcmp(key, "xesam:title") == 0 &&
                dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
                const char *s = NULL;
                dbus_message_iter_get_basic(&val, &s);
                sc_strlcpy(out->title, s ? s : "", sizeof out->title);
                sc_utf8_clamp(out->title, 128);
            } else if (strcmp(key, "xesam:url") == 0 &&
                       dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
                const char *s = NULL;
                dbus_message_iter_get_basic(&val, &s);
                if (s && sc_strcasestr(s, "soundcloud"))
                    *is_sc = 1;
                if (s && sc_is_track_url(s))
                    sc_strlcpy(out->url, s, sizeof out->url);
            } else if (strcmp(key, "mpris:artUrl") == 0 &&
                       dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_STRING) {
                const char *s = NULL;
                dbus_message_iter_get_basic(&val, &s);
                if (s && sc_strcasestr(s, "sndcdn.com"))
                    *is_sc = 1;
                if (s && sc_is_http_url(s) && !sc_istartswith(s, "file:")) {
                    sc_strlcpy(out->art_url, s, sizeof out->art_url);
                    sc_upgrade_art_url(out->art_url);
                }
            } else if (strcmp(key, "xesam:artist") == 0 &&
                       dbus_message_iter_get_arg_type(&val) == DBUS_TYPE_ARRAY) {
                DBusMessageIter arr;
                dbus_message_iter_recurse(&val, &arr);
                if (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&arr, &s);
                    sc_strlcpy(out->artist, s ? s : "", sizeof out->artist);
                    sc_utf8_clamp(out->artist, 128);
                }
            }
        }
        dbus_message_iter_next(&dict);
    }
}

int mpris_query(TrackInfo *out)
{
    DBusMessage *msg, *reply;
    DBusError err;
    DBusMessageIter it, arr;
    int found = 0;

    track_clear(out);
    if (!g_bus)
        return -1;

    msg = dbus_message_new_method_call("org.freedesktop.DBus",
                                       "/org/freedesktop/DBus",
                                       "org.freedesktop.DBus",
                                       "ListNames");
    if (!msg)
        return -1;
    dbus_error_init(&err);
    reply = dbus_connection_send_with_reply_and_block(g_bus, msg, 120, &err);
    dbus_message_unref(msg);
    if (!reply) {
        dbus_error_free(&err);
        return -1;
    }

    if (!dbus_message_iter_init(reply, &it) ||
        dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) {
        dbus_message_unref(reply);
        return -1;
    }
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRING) {
        const char *name = NULL;
        dbus_message_iter_get_basic(&arr, &name);
        if (name && strncmp(name, "org.mpris.MediaPlayer2.", 23) == 0) {
            const char *p = name + 23;
            DBusMessage *st = NULL, *md = NULL;
            char status[32];
            TrackInfo tmp;
            int is_sc = 0;

            if (!sc_strcasestr(p, "chrom") && !sc_strcasestr(p, "firefox") &&
                !sc_strcasestr(p, "brave") && !sc_strcasestr(p, "zen") &&
                !sc_strcasestr(p, "vivaldi") && !sc_strcasestr(p, "edge") &&
                !sc_strcasestr(p, "librewolf") && !sc_strcasestr(p, "floorp") &&
                !sc_strcasestr(p, "opera") && !sc_strcasestr(p, "browser") &&
                !sc_strcasestr(p, "plasma")) {
                dbus_message_iter_next(&arr);
                continue;
            }

            track_clear(&tmp);
            if (get_player_prop(name, "PlaybackStatus", &st) == 0 &&
                variant_string(st, status, sizeof status) == 0) {
                if (sc_strcasecmp(status, "Playing") == 0 ||
                    sc_strcasecmp(status, "Paused") == 0) {
                    tmp.paused = sc_strcasecmp(status, "Paused") == 0;
                    if (get_player_prop(name, "Metadata", &md) == 0) {
                        dict_read_metadata(md, &tmp, &is_sc);
                        dbus_message_unref(md);
                    }
                    if (is_sc && tmp.title[0]) {
                        tmp.kind = TRACK_PLAYING;
                        sc_strlcpy(tmp.details, tmp.title, sizeof tmp.details);
                        *out = tmp;
                        found = 1;
                    }
                }
            }
            if (st)
                dbus_message_unref(st);
            if (found) {
                dbus_message_iter_next(&arr);
                break;
            }
        }
        dbus_message_iter_next(&arr);
    }
    dbus_message_unref(reply);
    return found ? 0 : -1;
}
