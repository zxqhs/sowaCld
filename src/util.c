#include "util.h"
#include "platform.h"

#include <ctype.h>
#include <string.h>
#include <time.h>

#include <stdio.h>

#if defined(SC_WINDOWS)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <errno.h>
#  include <fcntl.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

void sc_strlcpy(char *dst, const char *src, size_t cap)
{
    size_t i;

    if (!dst || cap == 0)
        return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (i = 0; i + 1 < cap && src[i]; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

void sc_trim(char *s)
{
    char *start, *end;
    size_t n;

    if (!s)
        return;
    start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    if (start != s)
        memmove(s, start, strlen(start) + 1);
    n = strlen(s);
    end = s + n;
    while (end > s && isspace((unsigned char)end[-1]))
        end--;
    *end = '\0';
}

int sc_strcasecmp(const char *a, const char *b)
{
    unsigned char ca, cb;

    if (!a) a = "";
    if (!b) b = "";
    for (;;) {
        ca = (unsigned char)tolower((unsigned char)*a++);
        cb = (unsigned char)tolower((unsigned char)*b++);
        if (ca != cb)
            return (int)ca - (int)cb;
        if (!ca)
            return 0;
    }
}

int sc_istartswith(const char *s, const char *prefix)
{
    unsigned char ca, cb;

    if (!s) s = "";
    if (!prefix) prefix = "";
    while (*prefix) {
        ca = (unsigned char)tolower((unsigned char)*s++);
        cb = (unsigned char)tolower((unsigned char)*prefix++);
        if (ca != cb)
            return 0;
    }
    return 1;
}

char *sc_strcasestr(const char *hay, const char *needle)
{
    size_t nlen;
    const char *p;

    if (!hay) hay = "";
    if (!needle || !*needle)
        return (char *)hay;
    nlen = strlen(needle);
    for (p = hay; *p; p++) {
        size_t i;
        for (i = 0; i < nlen; i++) {
            unsigned char ca = (unsigned char)tolower((unsigned char)p[i]);
            unsigned char cb = (unsigned char)tolower((unsigned char)needle[i]);
            if (!p[i] || ca != cb)
                break;
        }
        if (i == nlen)
            return (char *)p;
    }
    return NULL;
}

void sc_utf8_clamp(char *s, size_t max_bytes)
{
    size_t n;

    if (!s)
        return;
    n = strlen(s);
    if (n <= max_bytes)
        return;
    n = max_bytes;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    s[n] = '\0';
}

int64_t sc_now_ms(void)
{
#if defined(SC_WINDOWS)
    FILETIME ft;
    uint64_t t;

    GetSystemTimeAsFileTime(&ft);
    t = ((uint64_t)ft.dwHighDateTime << 32) | (uint64_t)ft.dwLowDateTime;
    return (int64_t)(t / 10000ULL - 11644473600000ULL);
#else
    struct timespec ts;

    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000L);
#endif
}

void sc_sleep_ms(int ms)
{
    if (ms <= 0)
        return;
#if defined(SC_WINDOWS)
    Sleep((DWORD)ms);
#else
    {
        struct timespec ts;

        ts.tv_sec = ms / 1000;
        ts.tv_nsec = (long)(ms % 1000) * 1000000L;
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
            ;
    }
#endif
}

int sc_is_http_url(const char *s)
{
    const unsigned char *p;

    if (!s || !s[0])
        return 0;
    if (!sc_istartswith(s, "https://") && !sc_istartswith(s, "http://"))
        return 0;
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p < 32 || *p == 127)
            return 0;
    }
    return 1;
}

int sc_is_track_url(const char *s)
{
    const char *host, *path, *slash;

    if (!sc_is_http_url(s))
        return 0;
    if (sc_strcasestr(s, "on.soundcloud.com/"))
        return 1;
    if (!sc_strcasestr(s, "soundcloud.com/"))
        return 0;
    host = strstr(s, "://");
    if (!host)
        return 0;
    host += 3;
    path = strchr(host, '/');
    if (!path || !path[1] || path[1] == '?' || path[1] == '#')
        return 0;
    path++;
    if (sc_istartswith(path, "search") || sc_istartswith(path, "discover") ||
        sc_istartswith(path, "you/") || sc_istartswith(path, "you?") ||
        sc_istartswith(path, "feed") || sc_istartswith(path, "stream") ||
        sc_istartswith(path, "messages") || sc_istartswith(path, "settings"))
        return 0;
    slash = strchr(path, '/');
    if (!slash || !slash[1] || slash[1] == '?' || slash[1] == '#')
        return 0;
    return 1;
}

void sc_upgrade_art_url(char *url)
{
    static const char *from[] = {
        "-large.", "-t200x200.", "-t300x300.", "-crop.", NULL
    };
    const char *repl = "-t500x500.";
    size_t i;

    if (!url || !url[0])
        return;
    for (i = 0; from[i]; i++) {
        char *p = strstr(url, from[i]);
        if (!p)
            continue;
        {
            char tmp[512];
            size_t head = (size_t)(p - url);
            snprintf(tmp, sizeof tmp, "%.*s%s%s",
                     (int)head, url, repl, p + strlen(from[i]));
            sc_strlcpy(url, tmp, 512);
            return;
        }
    }
}

int sc_json_string(const char *json, const char *key, char *out, size_t cap)
{
    char pat[80];
    const char *p;
    size_t i = 0;

    if (!json || !key || !out || cap == 0)
        return -1;
    snprintf(pat, sizeof pat, "\"%s\"", key);
    p = strstr(json, pat);
    if (!p)
        return -1;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != ':')
        return -1;
    p++;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p != '"')
        return -1;
    p++;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) {
            p++;
            out[i++] = *p++;
            continue;
        }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i > 0 ? 0 : -1;
}

int sc_http_get(const char *url, char *out, size_t cap)
{
#if defined(SC_WINDOWS)
    (void)url;
    (void)out;
    (void)cap;
    return -1;
#else
    int fds[2];
    pid_t pid;
    size_t n = 0;
    int status;

    if (!sc_is_http_url(url) || !out || cap == 0)
        return -1;
    if (pipe(fds) != 0)
        return -1;
    pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        int dn;
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDERR_FILENO);
            close(dn);
        }
        execlp("curl", "curl", "-fsS", "--max-time", "2", "-L",
               "-A", "soundcloud-rpc",
               "-H", "Accept: application/json",
               url, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    while (n + 1 < cap) {
        ssize_t r = read(fds[0], out + n, cap - 1 - n);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    out[n] = '\0';
    close(fds[0]);
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || n == 0)
        return -1;
    return 0;
#endif
}

void sc_urlenc(char *dst, size_t cap, const char *src)
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
