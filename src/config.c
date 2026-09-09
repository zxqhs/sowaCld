#include "config.h"
#include "log.h"
#include "platform.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SC_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#endif

void config_defaults(Config *c)
{
    memset(c, 0, sizeof *c);
    c->poll_ms = 2000;
    c->idle_scans_to_clear = 2;
    c->activity_type = 0;
    sc_strlcpy(c->activity_name, "sowaCld", sizeof c->activity_name);
    sc_strlcpy(c->large_image_key, "soundcloud", sizeof c->large_image_key);
    sc_strlcpy(c->small_image_play, "play", sizeof c->small_image_play);
    sc_strlcpy(c->small_image_pause, "pause", sizeof c->small_image_pause);
    sc_strlcpy(c->button_label, "Open Track", sizeof c->button_label);
    c->button_url[0] = '\0';
    c->log_level = LOG_INFO;
}

int config_client_id_ok(const char *id)
{
    size_t n, i;

    if (!id)
        return 0;
    n = strlen(id);
    if (n < 17 || n > 22)
        return 0;
    {
        int allzero = 1;
        for (i = 0; i < n; i++) {
            if (!isdigit((unsigned char)id[i]))
                return 0;
            if (id[i] != '0')
                allzero = 0;
        }
        if (allzero)
            return 0;
    }
    return 1;
}

static int parse_level(const char *v)
{
    if (sc_strcasecmp(v, "error") == 0) return LOG_ERROR;
    if (sc_strcasecmp(v, "warn")  == 0) return LOG_WARN;
    if (sc_strcasecmp(v, "info")  == 0) return LOG_INFO;
    if (sc_strcasecmp(v, "debug") == 0) return LOG_DEBUG;
    return -1;
}

int config_load(Config *c, const char *path)
{
    FILE *fp;
    char line[512];
    int lineno = 0;

    fp = fopen(path, "r");
    if (!fp)
        return -1;

    while (fgets(line, sizeof line, fp)) {
        char *eq, *key, *val;
        size_t len;

        lineno++;
        len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        sc_trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;
        eq = strchr(line, '=');
        if (!eq) {
            LOG_W("%s:%d: ignored line (no '=')", path, lineno);
            continue;
        }
        *eq = '\0';
        key = line;
        val = eq + 1;
        sc_trim(key);
        sc_trim(val);
        if (val[0] == '"' || val[0] == '\'') {
            size_t vlen = strlen(val);
            if (vlen >= 2 && val[vlen - 1] == val[0]) {
                val[vlen - 1] = '\0';
                val++;
            }
        }

        if (strcmp(key, "client_id") == 0)
            sc_strlcpy(c->client_id, val, sizeof c->client_id);
        else if (strcmp(key, "poll_ms") == 0)
            c->poll_ms = atoi(val);
        else if (strcmp(key, "idle_scans_to_clear") == 0)
            c->idle_scans_to_clear = atoi(val);
        else if (strcmp(key, "activity_type") == 0)
            c->activity_type = atoi(val);
        else if (strcmp(key, "activity_name") == 0)
            sc_strlcpy(c->activity_name, val, sizeof c->activity_name);
        else if (strcmp(key, "large_image_key") == 0)
            sc_strlcpy(c->large_image_key, val, sizeof c->large_image_key);
        else if (strcmp(key, "small_image_play") == 0)
            sc_strlcpy(c->small_image_play, val, sizeof c->small_image_play);
        else if (strcmp(key, "small_image_pause") == 0)
            sc_strlcpy(c->small_image_pause, val, sizeof c->small_image_pause);
        else if (strcmp(key, "button_label") == 0)
            sc_strlcpy(c->button_label, val, sizeof c->button_label);
        else if (strcmp(key, "button_url") == 0)
            sc_strlcpy(c->button_url, val, sizeof c->button_url);
        else if (strcmp(key, "log_level") == 0) {
            int lv = parse_level(val);
            if (lv >= 0)
                c->log_level = lv;
            else
                LOG_W("%s:%d: unknown log_level '%s'", path, lineno, val);
        } else {
            LOG_W("%s:%d: unknown key '%s'", path, lineno, key);
        }
    }
    fclose(fp);

    if (c->poll_ms < 500)
        c->poll_ms = 500;
    if (c->poll_ms > 30000)
        c->poll_ms = 30000;
    if (c->idle_scans_to_clear < 1)
        c->idle_scans_to_clear = 1;
    if (c->activity_type != 0 && c->activity_type != 2)
        c->activity_type = 0;
    sc_utf8_clamp(c->activity_name, 128);
    sc_utf8_clamp(c->button_label, 32);

    return 0;
}

int config_find_default(char *out, size_t cap)
{
#if defined(SC_WINDOWS)
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *slash;

    if (n == 0 || n >= MAX_PATH)
        return -1;
    slash = strrchr(exe, '\\');
    if (!slash)
        slash = strrchr(exe, '/');
    if (slash)
        slash[1] = '\0';
    snprintf(out, cap, "%sconfig.ini", exe);
    return 0;
#else
    const char *home;
    FILE *fp;

    if ((fp = fopen("config.ini", "r"))) {
        fclose(fp);
        sc_strlcpy(out, "config.ini", cap);
        return 0;
    }
    home = getenv("HOME");
    if (home && home[0]) {
        snprintf(out, cap, "%s/.config/soundcloud-rpc/config.ini", home);
        if ((fp = fopen(out, "r"))) {
            fclose(fp);
            return 0;
        }
    }
    sc_strlcpy(out, "config.ini", cap);
    return -1;
#endif
}
