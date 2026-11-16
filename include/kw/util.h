/* Small allocation and string-buffer helpers shared by every module. */
#ifndef KW_UTIL_H
#define KW_UTIL_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Allocation wrappers. Out of memory is unrecoverable for this tool, so they
 * print a diagnostic and exit instead of returning NULL. */
void *kw_xrealloc(void *ptr, size_t size);
void *kw_xcalloc(size_t count, size_t size);
char *kw_xstrdup(const char *s);

/* Grow `*items` (element size `elem`) so it can hold `need` elements. */
void kw_grow(void **items, size_t *cap, size_t need, size_t elem);

typedef struct {
    char *data; /* always NUL-terminated once anything is appended */
    size_t len;
    size_t cap;
} kw_strbuf;

void kw_sb_init(kw_strbuf *sb);
void kw_sb_free(kw_strbuf *sb);
void kw_sb_puts(kw_strbuf *sb, const char *s);
void kw_sb_printf(kw_strbuf *sb, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
/* Append `s` as a JSON string literal, quotes included. */
void kw_sb_json_string(kw_strbuf *sb, const char *s);
/* Hand the buffer to the caller (never NULL) and reset `sb`. */
char *kw_sb_steal(kw_strbuf *sb);

/* Render a nanosecond duration as "850 ms" or "12.4 s" into `out`. */
void kw_format_duration(uint64_t ns, char *out, size_t out_size);

#endif
