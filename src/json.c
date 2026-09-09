#include "json.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void jb_init(JsonBuf *b, char *storage, size_t cap)
{
    b->p = storage;
    b->cap = cap;
    b->len = 0;
    b->err = 0;
    if (!storage || cap == 0)
        b->err = 1;
    else
        storage[0] = '\0';
}

void jb_puts(JsonBuf *b, const char *s)
{
    size_t n;

    if (!b || b->err)
        return;
    if (!s)
        s = "";
    n = strlen(s);
    if (b->len + n + 1 > b->cap) {
        b->err = 1;
        return;
    }
    memcpy(b->p + b->len, s, n + 1);
    b->len += n;
}

void jb_printf(JsonBuf *b, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (!b || b->err)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= b->cap - b->len) {
        b->err = 1;
        return;
    }
    b->len += (size_t)n;
}

void jb_str(JsonBuf *b, const char *s)
{
    const unsigned char *p;

    if (!b || b->err)
        return;
    if (!s)
        s = "";
    jb_puts(b, "\"");
    for (p = (const unsigned char *)s; *p; p++) {
        char tmp[8];
        unsigned char c = *p;

        if (c == '"' || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            tmp[2] = '\0';
            jb_puts(b, tmp);
        } else if (c == '\b') {
            jb_puts(b, "\\b");
        } else if (c == '\f') {
            jb_puts(b, "\\f");
        } else if (c == '\n') {
            jb_puts(b, "\\n");
        } else if (c == '\r') {
            jb_puts(b, "\\r");
        } else if (c == '\t') {
            jb_puts(b, "\\t");
        } else if (c < 0x20) {
            snprintf(tmp, sizeof tmp, "\\u%04x", c);
            jb_puts(b, tmp);
        } else {
            tmp[0] = (char)c;
            tmp[1] = '\0';
            jb_puts(b, tmp);
        }
        if (b->err)
            return;
    }
    jb_puts(b, "\"");
}

const char *jb_finish(JsonBuf *b)
{
    if (!b || b->err || !b->p)
        return NULL;
    b->p[b->len] = '\0';
    return b->p;
}
