/* main.c - command line entry point of zevic0 */
#include <stdarg.h>
#include "ast.h"
#include "driver.h"
#include "gen.h"
#include "lexer.h"
#include "module.h"
#include "parser.h"
#include "types.h"
#include "util.h"

#ifndef ZEVIC_VERSION
#define ZEVIC_VERSION "dev" /* the Makefile passes the contents of VERSION */
#endif

static void usage(void) {
    fputs("usage: zevic <file.zev>... [native files] [-o output]\n"
          "\n"
          "  -o <file>          output file\n"
          "  --test             build the test runner (every `test` block) instead of the program\n"
          "  --lib              build a static library (with --emit-header for the C header)\n"
          "  --pic              with --lib: position independent code, so the archive can be linked into a shared library\n"
          "  --emit-ll          write the LLVM IR and stop\n"
          "  --emit-header <f>  also write a C header for the `export \"C\"` functions\n"
          "  -O0 -O1 -O2        optimisation level (default -O0)\n"
          "  -g                 debug information\n"
          "  -l<name> -L<dir> -I<dir> -D<k=v>   options passed to clang\n"
          "  --cc <path>        use this clang\n"
          "  --version          print the version\n"
          "\n"
          "Files ending in .c .cpp .o .a are linked into the program.\n",
          stderr);
}

static bool ends_with(const char *s, const char *suffix) {
    size_t n = strlen(s), m = strlen(suffix);
    return n >= m && strcmp(s + n - m, suffix) == 0;
}

static char *dir_name(const char *path) {
    const char *last = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') last = p;
    return last ? zstrndup(path, (size_t)(last - path)) : zstrdup(".");
}

static char *base_no_ext(const char *path) {
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    const char *dot = strrchr(b, '.');
    return dot ? zstrndup(b, (size_t)(dot - b)) : zstrdup(b);
}

/* ---- debugging dumps ---- */

static void dump_tokens(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) die("cannot read %s", path);
    int file = src_add(path, text);
    TokenList tl = lex(file, text, len, 1, 1);
    for (int i = 0; i < tl.n; i++) {
        Token *t = &tl.v[i];
        printf("%d:%d %s", t->pos.line, t->pos.col, tok_name(t->k));
        switch (t->k) {
        case TK_IDENT: printf(" %s%s", t->text, t->quoted ? " (quoted)" : ""); break;
        case TK_INT: case TK_CHAR: case TK_BYTE:
            printf(" %llu", (unsigned long long)t->ival);
            if (t->suffix) printf(" _%s", t->suffix);
            break;
        case TK_FLOAT:
            printf(" %g", t->fval);
            if (t->suffix) printf(" _%s", t->suffix);
            break;
        case TK_STRING:
            for (int k = 0; k < t->nparts; k++) {
                StrPart *p = &t->parts[k];
                printf(" %s[", p->is_expr ? "expr" : "text");
                for (int j = 0; j < p->n; j++) {
                    unsigned char c = (unsigned char)p->s[j];
                    if (c == '\n') printf("\\n");
                    else if (c < 32) printf("\\x%02x", c);
                    else putchar(c);
                }
                printf("]");
            }
            break;
        default: break;
        }
        printf("\n");
    }
}

static void dump_ast(const char *path) {
    size_t len;
    char *text = read_file(path, &len);
    if (!text) die("cannot read %s", path);
    int file = src_add(path, text);
    SourceFile *sf = parse_file(file, text, len);
    if (!zv_error_count) ast_dump(sf, stdout);
}


/* `--selftest-types`: build a fixed set of types and print their names, sizes and LLVM types (tests the Zevic port) */
static void st_show(Type *t) {
    ty_layout(t);
    printf("%s | size=%lld align=%lld ll=%s words=%d id=%d\n", t->name, (long long)t->size, (long long)t->align, t->ll,
           t->payload_words, t->id);
}

static Vec st_vec(int n, ...) {
    Vec v = {0};
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n; i++) vec_push(&v, va_arg(ap, Type *));
    va_end(ap);
    return v;
}

static void st_field(Type *t, const char *name, Type *ft) {
    Field *f = zalloc(sizeof *f);
    f->name = zstrdup(name);
    f->t = ft;
    vec_push(&t->fields, f);
}

static void st_variant(Type *t, const char *name, int tag, Vec fts) {
    Variant *v = zalloc(sizeof *v);
    v->name = zstrdup(name);
    v->tag = tag;
    for (int i = 0; i < fts.n; i++) {
        Field *f = zalloc(sizeof *f);
        f->name = zsprintf("%d", i);
        f->t = fts.v[i];
        vec_push(&v->fields, f);
    }
    if (fts.n) v->payload = ty_tuple(fts);
    vec_push(&t->variants, v);
}

static void selftest_types(void) {
    types_init();
    Type *all[] = {ty_i8, ty_i16, ty_i32, ty_i64, ty_u8, ty_u16, ty_u32, ty_u64, ty_f32, ty_f64, ty_bool, ty_char, ty_string, ty_unit, ty_never};
    for (size_t i = 0; i < sizeof all / sizeof *all; i++) st_show(all[i]);
    st_show(ty_tuple(st_vec(2, ty_i32, ty_string)));
    st_show(ty_tuple(st_vec(0)));
    st_show(ty_tuple(st_vec(3, ty_i8, ty_i64, ty_u16)));
    Type *fn = ty_fn(st_vec(2, ty_i32, ty_bool), ty_unit);
    st_show(fn);
    st_show(ty_ptr(ty_i8));
    st_show(ty_param("T"));
    const char *cs[] = {"CChar", "CInt", "CUInt", "CLong", "CULong", "CSizeT", "CString", "CVoid", "CBool", "CDouble", "CFloat"};
    for (size_t i = 0; i < sizeof cs / sizeof *cs; i++) st_show(ty_c_by_name(cs[i]));
    printf("cbyname unknown: %s\n", ty_c_by_name("CNope") ? "found" : "none");
    st_show(ty_ctype("MyC", 2, true));
    printf("prim String: %s, prim Never: %s\n", ty_prim_by_name("String") ? "yes" : "no", ty_prim_by_name("Never") ? "yes" : "no");
    Type *u1 = ty_union(st_vec(2, ty_string, ty_i32));
    st_show(u1);
    Type *u2 = ty_union(st_vec(3, ty_i32, ty_string, ty_bool));
    st_show(u2);
    Type *u3 = ty_union(st_vec(2, ty_union(st_vec(2, ty_i32, ty_bool)), ty_string));
    printf("same: %s\n", u2 == u3 ? "yes" : "no");
    printf("one member: %s\n", ty_union(st_vec(2, ty_i32, ty_i32))->name);
    st_show(ty_union(st_vec(2, fn, ty_i32)));
    st_show(ty_union(st_vec(2, ty_unit, ty_i32)));
    st_show(ty_union(st_vec(2, ty_unit, ty_bool)));
    st_show(ty_union(st_vec(2, ty_tuple(st_vec(3, ty_i32, ty_i32, ty_i32)), ty_i8)));
    for (int i = 0; i < u2->variants.n; i++) {
        Variant *v = u2->variants.v[i];
        printf("  variant %s tag=%d payload=%s\n", v->name, v->tag, v->payload->name);
    }
    Type *pt = ty_named(T_STRUCT, NULL, "Main.Point", (Vec){0});
    st_field(pt, "X", ty_i32);
    st_field(pt, "Y", ty_f64);
    st_show(pt);
    Type *empty = ty_named(T_STRUCT, NULL, "Main.Empty", (Vec){0});
    st_show(empty);
    Type *holder = ty_named(T_STRUCT, NULL, "Main.Holder", (Vec){0});
    st_field(holder, "A", ty_bool);
    st_field(holder, "U", u2);
    st_field(holder, "P", pt);
    st_show(holder);
    Type *sh = ty_named(T_ENUM, NULL, "Main.Shape", (Vec){0});
    st_variant(sh, "Circle", 0, st_vec(1, ty_f64));
    st_variant(sh, "Rect", 1, st_vec(2, ty_f64, ty_f64));
    st_variant(sh, "Dot", 2, st_vec(0));
    st_show(sh);
    Type *nothing = ty_named(T_ENUM, NULL, "Main.Color", (Vec){0});
    st_variant(nothing, "Red", 0, st_vec(0));
    st_variant(nothing, "Blue", 1, st_vec(0));
    st_show(nothing);
    Type *box = ty_named(T_CLASS, NULL, "Main.Box", st_vec(1, ty_i32));
    st_show(box);
    Type *map = ty_named(T_CLASS, NULL, "Main.Map", st_vec(2, ty_string, ty_union(st_vec(2, ty_i32, ty_string))));
    st_show(map);
    st_show(ty_fn(st_vec(1, u1), u2));
    printf("is_int %d%d%d signed %d%d float %d%d value_agg %d%d%d bits %d%d%d%d\n", ty_is_int(ty_u8), ty_is_int(ty_f32), ty_is_int(ty_bool),
           ty_is_signed(ty_i8), ty_is_signed(ty_u8), ty_is_float(ty_f32), ty_is_float(ty_i32), ty_is_value_agg(u2), ty_is_value_agg(box),
           ty_is_value_agg(ty_string), ty_bits(ty_i8), ty_bits(ty_u16), ty_bits(ty_i32), ty_bits(ty_bool));
}

/* `--dump-modules <file>...`: load the files the way a build does, resolve the imports and print the module table */
static Program *load_program(int nfiles, char **files) {
    char *home = driver_find_home("zevic");
    if (!home) die("cannot find the Zevic standard library");
    Program *pg = prog_new(zsprintf("%s/packages/Zevic/src", home));
    vec_push(&pg->search_dirs, dir_name(files[0]));
    vec_push(&pg->search_dirs, zstrdup("."));
    for (int i = 0; i < nfiles; i++)
        if (!prog_load_file(pg, files[i], false)) die("cannot read %s", files[i]);
    char *core = zsprintf("%s/packages/Zevic/src/Core.zev", home);
    if (file_exists(core)) prog_load_file(pg, core, true);
    prog_resolve(pg);
    return pg;
}

static void dump_types(int nfiles, char **files) {
    Program *pg = load_program(nfiles, files);
    if (zv_error_count) return;
    gen_dump_types(gen_new(pg));
}

static void dump_modules(int nfiles, char **files) {
    Program *pg = load_program(nfiles, files);
    if (zv_error_count) return;
    for (int i = 0; i < pg->files.n; i++) {
        SourceFile *sf = pg->files.v[i];
        printf("file %s%s\n", src_name(sf->pos.file), sf->is_std ? " std" : "");
    }
    for (int i = 0; i < pg->modules.n; i++) {
        Module *m = pg->modules.v[i];
        printf("module %s%s\n", m->name, m->is_std ? " std" : "");
        for (int k = 0; k < m->deps.n; k++) printf("  dep %s\n", ((Module *)m->deps.v[k])->name);
        for (int k = 0; k < m->files.n; k++) {
            SourceFile *sf = m->files.v[k];
            printf("  source %s\n", src_name(sf->pos.file));
            for (int j = 0; j < sf->imports.n; j++) {
                Node *im = sf->imports.v[j];
                char *name = path_join(&im->path, ".");
                const char *key = im->name ? im->name : name;
                Module *t = dict_get(&sf->import_map, key);
                printf("    import %s -> %s\n", key, t ? t->name : "?");
                if (!im->name && strncmp(name, "Zevic.", 6) == 0) {
                    t = dict_get(&sf->import_map, name + 6);
                    printf("    import %s -> %s\n", name + 6, t ? t->name : "?");
                }
            }
            for (int j = 0; j < sf->decls.n; j++) {
                Node *d = sf->decls.v[j];
                if (d->kind == D_TEST) continue;
                if (d->kind == D_EXTERN) {
                    for (int q = 0; q < d->list.n; q++) {
                        Node *e = d->list.v[q];
                        Decl *x = module_find(m, e->name);
                        printf("    decl %s %s abi=%s\n", e->name, x && x->is_pub ? "pub" : "priv", x && x->abi ? x->abi : "-");
                    }
                    continue;
                }
                Decl *x = module_find(m, d->name);
                printf("    decl %s %s\n", d->name, x && x->is_pub ? "pub" : "priv");
            }
        }
        for (int k = 0; k < m->tests.n; k++) {
            Decl *t = m->tests.v[k];
            printf("  test %s %s\n", t->name, t->test_name);
        }
    }
}

static const char *path_base_of(const char *path) {
    const char *b = path;
    for (const char *c = path; *c; c++)
        if (*c == '/' || *c == '\\') b = c + 1;
    return b;
}

int main(int argc, char **argv) {
    zv_utf8_args(&argc, &argv);
    BuildOpts opts = {0};
    Vec zev_files = {0};
    const char *out = NULL;
    bool emit_ll = false;
    bool test_mode = false;
    bool lib_mode = false;
    const char *header_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--dump-tokens") == 0 && i + 1 < argc) {
            dump_tokens(argv[i + 1]);
            return zv_error_count ? 1 : 0;
        } else if (strcmp(a, "--dump-ast") == 0 && i + 1 < argc) {
            dump_ast(argv[i + 1]);
            return zv_error_count ? 1 : 0;
        } else if (strcmp(a, "--selftest-types") == 0) {
            selftest_types();
            return 0;
        } else if (strcmp(a, "--dump-types") == 0 && i + 1 < argc) {
            dump_types(argc - i - 1, argv + i + 1);
            return zv_error_count ? 1 : 0;
        } else if (strcmp(a, "--dump-modules") == 0 && i + 1 < argc) {
            dump_modules(argc - i - 1, argv + i + 1);
            return zv_error_count ? 1 : 0;
        } else if (strcmp(a, "--version") == 0) {
            printf("zevic %s\n", ZEVIC_VERSION);
            return 0;
        } else if (strcmp(a, "--print-stdlib") == 0) {
            char *home = driver_find_home(argv[0]);
            if (!home) return 2;
            printf("%s/packages/Zevic\n", home);
            return 0;
        } else if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return 0;
        } else if (strcmp(a, "-o") == 0 && i + 1 < argc) {
            out = argv[++i];
        } else if (strcmp(a, "--emit-header") == 0 && i + 1 < argc) {
            header_path = argv[++i];
        } else if (strcmp(a, "--lib") == 0) {
            lib_mode = true;
        } else if (strcmp(a, "--pic") == 0) {
            opts.pic = true;
        } else if (strcmp(a, "--test") == 0) {
            test_mode = true;
        } else if (strcmp(a, "--emit-ll") == 0) {
            emit_ll = true;
        } else if (strcmp(a, "--keep-ll") == 0) {
            opts.keep_ll = true;
        } else if (strcmp(a, "-O0") == 0 || strcmp(a, "-O1") == 0 || strcmp(a, "-O2") == 0) {
            opts.opt = a;
        } else if (strcmp(a, "-g") == 0) {
            opts.debug = true;
        } else if (strcmp(a, "--cc") == 0 && i + 1 < argc) {
            opts.cc = argv[++i];
        } else if (strcmp(a, "--cxx") == 0 && i + 1 < argc) {
            opts.cxx = argv[++i];
        } else if (strncmp(a, "-l", 2) == 0 && a[2]) {
            vec_push(&opts.libs, (void *)(a + 2));
        } else if (strncmp(a, "-L", 2) == 0 && a[2]) {
            vec_push(&opts.libdirs, (void *)(a + 2));
        } else if (strncmp(a, "-I", 2) == 0 && a[2]) {
            vec_push(&opts.incdirs, (void *)(a + 2));
        } else if (strncmp(a, "-D", 2) == 0 && a[2]) {
            vec_push(&opts.defines, (void *)(a + 2));
        } else if (ends_with(a, ".zev")) {
            vec_push(&zev_files, (void *)a);
        } else if (ends_with(a, ".c") || ends_with(a, ".cpp") || ends_with(a, ".cc") || ends_with(a, ".o") ||
                   ends_with(a, ".a") || ends_with(a, ".obj") || ends_with(a, ".lib")) {
            vec_push(&opts.inputs, (void *)a);
        } else if (a[0] == '-') {
            fprintf(stderr, "error: unknown option `%s`\n", a);
            usage();
            return 1;
        } else {
            fprintf(stderr, "error: don't know what to do with `%s` (expected a .zev file)\n", a);
            return 1;
        }
    }
    if (!test_mode) {
        /* `_test.zev` files only belong to the test build */
        size_t keep = 0;
        for (int i = 0; i < zev_files.n; i++)
            if (!ends_with(zev_files.v[i], "_test.zev")) zev_files.v[keep++] = zev_files.v[i];
        zev_files.n = (int)keep;
    }
    if (zev_files.n == 0) {
        usage();
        return 1;
    }

    char *home = driver_find_home(argv[0]);
    if (!home) {
        fprintf(stderr, "error: cannot find the Zevic standard library and runtime.\n"
                        "Set ZEVIC_HOME to the directory that contains `packages/Zevic/` and `runtime/`.\n"
                        "Looked in:\n%s",
                driver_home_report());
        return 2;
    }

    Program *pg = prog_new(zsprintf("%s/packages/Zevic/src", home));
    vec_push(&pg->search_dirs, dir_name(zev_files.v[0]));
    vec_push(&pg->search_dirs, zstrdup("."));
    for (int i = 0; i < zev_files.n; i++) {
        if (!prog_load_file(pg, zev_files.v[i], false)) {
            fprintf(stderr, "error: cannot read `%s`\n", (char *)zev_files.v[i]);
            return 1;
        }
    }
    char *core = zsprintf("%s/packages/Zevic/src/Core.zev", home);
    if (file_exists(core)) prog_load_file(pg, core, true);
    prog_resolve(pg);

    SB ll = {0};
    Gen *g = gen_new(pg);
    if (test_mode) gen_set_test_mode(g);
    if (lib_mode) gen_set_lib_mode(g);
    bool ok = zv_error_count == 0 && gen_program(g, &ll);
    if (!ok || zv_error_count) {
        fprintf(stderr, "%d error%s found\n", zv_error_count, zv_error_count == 1 ? "" : "s");
        return 1;
    }

    char *base = base_no_ext(zev_files.v[0]);
    if (header_path) {
        SB h = {0};
        SB guard = {0};
        for (const char *c = path_base_of(header_path); *c; c++)
            sb_putc(&guard, (*c >= 'a' && *c <= 'z') ? (char)(*c - 32) : ((*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9')) ? *c : '_');
        gen_header(g, zsprintf("ZEVIC_%s", sb_str(&guard)), &h);
        FILE *hf = zv_fopen(header_path, "wb");
        if (!hf) {
            fprintf(stderr, "error: cannot write `%s`\n", header_path);
            return 2;
        }
        fwrite(h.p, 1, h.len, hf);
        fclose(hf);
    }
    if (emit_ll) {
        const char *path = out ? out : zsprintf("%s.ll", base);
        FILE *f = zv_fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "error: cannot write `%s`\n", path);
            return 2;
        }
        fwrite(ll.p, 1, ll.len, f);
        fclose(f);
        return 0;
    }

    char *clang = driver_find_clang(opts.cc);
    if (!clang) return 2;
#ifdef _WIN32
    const char *exe_suffix = ".exe";
#else
    const char *exe_suffix = "";
#endif
    char *exe = out ? zstrdup(out) : zsprintf("%s%s", base, exe_suffix);
    char *ll_path = zsprintf("%s.zevic.ll", exe);
    FILE *f = zv_fopen(ll_path, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot write `%s`\n", ll_path);
        return 2;
    }
    fwrite(ll.p, 1, ll.len, f);
    fclose(f);
    int rc = lib_mode ? driver_build_lib(clang, home, ll_path, exe, &opts) : driver_build(clang, home, ll_path, exe, &opts);
    if (!opts.keep_ll) driver_remove(ll_path);
    if (rc != 0) {
        fprintf(stderr, "error: clang failed (exit code %d) while building `%s`\n", rc, exe);
        return 2;
    }
    return 0;
}
