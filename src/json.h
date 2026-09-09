#ifndef SC_JSON_H
#define SC_JSON_H

#include <stddef.h>

typedef struct {
    char  *p;
    size_t cap;
    size_t len;
    int    err;
} JsonBuf;

void        jb_init(JsonBuf *b, char *storage, size_t cap);
void        jb_puts(JsonBuf *b, const char *s);
void        jb_printf(JsonBuf *b, const char *fmt, ...);
void        jb_str(JsonBuf *b, const char *s);
const char *jb_finish(JsonBuf *b);

#endif /* SC_JSON_H */
