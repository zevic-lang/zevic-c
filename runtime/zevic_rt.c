/* Zevic runtime. Everything here is called from generated LLVM IR or from the
 * standard library's `extern "zevic"` declarations. All names start with zevic_.
 *
 * ABI rules (see docs/design/ir-notes.md):
 *  - a Zevic String argument arrives as two parameters: (const char *p, int64_t n)
 *  - a String result is written through a leading `zevic_str *out` parameter
 *  - bools cross the boundary as int32_t
 */
#ifdef _WIN32
/* winsock2.h has to come before anything that includes windows.h (gc.h does): windows.h pulls in the old winsock.h,
 * whose definitions clash with winsock2.h. WIN32_LEAN_AND_MEAN keeps windows.h from doing that. */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

#include "zevic_rt.h"

#include "gc.h"

#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <poll.h>
#endif
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>
#ifdef _WIN32
#else
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#ifndef S_IFMT
#define S_IFMT _S_IFMT
#define S_IFDIR _S_IFDIR
#endif
#endif

static int g_argc;
static char **g_argv;

#ifdef _WIN32
/* Strings in Zevic are UTF-8, but the C library of Windows (argv, getenv, stat ...) speaks the ANSI code page. So
 * every string that comes from or goes to the system is converted, and the wide functions are used. */
static wchar_t *rt_wide(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc(sizeof(wchar_t) * (size_t)n);
    if (!w) exit(101);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *rt_utf8(const wchar_t *w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return strdup("");
    char *s = malloc((size_t)n);
    if (!s) exit(101);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
    return s;
}

/* the command line split by the rules of the Microsoft C runtime */
static char **rt_command_line(int *argc) {
    const wchar_t *p = GetCommandLineW();
    int cap = 8, n = 0;
    char **v = malloc(sizeof(char *) * (size_t)cap);
    wchar_t *buf = malloc(sizeof(wchar_t) * (wcslen(p) + 1));
    if (!v || !buf) exit(101);
    for (;;) {
        while (*p == L' ' || *p == L'\t') p++;
        if (!*p) break;
        size_t len = 0;
        int quoted = 0;
        for (;;) {
            size_t bs = 0;
            while (*p == L'\\') {
                bs++;
                p++;
            }
            if (*p == L'"') {
                for (size_t k = 0; k < bs / 2; k++) buf[len++] = L'\\';
                if (bs % 2) {
                    buf[len++] = L'"';
                } else if (quoted && p[1] == L'"') {
                    buf[len++] = L'"';
                    p++;
                } else {
                    quoted = !quoted;
                }
                p++;
            } else {
                for (size_t k = 0; k < bs; k++) buf[len++] = L'\\';
                if (!*p || (!quoted && (*p == L' ' || *p == L'\t'))) break;
                buf[len++] = *p++;
            }
        }
        buf[len] = 0;
        if (n + 1 >= cap) {
            cap *= 2;
            v = realloc(v, sizeof(char *) * (size_t)cap);
            if (!v) exit(101);
        }
        v[n++] = rt_utf8(buf);
    }
    free(buf);
    v[n] = NULL;
    *argc = n;
    return v;
}
#endif

void zevic_init(int argc, char **argv) {
    /* The heap never holds code; macOS/arm64 refuses executable mappings ("Cannot allocate executable pages") */
    GC_set_pages_executable(0);
    GC_INIT();
    g_argc = argc;
    g_argv = argv;
#ifdef _WIN32
    SetConsoleOutputCP(65001);
    if (argc > 0 && argv) { /* not a library user calling zevic_init(0, 0) */
        int n = 0;
        char **v = rt_command_line(&n);
        if (n > 0) {
            g_argc = n;
            g_argv = v;
        }
    }
#endif
}

/* ---- memory ---- */

void *zevic_alloc(int64_t size) {
    if (size <= 0) size = 1;
    void *p = GC_MALLOC((size_t)size); /* cleared, scanned for pointers */
    if (!p) {
        fputs("panic: out of memory\n", stderr);
        exit(101);
    }
    return p;
}

/* Memory that holds no pointers (string bytes): the collector does not scan it. Not cleared. */
void *zevic_alloc_atomic(int64_t size) {
    if (size <= 0) size = 1;
    void *p = GC_MALLOC_ATOMIC((size_t)size);
    if (!p) {
        fputs("panic: out of memory\n", stderr);
        exit(101);
    }
    return p;
}

/* ---- panic ---- */

/* While the test runner is working, a panic or a failed assertion jumps back to it instead of ending the program. */
static _Thread_local jmp_buf *g_jmp;
static _Thread_local int g_jmp_kind; /* 1: a panic, 2: a failed assertion */
static _Thread_local char g_fail[2048];
static _Thread_local const char *g_at_file = "?";
static _Thread_local int g_at_line;

/* The file name without its directory (what the test report shows). */
static const char *base_name(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++)
        if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

void zevic_panic(const char *msg, int64_t len, const char *file, int32_t line) {
    if (g_jmp) {
        snprintf(g_fail, sizeof g_fail, "%s:%d: panic: %.*s", base_name(file), (int)line, (int)len, msg);
        g_jmp_kind = 1;
        longjmp(*g_jmp, 1);
    }
    fflush(stdout);
    fprintf(stderr, "panic: %.*s\n  at %s:%d\n", (int)len, msg, file, (int)line);
    fflush(stderr);
    exit(101);
}

/* ---- output ---- */

static FILE *stream_of(int32_t s) {
    if (s == 2) {
        fflush(stdout);
        return stderr;
    }
    return stdout;
}

void zevic_print(const char *p, int64_t n) {
    fwrite(p, 1, (size_t)n, stdout);
}

void zevic_println(const char *p, int64_t n) {
    fwrite(p, 1, (size_t)n, stdout);
    fputc('\n', stdout);
}

void zevic_eprint(const char *p, int64_t n) {
    fflush(stdout);
    fwrite(p, 1, (size_t)n, stderr);
}

void zevic_eprintln(const char *p, int64_t n) {
    fflush(stdout);
    fwrite(p, 1, (size_t)n, stderr);
    fputc('\n', stderr);
}

/* ---- Printf pieces: the compiler splits the format and checks types ---- */

/* Stream 3 is not a file: Text.Format collects the pieces in a per-thread buffer (zevic_fmt_begin / zevic_fmt_end). */
static _Thread_local char *g_fmt_buf;
static _Thread_local int64_t g_fmt_len, g_fmt_cap;

static void fmt_reserve(int64_t extra) {
    if (g_fmt_len + extra + 1 <= g_fmt_cap) return;
    int64_t cap = g_fmt_cap ? g_fmt_cap : 256;
    while (cap < g_fmt_len + extra + 1) cap *= 2;
    char *p = realloc(g_fmt_buf, (size_t)cap);
    if (!p) exit(101);
    g_fmt_buf = p;
    g_fmt_cap = cap;
}

static void out_bytes(int32_t s, const char *p, int64_t n) {
    if (s == 3) {
        fmt_reserve(n);
        if (n > 0) memcpy(g_fmt_buf + g_fmt_len, p, (size_t)n);
        g_fmt_len += n;
        return;
    }
    fwrite(p, 1, (size_t)n, stream_of(s));
}

static void out_fmt(int32_t s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    if (s == 3) {
        va_list cp;
        va_copy(cp, ap);
        int need = vsnprintf(NULL, 0, fmt, cp);
        va_end(cp);
        if (need > 0) {
            fmt_reserve(need);
            vsnprintf(g_fmt_buf + g_fmt_len, (size_t)need + 1, fmt, ap);
            g_fmt_len += need;
        }
    } else {
        vfprintf(stream_of(s), fmt, ap);
    }
    va_end(ap);
}

void zevic_fmt_begin(void) {
    g_fmt_len = 0;
}

void zevic_fmt_end(zevic_str *out) {
    fmt_reserve(0);
    g_fmt_buf[g_fmt_len] = 0;
    char *p = zevic_alloc_atomic(g_fmt_len + 1);
    if (g_fmt_len > 0) memcpy(p, g_fmt_buf, (size_t)g_fmt_len);
    p[g_fmt_len] = 0;
    out->p = p;
    out->n = g_fmt_len;
    g_fmt_len = 0;
}

void zevic_printf_lit(int32_t s, const char *p, int64_t n) {
    out_bytes(s, p, n);
}

void zevic_printf_i(int32_t s, const char *fmt, int64_t v) {
    out_fmt(s, fmt, (long long)v);
}

void zevic_printf_f(int32_t s, const char *fmt, double v) {
    out_fmt(s, fmt, v);
}

void zevic_printf_s(int32_t s, const char *fmt, const char *p, int64_t n) {
    char *buf = malloc((size_t)n + 1);
    if (!buf) exit(101);
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    out_fmt(s, fmt, buf);
    free(buf);
}

static int utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

void zevic_printf_c(int32_t s, const char *fmt, uint32_t cp) {
    char buf[5];
    int n = utf8_encode(cp, buf);
    buf[n] = 0;
    out_fmt(s, fmt, buf);
}

/* ---- strings ---- */

static void str_set(zevic_str *out, const char *src, int64_t n) {
    char *p = zevic_alloc_atomic(n + 1);
    if (n > 0) memcpy(p, src, (size_t)n);
    p[n] = 0;
    out->p = p;
    out->n = n;
}

void zevic_str_concat(zevic_str *out, const char *a, int64_t an, const char *b, int64_t bn) {
    char *p = zevic_alloc_atomic(an + bn + 1);
    if (an > 0) memcpy(p, a, (size_t)an);
    if (bn > 0) memcpy(p + an, b, (size_t)bn);
    p[an + bn] = 0;
    out->p = p;
    out->n = an + bn;
}

int32_t zevic_str_eq(const char *a, int64_t an, const char *b, int64_t bn) {
    return an == bn && (an == 0 || memcmp(a, b, (size_t)an) == 0);
}

int32_t zevic_str_cmp(const char *a, int64_t an, const char *b, int64_t bn) {
    int64_t m = an < bn ? an : bn;
    int c = m > 0 ? memcmp(a, b, (size_t)m) : 0;
    if (c != 0) return c < 0 ? -1 : 1;
    return an < bn ? -1 : an > bn ? 1 : 0;
}

int64_t zevic_str_char_count(const char *p, int64_t n) {
    int64_t c = 0;
    for (int64_t i = 0; i < n; i++)
        if (((unsigned char)p[i] & 0xC0) != 0x80) c++;
    return c;
}

void zevic_i64_to_str(zevic_str *out, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%lld", (long long)v);
    str_set(out, buf, n);
}

void zevic_u64_to_str(zevic_str *out, int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof buf, "%llu", (unsigned long long)v);
    str_set(out, buf, n);
}

void zevic_f64_to_str(zevic_str *out, double v) {
    char buf[64];
    if (isnan(v)) {
        str_set(out, "NaN", 3);
        return;
    }
    if (isinf(v)) {
        if (v < 0) str_set(out, "-Infinity", 9);
        else str_set(out, "Infinity", 8);
        return;
    }
    int n = 0, prec = 1;
    for (prec = 1; prec <= 17; prec++) {
        n = snprintf(buf, sizeof buf, "%.*g", prec, v);
        if (strtod(buf, NULL) == v) break;
    }
    /* %g switches to exponent notation early (1000 -> 1e+03); keep plain digits for moderate sizes */
    char *ep = strchr(buf, 'e');
    if (ep) {
        int e = atoi(ep + 1);
        if (e >= -5 && e < 16) {
            int decimals = prec - 1 - e;
            if (decimals < 0) decimals = 0;
            n = snprintf(buf, sizeof buf, "%.*f", decimals, v);
        }
    }
    if (!strpbrk(buf, ".e")) {
        buf[n++] = '.';
        buf[n++] = '0';
        buf[n] = 0;
    }
    str_set(out, buf, n);
}

void zevic_bool_to_str(zevic_str *out, int32_t v) {
    if (v) str_set(out, "true", 4);
    else str_set(out, "false", 5);
}

void zevic_char_to_str(zevic_str *out, uint32_t cp) {
    char buf[5];
    int n = utf8_encode(cp, buf);
    str_set(out, buf, n);
}

/* ---- text to numbers ---- */

/* Is the text a plain decimal number: [+-] digits [. digits] [e [+-] digits] ? */
int32_t zevic_f64_valid(const char *p, int64_t n) {
    int64_t i = 0, digits = 0;
    if (i < n && (p[i] == '+' || p[i] == '-')) i++;
    while (i < n && p[i] >= '0' && p[i] <= '9') { i++; digits++; }
    if (i < n && p[i] == '.') {
        i++;
        int64_t frac = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') { i++; frac++; }
        if (frac == 0) return 0;
    }
    if (digits == 0) return 0;
    if (i < n && (p[i] == 'e' || p[i] == 'E')) {
        i++;
        if (i < n && (p[i] == '+' || p[i] == '-')) i++;
        int64_t ed = 0;
        while (i < n && p[i] >= '0' && p[i] <= '9') { i++; ed++; }
        if (ed == 0) return 0;
    }
    return i == n;
}

uint64_t zevic_f64_bits(double x) {
    uint64_t u;
    memcpy(&u, &x, sizeof u);
    return u;
}

double zevic_f64_parse(const char *p, int64_t n) {
    char *buf = malloc((size_t)n + 1);
    if (!buf) exit(101);
    memcpy(buf, p, (size_t)n);
    buf[n] = 0;
    double v = strtod(buf, NULL);
    free(buf);
    return v;
}

/* ---- string builder (used for "${...}" interpolation) ---- */

typedef struct {
    char *p;
    int64_t n, cap;
} sbuf;

void *zevic_sb_new(void) {
    return zevic_alloc(sizeof(sbuf));
}

void zevic_sb_add(void *h, const char *p, int64_t n) {
    sbuf *b = h;
    if (b->n + n + 1 > b->cap) {
        int64_t nc = b->cap ? b->cap : 32;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = zevic_alloc_atomic(nc);
        if (b->n) memcpy(np, b->p, (size_t)b->n);
        b->p = np;
        b->cap = nc;
    }
    if (n > 0) memcpy(b->p + b->n, p, (size_t)n);
    b->n += n;
    b->p[b->n] = 0;
}

void zevic_sb_finish(zevic_str *out, void *h) {
    sbuf *b = h;
    if (!b->p) {
        str_set(out, "", 0);
        return;
    }
    out->p = b->p;
    out->n = b->n;
}

/* ---- StringBuilder (Zevic.Core). Same buffer as the interpolation builder; ToString copies, because
 * a builder can be cleared or cut back and then written again. ---- */

void *zevic_strbuf_new(void) {
    return zevic_sb_new();
}

void zevic_strbuf_add(void *h, const char *p, int64_t n) {
    zevic_sb_add(h, p, n);
}

int64_t zevic_strbuf_len(void *h) {
    return ((sbuf *)h)->n;
}

/* Keep the first n bytes (n is checked by the caller). */
void zevic_strbuf_truncate(void *h, int64_t n) {
    sbuf *b = h;
    b->n = n;
    if (b->p) b->p[n] = 0;
}

void zevic_strbuf_snapshot(zevic_str *out, void *h) {
    sbuf *b = h;
    char *c = zevic_alloc_atomic(b->n + 1);
    if (b->n) memcpy(c, b->p, (size_t)b->n);
    c[b->n] = 0;
    out->p = c;
    out->n = b->n;
}

/* ---- C strings ---- */

/* Length of the valid UTF-8 sequence at p (0 if invalid). */
static int utf8_seq(const unsigned char *p, int64_t left) {
    unsigned c = p[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return left >= 2 && (p[1] & 0xC0) == 0x80 ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (left < 3 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && p[1] < 0xA0) return 0;
        if (c == 0xED && p[1] >= 0xA0) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (left < 4 || (p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && p[1] < 0x90) return 0;
        if (c == 0xF4 && p[1] >= 0x90) return 0;
        return 4;
    }
    return 0;
}

/* Copy a NUL-terminated C string into a new String.
 * lossy = 0: returns 0 (and leaves *out empty) when the bytes are not UTF-8.
 * lossy = 1: bad bytes become U+FFFD; always returns 1. A NULL pointer gives "". */
int32_t zevic_str_from_c(zevic_str *out, const char *c, int32_t lossy) {
    if (!c) {
        str_set(out, "", 0);
        return 1;
    }
    int64_t n = (int64_t)strlen(c);
    const unsigned char *u = (const unsigned char *)c;
    if (!lossy) {
        for (int64_t i = 0; i < n;) {
            int k = utf8_seq(u + i, n - i);
            if (!k) {
                str_set(out, "", 0);
                return 0;
            }
            i += k;
        }
        str_set(out, c, n);
        return 1;
    }
    char *p = zevic_alloc_atomic(n * 3 + 1);
    int64_t m = 0;
    for (int64_t i = 0; i < n;) {
        int k = utf8_seq(u + i, n - i);
        if (k) {
            memcpy(p + m, u + i, (size_t)k);
            m += k;
            i += k;
        } else {
            memcpy(p + m, "\xEF\xBF\xBD", 3);
            m += 3;
            i++;
        }
    }
    p[m] = 0;
    out->p = p;
    out->n = m;
    return 1;
}

/* ---- bytes and checksums (Zevic.Bytes, Zevic.Digest) ---- */

/* Is the byte sequence well-formed UTF-8? */
int32_t zevic_utf8_valid(const uint8_t *p, int64_t n) {
    for (int64_t i = 0; i < n;) {
        int k = utf8_seq(p + i, n - i);
        if (!k) return 0;
        i += k;
    }
    return 1;
}

/* The bytes as a String (a copy). lossy = 1: bad sequences become U+FFFD. lossy = 0: the bytes as they are
 * (the caller has checked them, or the String is only passed on to something that takes any bytes). */
void zevic_bytes_to_str(zevic_str *out, const uint8_t *p, int64_t n, int32_t lossy) {
    char *b = zevic_alloc_atomic(n * (lossy ? 3 : 1) + 1);
    int64_t m = 0;
    if (!lossy) {
        if (n) memcpy(b, p, (size_t)n);
        m = n;
    } else {
        for (int64_t i = 0; i < n;) {
            int k = utf8_seq(p + i, n - i);
            if (k) {
                memcpy(b + m, p + i, (size_t)k);
                m += k;
                i += k;
            } else {
                memcpy(b + m, "\xEF\xBF\xBD", 3);
                m += 3;
                i++;
            }
        }
    }
    b[m] = 0;
    out->p = b;
    out->n = m;
}

/* Copy the bytes of a String into a buffer that is big enough. */
void zevic_bytes_from_str(uint8_t *dst, const char *p, int64_t n) {
    if (n > 0) memcpy(dst, p, (size_t)n);
}

static uint32_t g_crc_table[256];
static int g_crc_ready;

/* CRC-32 (the one in zip and PNG). Pass 0 as `prev` to start; pass an earlier result to go on. */
uint32_t zevic_crc32(const uint8_t *p, int64_t n, uint32_t prev) {
    if (!g_crc_ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            g_crc_table[i] = c;
        }
        g_crc_ready = 1;
    }
    uint32_t c = ~prev;
    for (int64_t i = 0; i < n; i++) c = g_crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return ~c;
}

uint32_t zevic_adler32(const uint8_t *p, int64_t n, uint32_t prev) {
    uint32_t a = prev & 0xFFFF, b = (prev >> 16) & 0xFFFF;
    if (prev == 0) a = 1;
    for (int64_t i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static const uint32_t sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *blk) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) | ((uint32_t)blk[i * 4 + 2] << 8) | blk[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + sha256_k[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

/* SHA-256 of n bytes: writes the 32-byte digest to out. */
void zevic_sha256(uint8_t *out, const uint8_t *p, int64_t n) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    int64_t i = 0;
    for (; i + 64 <= n; i += 64) sha256_block(h, p + i);
    uint8_t tail[128];
    int64_t rem = n - i;
    if (rem > 0) memcpy(tail, p + i, (size_t)rem);
    tail[rem] = 0x80;
    int64_t total = (rem + 1 + 8 <= 64) ? 64 : 128;
    memset(tail + rem + 1, 0, (size_t)(total - rem - 1));
    uint64_t bits = (uint64_t)n * 8u;
    for (int k = 0; k < 8; k++) tail[total - 1 - k] = (uint8_t)(bits >> (8 * k));
    sha256_block(h, tail);
    if (total == 128) sha256_block(h, tail + 64);
    for (int k = 0; k < 8; k++) {
        out[k * 4] = (uint8_t)(h[k] >> 24);
        out[k * 4 + 1] = (uint8_t)(h[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(h[k] >> 8);
        out[k * 4 + 3] = (uint8_t)h[k];
    }
}

/* A copy of the bytes in malloc'd memory, NUL-terminated, for C to keep and free. */
char *zevic_new_cstring(const char *p, int64_t n) {
    char *c = malloc((size_t)n + 1);
    if (!c) {
        fputs("panic: out of memory\n", stderr);
        exit(101);
    }
    if (n > 0) memcpy(c, p, (size_t)n);
    c[n] = 0;
    return c;
}

/* ---- hashing (Hashable for the built-in types) ---- */

uint64_t zevic_hash_u64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

uint64_t zevic_str_hash(const char *p, int64_t n) {
    uint64_t h = 0xcbf29ce484222325ULL; /* FNV-1a */
    for (int64_t i = 0; i < n; i++) {
        h ^= (unsigned char)p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ---- handles: a table that lets C hold a number instead of a Zevic object ----
 * The table lives in memory the collector scans but never frees, so an object stays alive while it is listed. */

static void **g_handles;
static uint64_t g_handle_n, g_handle_cap;
static uint64_t *g_handle_free;
static uint64_t g_handle_nfree, g_handle_freecap;

uint64_t zevic_handle_new(void *obj) {
    if (g_handle_nfree > 0) {
        uint64_t id = g_handle_free[--g_handle_nfree];
        g_handles[id - 1] = obj;
        return id;
    }
    if (g_handle_n == g_handle_cap) {
        uint64_t nc = g_handle_cap ? g_handle_cap * 2 : 64;
        void **nt = GC_MALLOC_UNCOLLECTABLE((size_t)nc * sizeof(void *));
        if (!nt) {
            fputs("panic: out of memory\n", stderr);
            exit(101);
        }
        if (g_handles) {
            memcpy(nt, g_handles, (size_t)g_handle_n * sizeof(void *));
            GC_FREE(g_handles);
        }
        g_handles = nt;
        g_handle_cap = nc;
    }
    g_handles[g_handle_n++] = obj;
    return g_handle_n;
}

void *zevic_handle_get(uint64_t id) {
    if (id == 0 || id > g_handle_n) return NULL;
    return g_handles[id - 1];
}

void zevic_handle_release(uint64_t id) {
    if (id == 0 || id > g_handle_n || !g_handles[id - 1]) return;
    g_handles[id - 1] = NULL;
    if (g_handle_nfree == g_handle_freecap) {
        g_handle_freecap = g_handle_freecap ? g_handle_freecap * 2 : 64;
        g_handle_free = realloc(g_handle_free, (size_t)g_handle_freecap * sizeof(uint64_t));
        if (!g_handle_free) exit(101);
    }
    g_handle_free[g_handle_nfree++] = id;
}

/* ---- environment ---- */

static char *nul_copy(const char *p, int64_t n) {
    char *b = malloc((size_t)n + 1);
    if (!b) exit(101);
    memcpy(b, p, (size_t)n);
    b[n] = 0;
    return b;
}

int32_t zevic_env_has(const char *name, int64_t nn) {
    char *k = nul_copy(name, nn);
#ifdef _WIN32
    wchar_t *wk = rt_wide(k);
    int has = wk && _wgetenv(wk) != NULL;
    free(wk);
#else
    int has = getenv(k) != NULL;
#endif
    free(k);
    return has;
}

void zevic_env_get(zevic_str *out, const char *name, int64_t nn) {
    char *k = nul_copy(name, nn);
#ifdef _WIN32
    wchar_t *wk = rt_wide(k);
    const wchar_t *wv = wk ? _wgetenv(wk) : NULL;
    char *v8 = wv ? rt_utf8(wv) : strdup("");
    free(wk);
    free(k);
    str_set(out, v8, (int64_t)strlen(v8));
    free(v8);
#else
    const char *v = getenv(k);
    free(k);
    if (!v) v = "";
    str_set(out, v, (int64_t)strlen(v));
#endif
}

int32_t zevic_env_set(const char *name, int64_t nn, const char *val, int64_t vn) {
    char *k = nul_copy(name, nn), *v = nul_copy(val, vn);
#ifdef _WIN32
    wchar_t *wk = rt_wide(k), *wv = rt_wide(v);
    int rc = wk && wv ? _wputenv_s(wk, wv) : -1;
    free(wk);
    free(wv);
#else
    int rc = setenv(k, v, 1);
#endif
    free(k);
    free(v);
    return rc == 0;
}

int32_t zevic_argc(void) {
    return g_argc;
}

void zevic_arg(zevic_str *out, int32_t i) {
    if (i < 0 || i >= g_argc) {
        str_set(out, "", 0);
        return;
    }
    str_set(out, g_argv[i], (int64_t)strlen(g_argv[i]));
}

/* The full path of the running program ("" if the system will not tell). */
void zevic_exe_path(zevic_str *out) {
    char buf[4096];
#ifdef _WIN32
    wchar_t wbuf[32768];
    DWORD n = GetModuleFileNameW(NULL, wbuf, (DWORD)(sizeof wbuf / sizeof wbuf[0]));
    if (n > 0 && n < sizeof wbuf / sizeof wbuf[0]) {
        char *u = rt_utf8(wbuf);
        str_set(out, u, (int64_t)strlen(u));
        free(u);
        return;
    }
#elif defined(__APPLE__)
    uint32_t sz = sizeof buf;
    if (_NSGetExecutablePath(buf, &sz) == 0) {
        char *real = realpath(buf, NULL);
        str_set(out, real ? real : buf, (int64_t)strlen(real ? real : buf));
        free(real);
        return;
    }
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        str_set(out, buf, (int64_t)n);
        return;
    }
#endif
    str_set(out, "", 0);
}

void zevic_cwd(zevic_str *out) {
#ifdef _WIN32
    wchar_t wbuf[32768];
    if (_wgetcwd(wbuf, (int)(sizeof wbuf / sizeof wbuf[0]))) {
        char *u = rt_utf8(wbuf);
        str_set(out, u, (int64_t)strlen(u));
        free(u);
    } else {
        str_set(out, "", 0);
    }
#else
    char buf[4096];
    char *r = getcwd(buf, sizeof buf);
    str_set(out, r ? buf : "", r ? (int64_t)strlen(buf) : 0);
#endif
}

void zevic_exit(int32_t code) {
    fflush(stdout);
    fflush(stderr);
    exit(code);
}

/* ---- input ---- */

/* One line without its line ending (\n or \r\n). Whether a line was read (0 at the end of the input) is
 * answered by zevic_read_line_ok() straight afterwards, because a String result takes the return slot. */
static _Thread_local int32_t g_read_ok;

int32_t zevic_read_line_ok(void) {
    return g_read_ok;
}

void zevic_read_line(zevic_str *out) {
    g_read_ok = 0;
    fflush(stdout);
    int64_t cap = 128, n = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    int c, got = 0;
    while ((c = fgetc(stdin)) != EOF) {
        got = 1;
        if (c == '\n') break;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
        buf[n++] = (char)c;
    }
    if (!got) {
        free(buf);
        str_set(out, "", 0);
        return;
    }
    if (n > 0 && buf[n - 1] == '\r') n--;
    str_set(out, buf, n);
    free(buf);
    g_read_ok = 1;
}

/* One UTF-8 character as a code point, or -1 at the end of the input. */
int32_t zevic_read_char(void) {
    fflush(stdout);
    int c = fgetc(stdin);
    if (c == EOF) return -1;
    if (c < 0x80) return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
    uint32_t cp = extra == 3 ? (uint32_t)(c & 0x07) : extra == 2 ? (uint32_t)(c & 0x0F) : extra == 1 ? (uint32_t)(c & 0x1F) : 0xFFFD;
    for (int i = 0; i < extra; i++) {
        int d = fgetc(stdin);
        if (d == EOF || (d & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (uint32_t)(d & 0x3F);
    }
    return (int32_t)cp;
}

void zevic_read_all(zevic_str *out) {
    fflush(stdout);
    int64_t cap = 4096, n = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    size_t r;
    while ((r = fread(buf + n, 1, (size_t)(cap - n), stdin)) > 0) {
        n += (int64_t)r;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    str_set(out, buf, n);
    free(buf);
}

/* ---- text helpers ---- */

/* Is [from, to) inside the string and on character boundaries? */
int32_t zevic_str_slice_ok(const char *p, int64_t n, int64_t from, int64_t to) {
    if (from < 0 || to < from || to > n) return 0;
    if (from < n && ((unsigned char)p[from] & 0xC0) == 0x80) return 0;
    if (to < n && ((unsigned char)p[to] & 0xC0) == 0x80) return 0;
    return 1;
}

void zevic_str_slice(zevic_str *out, const char *p, int64_t n, int64_t from, int64_t to) {
    (void)n;
    str_set(out, p + from, to - from);
}

/* Byte position of the first occurrence of q in p at or after `from`, or -1. */
int64_t zevic_str_index(const char *p, int64_t n, const char *q, int64_t m, int64_t from) {
    if (from < 0) from = 0;
    if (m == 0) return from <= n ? from : -1;
    for (int64_t i = from; i + m <= n; i++)
        if (p[i] == q[0] && memcmp(p + i, q, (size_t)m) == 0) return i;
    return -1;
}

/* Do the bytes of q sit in p at byte position `at`? (No character boundaries are needed.) */
int32_t zevic_str_has_at(const char *p, int64_t n, const char *q, int64_t m, int64_t at) {
    if (at < 0 || at + m > n) return 0;
    return m == 0 || memcmp(p + at, q, (size_t)m) == 0;
}

/* ASCII case conversion: letters outside ASCII are left as they are. */
void zevic_str_upper(zevic_str *out, const char *p, int64_t n) {
    char *b = zevic_alloc_atomic(n + 1);
    for (int64_t i = 0; i < n; i++) b[i] = (p[i] >= 'a' && p[i] <= 'z') ? (char)(p[i] - 32) : p[i];
    b[n] = 0;
    out->p = b;
    out->n = n;
}

void zevic_str_lower(zevic_str *out, const char *p, int64_t n) {
    char *b = zevic_alloc_atomic(n + 1);
    for (int64_t i = 0; i < n; i++) b[i] = (p[i] >= 'A' && p[i] <= 'Z') ? (char)(p[i] + 32) : p[i];
    b[n] = 0;
    out->p = b;
    out->n = n;
}

/* ---- time ---- */

int64_t zevic_time_monotonic_ns(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

int64_t zevic_time_unix_ns(void) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; /* 100 ns since 1601 */
    return (int64_t)((t - 116444736000000000ULL) * 100ULL);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
#endif
}

/* The local time zone's offset from UTC (seconds, east is positive) at a moment given as Unix seconds. It is
 * found by reading the local calendar time and comparing it with the moment, so it needs no tm_gmtoff. */
static int64_t rt_days_from_civil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int32_t zevic_local_offset(int64_t unix_seconds) {
    time_t t = (time_t)unix_seconds;
    struct tm lt;
#ifdef _WIN32
    if (localtime_s(&lt, &t) != 0) return 0;
#else
    if (!localtime_r(&t, &lt)) return 0;
#endif
    int64_t as_utc = rt_days_from_civil(lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday) * 86400 +
                     lt.tm_hour * 3600 + lt.tm_min * 60 + lt.tm_sec;
    return (int32_t)(as_utc - unix_seconds);
}

void zevic_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    fflush(stdout);
#ifdef _WIN32
    Sleep((DWORD)((ns + 999999) / 1000000));
#else
    struct timespec ts = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    while (nanosleep(&ts, &ts) != 0) {}
#endif
}

/* ---- files ----
 * A failing call leaves the reason in g_fs_errno; zevic_fs_errkind() and zevic_fs_errmsg() describe it. */

static _Thread_local int g_fs_errno;

/* Did the last file call fail? */
int32_t zevic_fs_failed(void) {
    return g_fs_errno != 0;
}

int32_t zevic_fs_errkind(void) {
    if (g_fs_errno == ENOENT) return 0; /* NotFound */
    if (g_fs_errno == EACCES || g_fs_errno == EPERM) return 1; /* Permission */
    return 2; /* Io */
}

void zevic_fs_errmsg(zevic_str *out) {
    const char *m = strerror(g_fs_errno);
    str_set(out, m, (int64_t)strlen(m));
}

static FILE *fs_fopen(const char *path, const char *mode) {
#ifdef _WIN32
    /* paths are UTF-8 */
    wchar_t *wp = rt_wide(path), *wm = rt_wide(mode);
    FILE *f = NULL;
    if (wp && wm) f = _wfopen(wp, wm);
    else errno = EINVAL;
    free(wp);
    free(wm);
    return f;
#else
    return fopen(path, mode);
#endif
}

void zevic_fs_read_all(zevic_str *out, const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    FILE *f = fs_fopen(path, "rb");
    free(path);
    if (!f) {
        g_fs_errno = errno;
        str_set(out, "", 0);
        return;
    }
    int64_t cap = 4096, len = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    size_t r;
    while ((r = fread(buf + len, 1, (size_t)(cap - len), f)) > 0) {
        len += (int64_t)r;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    if (ferror(f)) g_fs_errno = errno ? errno : EIO;
    fclose(f);
    str_set(out, buf, len);
    free(buf);
}

/* append = 0: replace the file; 1: add to its end. Returns 1 on success, 0 on failure. */
int32_t zevic_fs_write_all(const char *p, int64_t n, const char *d, int64_t dn, int32_t append) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    FILE *f = fs_fopen(path, append ? "ab" : "wb");
    free(path);
    if (!f) {
        g_fs_errno = errno;
        return 0;
    }
    int ok = dn == 0 || fwrite(d, 1, (size_t)dn, f) == (size_t)dn;
    if (!ok) g_fs_errno = errno ? errno : EIO;
    if (fclose(f) != 0 && ok) {
        g_fs_errno = errno ? errno : EIO;
        ok = 0;
    }
    return ok;
}

/* 0: does not exist, 1: a file, 2: a directory */
int32_t zevic_fs_kind(const char *p, int64_t n) {
    char *path = nul_copy(p, n);
#ifdef _WIN32
    struct _stat64 st;
    wchar_t *wp = rt_wide(path);
    int rc = wp ? _wstat64(wp, &st) : -1;
    free(wp);
#else
    struct stat st;
    int rc = stat(path, &st);
#endif
    free(path);
    if (rc != 0) return 0;
    return (st.st_mode & S_IFMT) == S_IFDIR ? 2 : 1;
}

/* Write raw bytes to a file (any bytes, not only UTF-8). append = 1 adds at the end. 1 on success. */
int32_t zevic_fs_write_bytes(const char *p, int64_t n, const uint8_t *d, int64_t dn, int32_t append) {
    return zevic_fs_write_all(p, n, (const char *)d, dn, append);
}

/* ---- more file calls (Zevic.Fs): symlinks, size, time, copy, temp dir ---- */

/* Is the path itself a symbolic link (or, on Windows, a reparse point)? A broken link counts. */
int32_t zevic_fs_islink(const char *p, int64_t n) {
    char *path = nul_copy(p, n);
#ifdef _WIN32
    wchar_t *wp = rt_wide(path);
    DWORD attr = wp ? GetFileAttributesW(wp) : INVALID_FILE_ATTRIBUTES;
    free(wp);
    int r = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT) ? 1 : 0;
#else
    struct stat st;
    int r = lstat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK ? 1 : 0;
#endif
    free(path);
    return r;
}

/* Size in bytes and modification time (ns since 1970). On failure: 0, and zevic_fs_failed() is true. */
static int fs_stat_path(const char *p, int64_t n, int64_t *size, int64_t *mtime_ns) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
#ifdef _WIN32
    struct _stat64 st;
    wchar_t *wp = rt_wide(path);
    int rc = wp ? _wstat64(wp, &st) : -1;
    free(wp);
    if (rc == 0) *mtime_ns = (int64_t)st.st_mtime * 1000000000LL;
#else
    struct stat st;
    int rc = stat(path, &st);
    if (rc == 0) {
#ifdef __APPLE__
        *mtime_ns = (int64_t)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
        *mtime_ns = (int64_t)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
    }
#endif
    free(path);
    if (rc != 0) {
        g_fs_errno = errno ? errno : EIO;
        return 0;
    }
    *size = (int64_t)st.st_size;
    return 1;
}

int64_t zevic_fs_size(const char *p, int64_t n) {
    int64_t size = 0, mt = 0;
    return fs_stat_path(p, n, &size, &mt) ? size : 0;
}

int64_t zevic_fs_mtime_ns(const char *p, int64_t n) {
    int64_t size = 0, mt = 0;
    return fs_stat_path(p, n, &size, &mt) ? mt : 0;
}

/* Copy the bytes of a file (the permission bits too, where there are any). 1 on success. */
int32_t zevic_fs_copy(const char *a, int64_t an, const char *b, int64_t bn) {
    char *from = nul_copy(a, an);
    char *to = nul_copy(b, bn);
    g_fs_errno = 0;
    int ok = 0;
    FILE *in = fs_fopen(from, "rb");
    if (!in) {
        g_fs_errno = errno;
    } else {
        FILE *out = fs_fopen(to, "wb");
        if (!out) {
            g_fs_errno = errno;
        } else {
            char buf[65536];
            size_t got;
            ok = 1;
            while ((got = fread(buf, 1, sizeof buf, in)) > 0) {
                if (fwrite(buf, 1, got, out) != got) {
                    g_fs_errno = errno ? errno : EIO;
                    ok = 0;
                    break;
                }
            }
            if (ferror(in) && ok) {
                g_fs_errno = errno ? errno : EIO;
                ok = 0;
            }
            if (fclose(out) != 0 && ok) {
                g_fs_errno = errno ? errno : EIO;
                ok = 0;
            }
        }
        fclose(in);
    }
#ifndef _WIN32
    if (ok) {
        struct stat st;
        if (stat(from, &st) == 0) (void)chmod(to, st.st_mode & 07777);
    }
#endif
    free(from);
    free(to);
    return ok;
}

/* The directory for temporary files, without a trailing separator. */
void zevic_fs_temp_dir(zevic_str *out) {
#ifdef _WIN32
    wchar_t w[MAX_PATH + 2];
    DWORD k = GetTempPathW(MAX_PATH + 1, w);
    if (k == 0 || k > MAX_PATH) {
        str_set(out, "C:\\Windows\\Temp", 15);
        return;
    }
    while (k > 3 && (w[k - 1] == L'\\' || w[k - 1] == L'/')) w[--k] = 0;
    char *u = rt_utf8(w);
    int64_t l = (int64_t)strlen(u);
    char *b = zevic_alloc_atomic(l + 1);
    memcpy(b, u, (size_t)l + 1);
    free(u);
    out->p = b;
    out->n = l;
#else
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    int64_t l = (int64_t)strlen(t);
    while (l > 1 && t[l - 1] == '/') l--;
    char *b = zevic_alloc_atomic(l + 1);
    memcpy(b, t, (size_t)l);
    b[l] = 0;
    out->p = b;
    out->n = l;
#endif
}

/* "windows", "macos" or "linux" ("unix" for anything else). */
void zevic_os_name(zevic_str *out) {
#if defined(_WIN32)
    str_set(out, "windows", 7);
#elif defined(__APPLE__)
    str_set(out, "macos", 5);
#elif defined(__linux__)
    str_set(out, "linux", 5);
#else
    str_set(out, "unix", 4);
#endif
}

int32_t zevic_fs_remove(const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
#ifdef _WIN32
    /* the C library's remove() cannot remove a directory on Windows */
    wchar_t *wp = rt_wide(path);
    DWORD attr = wp ? GetFileAttributesW(wp) : INVALID_FILE_ATTRIBUTES;
    int rc = !wp ? -1 : (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) ? _wrmdir(wp) : _wremove(wp);
    if (rc != 0 && wp && errno == EACCES && attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY) &&
        (attr & FILE_ATTRIBUTE_READONLY)) {
        /* like rm -f: a read-only file (git writes its objects that way) is made writable first */
        SetFileAttributesW(wp, FILE_ATTRIBUTE_NORMAL);
        rc = _wremove(wp);
    }
    free(wp);
#else
    int rc = remove(path);
#endif
    free(path);
    if (rc != 0) {
        g_fs_errno = errno;
        return 0;
    }
    return 1;
}

int32_t zevic_fs_rename(const char *a, int64_t an, const char *b, int64_t bn) {
    char *from = nul_copy(a, an);
    char *to = nul_copy(b, bn);
    g_fs_errno = 0;
#ifdef _WIN32
    /* rename() refuses to replace an existing file on Windows; POSIX replaces it */
    wchar_t *wf = rt_wide(from), *wt = rt_wide(to);
    int rc = wf && wt && MoveFileExW(wf, wt, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
    free(wf);
    free(wt);
    if (rc != 0) errno = (GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND) ? ENOENT : EACCES;
#else
    int rc = rename(from, to);
#endif
    free(from);
    free(to);
    if (rc != 0) {
        g_fs_errno = errno;
        return 0;
    }
    return 1;
}

int32_t zevic_fs_mkdir(const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
#ifdef _WIN32
    wchar_t *wp = rt_wide(path);
    int rc = wp ? _wmkdir(wp) : -1;
    free(wp);
#else
    int rc = mkdir(path, 0777);
#endif
    free(path);
    if (rc != 0) {
        g_fs_errno = errno;
        return 0;
    }
    return 1;
}

/* The names in a directory (not "." and ".."), one per line. */
void zevic_fs_list(zevic_str *out, const char *p, int64_t n) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    int64_t cap = 256, len = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
#ifdef _WIN32
    char *pat = malloc((size_t)n + 3);
    memcpy(pat, path, (size_t)n);
    memcpy(pat + n, "\\*", 3);
    wchar_t *wpat = rt_wide(pat);
    WIN32_FIND_DATAW fd;
    HANDLE h = wpat ? FindFirstFileW(wpat, &fd) : INVALID_HANDLE_VALUE;
    free(pat);
    free(wpat);
    if (h == INVALID_HANDLE_VALUE) {
        g_fs_errno = ENOENT;
    } else {
        do {
            char *name = rt_utf8(fd.cFileName);
            if (!strcmp(name, ".") || !strcmp(name, "..")) {
                free(name);
                continue;
            }
            int64_t l = (int64_t)strlen(name);
            while (len + l + 2 > cap) { cap *= 2; buf = realloc(buf, (size_t)cap); if (!buf) exit(101); }
            memcpy(buf + len, name, (size_t)l);
            len += l;
            buf[len++] = '\n';
            free(name);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
#else
    DIR *d = opendir(path);
    if (!d) {
        g_fs_errno = errno;
    } else {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            const char *name = e->d_name;
            if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
            int64_t l = (int64_t)strlen(name);
            while (len + l + 2 > cap) {
                cap *= 2;
                buf = realloc(buf, (size_t)cap);
                if (!buf) exit(101);
            }
            memcpy(buf + len, name, (size_t)l);
            len += l;
            buf[len++] = '\n';
        }
        closedir(d);
    }
#endif
    free(path);
    str_set(out, buf, len);
    free(buf);
}

/* ---- open files: mode 0 read, 1 write (replace), 2 append ---- */

void *zevic_fs_open(const char *p, int64_t n, int32_t mode) {
    char *path = nul_copy(p, n);
    g_fs_errno = 0;
    FILE *f = fs_fopen(path, mode == 0 ? "rb" : mode == 1 ? "wb" : "ab");
    free(path);
    if (!f) g_fs_errno = errno;
    return f;
}

int32_t zevic_fs_close(void *h) {
    g_fs_errno = 0;
    if (fclose((FILE *)h) != 0) {
        g_fs_errno = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

int32_t zevic_fs_file_write(void *h, const char *d, int64_t dn) {
    g_fs_errno = 0;
    if (dn > 0 && fwrite(d, 1, (size_t)dn, (FILE *)h) != (size_t)dn) {
        g_fs_errno = errno ? errno : EIO;
        return 0;
    }
    return 1;
}

static _Thread_local int32_t g_file_line_ok;

int32_t zevic_fs_file_line_ok(void) {
    return g_file_line_ok;
}

/* One line (without \n or \r\n) from an open file; zevic_fs_file_line_ok() tells whether there was one. */
void zevic_fs_file_read_line(zevic_str *out, void *h) {
    FILE *f = h;
    g_file_line_ok = 0;
    int64_t cap = 128, n = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    int c, got = 0;
    while ((c = fgetc(f)) != EOF) {
        got = 1;
        if (c == '\n') break;
        if (n + 1 >= cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
        buf[n++] = (char)c;
    }
    if (got) {
        if (n > 0 && buf[n - 1] == '\r') n--;
        g_file_line_ok = 1;
    }
    str_set(out, buf, n);
    free(buf);
}

void zevic_fs_file_read_all(zevic_str *out, void *h) {
    FILE *f = h;
    int64_t cap = 4096, len = 0;
    char *buf = malloc((size_t)cap);
    if (!buf) exit(101);
    size_t r;
    while ((r = fread(buf + len, 1, (size_t)(cap - len), f)) > 0) {
        len += (int64_t)r;
        if (len == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    str_set(out, buf, len);
    free(buf);
}

/* ---- processes ----
 * A command is built with zevic_proc_begin / zevic_proc_arg, then run through the shell with stdout and stderr
 * redirected to temporary files. Every argument is quoted, so the shell does not interpret it. */


static _Thread_local char *g_proc_cmd;
static _Thread_local int64_t g_proc_len, g_proc_cap;
static _Thread_local int32_t g_proc_code;
static _Thread_local int32_t g_proc_failed;
static _Thread_local char *g_proc_err;
static _Thread_local int64_t g_proc_errlen;

static void proc_put(const char *p, int64_t n) {
    if (g_proc_len + n + 1 > g_proc_cap) {
        g_proc_cap = g_proc_cap ? g_proc_cap * 2 : 256;
        while (g_proc_cap < g_proc_len + n + 1) g_proc_cap *= 2;
        g_proc_cmd = realloc(g_proc_cmd, (size_t)g_proc_cap);
        if (!g_proc_cmd) exit(101);
    }
    memcpy(g_proc_cmd + g_proc_len, p, (size_t)n);
    g_proc_len += n;
    g_proc_cmd[g_proc_len] = 0;
}

void zevic_proc_begin(void) {
    g_proc_len = 0;
    proc_put("", 0);
}

void zevic_proc_arg(const char *p, int64_t n) {
    if (g_proc_len > 0) proc_put(" ", 1);
#ifdef _WIN32
    /* the rules of the C runtime's command line parser: backslashes only matter in front of a quote */
    proc_put("\"", 1);
    int64_t backslashes = 0;
    for (int64_t i = 0; i < n; i++) {
        if (p[i] == '\\') {
            backslashes++;
            continue;
        }
        if (p[i] == '"') {
            for (int64_t k = 0; k < backslashes; k++) proc_put("\\", 1); /* each one twice, then the quote's own */
        }
        for (int64_t k = 0; k < backslashes; k++) proc_put("\\", 1);
        if (p[i] == '"') proc_put("\\", 1);
        backslashes = 0;
        proc_put(p + i, 1);
    }
    for (int64_t k = 0; k < 2 * backslashes; k++) proc_put("\\", 1); /* in front of the closing quote */
    proc_put("\"", 1);
#else
    proc_put("'", 1);
    for (int64_t i = 0; i < n; i++) {
        if (p[i] == '\'') proc_put("'\\''", 4);
        else proc_put(p + i, 1);
    }
    proc_put("'", 1);
#endif
}

static char *temp_path(void) {
#ifdef _WIN32
    char dir[MAX_PATH], name[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, dir) || !GetTempFileNameA(dir, "zev", 0, name)) return NULL;
    return strdup(name);
#else
    char tmpl[] = "/tmp/zevic-proc-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return NULL;
    close(fd);
    return strdup(tmpl);
#endif
}

static char *slurp(const char *path, int64_t *len) {
    FILE *f = fopen(path, "rb");
    *len = 0;
    char *buf = malloc(1);
    if (!buf) exit(101);
    if (!f) return buf;
    int64_t cap = 1024;
    buf = realloc(buf, (size_t)cap);
    size_t r;
    while ((r = fread(buf + *len, 1, (size_t)(cap - *len), f)) > 0) {
        *len += (int64_t)r;
        if (*len == cap) {
            cap *= 2;
            buf = realloc(buf, (size_t)cap);
            if (!buf) exit(101);
        }
    }
    fclose(f);
    return buf;
}

#ifdef _WIN32
/* Starts the command line (without cmd.exe, whose quoting rules are a trap) and waits for it. `in`, `out` and `err`
 * are the child's standard handles. Returns the exit code, 127 when the program does not exist (what a POSIX shell
 * says), or -1 when it cannot be started for another reason. */
static int win_spawn(const char *cmdline, HANDLE in, HANDLE out, HANDLE err) {
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si);
    memset(&pi, 0, sizeof pi);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in;
    si.hStdOutput = out;
    si.hStdError = err;
    wchar_t *line = rt_wide(cmdline); /* CreateProcess may modify it */
    if (!line) return -1;
    BOOL ok = CreateProcessW(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    DWORD last = GetLastError();
    free(line);
    if (!ok) return (last == ERROR_FILE_NOT_FOUND || last == ERROR_PATH_NOT_FOUND) ? 127 : -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

static HANDLE win_open_inheritable(const char *path, DWORD access, DWORD disposition) {
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    return CreateFileA(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, disposition, FILE_ATTRIBUTE_NORMAL, NULL);
}

static HANDLE win_std_inheritable(DWORD which) {
    HANDLE h = GetStdHandle(which);
    if (h && h != INVALID_HANDLE_VALUE) SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    return h;
}
#endif

/* Runs the command; its standard output is the result. Afterwards zevic_proc_failed() says whether it could not
 * be run at all, zevic_proc_code() gives the exit code and zevic_proc_stderr() what it wrote to stderr. */
void zevic_proc_run(zevic_str *out) {
    fflush(stdout);
    fflush(stderr);
    g_proc_failed = 0;
    g_proc_code = -1;
    g_proc_errlen = 0;
    free(g_proc_err);
    g_proc_err = NULL;
    char *fo = temp_path(), *fe = temp_path();
    if (!fo || !fe) {
        g_proc_failed = 1;
        str_set(out, "", 0);
        return;
    }
    int64_t base = g_proc_len;
#ifdef _WIN32
    HANDLE ho = win_open_inheritable(fo, GENERIC_WRITE, CREATE_ALWAYS);
    HANDLE he = win_open_inheritable(fe, GENERIC_WRITE, CREATE_ALWAYS);
    if (ho == INVALID_HANDLE_VALUE || he == INVALID_HANDLE_VALUE) {
        if (ho != INVALID_HANDLE_VALUE) CloseHandle(ho);
        if (he != INVALID_HANDLE_VALUE) CloseHandle(he);
        g_proc_failed = 1;
        str_set(out, "", 0);
        return;
    }
    int rc = win_spawn(g_proc_cmd, win_std_inheritable(STD_INPUT_HANDLE), ho, he);
    CloseHandle(ho);
    CloseHandle(he);
    if (rc == 127) {
        /* what a POSIX shell does */
        FILE *ef = fopen(fe, "wb");
        if (ef) {
            fputs("program not found\n", ef);
            fclose(ef);
        }
    }
    if (rc < 0) g_proc_failed = 1;
    else g_proc_code = rc;
#else
    proc_put(" >", 2);
    proc_put(fo, (int64_t)strlen(fo));
    proc_put(" 2>", 3);
    proc_put(fe, (int64_t)strlen(fe));
    int rc = system(g_proc_cmd);
    if (rc == -1) g_proc_failed = 1;
    else if (WIFEXITED(rc)) g_proc_code = WEXITSTATUS(rc);
    else g_proc_code = 128 + (WIFSIGNALED(rc) ? WTERMSIG(rc) : 0);
#endif
    g_proc_len = base;
    g_proc_cmd[base] = 0;
    int64_t n;
    char *o = slurp(fo, &n);
    str_set(out, o, n);
    free(o);
    g_proc_err = slurp(fe, &g_proc_errlen);
    remove(fo);
    remove(fe);
    free(fo);
    free(fe);
}

/* Runs the command with this program's own standard input, output and error. Afterwards zevic_proc_failed()
 * and zevic_proc_code() tell what happened. */
void zevic_proc_inherit(void) {
    fflush(stdout);
    fflush(stderr);
    g_proc_failed = 0;
    g_proc_code = -1;
#ifdef _WIN32
    int rc = win_spawn(g_proc_cmd, win_std_inheritable(STD_INPUT_HANDLE), win_std_inheritable(STD_OUTPUT_HANDLE),
                       win_std_inheritable(STD_ERROR_HANDLE));
    if (rc < 0) g_proc_failed = 1;
    else g_proc_code = rc;
#else
    int rc = system(g_proc_cmd);
    if (rc == -1) g_proc_failed = 1;
    else if (WIFEXITED(rc)) g_proc_code = WEXITSTATUS(rc);
    else g_proc_code = 128 + (WIFSIGNALED(rc) ? WTERMSIG(rc) : 0);
#endif
}

int32_t zevic_proc_failed(void) {
    return g_proc_failed;
}

int32_t zevic_proc_code(void) {
    return g_proc_code;
}

void zevic_proc_stderr(zevic_str *out) {
    str_set(out, g_proc_err ? g_proc_err : "", g_proc_err ? g_proc_errlen : 0);
}

/* ---- tests ---- */

typedef struct {
    const char *name;
    int64_t len;
    void (*fn)(void);
} test_case;

static test_case *g_tests;
static int g_ntests, g_captests;

void zevic_test_add(const char *name, int64_t len, void (*fn)(void)) {
    if (g_ntests == g_captests) {
        g_captests = g_captests ? g_captests * 2 : 16;
        g_tests = realloc(g_tests, sizeof(test_case) * (size_t)g_captests);
        if (!g_tests) exit(101);
    }
    g_tests[g_ntests].name = name;
    g_tests[g_ntests].len = len;
    g_tests[g_ntests].fn = fn;
    g_ntests++;
}

/* Called by the compiler before every call into the module `Zevic.Test`: the line of the caller. */
void zevic_test_at(const char *file, int32_t line) {
    g_at_file = file;
    g_at_line = (int)line;
}

/* A failed assertion: ends the running test. Outside the test runner it ends the program like a panic. */
void zevic_test_fail(const char *msg, int64_t len) {
    if (g_jmp) {
        snprintf(g_fail, sizeof g_fail, "%s:%d: %.*s", base_name(g_at_file), g_at_line, (int)len, msg);
        g_jmp_kind = 2;
        longjmp(*g_jmp, 1);
    }
    zevic_panic(msg, len, g_at_file, (int32_t)g_at_line);
}

/* Runs f and reports whether it panicked (1) or returned (0). A failed assertion inside f is not a panic:
 * it goes on to the test runner. `f` is a closure: the code pointer and its environment. */
int32_t zevic_catch(void (*fn)(void *), void *env) {
    jmp_buf here;
    jmp_buf *outer = g_jmp;
    const char *file = g_at_file;
    int line = g_at_line;
    g_jmp = &here;
    if (setjmp(here) == 0) {
        fn(env);
        g_jmp = outer;
        g_at_file = file;
        g_at_line = line;
        return 0;
    }
    g_jmp = outer;
    g_at_file = file;
    g_at_line = line;
    if (g_jmp_kind == 2 && outer) longjmp(*outer, 1);
    return 1;
}

/* Runs the registered tests (the first program argument selects those whose name contains it).
 * The exit code is 1 when a test failed. */
int32_t zevic_test_main(void) {
    const char *filter = g_argc > 1 ? g_argv[1] : NULL;
    int total = 0;
    for (int i = 0; i < g_ntests; i++)
        if (!filter || strstr(g_tests[i].name, filter)) total++;
    printf("running %d test%s\n", total, total == 1 ? "" : "s");
    fflush(stdout);
    int passed = 0, failed = 0;
    for (int i = 0; i < g_ntests; i++) {
        test_case *t = &g_tests[i];
        if (filter && !strstr(t->name, filter)) continue;
        jmp_buf here;
        g_jmp = &here;
        g_jmp_kind = 0;
        int ok = 1;
        if (setjmp(here) == 0) {
            t->fn();
        } else {
            ok = 0;
        }
        g_jmp = NULL;
        fflush(stdout);
        if (ok) {
            printf("  ok    %s\n", t->name);
            passed++;
        } else {
            printf("  FAIL  %s\n        %s\n", t->name, g_fail);
            failed++;
        }
        fflush(stdout);
    }
    printf("\n%d passed, %d failed\n", passed, failed);
    fflush(stdout);
    return failed ? 1 : 0;
}

/* ---------------------------------------------------------------------------------------------
 * Threads, mutexes and condition variables (Zevic.Async). The collector is built with GC_THREADS, so
 * threads started here are known to it. `fn(env)` is a Zevic closure: the code pointer and its environment.
 * ------------------------------------------------------------------------------------------- */
#ifdef _WIN32
typedef struct { HANDLE h; } zevic_thread;
typedef struct { void (*fn)(void *); void *env; } zevic_thread_start_info;

static DWORD WINAPI zevic_thread_main(LPVOID p) {
    zevic_thread_start_info *si = p;
    si->fn(si->env);
    return 0;
}

void zevic_thread_start(void (*fn)(void *), void *env) {
    zevic_thread *t = malloc(sizeof *t);
    zevic_thread_start_info *si = GC_malloc_uncollectable(sizeof *si);
    si->fn = fn;
    si->env = env;
    t->h = CreateThread(NULL, 0, zevic_thread_main, si, 0, NULL);
    if (t->h) CloseHandle(t->h); /* detached: a task reports its end through its own result slot */
    free(t);
}

void *zevic_mutex_new(void) {
    CRITICAL_SECTION *m = malloc(sizeof *m);
    InitializeCriticalSection(m);
    return m;
}
void zevic_mutex_lock(void *m) { EnterCriticalSection(m); }
void zevic_mutex_unlock(void *m) { LeaveCriticalSection(m); }

void *zevic_cond_new(void) {
    CONDITION_VARIABLE *c = malloc(sizeof *c);
    InitializeConditionVariable(c);
    return c;
}
void zevic_cond_wait(void *c, void *m) { SleepConditionVariableCS(c, m, INFINITE); }
void zevic_cond_broadcast(void *c) { WakeAllConditionVariable(c); }
#else
#include <pthread.h>

typedef struct { void (*fn)(void *); void *env; } zevic_thread_start_info;

static void *zevic_thread_main(void *p) {
    zevic_thread_start_info *si = p;
    void (*fn)(void *) = si->fn;
    void *env = si->env;
    fn(env);
    return NULL;
}

void zevic_thread_start(void (*fn)(void *), void *env) {
    pthread_t t;
    zevic_thread_start_info *si = GC_malloc(sizeof *si);
    si->fn = fn;
    si->env = env;
    if (pthread_create(&t, NULL, zevic_thread_main, si) != 0) {
        fprintf(stderr, "panic: cannot start a thread\n");
        exit(101);
    }
    pthread_detach(t); /* a task reports its end through its own result slot */
}

void *zevic_mutex_new(void) {
    pthread_mutex_t *m = malloc(sizeof *m);
    pthread_mutex_init(m, NULL);
    return m;
}
void zevic_mutex_lock(void *m) { pthread_mutex_lock(m); }
void zevic_mutex_unlock(void *m) { pthread_mutex_unlock(m); }

void *zevic_cond_new(void) {
    pthread_cond_t *c = malloc(sizeof *c);
    pthread_cond_init(c, NULL);
    return c;
}
void zevic_cond_wait(void *c, void *m) { pthread_cond_wait(c, m); }
void zevic_cond_broadcast(void *c) { pthread_cond_broadcast(c); }
#endif

/* ---------------------------------------------------------------------------------------------
 * Sockets (Zevic.Http): TCP over POSIX sockets, or Winsock. A socket is an I64. A failing call returns -1 (or 0
 * for zevic_net_write) and leaves the reason for zevic_net_failed() / zevic_net_errmsg().
 * ------------------------------------------------------------------------------------------- */
#ifdef _WIN32
typedef SOCKET zevic_sock;
#define ZEVIC_BAD_SOCK INVALID_SOCKET
#define zevic_sock_close closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
typedef int zevic_sock;
#define ZEVIC_BAD_SOCK (-1)
#define zevic_sock_close close
#endif

static _Thread_local int g_net_failed;
static _Thread_local int g_net_timeout; /* the last failure was a time limit */
static _Thread_local char g_net_msg[256];

static void net_fail(const char *what) {
    g_net_failed = 1;
#ifdef _WIN32
    int e = WSAGetLastError();
    g_net_timeout = e == WSAETIMEDOUT || e == WSAEWOULDBLOCK;
    snprintf(g_net_msg, sizeof g_net_msg, "%s (error %d)", what, e);
#else
    g_net_timeout = errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
    snprintf(g_net_msg, sizeof g_net_msg, "%s: %s", what, strerror(errno));
#endif
}

static void net_init(void) {
#ifdef _WIN32
    static int done;
    if (!done) {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
        done = 1;
    }
#else
    signal(SIGPIPE, SIG_IGN); /* a write to a closed connection is an error, not a signal */
#endif
}

int32_t zevic_net_failed(void) { return g_net_failed; }

/* After a failure: was it a time limit (SetTimeout) that ran out? */
int32_t zevic_net_timed_out(void) { return g_net_failed && g_net_timeout; }

void zevic_net_errmsg(zevic_str *out) { str_set(out, g_net_msg, (int64_t)strlen(g_net_msg)); }

static int net_resolve(const char *host, int64_t port, int passive, struct addrinfo **res) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (passive) hints.ai_flags = AI_PASSIVE;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", (int)port);
    return getaddrinfo(host, ps, &hints, res);
}

static void shut_clear(int64_t fd);

/* Listens on host:port (port 0 picks a free one). */
int64_t zevic_net_listen(const char *hp, int64_t hn, int64_t port) {
    net_init();
    g_net_failed = 0;
    char *host = nul_copy(hp, hn);
    struct addrinfo *ai = NULL;
    int rc = net_resolve(host, port, 1, &ai);
    free(host);
    if (rc != 0 || !ai) {
        g_net_failed = 1;
        g_net_timeout = 0;
        snprintf(g_net_msg, sizeof g_net_msg, "cannot resolve the address");
        return -1;
    }
    zevic_sock s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == ZEVIC_BAD_SOCK) {
        net_fail("cannot create a socket");
        freeaddrinfo(ai);
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    if (bind(s, ai->ai_addr, (int)ai->ai_addrlen) != 0 || listen(s, 128) != 0) {
        net_fail("cannot listen");
        zevic_sock_close(s);
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    shut_clear((int64_t)s); /* a stale mark of an earlier socket with this number */
    return (int64_t)s;
}

int64_t zevic_net_port(int64_t fd) {
    struct sockaddr_storage ss;
    socklen_t len = sizeof ss;
    if (getsockname((zevic_sock)fd, (struct sockaddr *)&ss, &len) != 0) return -1;
    if (ss.ss_family == AF_INET) return ntohs(((struct sockaddr_in *)&ss)->sin_port);
    if (ss.ss_family == AF_INET6) return ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
    return -1;
}

/* Waits for a connection. -1 when the listener was shut down or closed. */
/* The collector interrupts blocked system calls with signals (to stop threads): those are retried. */
#ifdef _WIN32
#define NET_INTR() 0
static int net_connect(zevic_sock s, const struct sockaddr *a, int len) { return connect(s, a, len); }
#else
#define NET_INTR() (errno == EINTR)
static int net_connect(zevic_sock s, const struct sockaddr *a, int len) {
    if (connect(s, a, len) == 0) return 0;
    if (errno != EINTR) return -1;
    /* the connection goes on in the background: wait until it is done */
    struct pollfd p = {s, POLLOUT, 0};
    int r;
    do {
        r = poll(&p, 1, -1);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    int err = 0;
    socklen_t el = sizeof err;
    if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err != 0) {
        errno = err;
        return -1;
    }
    return 0;
}
#endif

/* Sockets that Close asked to stop. shutdown() wakes a blocked accept() on Linux only (macOS answers ENOTCONN for a
 * listening socket, Windows does too), so accept waits in short slices and looks at this list in between. */
#define ZEVIC_SHUT_MAX 64
static int64_t g_shut[ZEVIC_SHUT_MAX];

static void shut_clear(int64_t fd) {
    for (int i = 0; i < ZEVIC_SHUT_MAX; i++) {
        if (__atomic_load_n(&g_shut[i], __ATOMIC_ACQUIRE) == fd + 1) __atomic_store_n(&g_shut[i], 0, __ATOMIC_RELEASE);
    }
}

static void shut_mark(int64_t fd) {
    for (int i = 0; i < ZEVIC_SHUT_MAX; i++) {
        int64_t expect = 0;
        if (__atomic_compare_exchange_n(&g_shut[i], &expect, fd + 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    }
}

static int shut_marked(int64_t fd) {
    for (int i = 0; i < ZEVIC_SHUT_MAX; i++)
        if (__atomic_load_n(&g_shut[i], __ATOMIC_ACQUIRE) == fd + 1) return 1;
    return 0;
}

/* 1: a connection is waiting, 0: the time slice ran out, -1: error */
static int net_wait_readable(zevic_sock s, int ms) {
#ifdef _WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    struct timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = select(0, &set, NULL, NULL, &tv);
    return r < 0 ? -1 : r > 0;
#else
    struct pollfd p = {s, POLLIN, 0};
    int r = poll(&p, 1, ms);
    if (r < 0) return errno == EINTR ? 0 : -1;
    return r > 0;
#endif
}

int64_t zevic_net_accept(int64_t fd) {
    g_net_failed = 0;
    zevic_sock c = ZEVIC_BAD_SOCK;
    for (;;) {
        if (shut_marked(fd)) {
            net_fail("cannot accept: the socket was shut down");
            return -1;
        }
        int w = net_wait_readable((zevic_sock)fd, 50);
        if (w < 0) {
            net_fail("cannot accept");
            return -1;
        }
        if (w == 0) continue;
        c = accept((zevic_sock)fd, NULL, NULL);
        if (c != ZEVIC_BAD_SOCK) break;
        if (!NET_INTR() && shut_marked(fd)) {
            net_fail("cannot accept: the socket was shut down");
            return -1;
        }
        if (!NET_INTR()) {
            net_fail("cannot accept");
            return -1;
        }
    }
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return (int64_t)c;
}

/* Receive and send time limits for one socket, in milliseconds (0: none). */
void zevic_net_timeout(int64_t fd, int64_t ms) {
#ifdef _WIN32
    DWORD t = (DWORD)ms;
    setsockopt((zevic_sock)fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt((zevic_sock)fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval tv;
    tv.tv_sec = (time_t)(ms / 1000);
    tv.tv_usec = (suseconds_t)((ms % 1000) * 1000);
    setsockopt((zevic_sock)fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt((zevic_sock)fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
}

int64_t zevic_net_connect(const char *hp, int64_t hn, int64_t port, int64_t timeout_ms) {
    net_init();
    g_net_failed = 0;
    char *host = nul_copy(hp, hn);
    struct addrinfo *ai = NULL;
    int rc = net_resolve(host, port, 0, &ai);
    if (rc != 0 || !ai) {
        g_net_failed = 1;
        g_net_timeout = 0;
        snprintf(g_net_msg, sizeof g_net_msg, "cannot find the host \"%.200s\"", host);
        free(host);
        return -1;
    }
    free(host);
    zevic_sock s = ZEVIC_BAD_SOCK;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == ZEVIC_BAD_SOCK) continue;
        zevic_net_timeout((int64_t)s, timeout_ms);
        if (net_connect(s, a->ai_addr, (int)a->ai_addrlen) == 0) break;
        zevic_sock_close(s);
        s = ZEVIC_BAD_SOCK;
    }
    freeaddrinfo(ai);
    if (s == ZEVIC_BAD_SOCK) {
        net_fail("cannot connect");
        return -1;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    return (int64_t)s;
}

/* Reads up to `max` bytes; the result is empty at the end of the stream or on an error (check zevic_net_failed). */
void zevic_net_read(zevic_str *out, int64_t fd, int64_t max) {
    g_net_failed = 0;
    if (max > 1 << 20) max = 1 << 20;
    if (max < 1) max = 1;
    char *buf = malloc((size_t)max);
    if (!buf) exit(101);
    int64_t n;
    do {
        n = (int64_t)recv((zevic_sock)fd, buf, (int)max, 0);
    } while (n < 0 && NET_INTR());
    if (n < 0) {
        net_fail("cannot read");
        n = 0;
    }
    str_set(out, buf, n);
    free(buf);
}

/* Writes everything. 1 on success, 0 on failure. */
int32_t zevic_net_write(int64_t fd, const char *d, int64_t dn) {
    g_net_failed = 0;
    int64_t sent = 0;
    while (sent < dn) {
        int64_t n;
        do {
            n = (int64_t)send((zevic_sock)fd, d + sent, (int)(dn - sent > (1 << 20) ? (1 << 20) : dn - sent), 0);
        } while (n < 0 && NET_INTR());
        if (n <= 0) {
            net_fail("cannot write");
            return 0;
        }
        sent += n;
    }
    return 1;
}

/* Wakes up threads blocked on this socket (an accept in another thread returns -1). */
void zevic_net_shutdown(int64_t fd) {
    shut_mark(fd);
#ifdef _WIN32
    shutdown((zevic_sock)fd, SD_BOTH);
#else
    shutdown((zevic_sock)fd, SHUT_RDWR);
#endif
}

void zevic_net_close(int64_t fd) {
    zevic_sock_close((zevic_sock)fd);
    shut_clear(fd);
}

/* ---- UDP and name lookup (Zevic.Net) ---- */

static _Thread_local char g_peer_host[128];
static _Thread_local int64_t g_peer_port;

/* A UDP socket bound to host:port (host "" is every address; port 0 picks a free one). */
int64_t zevic_udp_bind(const char *hp, int64_t hn, int64_t port) {
    net_init();
    g_net_failed = 0;
    g_net_timeout = 0;
    char *host = nul_copy(hp, hn);
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", (int)port);
    int rc = getaddrinfo(host[0] ? host : NULL, ps, &hints, &ai);
    free(host);
    if (rc != 0 || !ai) {
        g_net_failed = 1;
        snprintf(g_net_msg, sizeof g_net_msg, "cannot resolve the address");
        return -1;
    }
    zevic_sock s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (s == ZEVIC_BAD_SOCK) {
        net_fail("cannot create a socket");
        freeaddrinfo(ai);
        return -1;
    }
    if (bind(s, ai->ai_addr, (int)ai->ai_addrlen) != 0) {
        net_fail("cannot bind");
        zevic_sock_close(s);
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    return (int64_t)s;
}

/* Sends one datagram to host:port from this socket. 1 on success. */
int32_t zevic_udp_send_to(int64_t fd, const char *hp, int64_t hn, int64_t port, const char *d, int64_t dn) {
    g_net_failed = 0;
    g_net_timeout = 0;
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    if (getsockname((zevic_sock)fd, (struct sockaddr *)&ss, &sl) != 0) {
        net_fail("cannot send");
        return 0;
    }
    char *host = nul_copy(hp, hn);
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = ss.ss_family;
    hints.ai_socktype = SOCK_DGRAM;
    char ps[16];
    snprintf(ps, sizeof ps, "%d", (int)port);
    int rc = getaddrinfo(host, ps, &hints, &ai);
    if (rc != 0 || !ai) {
        g_net_failed = 1;
        snprintf(g_net_msg, sizeof g_net_msg, "cannot find the host \"%.200s\"", host);
        free(host);
        return 0;
    }
    free(host);
    int64_t n;
    do {
        n = (int64_t)sendto((zevic_sock)fd, d, (int)dn, 0, ai->ai_addr, (int)ai->ai_addrlen);
    } while (n < 0 && NET_INTR());
    freeaddrinfo(ai);
    if (n != dn) {
        net_fail("cannot send");
        return 0;
    }
    return 1;
}

/* Receives one datagram (at most `max` bytes; more is cut off). On failure the result is empty and
 * zevic_net_failed() is true; an empty datagram is a success. The sender is in zevic_udp_peer_*. */
void zevic_udp_recv(zevic_str *out, int64_t fd, int64_t max) {
    g_net_failed = 0;
    g_net_timeout = 0;
    if (max > 65535) max = 65535;
    if (max < 1) max = 1;
    char *buf = malloc((size_t)max);
    if (!buf) exit(101);
    struct sockaddr_storage ss;
    socklen_t sl;
    int64_t n;
    do {
        sl = sizeof ss;
        n = (int64_t)recvfrom((zevic_sock)fd, buf, (int)max, 0, (struct sockaddr *)&ss, &sl);
    } while (n < 0 && NET_INTR());
    if (n < 0) {
        net_fail("cannot receive");
        n = 0;
        g_peer_host[0] = 0;
        g_peer_port = 0;
    } else {
        char serv[16];
        if (getnameinfo((struct sockaddr *)&ss, sl, g_peer_host, sizeof g_peer_host, serv, sizeof serv,
                        NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
            g_peer_host[0] = 0;
            g_peer_port = 0;
        } else {
            g_peer_port = atoi(serv);
        }
    }
    str_set(out, buf, n);
    free(buf);
}

void zevic_udp_peer_host(zevic_str *out) { str_set(out, g_peer_host, (int64_t)strlen(g_peer_host)); }

int64_t zevic_udp_peer_port(void) { return g_peer_port; }

/* The addresses of a host name, as numbers, one per line (no repeats). */
void zevic_net_resolve(zevic_str *out, const char *hp, int64_t hn) {
    net_init();
    g_net_failed = 0;
    g_net_timeout = 0;
    char *host = nul_copy(hp, hn);
    struct addrinfo hints, *ai = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &ai);
    if (rc != 0 || !ai) {
        g_net_failed = 1;
        snprintf(g_net_msg, sizeof g_net_msg, "cannot find the host \"%.200s\"", host);
        free(host);
        str_set(out, "", 0);
        return;
    }
    free(host);
    size_t cap = 1024, len = 0;
    char *res = malloc(cap);
    if (!res) exit(101);
    res[0] = 0;
    for (struct addrinfo *a = ai; a; a = a->ai_next) {
        char h[128];
        if (getnameinfo(a->ai_addr, (socklen_t)a->ai_addrlen, h, sizeof h, NULL, 0, NI_NUMERICHOST) != 0) continue;
        size_t hl = strlen(h);
        /* skip an address already in the list */
        int dup = 0;
        for (const char *q = res; *q;) {
            const char *e = strchr(q, '\n');
            size_t ql = e ? (size_t)(e - q) : strlen(q);
            if (ql == hl && memcmp(q, h, hl) == 0) dup = 1;
            q = e ? e + 1 : q + ql;
        }
        if (dup) continue;
        if (len + hl + 2 > cap) {
            cap = (len + hl + 2) * 2;
            res = realloc(res, cap);
            if (!res) exit(101);
        }
        if (len) res[len++] = '\n';
        memcpy(res + len, h, hl);
        len += hl;
        res[len] = 0;
    }
    freeaddrinfo(ai);
    str_set(out, res, (int64_t)len);
    free(res);
}

/* ---- Deflate / Inflate (RFC 1951) ---------------------------------------
 * Inflate is a full decoder (stored, fixed and dynamic blocks). Deflate finds
 * matches with hash chains and writes fixed-Huffman blocks, or stored blocks
 * when that is smaller. The status of the last call is read with
 * zevic_flate_status(): 0 ok, 1 corrupt or cut-off data, 2 over the limit. */

static int32_t g_flate_status = 0;
static int64_t g_flate_consumed = 0;

int32_t zevic_flate_status(void) { return g_flate_status; }
int64_t zevic_flate_consumed(void) { return g_flate_consumed; }

typedef struct {
    uint8_t *b;
    int64_t n, cap;
} flate_buf;

static int flate_put(flate_buf *o, const void *p, int64_t n) {
    if (o->n + n > o->cap) {
        int64_t c = o->cap ? o->cap : 4096;
        while (c < o->n + n) c *= 2;
        uint8_t *nb = realloc(o->b, (size_t)c);
        if (!nb) return 0;
        o->b = nb;
        o->cap = c;
    }
    if (n) memcpy(o->b + o->n, p, (size_t)n);
    o->n += n;
    return 1;
}

static void flate_finish(zevic_str *out, flate_buf *o) {
    char *r = zevic_alloc_atomic(o->n + 1);
    if (o->n) memcpy(r, o->b, (size_t)o->n);
    r[o->n] = 0;
    out->p = r;
    out->n = o->n;
    free(o->b);
}

typedef struct {
    const uint8_t *p;
    int64_t n, pos;
    uint32_t bits;
    int cnt;
} flate_in;

static int flate_getbits(flate_in *s, int need, uint32_t *v) {
    while (s->cnt < need) {
        if (s->pos >= s->n) return 0;
        s->bits |= (uint32_t)s->p[s->pos++] << s->cnt;
        s->cnt += 8;
    }
    *v = s->bits & ((1u << need) - 1);
    s->bits >>= need;
    s->cnt -= need;
    return 1;
}

typedef struct {
    uint16_t count[16];
    uint16_t sym[288];
} flate_huff;

static int flate_build(flate_huff *h, const uint8_t *len, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[len[i]]++;
    if (h->count[0] == n) return 1;
    int left = 1;
    for (int l = 1; l < 16; l++) {
        left <<= 1;
        left -= h->count[l];
        if (left < 0) return 0;
    }
    uint16_t offs[16];
    offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++)
        if (len[i]) h->sym[offs[len[i]]++] = (uint16_t)i;
    return 1;
}

static int flate_decode(flate_in *s, const flate_huff *h) {
    int code = 0, first = 0, index = 0;
    for (int l = 1; l < 16; l++) {
        uint32_t b;
        if (!flate_getbits(s, 1, &b)) return -1;
        code |= (int)b;
        int c = h->count[l];
        if (code - c < first) return h->sym[index + (code - first)];
        index += c;
        first += c;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const uint16_t k_len_base[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t k_len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t k_dist_base[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t k_dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/* returns 0 ok, 1 corrupt, 2 over the limit */
static int flate_codes(flate_in *s, flate_buf *o, const flate_huff *lit, const flate_huff *dist, int64_t limit) {
    for (;;) {
        int sym = flate_decode(s, lit);
        if (sym < 0) return 1;
        if (sym < 256) {
            if (o->n + 1 > limit) return 2;
            uint8_t c = (uint8_t)sym;
            if (!flate_put(o, &c, 1)) return 2;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return 1;
            uint32_t e;
            if (!flate_getbits(s, k_len_extra[sym], &e)) return 1;
            int64_t len = k_len_base[sym] + (int64_t)e;
            int ds = flate_decode(s, dist);
            if (ds < 0 || ds >= 30) return 1;
            if (!flate_getbits(s, k_dist_extra[ds], &e)) return 1;
            int64_t d = k_dist_base[ds] + (int64_t)e;
            if (d > o->n) return 1;
            if (o->n + len > limit) return 2;
            if (!flate_put(o, NULL, 0)) return 2;
            for (int64_t i = 0; i < len; i++) {
                uint8_t c = o->b[o->n - d];
                if (!flate_put(o, &c, 1)) return 2;
            }
        }
    }
}

void zevic_inflate(zevic_str *out, const uint8_t *p, int64_t n, int64_t limit) {
    flate_in s = {p, n, 0, 0, 0};
    flate_buf o = {0, 0, 0};
    int st = 0;
    uint32_t last = 0;
    do {
        uint32_t type;
        if (!flate_getbits(&s, 1, &last) || !flate_getbits(&s, 2, &type)) { st = 1; break; }
        if (type == 0) {
            s.bits = 0;
            s.cnt = 0;
            if (s.pos + 4 > s.n) { st = 1; break; }
            uint32_t len = s.p[s.pos] | (uint32_t)s.p[s.pos + 1] << 8;
            uint32_t nlen = s.p[s.pos + 2] | (uint32_t)s.p[s.pos + 3] << 8;
            s.pos += 4;
            if ((len ^ 0xffff) != nlen || s.pos + len > s.n) { st = 1; break; }
            if (o.n + len > limit) { st = 2; break; }
            if (!flate_put(&o, s.p + s.pos, len)) { st = 2; break; }
            s.pos += len;
        } else if (type == 1) {
            uint8_t l[288];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            flate_huff lit, dist;
            flate_build(&lit, l, 288);
            uint8_t dl[30];
            for (i = 0; i < 30; i++) dl[i] = 5;
            flate_build(&dist, dl, 30);
            st = flate_codes(&s, &o, &lit, &dist, limit);
            if (st) break;
        } else if (type == 2) {
            uint32_t nlen, ndist, ncode;
            if (!flate_getbits(&s, 5, &nlen) || !flate_getbits(&s, 5, &ndist) || !flate_getbits(&s, 4, &ncode)) { st = 1; break; }
            nlen += 257;
            ndist += 1;
            ncode += 4;
            if (nlen > 286 || ndist > 30) { st = 1; break; }
            static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lens[320];
            memset(lens, 0, sizeof lens);
            int bad = 0;
            for (uint32_t i = 0; i < ncode; i++) {
                uint32_t v;
                if (!flate_getbits(&s, 3, &v)) { bad = 1; break; }
                lens[order[i]] = (uint8_t)v;
            }
            flate_huff cl;
            if (bad || !flate_build(&cl, lens, 19)) { st = 1; break; }
            uint8_t all[320];
            memset(all, 0, sizeof all);
            uint32_t idx = 0;
            while (idx < nlen + ndist) {
                int sym = flate_decode(&s, &cl);
                if (sym < 0) { bad = 1; break; }
                if (sym < 16) {
                    all[idx++] = (uint8_t)sym;
                } else {
                    uint32_t rep, v;
                    uint8_t prev = 0;
                    if (sym == 16) {
                        if (idx == 0 || !flate_getbits(&s, 2, &v)) { bad = 1; break; }
                        prev = all[idx - 1];
                        rep = 3 + v;
                    } else if (sym == 17) {
                        if (!flate_getbits(&s, 3, &v)) { bad = 1; break; }
                        rep = 3 + v;
                    } else {
                        if (!flate_getbits(&s, 7, &v)) { bad = 1; break; }
                        rep = 11 + v;
                    }
                    if (idx + rep > nlen + ndist) { bad = 1; break; }
                    while (rep--) all[idx++] = prev;
                }
            }
            flate_huff lit, dist;
            if (bad || all[256] == 0 || !flate_build(&lit, all, (int)nlen) || !flate_build(&dist, all + nlen, (int)ndist)) { st = 1; break; }
            st = flate_codes(&s, &o, &lit, &dist, limit);
            if (st) break;
        } else {
            st = 1;
            break;
        }
    } while (!last);
    g_flate_status = st;
    g_flate_consumed = s.pos;
    flate_finish(out, &o);
}

typedef struct {
    flate_buf *o;
    uint32_t acc;
    int cnt;
} flate_out;

static void flate_wbits(flate_out *w, uint32_t v, int n) {
    w->acc |= v << w->cnt;
    w->cnt += n;
    while (w->cnt >= 8) {
        uint8_t c = (uint8_t)w->acc;
        flate_put(w->o, &c, 1);
        w->acc >>= 8;
        w->cnt -= 8;
    }
}

static uint32_t flate_rev(uint32_t v, int n) {
    uint32_t r = 0;
    for (int i = 0; i < n; i++) {
        r = (r << 1) | (v & 1);
        v >>= 1;
    }
    return r;
}

/* fixed-Huffman literal/length symbol */
static void flate_wsym(flate_out *w, int sym) {
    if (sym < 144) flate_wbits(w, flate_rev(0x30 + sym, 8), 8);
    else if (sym < 256) flate_wbits(w, flate_rev(0x190 + (sym - 144), 9), 9);
    else if (sym < 280) flate_wbits(w, flate_rev(sym - 256, 7), 7);
    else flate_wbits(w, flate_rev(0xc0 + (sym - 280), 8), 8);
}

void zevic_deflate(zevic_str *out, const uint8_t *p, int64_t n, int32_t level) {
    flate_buf o = {0, 0, 0};
    g_flate_status = 0;
    if (level < 0) level = 6;
    if (level > 9) level = 9;
    if (level > 0) {
        flate_out w = {&o, 0, 0};
        flate_wbits(&w, 1, 1);
        flate_wbits(&w, 1, 2);
        enum { HB = 15, HS = 1 << HB };
        int64_t *head = malloc(sizeof(int64_t) * HS);
        int64_t *prev = n ? malloc(sizeof(int64_t) * (size_t)n) : NULL;
        if (!head || (n && !prev)) {
            free(head);
            free(prev);
            g_flate_status = 2;
            flate_finish(out, &o);
            return;
        }
        for (int i = 0; i < HS; i++) head[i] = -1;
        int chain = level * level * 2 + 4;
        int64_t i = 0;
        while (i < n) {
            int64_t best = 0, bestd = 0;
            if (i + 3 <= n) {
                uint32_t h = ((uint32_t)p[i] << 10 ^ (uint32_t)p[i + 1] << 5 ^ p[i + 2]) & (HS - 1);
                int64_t c = head[h];
                int tries = chain;
                int64_t maxl = n - i < 258 ? n - i : 258;
                while (c >= 0 && i - c <= 32768 && tries--) {
                    int64_t l = 0;
                    while (l < maxl && p[c + l] == p[i + l]) l++;
                    if (l > best) {
                        best = l;
                        bestd = i - c;
                        if (l == maxl) break;
                    }
                    c = prev[c];
                }
            }
            int64_t adv = 1;
            if (best >= 3) {
                int ls = 0;
                while (ls < 28 && k_len_base[ls + 1] <= best) ls++;
                flate_wsym(&w, 257 + ls);
                flate_wbits(&w, (uint32_t)(best - k_len_base[ls]), k_len_extra[ls]);
                int ds = 0;
                while (ds < 29 && k_dist_base[ds + 1] <= bestd) ds++;
                flate_wbits(&w, flate_rev((uint32_t)ds, 5), 5);
                flate_wbits(&w, (uint32_t)(bestd - k_dist_base[ds]), k_dist_extra[ds]);
                adv = best;
            } else {
                flate_wsym(&w, p[i]);
            }
            for (int64_t k = 0; k < adv; k++) {
                if (i + k + 3 <= n) {
                    uint32_t h = ((uint32_t)p[i + k] << 10 ^ (uint32_t)p[i + k + 1] << 5 ^ p[i + k + 2]) & (HS - 1);
                    prev[i + k] = head[h];
                    head[h] = i + k;
                } else if (prev) {
                    prev[i + k] = -1;
                }
            }
            i += adv;
        }
        flate_wsym(&w, 256);
        if (w.cnt) flate_wbits(&w, 0, 8 - w.cnt);
        free(head);
        free(prev);
        if (o.n < n + 5 * (n / 65535 + 1)) {
            flate_finish(out, &o);
            return;
        }
        free(o.b);
        o.b = NULL;
        o.n = o.cap = 0;
    }
    int64_t pos = 0;
    do {
        int64_t len = n - pos > 65535 ? 65535 : n - pos;
        uint8_t hdr[5] = {(uint8_t)(pos + len >= n ? 1 : 0), (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)~len, (uint8_t)(~len >> 8)};
        flate_put(&o, hdr, 5);
        flate_put(&o, p + pos, len);
        pos += len;
    } while (pos < n);
    flate_finish(out, &o);
}
