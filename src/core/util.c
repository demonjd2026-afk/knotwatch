#include "kw/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die_oom(void)
{
    fputs("knotwatch: out of memory\n", stderr);
    exit(70);
}

void *kw_xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size ? size : 1);
    if (p == NULL) {
        die_oom();
    }
    return p;
}

void *kw_xcalloc(size_t count, size_t size)
{
    void *p = calloc(count ? count : 1, size ? size : 1);
    if (p == NULL) {
        die_oom();
    }
    return p;
}

char *kw_xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *p = kw_xrealloc(NULL, n);
    memcpy(p, s, n);
    return p;
}

void kw_grow(void **items, size_t *cap, size_t need, size_t elem)
{
    if (need <= *cap) {
        return;
    }
    size_t next = *cap ? *cap : 16;
    while (next < need) {
        if (next > SIZE_MAX / 2) {
            die_oom();
        }
        next *= 2;
    }
    if (elem != 0 && next > SIZE_MAX / elem) {
        die_oom();
    }
    *items = kw_xrealloc(*items, next * elem);
    *cap = next;
}

void kw_sb_init(kw_strbuf *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

void kw_sb_free(kw_strbuf *sb)
{
    free(sb->data);
    kw_sb_init(sb);
}

static void sb_reserve(kw_strbuf *sb, size_t extra)
{
    if (extra > SIZE_MAX - sb->len - 1) {
        die_oom();
    }
    void *data = sb->data;
    kw_grow(&data, &sb->cap, sb->len + extra + 1, 1);
    sb->data = data;
}

void kw_sb_puts(kw_strbuf *sb, const char *s)
{
    size_t n = strlen(s);
    sb_reserve(sb, n);
    memcpy(sb->data + sb->len, s, n + 1);
    sb->len += n;
}

void kw_sb_printf(kw_strbuf *sb, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list probe;
    va_copy(probe, ap);
    int n = vsnprintf(NULL, 0, fmt, probe);
    va_end(probe);
    if (n < 0) {
        va_end(ap);
        return;
    }
    sb_reserve(sb, (size_t)n);
    vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t)n;
}

void kw_sb_json_string(kw_strbuf *sb, const char *s)
{
    kw_sb_puts(sb, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':
            kw_sb_puts(sb, "\\\"");
            break;
        case '\\':
            kw_sb_puts(sb, "\\\\");
            break;
        case '\n':
            kw_sb_puts(sb, "\\n");
            break;
        case '\r':
            kw_sb_puts(sb, "\\r");
            break;
        case '\t':
            kw_sb_puts(sb, "\\t");
            break;
        default:
            if (*p < 0x20) {
                kw_sb_printf(sb, "\\u%04x", (unsigned)*p);
            } else {
                char one[2] = {(char)*p, '\0'};
                kw_sb_puts(sb, one);
            }
        }
    }
    kw_sb_puts(sb, "\"");
}

char *kw_sb_steal(kw_strbuf *sb)
{
    char *out = sb->data ? sb->data : kw_xstrdup("");
    kw_sb_init(sb);
    return out;
}

void kw_format_duration(uint64_t ns, char *out, size_t out_size)
{
    if (ns < 1000000000ULL) {
        snprintf(out, out_size, "%llu ms", (unsigned long long)(ns / 1000000ULL));
    } else {
        snprintf(out, out_size, "%.1f s", (double)ns / 1e9);
    }
}
