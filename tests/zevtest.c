/* zevtest.c - the test runner of the repository (C11; builds with clang or gcc on Linux, macOS and Windows).
 *
 *   zevtest golden   [pattern]   compile and run every program of tests/golden
 *   zevtest examples             build and run examples/, run `zevpm test` of the packages in examples/apps
 *   zevtest zevfmt               zevfmt: same meaning, idempotent, repository formatted
 *   zevtest zevpm                zevpm: packages, dependencies (git, path), lock file, tests, libraries
 *   zevtest install              the release layout: only `bin` on PATH, started from another directory
 *   zevtest all                  all of the above
 *
 * Environment: ZEVIC, ZEVPM, ZEVFMT (paths of the executables; default build/zevic[.exe] ...),
 *              ZEVIC_TEST_TIMEOUT (seconds per command, default 180), CC (C compiler for the library test).
 * Exit code: 0 when everything passed, 1 otherwise. */

#ifdef __APPLE__
#define _DARWIN_C_SOURCE
#else
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#endif
#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS

#include <ctype.h>
#include <errno.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <direct.h>
#include <io.h>
#include <process.h>
#include <windows.h>
#define EXE ".exe"
#define IS_WIN 1
#else
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#define EXE ""
#define IS_WIN 0
#endif

/* ------------------------------------------------------------------ strings and memory */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s);
    char *p = xmalloc(n + 1);
    memcpy(p, s, n + 1);
    return p;
}

static char *vfmt(const char *f, va_list ap) {
    va_list c;
    va_copy(c, ap);
    int n = vsnprintf(NULL, 0, f, c);
    va_end(c);
    char *p = xmalloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, f, ap);
    return p;
}

static char *fmt(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    char *p = vfmt(f, ap);
    va_end(ap);
    return p;
}

static bool contains(const char *s, const char *sub) { return strstr(s, sub) != NULL; }

static bool ends_with(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static bool starts_with(const char *s, const char *pre) { return strncmp(s, pre, strlen(pre)) == 0; }

/* the text without CR before LF */
static char *no_cr(const char *s) {
    char *o = xmalloc(strlen(s) + 1), *w = o;
    for (; *s; s++)
        if (!(*s == '\r' && s[1] == '\n')) *w++ = *s;
    *w = 0;
    return o;
}

/* the text without surrounding white space */
static char *strip(const char *s) {
    while (isspace((unsigned char)*s)) s++;
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) n--;
    char *o = xmalloc(n + 1);
    memcpy(o, s, n);
    o[n] = 0;
    return o;
}

typedef struct {
    char **v;
    int n, cap;
} List;

static void push(List *l, char *s) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, (size_t)l->cap * sizeof *l->v);
        if (!l->v) exit(2);
    }
    l->v[l->n++] = s;
}

/* the lines of a text (without the line ending) */
static List lines_of(const char *text) {
    List l = {0};
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        size_t m = n;
        if (m && p[m - 1] == '\r') m--;
        char *s = xmalloc(m + 1);
        memcpy(s, p, m);
        s[m] = 0;
        push(&l, s);
        p += n + (e ? 1 : 0);
    }
    return l;
}

/* white-space separated words */
static List words_of(const char *text) {
    List l = {0};
    const char *p = text;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) p++;
        const char *s = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (p > s) {
            char *w = xmalloc((size_t)(p - s) + 1);
            memcpy(w, s, (size_t)(p - s));
            w[p - s] = 0;
            push(&l, w);
        }
    }
    return l;
}

static char *replace_all(const char *s, const char *from, const char *to) {
    size_t fl = strlen(from), tl = strlen(to), n = 0;
    for (const char *p = s; (p = strstr(p, from)); p += fl) n++;
    char *o = xmalloc(strlen(s) + n * (tl > fl ? tl - fl : 0) + 1), *w = o;
    const char *p = s, *q;
    while ((q = strstr(p, from))) {
        memcpy(w, p, (size_t)(q - p));
        w += q - p;
        memcpy(w, to, tl);
        w += tl;
        p = q + fl;
    }
    strcpy(w, p);
    return o;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* ------------------------------------------------------------------ files */

static char *join(const char *a, const char *b) {
    size_t n = strlen(a);
    if (n == 0) return xstrdup(b);
    if (a[n - 1] == '/' || a[n - 1] == '\\') return fmt("%s%s", a, b);
    return fmt("%s/%s", a, b);
}

static bool is_file(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFREG;
}

static bool is_dir(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static bool exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0;
}

/* The whole file (NUL-terminated), or NULL. */
static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *b = xmalloc(cap);
    size_t r;
    while ((r = fread(b + n, 1, cap - n - 1, f)) > 0) {
        n += r;
        if (n + 1 >= cap) {
            cap *= 2;
            b = realloc(b, cap);
            if (!b) exit(2);
        }
    }
    fclose(f);
    b[n] = 0;
    return b;
}

static int make_dir(const char *p) {
#ifdef _WIN32
    return _mkdir(p);
#else
    return mkdir(p, 0777);
#endif
}

static void mkdirs(const char *path) {
    char *p = xstrdup(path);
    for (char *c = p + 1; *c; c++)
        if ((*c == '/' || *c == '\\') && c[-1] != ':') {
            char s = *c;
            *c = 0;
            if (!is_dir(p)) make_dir(p);
            *c = s;
        }
    if (!is_dir(p)) make_dir(p);
    free(p);
}

static char *dir_of(const char *path) {
    const char *last = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) return xstrdup(".");
    char *o = xmalloc((size_t)(last - path) + 1);
    memcpy(o, path, (size_t)(last - path));
    o[last - path] = 0;
    return o;
}

static bool spit(const char *path, const char *data) {
    char *d = dir_of(path);
    mkdirs(d);
    free(d);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(data, 1, strlen(data), f);
    return fclose(f) == 0;
}

static bool append_file(const char *path, const char *data) {
    FILE *f = fopen(path, "ab");
    if (!f) return false;
    fwrite(data, 1, strlen(data), f);
    return fclose(f) == 0;
}

/* The names in a directory (without . and ..), sorted. */
static List list_dir(const char *path) {
    List l = {0};
#ifdef _WIN32
    char *pat = join(path, "*");
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    free(pat);
    if (h == INVALID_HANDLE_VALUE) return l;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) push(&l, xstrdup(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(path);
    if (!d) return l;
    struct dirent *e;
    while ((e = readdir(d)))
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) push(&l, xstrdup(e->d_name));
    closedir(d);
#endif
    if (l.n) qsort(l.v, (size_t)l.n, sizeof *l.v, cmp_str);
    return l;
}

/* rm -rf; read-only files (git writes its objects that way) are made writable first. */
static void rmrf(const char *path) {
    if (!exists(path)) return;
    if (is_dir(path)) {
        List l = list_dir(path);
        for (int i = 0; i < l.n; i++) {
            char *c = join(path, l.v[i]);
            rmrf(c);
            free(c);
        }
#ifdef _WIN32
        _rmdir(path);
#else
        chmod(path, 0777);
        rmdir(path);
#endif
    } else {
#ifdef _WIN32
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
#else
        chmod(path, 0666);
#endif
        remove(path);
    }
}

static bool copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb");
    if (!a) return false;
    char *d = dir_of(to);
    mkdirs(d);
    free(d);
    FILE *b = fopen(to, "wb");
    if (!b) {
        fclose(a);
        return false;
    }
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, b);
    fclose(a);
    fclose(b);
#ifndef _WIN32
    struct stat st;
    if (stat(from, &st) == 0) chmod(to, st.st_mode & 0777);
#endif
    return true;
}

static void copy_tree(const char *from, const char *to) {
    if (is_dir(from)) {
        mkdirs(to);
        List l = list_dir(from);
        for (int i = 0; i < l.n; i++) {
            char *a = join(from, l.v[i]), *b = join(to, l.v[i]);
            copy_tree(a, b);
            free(a);
            free(b);
        }
    } else {
        copy_file(from, to);
    }
}

/* every file below `dir` whose name ends with `ext`, in sorted order */
static void walk(const char *dir, const char *ext, List *out) {
    List l = list_dir(dir);
    for (int i = 0; i < l.n; i++) {
        char *p = join(dir, l.v[i]);
        if (is_dir(p)) walk(p, ext, out);
        else if (ends_with(p, ext)) push(out, p);
    }
}

static char *absolute(const char *p) {
    if (p[0] == '/' || (p[0] && p[1] == ':')) return xstrdup(p);
    char buf[4096];
#ifdef _WIN32
    if (!_getcwd(buf, sizeof buf)) return xstrdup(p);
#else
    if (!getcwd(buf, sizeof buf)) return xstrdup(p);
#endif
    for (char *c = buf; *c; c++)
        if (*c == '\\') *c = '/';
    return join(buf, p);
}

static int g_tmp_counter;

static int next_id(void) {
#ifdef _WIN32
    return (int)InterlockedIncrement((volatile LONG *)&g_tmp_counter);
#else
    return __atomic_add_fetch(&g_tmp_counter, 1, __ATOMIC_SEQ_CST);
#endif
}

static char *g_tmp_root;

/* A fresh directory below the test root. */
static char *temp_dir(const char *prefix) {
    char *d = fmt("%s/%s-%d", g_tmp_root, prefix, next_id());
    mkdirs(d);
    return d;
}

static void init_tmp_root(void) {
    const char *base = getenv("ZEVTEST_TMP");
    char *b = NULL;
#ifdef _WIN32
    char buf[MAX_PATH + 1];
    if (!base || !*base) {
        DWORD n = GetTempPathA(sizeof buf, buf);
        b = xstrdup(n ? buf : ".");
        for (char *c = b; *c; c++)
            if (*c == '\\') *c = '/';
    }
    int pid = (int)GetCurrentProcessId();
#else
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = "/tmp";
    int pid = (int)getpid();
#endif
    if (!b) b = xstrdup(base);
    g_tmp_root = fmt("%s/zevtest-%d", b, pid);
    mkdirs(g_tmp_root);
    g_tmp_root = absolute(g_tmp_root);
}

/* ------------------------------------------------------------------ running programs */

typedef struct {
    int code; /* exit code; -1 when it could not be started, -2 after a timeout */
    char *out, *err;
} Run;

static int g_timeout = 180;

static void set_env(const char *k, const char *v) {
#ifdef _WIN32
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1);
    else unsetenv(k);
#endif
}

static double now_s(void) {
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static char *quote_arg(const char *a) {
    /* Windows command line quoting: wrap in quotes when needed, escape quotes */
    if (*a && !strpbrk(a, " \t\"")) return xstrdup(a);
    size_t n = strlen(a);
    char *o = xmalloc(n * 2 + 3), *w = o;
    *w++ = '"';
    for (const char *p = a; *p; p++) {
        if (*p == '"') *w++ = '\\';
        *w++ = *p;
    }
    *w++ = '"';
    *w = 0;
    return o;
}

static Run run_argv(char **argv, const char *cwd, const char *input) {
    Run r = {-1, xstrdup(""), xstrdup("")};
    char *tmp = temp_dir("run");
    char *fin = join(tmp, "in"), *fout = join(tmp, "out"), *ferr = join(tmp, "err");
    spit(fin, input ? input : "");
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE hin = CreateFileA(fin, GENERIC_READ, FILE_SHARE_READ, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE hout = CreateFileA(fout, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    HANDLE herr = CreateFileA(ferr, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    size_t cap = 1;
    for (int i = 0; argv[i]; i++) cap += strlen(argv[i]) * 2 + 3;
    char *cmd = xmalloc(cap);
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) {
        char *q = quote_arg(argv[i]);
        if (i) strcat(cmd, " ");
        strcat(cmd, q);
        free(q);
    }
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hin;
    si.hStdOutput = hout;
    si.hStdError = herr;
    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, cwd, &si, &pi);
    if (ok) {
        DWORD w = WaitForSingleObject(pi.hProcess, (DWORD)g_timeout * 1000);
        if (w == WAIT_TIMEOUT) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            r.code = -2;
        } else {
            DWORD ec = 0;
            GetExitCodeProcess(pi.hProcess, &ec);
            r.code = (int)ec;
        }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hin);
    CloseHandle(hout);
    CloseHandle(herr);
    free(cmd);
#else
    int in = open(fin, O_RDONLY);
    int out = open(fout, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int err = open(ferr, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    pid_t pid = fork();
    if (pid == 0) {
        if (cwd && chdir(cwd) != 0) _exit(126);
        dup2(in, 0);
        dup2(out, 1);
        dup2(err, 2);
        close(in);
        close(out);
        close(err);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(in);
    close(out);
    close(err);
    if (pid > 0) {
        double start = now_s();
        int status = 0;
        for (;;) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                if (WIFEXITED(status)) r.code = WEXITSTATUS(status);
                else r.code = 128 + WTERMSIG(status);
                if (r.code == 127 && !exists(fout)) r.code = -1;
                break;
            }
            if (w < 0) break;
            if (now_s() - start > g_timeout) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                r.code = -2;
                break;
            }
            struct timespec ts = {0, 5 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
    }
#endif
    char *o = slurp(fout), *e = slurp(ferr);
    if (o) r.out = o;
    if (e) r.err = e;
    rmrf(tmp);
    free(tmp);
    return r;
}

/* run(cwd, "prog", "arg", ..., NULL) */
static Run run_cwd(const char *cwd, ...) {
    List l = {0};
    va_list ap;
    va_start(ap, cwd);
    for (char *a; (a = va_arg(ap, char *));) push(&l, a);
    va_end(ap);
    push(&l, NULL);
    Run r = run_argv(l.v, cwd, NULL);
    l.n--;
    free(l.v);
    return r;
}

static Run run_list(const char *cwd, List *l, const char *input) {
    push(l, NULL);
    Run r = run_argv(l->v, cwd, input);
    l->n--;
    return r;
}

/* ------------------------------------------------------------------ results */

static char *g_zevic, *g_zevpm, *g_zevfmt;

static char *exe_path(const char *envname, const char *name) {
    const char *e = getenv(envname);
    if (e && *e) return absolute(e);
    return absolute(fmt("build/%s%s", name, EXE));
}

static int g_failed, g_passed;

static void report(const char *name, bool ok, const char *msg) {
    if (ok) {
        printf("ok    %s\n", name);
        g_passed++;
    } else {
        printf("FAIL  %s\n", name);
        g_failed++;
        if (msg && *msg) {
            List l = lines_of(msg);
            for (int i = 0; i < l.n; i++) printf("      %s\n", l.v[i]);
        }
    }
    fflush(stdout);
}

/* Failure inside a scenario: the message is kept and the scenario ends. */
static jmp_buf g_jmp;
static char *g_fail_msg;

static void fail(const char *f, ...) {
    va_list ap;
    va_start(ap, f);
    g_fail_msg = vfmt(f, ap);
    va_end(ap);
    longjmp(g_jmp, 1);
}

#define expect(cond, ...)                                                                                              \
    do {                                                                                                               \
        if (!(cond)) fail(__VA_ARGS__);                                                                                \
    } while (0)

/* ------------------------------------------------------------------ golden tests */

typedef struct {
    char *dir, *name;
    bool ok;
    char *msg;
} Golden;

static char *read_or(const char *path, const char *dflt) {
    char *s = slurp(path);
    return s ? s : (char *)dflt;
}

/* the first differing line of two texts, for the report */
static char *first_diff(const char *want, const char *got) {
    List a = lines_of(want), b = lines_of(got);
    for (int i = 0; i < a.n || i < b.n; i++) {
        const char *x = i < a.n ? a.v[i] : "(end of output)", *y = i < b.n ? b.v[i] : "(end of output)";
        if (strcmp(x, y) != 0) return fmt("line %d:\n  expected: %s\n  actual:   %s", i + 1, x, y);
    }
    return xstrdup("");
}

static char *missing_lines(const char *want, const char *got) {
    List l = lines_of(want);
    for (int i = 0; i < l.n; i++) {
        char *s = strip(l.v[i]);
        bool blank = !*s;
        free(s);
        if (!blank && !contains(got, l.v[i])) return xstrdup(l.v[i]);
    }
    return NULL;
}

static void golden_one(Golden *t) {
    const char *d = t->dir;
    t->ok = false;
    List src = {0};
    char *input = join(d, "input.zev");
    List all = list_dir(d);
    if (is_file(input)) push(&src, input);
    for (int i = 0; i < all.n; i++)
        if (ends_with(all.v[i], ".zev") && strcmp(all.v[i], "input.zev") != 0) push(&src, join(d, all.v[i]));
    for (int i = 0; i < all.n; i++)
        if (ends_with(all.v[i], ".c")) push(&src, join(d, all.v[i]));
    for (int i = 0; i < all.n; i++)
        if (ends_with(all.v[i], ".cpp")) push(&src, join(d, all.v[i]));
    char *tmp = temp_dir("golden");
    List extra = words_of(read_or(join(d, "args.txt"), ""));
    for (int i = 0; i < extra.n; i++) push(&src, replace_all(replace_all(extra.v[i], "{dir}", d), "{tmp}", tmp));
    char *exe = join(tmp, "prog" EXE);
    List cmd = {0};
    push(&cmd, g_zevic);
    for (int i = 0; i < src.n; i++) push(&cmd, src.v[i]);
    push(&cmd, "-o");
    push(&cmd, exe);
    Run c = run_list(NULL, &cmd, NULL);
    char *want_err = slurp(join(d, "expected_error.txt"));
    if (want_err) {
        if (c.code != 1) t->msg = fmt("expected a compile error (exit 1), got exit %d\n%s", c.code, c.err);
        else {
            char *m = missing_lines(want_err, c.err);
            if (m) t->msg = fmt("missing in the error output: %s\n--- stderr ---\n%s", m, c.err);
            else t->ok = true;
        }
        goto done;
    }
    if (c.code != 0) {
        t->msg = fmt("compilation failed (exit %d)\n%s", c.code, c.err);
        goto done;
    }
    {
        List run = {0};
        push(&run, exe);
        List ra = words_of(read_or(join(d, "run_args.txt"), ""));
        for (int i = 0; i < ra.n; i++) push(&run, ra.v[i]);
        Run p = run_list(NULL, &run, read_or(join(d, "input.txt"), ""));
        char *want_out = no_cr(read_or(join(d, "expected_output.txt"), ""));
        int want_code = atoi(read_or(join(d, "expected_exit_code"), "0"));
        char *got = no_cr(p.out);
        if (p.code == -2) {
            t->msg = fmt("the program timed out after %d seconds", g_timeout);
        } else if (strcmp(got, want_out) != 0) {
            t->msg = fmt("output differs: %s", first_diff(want_out, got));
        } else if (p.code != want_code) {
            t->msg = fmt("exit code %d, expected %d\n%s", p.code, want_code, p.err);
        } else {
            char *we = slurp(join(d, "expected_stderr.txt"));
            char *m = we ? missing_lines(we, p.err) : NULL;
            if (m) t->msg = fmt("missing in stderr: %s\n--- stderr ---\n%s", m, p.err);
            else t->ok = true;
        }
    }
done:
    rmrf(tmp);
}

#ifdef _WIN32
typedef HANDLE Thread;
#else
typedef pthread_t Thread;
#endif

typedef struct {
    Golden *tests;
    int n;
    int next;
} Pool;

static Pool g_pool;

static int pool_take(void) {
#ifdef _WIN32
    return (int)InterlockedIncrement((volatile LONG *)&g_pool.next) - 1;
#else
    return __atomic_fetch_add(&g_pool.next, 1, __ATOMIC_SEQ_CST);
#endif
}

#ifdef _WIN32
static unsigned __stdcall worker(void *arg) {
    (void)arg;
    for (int i; (i = pool_take()) < g_pool.n;) golden_one(&g_pool.tests[i]);
    return 0;
}
#else
static void *worker(void *arg) {
    (void)arg;
    for (int i; (i = pool_take()) < g_pool.n;) golden_one(&g_pool.tests[i]);
    return NULL;
}
#endif

static int cpu_count(void) {
#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 2;
#endif
}

static void test_golden(const char *pattern) {
    char *root = "tests/golden";
    List names = list_dir(root);
    Golden *tests = xmalloc(sizeof *tests * (size_t)(names.n + 1));
    int n = 0;
    for (int i = 0; i < names.n; i++) {
        char *d = join(root, names.v[i]);
        if (!is_dir(d) || !contains(names.v[i], pattern)) continue;
        tests[n].dir = absolute(d);
        tests[n].name = names.v[i];
        tests[n].msg = NULL;
        n++;
    }
    if (n == 0) {
        printf("FAIL  golden: no tests found\n");
        g_failed++;
        return;
    }
    g_pool.tests = tests;
    g_pool.n = n;
    g_pool.next = 0;
    int nt = cpu_count();
    if (nt > n) nt = n;
    if (nt > 16) nt = 16;
    Thread th[16];
    for (int i = 0; i < nt; i++) {
#ifdef _WIN32
        th[i] = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, NULL);
#else
        pthread_create(&th[i], NULL, worker, NULL);
#endif
    }
    for (int i = 0; i < nt; i++) {
#ifdef _WIN32
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
#else
        pthread_join(th[i], NULL);
#endif
    }
    for (int i = 0; i < n; i++) report(tests[i].name, tests[i].ok, tests[i].msg);
}

/* ------------------------------------------------------------------ examples */

static char *spec_sample(void) {
    char *spec = slurp("docs/spec/zevic-spec.md");
    if (!spec) return NULL;
    char *p = strstr(spec, "## Appendix B");
    if (!p) return NULL;
    p = strstr(p, "```\n");
    if (!p) return NULL;
    p += 4;
    char *e = strstr(p, "\n```\n");
    if (!e) return NULL;
    char *code = xmalloc((size_t)(e - p) + 1);
    memcpy(code, p, (size_t)(e - p));
    code[e - p] = 0;
    return strip(code);
}

static char *check_single(const char *name, List *sources) {
    char *tmp = temp_dir("example");
    char *exe = join(tmp, "prog" EXE);
    List cmd = {0};
    push(&cmd, g_zevic);
    for (int i = 0; i < sources->n; i++) push(&cmd, sources->v[i]);
    push(&cmd, "-o");
    push(&cmd, exe);
    Run c = run_list(NULL, &cmd, NULL);
    char *res = NULL;
    if (c.code != 0) {
        res = fmt("compilation failed\n%s", c.err);
    } else {
        Run p = run_cwd(tmp, exe, NULL);
        if (p.code != 0) {
            res = fmt("exit code %d\n%s", p.code, p.err);
        } else {
            char *want_path = fmt("tests/examples_expected/%s.txt", name);
            char *got = no_cr(p.out);
            if (getenv("UPDATE_EXPECTED")) {
                spit(want_path, got);
            } else {
                char *want = slurp(want_path);
                if (!want) res = fmt("missing %s", want_path);
                else if (strcmp(got, no_cr(want)) != 0) res = fmt("output differs from %s: %s", want_path, first_diff(no_cr(want), got));
            }
        }
    }
    rmrf(tmp);
    return res;
}

static void test_examples(void) {
    char *sample = spec_sample();
    char *wc = slurp("examples/apps/wordcount/src/Main.zev");
    if (!sample) {
        // the heading is searched for by its text: renaming it (a translation, say) must be followed here
        printf("FAIL  no code block under the heading \"## Appendix B\" in docs/spec/zevic-spec.md\n");
        g_failed++;
    } else if (!wc || strcmp(sample, strip(wc)) != 0) {
        printf("FAIL  specification appendix B differs from examples/apps/wordcount/src/Main.zev\n");
        g_failed++;
    }
    List names = list_dir("examples");
    for (int i = 0; i < names.n; i++) {
        char *p = join("examples", names.v[i]);
        List src = {0};
        char *name;
        if (is_file(p) && ends_with(p, ".zev")) {
            push(&src, absolute(p));
            name = xstrdup(names.v[i]);
            name[strlen(name) - 4] = 0;
        } else if (is_dir(p) && isdigit((unsigned char)names.v[i][0]) && strchr(names.v[i], '_')) {
            List in = list_dir(p);
            for (int k = 0; k < in.n; k++)
                if (ends_with(in.v[k], ".zev")) push(&src, absolute(join(p, in.v[k])));
            for (int k = 0; k < in.n; k++)
                if (ends_with(in.v[k], ".c")) push(&src, absolute(join(p, in.v[k])));
            name = names.v[i];
        } else {
            continue;
        }
        char *err = check_single(name, &src);
        report(name, err == NULL, err);
    }
    char *apps = "examples/apps";
    List pk = list_dir(apps);
    for (int i = 0; i < pk.n; i++) {
        char *dir = absolute(join(apps, pk.v[i]));
        if (!is_dir(dir)) continue;
        Run p = run_cwd(dir, g_zevpm, "test", NULL);
        report(fmt("apps/%s (zevpm test)", pk.v[i]), p.code == 0, fmt("%s%s", p.out, p.err));
        run_cwd(dir, g_zevpm, "clean", NULL);
    }
}

/* ------------------------------------------------------------------ zevfmt */

static int cmp_lines(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

/* Equal ASTs; the order of the imports does not matter (the formatter sorts them). */
static bool same_ast(const char *a, const char *b) {
    List la = lines_of(a), lb = lines_of(b);
    List ia = {0}, ib = {0}, ra = {0}, rb = {0};
    for (int i = 0; i < la.n; i++) push(starts_with(la.v[i], "import ") ? &ia : &ra, la.v[i]);
    for (int i = 0; i < lb.n; i++) push(starts_with(lb.v[i], "import ") ? &ib : &rb, lb.v[i]);
    if (ia.n) qsort(ia.v, (size_t)ia.n, sizeof *ia.v, cmp_lines);
    if (ib.n) qsort(ib.v, (size_t)ib.n, sizeof *ib.v, cmp_lines);
    if (ia.n != ib.n || ra.n != rb.n) return false;
    for (int i = 0; i < ia.n; i++)
        if (strcmp(ia.v[i], ib.v[i]) != 0) return false;
    for (int i = 0; i < ra.n; i++)
        if (strcmp(ra.v[i], rb.v[i]) != 0) return false;
    return true;
}

static void test_zevfmt(void) {
    const char *dirs[] = {"packages/Zevic/src", "examples", "tools", "tests/zevfmt"};
    List files = {0};
    for (int i = 0; i < 4; i++) walk(dirs[i], ".zev", &files);
    char *tmp = temp_dir("zevfmt");
    char *once = join(tmp, "once.zev");
    for (int i = 0; i < files.n; i++) {
        const char *path = files.v[i];
        Run f = run_cwd(NULL, g_zevfmt, path, NULL);
        if (f.code != 0) {
            report(path, false, fmt("zevfmt failed\n%s", f.err));
            continue;
        }
        f.out = no_cr(f.out); /* standard output is in text mode on Windows: CRLF */
        spit(once, f.out);
        Run a0 = run_cwd(NULL, g_zevic, "--dump-ast", path, NULL);
        Run a1 = run_cwd(NULL, g_zevic, "--dump-ast", once, NULL);
        if (a1.code != 0 || !same_ast(a0.out, a1.out)) {
            report(path, false, fmt("the formatted source does not mean the same\n%s", a1.err));
            continue;
        }
        Run f2 = run_cwd(NULL, g_zevfmt, once, NULL);
        if (f2.code != 0 || strcmp(no_cr(f2.out), f.out) != 0) {
            report(path, false, "formatting is not idempotent");
            continue;
        }
        if (!starts_with(path, "tests/")) {
            char *orig = slurp(path);
            if (!orig || strcmp(no_cr(orig), f.out) != 0) {
                report(path, false, "not formatted (run `zevfmt -w`)");
                continue;
            }
        }
        char *expected = fmt("%.*s.expected", (int)strlen(path) - 4, path);
        char *want = slurp(expected);
        if (want && strcmp(no_cr(want), f.out) != 0) {
            report(path, false, fmt("differs from %s", expected));
            continue;
        }
        report(path, true, NULL);
    }
    rmrf(tmp);
}

/* ------------------------------------------------------------------ zevpm */

static Run sh_list(const char *cwd, List *l, bool check) {
    Run r = run_list(cwd, l, NULL);
    if (check && r.code != 0) fail("%s failed (%d)\n%s%s", l->v[0], r.code, r.out, r.err);
    return r;
}

static Run pm(const char *cwd, int check, ...) {
    List l = {0};
    push(&l, g_zevpm);
    va_list ap;
    va_start(ap, check);
    for (char *a; (a = va_arg(ap, char *));) push(&l, a);
    va_end(ap);
    return sh_list(cwd, &l, check);
}

static Run sh(const char *cwd, int check, ...) {
    List l = {0};
    va_list ap;
    va_start(ap, check);
    for (char *a; (a = va_arg(ap, char *));) push(&l, a);
    va_end(ap);
    return sh_list(cwd, &l, check);
}

static char *rd(const char *path) {
    char *s = slurp(path);
    if (!s) fail("cannot read %s", path);
    return no_cr(s);
}

static void wr(const char *path, const char *text) {
    if (!spit(path, text)) fail("cannot write %s", path);
}

static void make_lib(const char *d, const char *name, const char *body, const char *deps, const char *extra_path,
                     const char *extra_text) {
    wr(join(d, "zevic.toml"), fmt("[package]\nname = \"%s\"\nversion = \"1.0.0\"\nkind = \"lib\"\n\n[deps]\n%s\n", name, deps));
    wr(join(d, fmt("src/%s.zev", name)), fmt("module %s\n\n%s\n", name, body));
    if (extra_path) wr(join(d, extra_path), extra_text);
}

static char *git_c[] = {"git", "-c", "user.name=t", "-c", "user.email=t@t", "-c", "init.defaultBranch=main"};

static Run git(const char *cwd, ...) {
    List l = {0};
    for (int i = 0; i < 7; i++) push(&l, git_c[i]);
    va_list ap;
    va_start(ap, cwd);
    for (char *a; (a = va_arg(ap, char *));) push(&l, a);
    va_end(ap);
    return sh_list(cwd, &l, true);
}

static char *git_repo(const char *d, const char *tag) {
    git(d, "init", "-q", NULL);
    git(d, "add", "-A", NULL);
    git(d, "commit", "-q", "-m", "init", NULL);
    if (tag) git(d, "tag", tag, NULL);
    return strip(git(d, "rev-parse", "HEAD", NULL).out);
}

static char *uri(const char *d) { return fmt("file://%s", replace_all(d, "\\", "/")); }

static bool have_git(void) {
    Run r = run_cwd(NULL, "git", "--version", NULL);
    return r.code == 0;
}

static void sc_new_run_test_clean(const char *tmp) {
    pm(tmp, true, "new", "my-app", NULL);
    char *app = join(tmp, "my-app");
    expect(contains(rd(join(app, "zevic.toml")), "name = \"MyApp\""), "package name is PascalCase");
    expect(strcmp(rd(join(app, ".gitignore")), "build/\n") == 0, ".gitignore");
    char *out = pm(app, true, "run", NULL).out;
    expect(contains(out, "Hello from MyApp!"), "run output: %s", out);
    wr(join(app, "src/Main.zev"),
       "module Main\n\nimport Zevic.Console\nimport Zevic.Env\n\nfn Main() {\n    Console.Println(\"args=${Env.RawArgs().Len()}\")\n}\n");
    out = pm(app, true, "run", "--", "a", "b", NULL).out;
    expect(contains(out, "args="), "arguments reach the program: %s", out);
    wr(join(app, "src/Math_test.zev"),
       "module Main\n\nimport Zevic.Test\n\ntest \"one\" {\n    Test.Equal(1, 1)\n}\n\ntest \"two fails\" {\n    Test.Equal(1, 2)\n}\n");
    Run p = pm(app, false, "test", NULL);
    expect(p.code == 1, "a failing test gives exit code 1, got %d", p.code);
    expect(contains(p.out, "1 passed, 1 failed") && contains(p.out, "Math_test.zev:"), "test report: %s", p.out);
    p = pm(app, true, "test", "one", NULL);
    expect(contains(p.out, "1 passed, 0 failed"), "name filter: %s", p.out);
    pm(app, true, "build", "--release", NULL);
    pm(app, true, "clean", NULL);
    expect(!exists(join(app, "build")), "clean removes build/");
    expect(pm(tmp, false, "new", "my-app", NULL).code != 0, "new refuses an existing directory");
}

static void sc_path_dependency(const char *tmp) {
    char *util = join(tmp, "util");
    make_lib(util, "Util", "fn Twice(x: I32) -> I32 { x * 2 }", "", NULL, NULL);
    pm(tmp, true, "new", "app", NULL);
    char *app = join(tmp, "app");
    append_file(join(app, "zevic.toml"), "\n[deps]\nUtil = { path = \"../util\" }\n");
    wr(join(app, "src/Main.zev"),
       "module Main\n\nimport Util\nimport Zevic.Console\n\nfn Main() {\n    Console.Println(\"${Util.Twice(21)}\")\n}\n");
    char *out = strip(pm(app, true, "run", NULL).out);
    expect(strcmp(out, "42") == 0, "path dependency works: %s", out);
    expect(!exists(join(app, "zevic.lock")), "path dependencies are not locked");
}

static void sc_native_in_dependency(const char *tmp) {
    char *nat = join(tmp, "nat");
    make_lib(nat, "Nat",
             "extern \"C\" {\n    fn nat_triple(x: CInt) -> CInt\n}\n\nfn Triple(x: I32) -> I32 {\n    nat_triple(x.ToCInt()).ToI32()\n}",
             "", "native/nat.c", "int nat_triple(int x) { return x * 3; }\n");
    pm(tmp, true, "new", "app", NULL);
    char *app = join(tmp, "app");
    append_file(join(app, "zevic.toml"), "\n[deps]\nNat = { path = \"../nat\" }\n");
    wr(join(app, "src/Main.zev"),
       "module Main\n\nimport Nat\nimport Zevic.Console\n\nfn Main() {\n    Console.Println(\"${Nat.Triple(14)}\")\n}\n");
    char *out = strip(pm(app, true, "run", NULL).out);
    expect(strcmp(out, "42") == 0, "the dependency's native/ is built: %s", out);
}

static void sc_library_for_c(const char *tmp) {
    pm(tmp, true, "new", "geo", "--lib", NULL);
    char *geo = join(tmp, "geo");
    wr(join(geo, "src/Geo.zev"),
       "module Geo\n\nimport Zevic.Test\n\nexport \"C\" fn geo_add(a: CInt, b: CInt) -> CInt {\n"
       "    (a.ToI32() + b.ToI32()).ToCInt()\n}\n\ntest \"add through C types\" {\n"
       "    Test.Equal(geo_add(1.ToCInt(), 2.ToCInt()).ToI32(), 3)\n}\n");
    pm(geo, true, "build", NULL);
    char *lib = join(geo, IS_WIN ? "build/Geo.lib" : "build/libGeo.a");
    char *header = join(geo, "build/include/Geo.h");
    expect(exists(lib) && exists(header), "the library and its header exist");
    wr(join(geo, "use.c"),
       "#include <stdio.h>\n#include \"build/include/Geo.h\"\nint main(void) {\n    zevic_init(0, 0);\n"
       "    printf(\"%d\\n\", geo_add(2, 40));\n    return 0;\n}\n");
    const char *cc = getenv("CC");
    if (!cc || !*cc) cc = "clang";
    Run r = IS_WIN ? sh(geo, true, cc, "use.c", lib, "-lws2_32", "-o", "use" EXE, NULL)
                   : sh(geo, true, cc, "use.c", lib, "-lm", "-pthread", "-o", "use" EXE, NULL);
    (void)r;
    char *out = strip(sh(geo, true, join(geo, "use" EXE), NULL).out);
    expect(strcmp(out, "42") == 0, "C calls the Zevic library: %s", out);
    Run p = pm(geo, true, "test", NULL);
    expect(contains(p.out, "1 passed, 0 failed"), "tests of a library: %s", p.out);
    expect(pm(geo, false, "run", NULL).code != 0, "a library cannot be run");
}

static void sc_git_dependencies(const char *tmp) {
    if (!have_git()) {
        printf("      (git not found: skipped)\n");
        return;
    }
    set_env("ZEVPM_HOME", join(tmp, "zevpm-home"));
    char *b = join(tmp, "b");
    make_lib(b, "Bee", "fn Name() -> String { \"bee 1\" }", "", NULL, NULL);
    char *rev_b1 = git_repo(b, "v1.0.0");
    wr(join(b, "src/Bee.zev"), "module Bee\n\nfn Name() -> String { \"bee 2\" }\n");
    git(b, "commit", "-q", "-am", "second", NULL);
    git(b, "tag", "v2.0.0", NULL);
    char *a = join(tmp, "a");
    make_lib(a, "Ay", "import Bee\n\nfn Name() -> String { \"ay+\" + Bee.Name() }",
             fmt("Bee = { git = \"%s\", tag = \"v1.0.0\" }", uri(b)), NULL, NULL);
    git_repo(a, "v1.0.0");

    pm(tmp, true, "new", "app", NULL);
    char *app = join(tmp, "app");
    wr(join(app, "src/Main.zev"),
       "module Main\n\nimport Ay\nimport Zevic.Console\n\nfn Main() {\n    Console.Println(Ay.Name())\n}\n");
    char *out = pm(app, true, "get", fmt("%s@v1.0.0", uri(a)), NULL).out;
    expect(contains(out, "Added dependency `Ay`"), "get: %s", out);
    char *toml = rd(join(app, "zevic.toml"));
    expect(contains(toml, fmt("Ay = { git = \"%s\", tag = \"v1.0.0\" }", uri(a))), "zevic.toml has the dependency:\n%s", toml);
    char *lock = rd(join(app, "zevic.lock"));
    expect(contains(lock, "name = \"Ay\"") && contains(lock, "name = \"Bee\"") && contains(lock, rev_b1),
           "the lock has both packages:\n%s", lock);
    out = strip(pm(app, true, "run", NULL).out);
    expect(strcmp(out, "ay+bee 1") == 0, "transitive dependency is built: %s", out);
    rmrf(a);
    rmrf(b);
    out = strip(pm(app, true, "run", NULL).out);
    expect(strcmp(out, "ay+bee 1") == 0, "built from the cache: %s", out);
    char *b2 = join(tmp, "b2");
    make_lib(b2, "Bee", "fn Name() -> String { \"other\" }", "", NULL, NULL);
    git_repo(b2, "v9.0.0");
    append_file(join(app, "zevic.toml"), fmt("Bee = { git = \"%s\", tag = \"v9.0.0\" }\n", uri(b2)));
    Run p = pm(app, false, "build", NULL);
    expect(p.code == 1 && contains(p.err, "two different versions"), "conflict is an error: %s", p.err);
    expect(contains(pm(app, true, "remove", "Bee", NULL).out, "Removed dependency `Bee`"), "remove");
    expect(!contains(rd(join(app, "zevic.toml")), "Bee = "), "line is gone");
    expect(pm(app, false, "remove", "Nope", NULL).code == 1, "removing an unknown name fails");
    pm(app, true, "remove", "Ay", NULL);
    expect(!exists(join(app, "zevic.lock")), "an empty lock is removed");
}

static void sc_git_update(const char *tmp) {
    if (!have_git()) {
        printf("      (git not found: skipped)\n");
        return;
    }
    set_env("ZEVPM_HOME", join(tmp, "zevpm-home"));
    char *c = join(tmp, "c");
    make_lib(c, "Cee", "fn Name() -> String { \"cee 1\" }", "", NULL, NULL);
    git_repo(c, NULL);
    pm(tmp, true, "new", "app", NULL);
    char *app = join(tmp, "app");
    wr(join(app, "src/Main.zev"),
       "module Main\n\nimport Cee\nimport Zevic.Console\n\nfn Main() {\n    Console.Println(Cee.Name())\n}\n");
    pm(app, true, "get", uri(c), NULL);
    expect(strcmp(strip(pm(app, true, "run", NULL).out), "cee 1") == 0, "first version");
    wr(join(c, "src/Cee.zev"), "module Cee\n\nfn Name() -> String { \"cee 2\" }\n");
    git(c, "commit", "-q", "-am", "two", NULL);
    expect(strcmp(strip(pm(app, true, "run", NULL).out), "cee 1") == 0, "the lock keeps the old commit");
    pm(app, true, "update", NULL);
    expect(strcmp(strip(pm(app, true, "run", NULL).out), "cee 2") == 0, "update moves to the newest commit");
}

static void sc_manifest_errors(const char *tmp) {
    pm(tmp, true, "new", "app", NULL);
    char *app = join(tmp, "app");
    char *toml = join(app, "zevic.toml");
    char *good = rd(toml);
    struct {
        char *text;
        const char *want;
    } cases[] = {
        {replace_all(good, "name = \"App\"", "name = \"app\""), "PascalCase"},
        {replace_all(good, "version = \"0.1.0\"", "version = \"1.0\""), "1.2.3"},
        {replace_all(good, "kind = \"bin\"", "kind = \"dll\""), "bin"},
        {fmt("%s\n[deps]\nUtil = { git = \"x\", path = \"y\" }\n", good), "both `git` and `path`"},
        {fmt("%s\n[deps]\nUtil = { git = \"x\", tag = \"a\", branch = \"b\" }\n", good), "only one of"},
        {fmt("%s\n[oops]\n", good), "unknown section"},
        {replace_all(good, "zevic = \">=0.1\"", "zevic = \">=99.0\""), "needs zevic"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        wr(toml, cases[i].text);
        Run p = pm(app, false, "build", NULL);
        expect(p.code != 0 && contains(p.err, cases[i].want), "expected `%s` in: %s", cases[i].want, p.err);
    }
}

typedef void (*Scenario)(const char *tmp);

static void scenario(const char *name, Scenario fn) {
    char *tmp = temp_dir("zevpm-test");
    g_fail_msg = NULL;
    if (setjmp(g_jmp) == 0) {
        fn(tmp);
        report(name, true, NULL);
    } else {
        report(name, false, g_fail_msg);
    }
    rmrf(tmp);
}

static void test_zevpm(void) {
    set_env("ZEVIC", g_zevic);
    scenario("new_run_test_clean", sc_new_run_test_clean);
    scenario("path_dependency", sc_path_dependency);
    scenario("native_in_dependency", sc_native_in_dependency);
    scenario("library_for_c", sc_library_for_c);
    scenario("git_dependencies", sc_git_dependencies);
    scenario("git_update", sc_git_update);
    scenario("manifest_errors", sc_manifest_errors);
}

/* ------------------------------------------------------------------ the release layout */

/* The layout of the release archive (bin/, packages/, runtime/, examples/) is copied to a directory whose name has a
 * space in it. Then only `bin` is put on PATH and everything is started from a different directory, with no ZEVIC,
 * ZEVIC_HOME or similar variable set: this is how a user works after unpacking the archive. */
static char *with_slashes(const char *s, bool back) { return replace_all(s, back ? "/" : "\\", back ? "\\" : "/"); }

static void sc_install(const char *tmp) {
    char *root = join(tmp, "zevic release");
    char *bin = join(root, "bin");
    mkdirs(bin);
    copy_file(g_zevic, join(bin, "zevic" EXE));
    copy_file(g_zevpm, join(bin, "zevpm" EXE));
    copy_file(g_zevfmt, join(bin, "zevfmt" EXE));
    copy_tree("packages", join(root, "packages"));
    copy_tree("runtime", join(root, "runtime"));
    copy_tree("examples", join(root, "examples"));
    copy_file("VERSION", join(root, "VERSION"));
    set_env("ZEVIC", NULL);
    set_env("ZEVIC_HOME", NULL);
    set_env("ZEVPM_HOME", join(tmp, "zevpm-home"));
    const char *old = getenv("PATH");
    char *oldpath = xstrdup(old ? old : "");
    char *binpath = IS_WIN ? with_slashes(bin, true) : bin;
    set_env("PATH", fmt("%s%c%s", binpath, IS_WIN ? ';' : ':', oldpath));
    char *work = join(tmp, "work dir");
    mkdirs(work);

    /* the compiler finds the standard library and the runtime from the place of its executable */
    Run v = sh(work, true, "zevic", "--version", NULL);
    expect(contains(v.out, "zevic "), "zevic --version: %s", v.out);
    Run sd = sh(work, true, "zevic", "--print-stdlib", NULL);
    expect(contains(sd.out, "packages"), "zevic --print-stdlib: %s", sd.out);
    wr(join(work, "hello.zev"), "module Main\n\nimport Zevic.Console\n\nfn Main() {\n    Console.Println(\"hi ${1 + 2}\")\n}\n");
    Run c = sh(work, false, "zevic", "hello.zev", "-o", "hello" EXE, NULL);
    expect(c.code == 0, "zevic hello.zev from a directory next to nothing:\n%s%s", c.out, c.err);
    Run h = sh(work, true, join(work, "hello" EXE), NULL);
    expect(contains(h.out, "hi 3"), "the program runs: %s", h.out);
    /* the same with an absolute path to the source and with backslashes (Windows) */
    char *abs_src = absolute(join(work, "hello.zev"));
    c = sh(work, false, "zevic", IS_WIN ? with_slashes(abs_src, true) : abs_src, "-o", "hello2" EXE, NULL);
    expect(c.code == 0, "zevic <absolute path>:\n%s%s", c.out, c.err);

    /* zevpm reads zevic.toml and starts the compiler it finds next to itself */
    Run n = sh(work, false, "zevpm", "new", "proj", NULL);
    expect(n.code == 0, "zevpm new:\n%s%s", n.out, n.err);
    char *proj = join(work, "proj");
    expect(is_file(join(proj, "zevic.toml")), "zevic.toml was created");
    Run r = sh(proj, false, "zevpm", "run", NULL);
    expect(r.code == 0 && contains(r.out, "Hello from Proj!"), "zevpm run:\n%s%s", r.out, r.err);
    Run t = sh(proj, false, "zevpm", "test", NULL);
    expect(t.code == 0, "zevpm test:\n%s%s", t.out, t.err);
    Run b = sh(proj, false, "zevpm", "build", "--release", NULL);
    expect(b.code == 0, "zevpm build --release:\n%s%s", b.out, b.err);
    /* a project in a subdirectory of itself: zevpm finds zevic.toml upwards? (no: it is read from the working directory) */

    /* zevfmt reads the files it is given: relative, absolute, with backslashes, and a directory */
    Run f1 = sh(proj, false, "zevfmt", "-check", "src/Main.zev", NULL);
    expect(f1.code == 0, "zevfmt -check src/Main.zev:\n%s%s", f1.out, f1.err);
    if (IS_WIN) {
        Run f2 = sh(proj, false, "zevfmt", "-check", "src\\Main.zev", NULL);
        expect(f2.code == 0, "zevfmt -check src\\Main.zev:\n%s%s", f2.out, f2.err);
    }
    char *abs_main = absolute(join(proj, "src/Main.zev"));
    Run f3 = sh(work, false, "zevfmt", "-check", IS_WIN ? with_slashes(abs_main, true) : abs_main, NULL);
    expect(f3.code == 0, "zevfmt -check <absolute path>:\n%s%s", f3.out, f3.err);
    Run f4 = sh(work, false, "zevfmt", "-check", "proj", NULL);
    expect(f4.code == 0, "zevfmt -check proj:\n%s%s", f4.out, f4.err);
    Run f5 = sh(work, false, "zevfmt", "proj/src/Main.zev", NULL);
    expect(f5.code == 0 && contains(f5.out, "module Main"), "zevfmt prints the formatted file:\n%s%s", f5.out, f5.err);

    /* an example of the release builds */
    Run e = sh(work, false, "zevic", join(root, "examples/01_hello.zev"), "-o", "ex" EXE, NULL);
    expect(e.code == 0, "zevic examples/01_hello.zev:\n%s%s", e.out, e.err);

    /* started by its full path, without PATH */
    set_env("PATH", oldpath);
    Run full = sh(work, false, join(bin, "zevic" EXE), "hello.zev", "-o", "hello3" EXE, NULL);
    expect(full.code == 0, "zevic by its full path:\n%s%s", full.out, full.err);
    Run fullpm = sh(proj, false, join(bin, "zevpm" EXE), "run", NULL);
    expect(fullpm.code == 0, "zevpm by its full path:\n%s%s", fullpm.out, fullpm.err);
}

static void sc_install_wrapper(const char *tmp) {
    const char *old = getenv("PATH");
    char *oldpath = xstrdup(old ? old : "");
    if (setjmp(g_jmp) == 0) {
        sc_install(tmp);
    } else {
        set_env("PATH", oldpath);
        longjmp(g_jmp, 1);
    }
    set_env("PATH", oldpath);
}

static void test_install(void) {
    scenario("release layout on PATH", sc_install_wrapper);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: zevtest golden [pattern] | examples | zevfmt | zevpm | install | all\n");
        return 2;
    }
    const char *t = getenv("ZEVIC_TEST_TIMEOUT");
    if (t && atoi(t) > 0) g_timeout = atoi(t);
    g_zevic = exe_path("ZEVIC", "zevic");
    g_zevpm = exe_path("ZEVPM", "zevpm");
    g_zevfmt = exe_path("ZEVFMT", "zevfmt");
    set_env("ZEVIC", g_zevic); /* zevpm starts this compiler; the path is absolute because zevpm changes directory */
    init_tmp_root();
    const char *mode = argv[1];
    bool all = strcmp(mode, "all") == 0;
    bool known = false;
    if (all || strcmp(mode, "golden") == 0) {
        test_golden(argc > 2 && !all ? argv[2] : "");
        known = true;
    }
    if (all || strcmp(mode, "examples") == 0) {
        test_examples();
        known = true;
    }
    if (all || strcmp(mode, "zevfmt") == 0) {
        test_zevfmt();
        known = true;
    }
    if (all || strcmp(mode, "zevpm") == 0) {
        test_zevpm();
        known = true;
    }
    if (all || strcmp(mode, "install") == 0) {
        test_install();
        known = true;
    }
    if (!known) {
        fprintf(stderr, "unknown mode `%s`\n", mode);
        return 2;
    }
    rmrf(g_tmp_root);
    printf("\n%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
