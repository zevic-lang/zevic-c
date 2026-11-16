#define _POSIX_C_SOURCE 200809L
#include "driver.h"

#include <ctype.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#ifdef _WIN32
#include <windows.h>
#define SEP '\\'
#else
#include <unistd.h>
#define SEP '/'
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifdef _WIN32
#define POPEN zv_popen
#define PCLOSE zv_pclose
#else
#define POPEN zv_popen
#define PCLOSE zv_pclose
#endif

/* ---- the running executable ---- */

static char *exe_path(const char *argv0) {
    char *p = zv_exe_path();
    if (p) return p;
    return zstrdup(argv0 ? argv0 : "zevic");
}

static char *dir_of(const char *path) {
    const char *last = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    if (!last) return zstrdup(".");
    return zstrndup(path, (size_t)(last - path));
}

static bool is_home(const char *dir) {
    char *rt = zsprintf("%s/runtime/zevic_rt.c", dir);
    char *sd = zsprintf("%s/packages/Zevic/zevic.toml", dir);
    return file_exists(rt) && file_exists(sd);
}

static char *g_home_report;

/* What driver_find_home looked at, for the error message when it finds nothing. */
const char *driver_home_report(void) { return g_home_report ? g_home_report : ""; }

char *driver_find_home(const char *argv0) {
    SB rep = {0};
    const char *env = zv_getenv("ZEVIC_HOME");
    if (env && *env) {
        if (is_home(env)) return zstrdup(env);
        sb_printf(&rep, "  ZEVIC_HOME=%s: has no runtime/zevic_rt.c and packages/Zevic/zevic.toml\n", env);
    }
    char *exe = exe_path(argv0);
    char *dir = dir_of(exe);
    sb_printf(&rep, "  the executable is %s\n", exe);
    char *cands[] = {zsprintf("%s/../share/zevic", dir), zsprintf("%s/..", dir), zsprintf("%s", dir), zstrdup(".")};
    for (int i = 0; i < 4; i++) {
        if (is_home(cands[i])) return cands[i];
        sb_printf(&rep, "  %s: no runtime/zevic_rt.c or packages/Zevic/zevic.toml\n", cands[i]);
    }
    g_home_report = sb_str(&rep);
    return NULL;
}

/* ---- running commands ---- */

static char *quote(const char *s) {
    SB sb = {0};
#ifdef _WIN32
    sb_putc(&sb, '"');
    sb_puts(&sb, s);
    sb_putc(&sb, '"');
#else
    sb_putc(&sb, '\'');
    for (; *s; s++) {
        if (*s == '\'') sb_puts(&sb, "'\\''");
        else sb_putc(&sb, *s);
    }
    sb_putc(&sb, '\'');
#endif
    return sb_str(&sb);
}

static int run(const char *cmd) {
#ifdef _WIN32
    char *full = zsprintf("\"%s\"", cmd); /* cmd.exe strips one pair of outer quotes */
    int rc = zv_system(full);
#else
    int rc = zv_system(cmd);
    if (rc != -1 && (rc & 0x7f) == 0) rc = (rc >> 8) & 0xff;
#endif
    return rc;
}

/* Run `cmd` and capture the first line of its output. */
static char *first_line(const char *cmd) {
    FILE *p = POPEN(cmd, "r");
    if (!p) return NULL;
    char buf[512];
    char *res = NULL;
    if (fgets(buf, sizeof buf, p)) {
        size_t n = strlen(buf);
        while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
        res = zstrdup(buf);
    }
    PCLOSE(p);
    return res;
}

/* ---- clang ---- */

/* Returns the major version, 0 when unknown, -1 when it does not run. */
static int clang_major(const char *path) {
#ifdef _WIN32
    char *cmd = zsprintf("\"%s --version 2>NUL\"", quote(path));
#else
    char *cmd = zsprintf("%s --version 2>/dev/null", quote(path));
#endif
    char *line = first_line(cmd);
    if (!line) return -1;
    if (strstr(line, "Apple clang")) return 99; /* Apple's numbering differs; it is recent enough */
    char *v = strstr(line, "clang version ");
    if (!v) return -1;
    return atoi(v + 14);
}

static void print_install_help(const char *why) {
    fprintf(stderr, "error: %s\n", why);
    fprintf(stderr,
            "zevic needs clang 15 or newer to build programs. Install LLVM with one of:\n"
            "  Ubuntu / Debian:  sudo apt install clang\n"
            "  Windows:          winget install LLVM.LLVM\n"
            "  macOS:            brew install llvm\n"
            "Then run zevic again. If clang is installed somewhere else, set ZEVIC_CLANG\n"
            "to its path or pass --cc <path>.\n");
}

char *driver_find_clang(const char *cli_cc) {
    Vec cands = {0};
    if (cli_cc) vec_push(&cands, (void *)cli_cc);
    const char *env = zv_getenv("ZEVIC_CLANG");
    if (env && *env) vec_push(&cands, (void *)env);
    vec_push(&cands, "clang");
#ifdef _WIN32
    vec_push(&cands, "C:\\Program Files\\LLVM\\bin\\clang.exe");
    vec_push(&cands, "C:\\Program Files (x86)\\LLVM\\bin\\clang.exe");
#else
    vec_push(&cands, "/opt/homebrew/opt/llvm/bin/clang");
    vec_push(&cands, "/usr/local/opt/llvm/bin/clang");
    for (int v = 22; v >= 15; v--) vec_push(&cands, zsprintf("/usr/lib/llvm-%d/bin/clang", v));
#endif
    int found_old = 0;
    for (int i = 0; i < cands.n; i++) {
        const char *c = cands.v[i];
        int m = clang_major(c);
        if (m >= 15 || m == 0) return zstrdup(c);
        if (m > 0 && m < 15) found_old = m;
        if (cli_cc && i == 0) {
            print_install_help(zsprintf("`%s` is not a working clang", cli_cc));
            return NULL;
        }
    }
    if (found_old) print_install_help(zsprintf("clang %d was found, but clang 15 or newer is required", found_old));
    else print_install_help("clang was not found");
    return NULL;
}


/* ---- the runtime object cache ----
 * The runtime and the garbage collector are C code. They are compiled once into object files that are kept in a
 * cache directory (keyed by their contents and the compiler), so a build only has to compile and link the program. */

static uint64_t fnv_add(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool make_dirs(const char *path) {
    char *p = zstrdup(path);
    for (char *c = p + 1; *c; c++) {
        if (*c == '/' || *c == '\\') {
            char save = *c;
            *c = 0;
            if (!dir_exists(p)) {
#ifdef _WIN32
                zv_mkdir(p);
#else
                zv_mkdir(p);
#endif
            }
            *c = save;
        }
    }
    if (!dir_exists(p)) {
#ifdef _WIN32
        zv_mkdir(p);
#else
        zv_mkdir(p);
#endif
    }
    return dir_exists(p);
}

static char *cache_dir(void) {
    const char *e = zv_getenv("ZEVIC_CACHE");
    char *dir = NULL;
    if (e && *e) {
        dir = zstrdup(e);
    } else {
#ifdef _WIN32
        const char *base = zv_getenv("LOCALAPPDATA");
        if (base && *base) dir = zsprintf("%s/zevic/cache", base);
#else
        const char *xdg = zv_getenv("XDG_CACHE_HOME");
        const char *home = zv_getenv("HOME");
        if (xdg && *xdg) dir = zsprintf("%s/zevic", xdg);
        else if (home && *home) dir = zsprintf("%s/.cache/zevic", home);
#endif
    }
    if (dir && make_dirs(dir)) return dir;
    return NULL;
}

/* The path of a cached object for `src`, compiling it first if needed. NULL when it cannot be cached. */
static char *cached_object(const char *clang, const char *label, const char *src, const char **key_files, const char *cflags) {
    char *dir = cache_dir();
    if (!dir) return NULL;
    uint64_t h = 1469598103934665603ULL;
    h = fnv_add(h, clang, strlen(clang));
    h = fnv_add(h, cflags, strlen(cflags));
    for (int i = 0; key_files[i]; i++) {
        size_t len = 0;
        char *text = read_file(key_files[i], &len);
        if (!text) return NULL;
        h = fnv_add(h, text, len);
    }
    char *obj = zsprintf("%s/%s-%016llx.o", dir, label, (unsigned long long)h);
    if (file_exists(obj)) return obj;
    char *tmp = zsprintf("%s.tmp%ld", obj, (long)
#ifdef _WIN32
                         GetCurrentProcessId()
#else
                         getpid()
#endif
    );
    SB cmd = {0};
    sb_printf(&cmd, "%s -c -x c %s %s -o %s", quote(clang), cflags, quote(src), quote(tmp));
    if (run(sb_str(&cmd)) != 0) {
        zv_remove(tmp);
        return NULL;
    }
    if (zv_rename(tmp, obj) != 0) {
#ifdef _WIN32
        /* another build may have won the race */
        zv_remove(tmp);
        if (!file_exists(obj)) return NULL;
#else
        zv_remove(tmp);
        return NULL;
#endif
    }
    return obj;
}

static bool is_cpp_file(const char *f) {
    size_t n = strlen(f);
    return (n > 4 && strcmp(f + n - 4, ".cpp") == 0) || (n > 3 && strcmp(f + n - 3, ".cc") == 0) ||
           (n > 4 && strcmp(f + n - 4, ".cxx") == 0);
}

/* clang++ next to clang: "clang" -> "clang++", "clang.exe" -> "clang++.exe", "clang-18" -> "clang++-18". */
static char *sibling_cxx(const char *clang) {
    const char *exe = "";
    size_t n = strlen(clang);
    if (n > 4 && strcmp(clang + n - 4, ".exe") == 0) {
        exe = ".exe";
        n -= 4;
    }
    const char *dash = NULL; /* a version suffix such as -18 */
    for (size_t i = n; i > 0 && clang[i - 1] != '/' && clang[i - 1] != '\\'; i--)
        if (clang[i - 1] == '-') {
            dash = clang + i - 1;
            break;
        }
    size_t stem = dash ? (size_t)(dash - clang) : n;
    return zsprintf("%.*s++%.*s%s", (int)stem, clang, (int)(n - stem), clang + stem, exe);
}

/* the runtime and the collector are built with thread support (Zevic.Async) */
#ifdef _WIN32
/* NO_MSGBOX_ON_ERROR: the collector reports fatal errors on stderr, not in a dialog (which would also need user32) */
#define ZEVIC_THREAD_CFLAGS "-DGC_THREADS -DGC_BUILTIN_ATOMIC -DNO_MSGBOX_ON_ERROR"
#else
#define ZEVIC_THREAD_CFLAGS "-DGC_THREADS -DGC_BUILTIN_ATOMIC -pthread"
#endif

int driver_build(const char *clang, const char *home, const char *ll_path, const char *out_path, BuildOpts *o) {
    SB cmd = {0};
    bool has_cpp = false;
    for (int i = 0; i < o->inputs.n; i++)
        if (is_cpp_file(o->inputs.v[i])) has_cpp = true;
    /* C++ code needs the C++ driver, so the standard library gets linked */
    const char *driver = has_cpp ? (o->cxx ? o->cxx : sibling_cxx(clang)) : clang;
    sb_puts(&cmd, quote(driver));
    sb_printf(&cmd, " %s -w -Wno-override-module", o->opt ? o->opt : "-O0");
    if (o->debug) sb_puts(&cmd, " -g");
    sb_printf(&cmd, " %s", quote(ll_path));
    /* the runtime and the garbage collector: cached objects, or the sources when there is no cache */
    char *gc_inc = zsprintf("%s/runtime/third_party/bgc/include", home);
    char *rt_src = zsprintf("%s/runtime/zevic_rt.c", home);
    char *gc_src = zsprintf("%s/runtime/third_party/bgc/extra/gc.c", home);
    char *cflags = zsprintf("-O2 -w " ZEVIC_THREAD_CFLAGS "%s -I%s", o->pic ? " -fPIC" : "", quote(gc_inc));
    const char *rt_keys[] = {rt_src, zsprintf("%s/runtime/zevic_rt.h", home), NULL};
    const char *gc_keys[] = {zsprintf("%s/runtime/third_party/bgc/VENDORED.txt", home), NULL};
    char *rt_obj = cached_object(clang, "zevic_rt", rt_src, rt_keys, cflags);
    char *gc_obj = cached_object(clang, "bgc", gc_src, gc_keys, cflags);
    if (rt_obj) sb_printf(&cmd, " %s", quote(rt_obj));
    else sb_printf(&cmd, " -x c %s -x none -I%s", quote(rt_src), quote(gc_inc));
    if (gc_obj) sb_printf(&cmd, " %s", quote(gc_obj));
    else sb_printf(&cmd, " -x c %s -x none -I%s", quote(gc_src), quote(gc_inc));
    for (int i = 0; i < o->inputs.n; i++) {
        const char *f = o->inputs.v[i];
        size_t fl = strlen(f);
        bool is_c = fl > 2 && strcmp(f + fl - 2, ".c") == 0;
        /* clang++ would read a .c file as C++ */
        if (is_c && has_cpp) sb_printf(&cmd, " -x c %s -x none", quote(f));
        else sb_printf(&cmd, " %s", quote(f));
    }
    for (int i = 0; i < o->incdirs.n; i++) sb_printf(&cmd, " -I%s", quote(o->incdirs.v[i]));
    for (int i = 0; i < o->defines.n; i++) sb_printf(&cmd, " -D%s", quote(o->defines.v[i]));
    for (int i = 0; i < o->libdirs.n; i++) sb_printf(&cmd, " -L%s", quote(o->libdirs.v[i]));
    for (int i = 0; i < o->libs.n; i++) sb_printf(&cmd, " -l%s", (char *)o->libs.v[i]);
    sb_printf(&cmd, " -o %s", quote(out_path));
#ifndef _WIN32
    sb_puts(&cmd, " -lm -pthread");
#else
    sb_puts(&cmd, " -lws2_32");
#endif
    return run(sb_str(&cmd));
}

/* clang's directory + `name`, if such a file exists there ("llvm-ar" next to "clang"). */
static char *sibling_tool(const char *clang, const char *name) {
    const char *last = NULL;
    for (const char *c = clang; *c; c++)
        if (*c == '/' || *c == '\\') last = c;
    if (!last) return NULL;
    char *path = zsprintf("%.*s/%s%s", (int)(last - clang), clang, name,
#ifdef _WIN32
                          ".exe"
#else
                          ""
#endif
    );
    return file_exists(path) ? path : NULL;
}

static int compile_object(const char *driver, BuildOpts *o, const char *src, const char *obj, bool force_c) {
    SB cmd = {0};
    sb_printf(&cmd, "%s -c -w -Wno-override-module %s", quote(driver), o->opt ? o->opt : "-O0");
    if (o->debug) sb_puts(&cmd, " -g");
    if (o->pic) sb_puts(&cmd, " -fPIC");
    if (force_c) sb_puts(&cmd, " -x c");
    sb_printf(&cmd, " %s", quote(src));
    for (int i = 0; i < o->incdirs.n; i++) sb_printf(&cmd, " -I%s", quote(o->incdirs.v[i]));
    for (int i = 0; i < o->defines.n; i++) sb_printf(&cmd, " -D%s", quote(o->defines.v[i]));
    sb_printf(&cmd, " -o %s", quote(obj));
    return run(sb_str(&cmd));
}

int driver_build_lib(const char *clang, const char *home, const char *ll_path, const char *out_path, BuildOpts *o) {
    Vec objs = {0};
    char *base = zsprintf("%s.objs", out_path);
    make_dirs(base);
    /* the program */
    char *main_obj = zsprintf("%s/zevic.o", base);
    if (compile_object(clang, o, ll_path, main_obj, false) != 0) return 1;
    vec_push(&objs, main_obj);
    /* the C and C++ files */
    for (int i = 0; i < o->inputs.n; i++) {
        const char *f = o->inputs.v[i];
        size_t fl = strlen(f);
        if (fl > 2 && (strcmp(f + fl - 2, ".o") == 0)) {
            vec_push(&objs, (void *)f);
            continue;
        }
        if ((fl > 2 && strcmp(f + fl - 2, ".a") == 0) || (fl > 4 && strcmp(f + fl - 4, ".lib") == 0)) {
            fprintf(stderr, "error: a library (`%s`) cannot be put into another library; pass it with -l when you link\n", f);
            return 1;
        }
        char *obj = zsprintf("%s/native%d.o", base, i);
        const char *drv = is_cpp_file(f) ? (o->cxx ? o->cxx : sibling_cxx(clang)) : clang;
        if (compile_object(drv, o, f, obj, false) != 0) return 1;
        vec_push(&objs, obj);
    }
    /* the runtime and the garbage collector travel with the library, so the C side needs nothing else */
    char *gc_inc = zsprintf("%s/runtime/third_party/bgc/include", home);
    char *rt_src = zsprintf("%s/runtime/zevic_rt.c", home);
    char *gc_src = zsprintf("%s/runtime/third_party/bgc/extra/gc.c", home);
    char *cflags = zsprintf("-O2 -w " ZEVIC_THREAD_CFLAGS "%s -I%s", o->pic ? " -fPIC" : "", quote(gc_inc));
    const char *rt_keys[] = {rt_src, zsprintf("%s/runtime/zevic_rt.h", home), NULL};
    const char *gc_keys[] = {zsprintf("%s/runtime/third_party/bgc/VENDORED.txt", home), NULL};
    char *rt_obj = cached_object(clang, "zevic_rt", rt_src, rt_keys, cflags);
    char *gc_obj = cached_object(clang, "bgc", gc_src, gc_keys, cflags);
    BuildOpts rt_opts = {0};
    rt_opts.opt = "-O2";
    rt_opts.pic = o->pic;
    vec_push(&rt_opts.incdirs, gc_inc);
    if (!rt_obj) {
        rt_obj = zsprintf("%s/zevic_rt.o", base);
        if (compile_object(clang, &rt_opts, rt_src, rt_obj, true) != 0) return 1;
    }
    if (!gc_obj) {
        gc_obj = zsprintf("%s/bgc.o", base);
        if (compile_object(clang, &rt_opts, gc_src, gc_obj, true) != 0) return 1;
    }
    vec_push(&objs, rt_obj);
    vec_push(&objs, gc_obj);
    /* the archive */
    char *ar = sibling_tool(clang, "llvm-ar");
    zv_remove(out_path);
    SB cmd = {0};
    sb_printf(&cmd, "%s rcs %s", quote(ar ? ar : "ar"), quote(out_path));
    for (int i = 0; i < objs.n; i++) sb_printf(&cmd, " %s", quote(objs.v[i]));
    int rc = run(sb_str(&cmd));
    if (rc != 0) fprintf(stderr, "error: cannot make the archive (`%s` failed; install binutils or LLVM)\n", ar ? "llvm-ar" : "ar");
    if (!o->keep_ll) {
        zv_remove(main_obj);
        for (int i = 0; i < o->inputs.n; i++) zv_remove(zsprintf("%s/native%d.o", base, i));
        zv_remove(zsprintf("%s/zevic_rt.o", base));
        zv_remove(zsprintf("%s/bgc.o", base));
#ifdef _WIN32
        _rmdir(base);
#else
        rmdir(base);
#endif
    }
    return rc;
}

void driver_remove(const char *path) {
    zv_remove(path);
}
