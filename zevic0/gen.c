#include "gen.h"

#include "types.h"

#include <math.h>

/* =========================================================================
 * State
 * ========================================================================= */

/* A mapping from type parameter names to types, plus `Self`. */
typedef struct Env {
    Vec names; /* char* */
    Vec types; /* Type* */
    Type *self;
} Env;

/* A function (or method) with its type arguments fixed: one LLVM function. */
typedef struct Inst {
    Decl *decl;
    Type *self;     /* owning type for methods */
    Env *env;
    char *name;     /* LLVM symbol without the @ */
    Vec ptypes;     /* Type* */
    Type *ret;
    bool emitted;
    bool is_extern;
    bool is_export; /* `export "C"`: the symbol is the plain name and the signature is C's */
    bool has_this; /* a method: the first parameter is `ptr %this` */
} Inst;

struct Gen {
    Program *pg;
    SB globals;     /* string constants */
    SB decls;       /* declare ... */
    SB funcs;       /* define ... */
    Dict declared;
    Dict insts;
    Vec worklist;
    Dict thunks;    /* thunk symbol -> the function it forwards to */
    Dict noenv;     /* "fn:%tN" -> symbol of a lambda that captures nothing (so it can go to C as a callback) */
    int nlambda;
    Dict defaults;  /* Decl* of default methods, per implementing type */
    int nvt;        /* method table counter */
    Dict vtables;   /* "Type|Interface" -> global name */
    Vec exports; /* Inst*: the `export "C"` functions */
    int nstr;
    Dict strconst;
    int tmp_global;
    bool test_mode; /* build the test runner instead of calling `Main` */
    bool lib_mode;  /* a library: no `Main`, no `main` */
};

/* The type of an expression that already produced an error: it matches everything. */
static Type *ty_err;

Gen *gen_new(Program *pg) {
    types_init();
    ty_err = ty_param("error");
    Gen *g = zalloc(sizeof *g);
    g->pg = pg;
    return g;
}

/* =========================================================================
 * Names
 * ========================================================================= */

static char *mangle_part(const char *s) {
    SB sb = {0};
    for (; *s; s++) {
        if (*s == '.') sb_putc(&sb, '_');
        else sb_putc(&sb, *s);
    }
    return sb_str(&sb);
}

/* Type names can hold characters (<>, ()) that are fine inside quoted LLVM names. */
static char *llname(const char *s) {
    return zsprintf("\"%s\"", s);
}

static Env *env_new(void) {
    return zalloc(sizeof(Env));
}

static void env_add(Env *e, const char *name, Type *t) {
    vec_push(&e->names, zstrdup(name));
    vec_push(&e->types, t);
}

static Type *env_get(Env *e, const char *name) {
    if (!e) return NULL;
    for (int i = e->names.n - 1; i >= 0; i--)
        if (strcmp(e->names.v[i], name) == 0) return e->types.v[i];
    return NULL;
}

/* =========================================================================
 * Declaration lookup
 * ========================================================================= */

static Decl *lookup_in_prelude(Gen *g, const char *name) {
    Module *core = prog_find_module(g->pg, "Zevic.Core");
    if (!core) return NULL;
    Decl *d = module_find(core, name);
    return d && d->is_pub ? d : NULL;
}

/* Find the declaration a dotted path starts with. `*used` is the number of
 * segments consumed (the module prefix plus the declaration name). */
static Decl *find_decl(Gen *g, SourceFile *f, char **segs, int n, int *used, Pos pos) {
    if (!f || !f->mod) return NULL;
    for (int k = n - 1; k >= 1; k--) {
        SB sb = {0};
        for (int i = 0; i < k; i++) sb_printf(&sb, "%s%s", i ? "." : "", segs[i]);
        Module *m = dict_get(&f->import_map, sb_str(&sb));
        if (!m) continue;
        Decl *d = module_find(m, segs[k]);
        if (!d) {
            diag_error(pos, "module `%s` has no member `%s`", m->name, segs[k]);
            *used = -1;
            return NULL;
        }
        if (!d->is_pub && d->mod != f->mod) {
            diag_error(pos, "`%s` is private to module `%s` (names starting with a lowercase letter are not exported)",
                       segs[k], m->name);
            *used = -1;
            return NULL;
        }
        *used = k + 1;
        return d;
    }
    Decl *d = module_find(f->mod, segs[0]);
    if (d) {
        *used = 1;
        return d;
    }
    d = lookup_in_prelude(g, segs[0]);
    if (d) {
        *used = 1;
        return d;
    }
    return NULL;
}

/* =========================================================================
 * Strings
 * ========================================================================= */

static char *ll_escape(const char *s, int n) {
    SB sb = {0};
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') sb_putc(&sb, (char)c);
        else sb_printf(&sb, "\\%02X", c);
    }
    return sb_str(&sb);
}

/* The LLVM constant for a string literal: `{ ptr @.str.N, i64 len }`. */
static char *str_const(Gen *g, const char *s, int n) {
    char *key = zstrndup(s, (size_t)n);
    /* keys cannot contain NULs; make them unique through the length prefix */
    key = zsprintf("%d:%s", n, key);
    char *name = dict_get(&g->strconst, key);
    if (!name) {
        name = zsprintf("@.str.%d", g->nstr++);
        dict_put(&g->strconst, key, name);
        sb_printf(&g->globals, "%s = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n", name, n + 1,
                  ll_escape(s, n));
    }
    return zsprintf("{ ptr %s, i64 %d }", name, n);
}

static void declare(Gen *g, const char *name, const char *line) {
    if (dict_has(&g->declared, name)) return;
    dict_put(&g->declared, name, (void *)line);
    sb_printf(&g->decls, "%s\n", line);
}

/* =========================================================================
 * Types from syntax
 * ========================================================================= */

static Type *resolve_type(Gen *g, SourceFile *f, Env *env, Node *te);
static Pos g_call_pos; /* the call being generated, for errors found while emitting it */
static bool g_allow_selfy_iface; /* set while resolving the interfaces a type implements or extends, and constraints */

static bool syntax_mentions_self(Node *te) {
    if (!te) return false;
    switch (te->kind) {
    case N_TYPE_NAME:
        if (te->path.n == 1 && strcmp(((char **)te->path.v)[0], "Self") == 0) return true;
        for (int i = 0; i < te->list.n; i++)
            if (syntax_mentions_self(te->list.v[i])) return true;
        return false;
    case N_TYPE_TUPLE: case N_TYPE_FN: case N_TYPE_UNION:
        for (int i = 0; i < te->list.n; i++)
            if (syntax_mentions_self(te->list.v[i])) return true;
        return syntax_mentions_self(te->a);
    case N_TYPE_PTR: return syntax_mentions_self(te->a);
    default: return false;
    }
}

/* Does an interface mention `Self` in a method signature? Such an interface only works as a constraint. */
static bool iface_uses_self(Node *decl) {
    for (int i = 0; i < decl->list.n; i++) {
        Node *m = decl->list.v[i];
        if (m->kind != D_FN) continue;
        for (int k = 0; k < m->list.n; k++)
            if (syntax_mentions_self(((Node *)m->list.v[k])->a)) return true;
        if (syntax_mentions_self(m->a)) return true;
    }
    return false;
}

static bool is_err(Type *t) { return t == ty_err; }
static const char *root_name(Node *n);

static char *qualified_name(Decl *d) {
    if (strcmp(d->mod->name, "Zevic.Core") == 0) return d->name;
    return zsprintf("%s.%s", d->mod->name, d->name);
}

static void ensure_members(Gen *g, Type *t);
static void check_conformance(Gen *g, Type *t);
static void check_bounds(Gen *g, SourceFile *f, Node *owner, Vec *targs, Env *base, const char *what, Pos pos);

static Type *instantiate(Gen *g, Decl *d, Vec targs, Pos pos) {
    Node *n = d->node;
    if (n->list2.n != targs.n) {
        if (n->list2.n == 0)
            diag_error(pos, "type `%s` takes no type arguments", d->name);
        else
            diag_error(pos, "type `%s` needs %d type argument(s), found %d", d->name, n->list2.n, targs.n);
        return ty_err;
    }
    if (targs.n) check_bounds(g, d->file, n, &targs, NULL, d->name, pos);
    TKind k = n->kind == D_STRUCT ? T_STRUCT : n->kind == D_CLASS ? T_CLASS : n->kind == D_INTERFACE ? T_IFACE : T_ENUM;
    if (d->mod->is_std && strcmp(d->mod->name, "Zevic.Intrinsics") == 0 && strcmp(d->name, "Buf") == 0) k = T_BUF;
    Type *t = ty_named(k, d, qualified_name(d), targs);
    ensure_members(g, t);
    return t;
}

/* Array<elem>: the class from Zevic.Core. */
static Type *array_type(Gen *g, Type *elem) {
    Decl *d = lookup_in_prelude(g, "Array");
    if (!d) {
        Pos none = {0};
        diag_error(none, "the standard library file Core.zev is missing (cannot find `Array`)");
        return ty_err;
    }
    Vec a = {0};
    vec_push(&a, elem);
    return instantiate(g, d, a, d->node->pos);
}

/* Resolve the fields (struct, class) or variants (enum) of a type. */
static Type *resolve_type(Gen *g, SourceFile *f, Env *env, Node *te);
static void ensure_members(Gen *g, Type *t) {
    if (t->members_done) return;
    t->members_done = true;
    if (!t->decl) return;
    Node *n = t->decl->node;
    Env *env = env_new();
    env->self = t;
    for (int i = 0; i < n->list2.n; i++) env_add(env, ((Node *)n->list2.v[i])->name, t->args.v[i]);
    SourceFile *f = t->decl->file;
    if (n->kind == D_STRUCT || n->kind == D_CLASS) {
        for (int i = 0; i < n->list.n; i++) {
            Node *m = n->list.v[i];
            if (m->kind != N_FIELD || m->flag) continue;
            Field *fl = zalloc(sizeof *fl);
            fl->name = m->name;
            fl->t = resolve_type(g, f, env, m->a);
            vec_push(&t->fields, fl);
        }
    } else if (n->kind == D_ENUM) {
        int tag = 0;
        for (int i = 0; i < n->list.n; i++) {
            Node *m = n->list.v[i];
            if (m->kind != N_VARIANT) continue;
            Variant *v = zalloc(sizeof *v);
            v->name = m->name;
            v->tag = tag++;
            v->named = m->flag;
            Vec ptypes = {0};
            if (m->flag) {
                for (int k = 0; k < m->list2.n; k++) {
                    Node *fn = m->list2.v[k];
                    Field *fl = zalloc(sizeof *fl);
                    fl->name = fn->name;
                    fl->t = resolve_type(g, f, env, fn->a);
                    vec_push(&v->fields, fl);
                    vec_push(&ptypes, fl->t);
                }
            } else {
                for (int k = 0; k < m->list.n; k++) {
                    Field *fl = zalloc(sizeof *fl);
                    fl->name = zsprintf("%d", k);
                    fl->t = resolve_type(g, f, env, m->list.v[k]);
                    vec_push(&v->fields, fl);
                    vec_push(&ptypes, fl->t);
                }
            }
            if (ptypes.n) v->payload = ty_tuple(ptypes);
            vec_push(&t->variants, v);
        }
    }
    if (n->list3.n && n->kind != D_INTERFACE) check_conformance(g, t);
}

static Type *resolve_type(Gen *g, SourceFile *f, Env *env, Node *te) {
    switch (te->kind) {
    case N_TYPE_TUPLE: {
        Vec elems = {0};
        for (int i = 0; i < te->list.n; i++) vec_push(&elems, resolve_type(g, f, env, te->list.v[i]));
        return ty_tuple(elems);
    }
    case N_TYPE_FN: {
        Vec ps = {0};
        for (int i = 0; i < te->list.n; i++) vec_push(&ps, resolve_type(g, f, env, te->list.v[i]));
        return ty_fn(ps, resolve_type(g, f, env, te->a));
    }
    case N_TYPE_PTR:
        return ty_ptr(resolve_type(g, f, env, te->a));
    case N_TYPE_UNION: {
        Vec ms = {0};
        bool bad = false;
        for (int i = 0; i < te->list.n; i++) {
            Type *m = resolve_type(g, f, env, te->list.v[i]);
            if (is_err(m)) {
                bad = true;
            } else if (m == ty_never) {
                diag_error(((Node *)te->list.v[i])->pos, "`Never` cannot be a member of a union");
                bad = true;
            } else {
                vec_push(&ms, m);
            }
        }
        if (bad) return ty_err;
        Type *u = ty_union(ms);
        if (u->k != T_UNION) diag_error(te->pos, "a union needs at least two different types (found only `%s`)", u->name);
        return u;
    }
    case N_TYPE_NAME: {
        char **segs = (char **)te->path.v;
        int n = te->path.n;
        if (n == 1) {
            const char *name = segs[0];
            Type *bound = env_get(env, name);
            if (bound) return bound;
            if (strcmp(name, "Self") == 0) {
                if (!env || !env->self) {
                    diag_error(te->pos, "`Self` can only be used inside a type or interface");
                    return ty_err;
                }
                return env->self;
            }
            Type *p = ty_prim_by_name(name);
            if (p) {
                if (te->list.n) diag_error(te->pos, "type `%s` takes no type arguments", name);
                return p;
            }
            if (strcmp(name, "Never") == 0) return ty_never;
            Type *ct = ty_c_by_name(name);
            if (ct) return ct;
        }
        int used = 0;
        Decl *d = find_decl(g, f, segs, n, &used, te->pos);
        if (used < 0) return ty_err;
        if (!d || used != n) {
            diag_error(te->pos, "unknown type `%s`", path_join(&te->path, "."));
            return ty_err;
        }
        if (d->node->kind != D_STRUCT && d->node->kind != D_CLASS && d->node->kind != D_ENUM && d->node->kind != D_INTERFACE) {
            diag_error(te->pos, "`%s` is not a type", d->name);
            return ty_err;
        }
        if (d->node->kind == D_INTERFACE && !g_allow_selfy_iface && iface_uses_self(d->node)) {
            diag_error(te->pos, "`%s` uses `Self`, so it can only be used as a constraint (`<T: %s>`), not as the type of a value", d->name, d->name);
            return ty_err;
        }
        Vec targs = {0};
        for (int i = 0; i < te->list.n; i++) vec_push(&targs, resolve_type(g, f, env, te->list.v[i]));
        return instantiate(g, d, targs, te->pos);
    }
    default:
        diag_error(te->pos, "expected a type");
        return ty_err;
    }
}

/* =========================================================================
 * --dump-types (tests the Zevic port of the type resolution)
 * ========================================================================= */

static void dump_type(Gen *g, Type *t) {
    (void)g;
    if (is_err(t)) {
        printf("<error>\n");
        return;
    }
    ty_layout(t);
    printf("%s | kind=%d size=%lld align=%lld ll=%s words=%d\n", t->name, (int)t->k, (long long)t->size, (long long)t->align,
           t->ll, t->payload_words);
    for (int i = 0; i < t->fields.n; i++) {
        Field *f = t->fields.v[i];
        printf("  field %s: %s\n", f->name, f->t->name);
    }
    for (int i = 0; i < t->variants.n; i++) {
        Variant *v = t->variants.v[i];
        printf("  variant %s tag=%d named=%d payload=%s\n", v->name, v->tag, v->named ? 1 : 0, v->payload ? v->payload->name : "-");
        for (int k = 0; k < v->fields.n; k++) {
            Field *f = v->fields.v[k];
            printf("    %s: %s\n", f->name, f->t->name);
        }
    }
}

void gen_dump_types(Gen *g) {
    Pos none = {0};
    for (int i = 0; i < g->pg->modules.n; i++) {
        Module *m = g->pg->modules.v[i];
        for (int k = 0; k < m->files.n; k++) {
            SourceFile *sf = m->files.v[k];
            for (int j = 0; j < sf->decls.n; j++) {
                Node *n = sf->decls.v[j];
                if (n->kind != D_STRUCT && n->kind != D_CLASS && n->kind != D_ENUM) continue;
                if (n->list2.n) continue;
                Decl *d = module_find(m, n->name);
                if (!d || d->node != n) continue;
                Vec none_args = {0};
                dump_type(g, instantiate(g, d, none_args, none));
            }
        }
    }
    /* generic instances */
    Decl *dopt = lookup_in_prelude(g, "Option"), *dres = lookup_in_prelude(g, "Result"), *dmap = lookup_in_prelude(g, "Map");
    Type *err = lookup_in_prelude(g, "Error") ? instantiate(g, lookup_in_prelude(g, "Error"), (Vec){0}, none) : ty_i32;
    Vec a1 = {0};
    vec_push(&a1, ty_string);
    if (dopt) dump_type(g, instantiate(g, dopt, a1, none));
    Vec a2 = {0};
    vec_push(&a2, ty_i32);
    vec_push(&a2, err);
    if (dres) dump_type(g, instantiate(g, dres, a2, none));
    Vec a3 = {0};
    vec_push(&a3, ty_string);
    vec_push(&a3, ty_i64);
    if (dmap) dump_type(g, instantiate(g, dmap, a3, none));
    dump_type(g, array_type(g, ty_i32));
    dump_type(g, array_type(g, array_type(g, ty_string)));
}

/* =========================================================================
 * Function instances
 * ========================================================================= */

static Env *make_env(Decl *d, Type *self, Vec *fn_targs) {
    Env *e = env_new();
    if (self) {
        e->self = self;
        Node *td = self->decl ? self->decl->node : NULL;
        if (td)
            for (int i = 0; i < td->list2.n && i < self->args.n; i++)
                env_add(e, ((Node *)td->list2.v[i])->name, self->args.v[i]);
    }
    if (d->iface && d->iface->decl) {
        /* a default method of an interface: the interface's type parameters are in scope */
        Node *itd = d->iface->decl->node;
        for (int i = 0; i < itd->list2.n && i < d->iface->args.n; i++)
            env_add(e, ((Node *)itd->list2.v[i])->name, d->iface->args.v[i]);
    }
    Node *fn = d->node;
    for (int i = 0; i < fn->list2.n; i++)
        if (fn_targs && i < fn_targs->n) env_add(e, ((Node *)fn->list2.v[i])->name, fn_targs->v[i]);
    return e;
}

/* Arguments and results of `extern "C"` functions are C types or pointers. */
static bool is_c_value_type(Type *t) {
    if (t->k == T_FN) {
        /* a callback: C types and pointers only */
        for (int i = 0; i < t->args.n; i++)
            if (!is_c_value_type(t->args.v[i])) return false;
        Type *r = t->ret;
        return r == ty_unit || (r->k == T_CTYPE && strcmp(r->name, "CVoid") == 0) || is_c_value_type(r);
    }
    return t->k == T_PTR || (t->k == T_CTYPE && strcmp(t->name, "CVoid") != 0);
}

static void check_c_signature(Inst *in, Node *fn) {
    int pi = 0;
    for (int i = 0; i < fn->list.n; i++) {
        Node *p = fn->list.v[i];
        if (p->flag3) continue;
        Type *pt = in->ptypes.v[pi++];
        if (is_err(pt)) continue;
        if (!is_c_value_type(pt)) {
            diag_error(p->pos, "parameter `%s` of the C function `%s` has type `%s`; C functions take only C types and pointers",
                       p->name, fn->name, pt->name);
            if (pt->k == T_STRUCT && pt->decl && pt->decl->abi) diag_note("structs are passed by pointer: write `*%s` and pass `&value`", pt->name);
            else if (pt == ty_string) diag_note("convert a String with `.ToC()`");
            else if (ty_is_numeric(pt)) diag_note("convert it with a method such as `.ToCInt()`");
        }
    }
    if (is_err(in->ret)) return;
    if (in->ret->k == T_CTYPE && strcmp(in->ret->name, "CVoid") == 0) {
        in->ret = ty_unit;
    } else if (in->ret != ty_unit && !is_c_value_type(in->ret)) {
        diag_error(fn->pos, "the C function `%s` returns `%s`; C functions return only C types, pointers and `CVoid`", fn->name,
                   in->ret->name);
    }
}

static Inst *get_inst(Gen *g, Decl *d, Type *self, Vec *targs) {
    Node *fn = d->node;
    SB key = {0};
    bool is_extern = d->abi != NULL;
    bool is_export = fn->is_export && !self;
    if (is_export) {
        sb_puts(&key, fn->name);
    } else if (is_extern) {
        sb_puts(&key, fn->name2 ? fn->name2 : fn->name);
    } else if (self) {
        sb_printf(&key, "zevic_%s_%s", mangle_part(self->name), fn->name);
    } else {
        sb_printf(&key, "zevic_%s_%s", mangle_part(d->mod->name), fn->name);
    }
    if (targs && targs->n && !is_extern) {
        sb_putc(&key, '<');
        for (int i = 0; i < targs->n; i++) sb_printf(&key, "%s%s", i ? ", " : "", ((Type *)targs->v[i])->name);
        sb_putc(&key, '>');
    }
    char *k = sb_str(&key);
    Inst *in = dict_get(&g->insts, k);
    if (in && is_export && in->decl != d) {
        diag_error(fn->pos, "the C name `%s` is exported twice", fn->name);
        diag_note("the other one is in module `%s`", in->decl->mod->name);
        return in;
    }
    if (in) return in;
    in = zalloc(sizeof *in);
    in->is_export = is_export;
    in->decl = d;
    in->self = self;
    in->name = k;
    in->is_extern = is_extern;
    in->has_this = self && !fn->is_static && !is_extern;
    in->env = make_env(d, self, targs);
    for (int i = 0; i < fn->list.n; i++) {
        Node *p = fn->list.v[i];
        if (p->flag3) continue; /* `...` */
        Type *pt = resolve_type(g, d->file, in->env, p->a);
        vec_push(&in->ptypes, pt);
    }
    in->ret = fn->a ? resolve_type(g, d->file, in->env, fn->a) : ty_unit;
    if (is_export) vec_push(&g->exports, in);
    if ((is_extern && strcmp(d->abi, "C") == 0) || is_export) check_c_signature(in, fn);
    if (is_export && fn->list2.n) diag_error(fn->pos, "an exported function cannot have type parameters");
    if (is_export && strcmp(fn->name, "Main") == 0 && d->mod && strcmp(d->mod->name, "Main") == 0)
        diag_error(fn->pos, "`Main` cannot be exported");
    dict_put(&g->insts, k, in);
    if (!is_extern) vec_push(&g->worklist, in);
    return in;
}

/* =========================================================================
 * Function context and IR emission
 * ========================================================================= */

typedef struct {
    Type *t;
    char *v; /* LLVM operand ("%t3", "42", "{ ptr @.str.0, i64 5 }", "zeroinitializer") */
} Val;

typedef struct Local {
    char *name;
    Type *t;
    char *ptr; /* the alloca */
    bool mut;
    Pos pos;
} Local;

typedef struct Scope {
    struct Scope *up;
    Vec locals; /* Local* */
    Vec defers; /* Node* */
} Scope;

typedef struct {
    char *brk, *cont;
    Scope *scope;
    bool brk_used; /* some `break` jumps to `brk` */
    const char *label; /* `name@ for ...`, or NULL */
} Loop;

typedef struct FnCtx {
    Gen *g;
    Inst *inst;
    SourceFile *file;
    Env *env;
    SB code, allocas, cold;
    int ntmp, nlbl, nslot;
    Node *wb_elem;   /* `xs[i].F = v` on a struct element: the element expression whose copy must be stored back */
    char *wb_name;   /* the local that holds that copy */
    char *cur;
    bool term;
    Type *ret;
    Scope *scope;
    Vec loops;
    bool has_this;   /* inside a method */
    char *this_slot; /* address of the `this` value (a slot holding the pointer, for classes) */
    Type *this_t;
    bool this_mut;   /* may `this` be changed? (mut fn and Init) */
    Dict heap;       /* names of variables that a lambda may capture: they live in heap cells */
} FnCtx;

/* Where a value lives, and whether it may be changed through this path. */
typedef struct {
    char *ptr; /* address of the value */
    Type *t;
    bool mut;
} Place;

static Place gen_place(FnCtx *F, Node *n);
static bool is_static_path(FnCtx *F, Node *n);

static Val unit_val(void) {
    Val v = {ty_unit, "zeroinitializer"};
    return v;
}

static Val err_val(void) {
    Val v = {ty_err, "undef"};
    return v;
}

/* The value of an expression that never completes, typed as `want`. */
static Val never_val(Type *want) {
    Val v = {want ? want : ty_never, "undef"};
    return v;
}

static char *tmp(FnCtx *F) {
    return zsprintf("%%t%d", F->ntmp++);
}

static char *newlabel(FnCtx *F, const char *hint) {
    return zsprintf("%s.%d", hint, F->nlbl++);
}

static void start_block(FnCtx *F, const char *label) {
    sb_printf(&F->code, "%s:\n", label);
    F->cur = zstrdup(label);
    F->term = false;
}

static void emit(FnCtx *F, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void emit(FnCtx *F, const char *fmt, ...) {
    if (F->term) start_block(F, newlabel(F, "dead")); /* unreachable code gets its own block */
    va_list ap;
    va_start(ap, fmt);
    char *s = zvsprintf(fmt, ap);
    va_end(ap);
    sb_printf(&F->code, "  %s\n", s);
}

/* Terminate the current block with a branch and start `label`. */
static void goto_block(FnCtx *F, const char *label) {
    if (!F->term) {
        sb_printf(&F->code, "  br label %%%s\n", label);
        F->term = true;
    }
    start_block(F, label);
}

static void br(FnCtx *F, const char *label) {
    emit(F, "br label %%%s", label);
    F->term = true;
}

static void condbr(FnCtx *F, const char *cond, const char *t, const char *f) {
    emit(F, "br i1 %s, label %%%s, label %%%s", cond, t, f);
    F->term = true;
}

static char *new_slot(FnCtx *F, Type *t) {
    char *name = zsprintf("%%s%d", F->nslot++);
    sb_printf(&F->allocas, "  %s = alloca %s, align %lld\n", name, ty_ll(t), (long long)(ty_align(t) < 1 ? 1 : ty_align(t)));
    return name;
}

static char *type_ll(Type *t) {
    return (char *)ty_ll(t);
}

static void type_error(Pos pos, Type *want, Type *got, const char *what) {
    if (is_err(want) || is_err(got)) return;
    diag_error(pos, "type mismatch%s%s: expected `%s`, found `%s`", what ? " in " : "", what ? what : "", want->name,
               got->name);
}

static bool is_user_type(Type *t);
static Val to_iface(FnCtx *F, Pos pos, Val v, Type *it, const char *what);
static Val union_coerce(FnCtx *F, Pos pos, Val v, Type *want, const char *what);
static int union_index(Type *u, Type *m);
static Val union_remap(FnCtx *F, Val v, Type *from, Type *to);
static Val union_extract(FnCtx *F, Val v, Type *m);
static char *tag_ptr(FnCtx *F, Type *et, char *slot);

/* Check that `v` has type `want`; a value of type Never fits everywhere. */
static Val coerce(FnCtx *F, Pos pos, Val v, Type *want, const char *what) {
    (void)F;
    if (v.t == want || is_err(v.t) || is_err(want)) return v;
    if (v.t == ty_never) return never_val(want);
    if (want->k == T_IFACE && is_user_type(v.t)) return to_iface(F, pos, v, want, what);
    if (want->k == T_UNION) return union_coerce(F, pos, v, want, what);
    /* like C, any pointer converts to `*CVoid` */
    if (v.t->k == T_PTR && want->k == T_PTR && want->args.n && want->args.v[0] == ty_c_by_name("CVoid")) {
        Val r = {want, v.v};
        return r;
    }
    type_error(pos, want, v.t, what);
    return err_val();
}

/* Is this expression a number literal whose type comes from the context? */
static bool is_untyped_lit(Node *n) {
    if (n->kind == N_UNARY && n->op == TK_MINUS) return is_untyped_lit(n->a);
    return (n->kind == N_INT || n->kind == N_FLOAT) && !n->suffix;
}

/* A runtime call that panics, in the cold section; returns the label to branch to. */
static char *cold_panic(FnCtx *F, const char *msg, Pos pos) {
    Gen *g = F->g;
    declare(g, "zevic_panic", "declare void @zevic_panic(ptr, i64, ptr, i32) noreturn");
    char *label = newlabel(F, "panic");
    const char *file = src_name(pos.file);
    char *m = str_const(g, msg, (int)strlen(msg));
    char *fl = str_const(g, file, (int)strlen(file));
    /* the constants are `{ ptr @x, i64 n }`; the runtime takes the pieces */
    char *mp = zstrndup(m + 6, strchr(m, ',') - (m + 6));
    char *fp = zstrndup(fl + 6, strchr(fl, ',') - (fl + 6));
    sb_printf(&F->cold, "%s:\n  call void @zevic_panic(ptr %s, i64 %d, ptr %s, i32 %d)\n  unreachable\n", label, mp,
              (int)strlen(msg), fp, pos.line);
    return label;
}

/* ---- scopes ---- */

static void push_scope(FnCtx *F) {
    Scope *s = zalloc(sizeof *s);
    s->up = F->scope;
    F->scope = s;
}

static Local *find_local(FnCtx *F, const char *name) {
    for (Scope *s = F->scope; s; s = s->up)
        for (int i = s->locals.n - 1; i >= 0; i--)
            if (strcmp(((Local *)s->locals.v[i])->name, name) == 0) return s->locals.v[i];
    return NULL;
}

/* Every identifier used inside `n`; `*uses_this` is set when `this` appears. */
static void collect_idents(Node *n, Dict *out, bool *uses_this) {
    if (!n) return;
    if (n->kind == N_IDENT && n->name) dict_put(out, n->name, (void *)1);
    if (n->kind == N_THIS && uses_this) *uses_this = true;
    collect_idents(n->a, out, uses_this);
    collect_idents(n->b, out, uses_this);
    collect_idents(n->c, out, uses_this);
    collect_idents(n->d, out, uses_this);
    for (int i = 0; i < n->list.n; i++) collect_idents(n->list.v[i], out, uses_this);
    for (int i = 0; i < n->list2.n; i++) collect_idents(n->list2.v[i], out, uses_this);
    for (int i = 0; i < n->list3.n; i++) collect_idents(n->list3.v[i], out, uses_this);
}

/* The names a lambda below `n` may capture. A variable with such a name is allocated on the heap when it is declared. */
static void collect_lambda_names(Node *n, Dict *heap) {
    if (!n) return;
    if (n->kind == N_LAMBDA || n->kind == N_ASYNC) { /* an `async` block becomes a lambda */
        bool t = false;
        collect_idents(n, heap, &t);
        return;
    }
    collect_lambda_names(n->a, heap);
    collect_lambda_names(n->b, heap);
    collect_lambda_names(n->c, heap);
    collect_lambda_names(n->d, heap);
    for (int i = 0; i < n->list.n; i++) collect_lambda_names(n->list.v[i], heap);
    for (int i = 0; i < n->list2.n; i++) collect_lambda_names(n->list2.v[i], heap);
    for (int i = 0; i < n->list3.n; i++) collect_lambda_names(n->list3.v[i], heap);
}

static void rt_decl(FnCtx *F, const char *name, const char *line);

static Local *declare_local(FnCtx *F, const char *name, Type *t, bool mut, Pos pos) {
    for (int i = 0; i < F->scope->locals.n; i++) {
        Local *o = F->scope->locals.v[i];
        if (strcmp(o->name, name) == 0) {
            diag_error(pos, "`%s` is already declared in this scope", name);
            diag_note("the first declaration is at line %d", o->pos.line);
            break;
        }
    }
    Local *l = zalloc(sizeof *l);
    l->name = zstrdup(name);
    l->t = t;
    l->mut = mut;
    l->pos = pos;
    if (F->heap.cap && dict_has(&F->heap, name) && ty_size(t) > 0) {
        /* a lambda may capture this variable: give it a cell on the heap that outlives the call */
        rt_decl(F, "zevic_alloc", "declare ptr @zevic_alloc(i64)");
        l->ptr = tmp(F);
        emit(F, "%s = call ptr @zevic_alloc(i64 %lld)", l->ptr, (long long)ty_size(t));
    } else {
        l->ptr = new_slot(F, t);
    }
    vec_push(&F->scope->locals, l);
    return l;
}

/* Bind a name to an existing cell (a captured variable inside a lambda). */
static Local *bind_local_ptr(FnCtx *F, const char *name, Type *t, bool mut, char *ptr, Pos pos) {
    Local *l = zalloc(sizeof *l);
    l->name = zstrdup(name);
    l->t = t;
    l->mut = mut;
    l->pos = pos;
    l->ptr = ptr;
    vec_push(&F->scope->locals, l);
    return l;
}

static Val load_local(FnCtx *F, Local *l) {
    if (l->t == ty_unit) return unit_val();
    char *t = tmp(F);
    emit(F, "%s = load %s, ptr %s", t, type_ll(l->t), l->ptr);
    Val v = {l->t, t};
    return v;
}

static void store_to(FnCtx *F, Type *t, Val v, const char *ptr) {
    emit(F, "store %s %s, ptr %s", type_ll(t), v.v, ptr);
}

static void run_defers_in(FnCtx *F, Scope *s);

/* Leave the innermost scope on the normal path. */
static void pop_scope(FnCtx *F) {
    Scope *s = F->scope;
    if (!F->term) run_defers_in(F, s);
    F->scope = s->up;
}

/* =========================================================================
 * Literals
 * ========================================================================= */

static Val gen_expr(FnCtx *F, Node *n, Type *expect);
static Val gen_index_read(FnCtx *F, Node *n, Type *expect);
static Val gen_array_lit(FnCtx *F, Node *n, Type *expect);
static Val gen_struct_lit(FnCtx *F, Node *n, Type *expect);
static void gen_index_assign(FnCtx *F, Node *n);
static bool is_pure_expr(Node *n);
static void gen_for_iter(FnCtx *F, Node *n);

static Type *type_from_suffix(const char *s) {
    return ty_prim_by_name(s);
}

/* The type a number literal takes when a union is expected: its only integer (or decimal) member; otherwise the
   default type (`I64`, `F64`), which then has to be a member. */
static Type *lit_member(Type *expect, bool is_float) {
    if (!expect || expect->k != T_UNION) return expect;
    Type *found = NULL;
    int cnt = 0;
    for (int i = 0; i < expect->args.n; i++) {
        Type *m = expect->args.v[i];
        if (is_float ? ty_is_float(m) : ty_is_int(m)) {
            found = m;
            cnt++;
        }
    }
    return cnt == 1 ? found : NULL;
}

static Val gen_int_lit_raw(FnCtx *F, Node *n, Type *expect, bool neg) {
    Type *t;
    expect = lit_member(expect, false);
    if (n->suffix) t = type_from_suffix(n->suffix);
    else if (expect && ty_is_int(expect)) t = expect;
    else if (expect && ty_is_float(expect)) {
        diag_error(n->pos, "expected `%s`, found an integer (write `%llu.0` for a decimal number)", expect->name,
                   (unsigned long long)n->ival);
        return err_val();
    } else t = ty_i64;
    int bits = ty_bits(t);
    uint64_t v = n->ival;
    if (ty_is_signed(t)) {
        uint64_t max = ((uint64_t)1 << (bits - 1)) - 1 + (neg ? 1 : 0);
        if (v > max) {
            diag_error(n->pos, "the number %s%llu does not fit in `%s` (range %lld to %lld)", neg ? "-" : "",
                       (unsigned long long)v, t->name, (long long)(-(int64_t)(max - (neg ? 1 : 0)) - 1),
                       (long long)(max - (neg ? 1 : 0)));
            return err_val();
        }
    } else {
        if (neg && v != 0) {
            diag_error(n->pos, "an unsigned type (`%s`) cannot hold a negative number", t->name);
            return err_val();
        }
        uint64_t max = bits == 64 ? UINT64_MAX : (((uint64_t)1 << bits) - 1);
        if (v > max) {
            diag_error(n->pos, "the number %llu does not fit in `%s` (maximum %llu)", (unsigned long long)v, t->name,
                       (unsigned long long)max);
            return err_val();
        }
    }
    int64_t sv = neg ? (int64_t)(0 - v) : (int64_t)v;
    Val r = {t, zsprintf("%lld", (long long)sv)};
    (void)F;
    return r;
}

static char *double_hex(double d) {
    union { double d; uint64_t u; } x;
    x.d = d;
    return zsprintf("0x%016llX", (unsigned long long)x.u);
}

static Val gen_float_lit_raw(FnCtx *F, Node *n, Type *expect, bool neg) {
    Type *t;
    expect = lit_member(expect, true);
    (void)F;
    if (n->suffix) t = type_from_suffix(n->suffix);
    else if (expect && ty_is_float(expect)) t = expect;
    else if (expect && ty_is_int(expect)) {
        diag_error(n->pos, "expected `%s`, found a decimal number", expect->name);
        return err_val();
    } else t = ty_f64;
    double d = neg ? -n->fval : n->fval;
    if (t == ty_f32) d = (double)(float)d; /* f32 constants are written as doubles that are exactly representable */
    Val r = {t, double_hex(d)};
    return r;
}

static Type *c_native(Type *t);

/* An unsuffixed literal where a C number is expected becomes that C type. */
static Val gen_int_lit(FnCtx *F, Node *n, Type *expect, bool neg) {
    Type *nat = (!n->suffix && expect) ? c_native(expect) : NULL;
    if (nat && ty_is_int(nat)) {
        Val v = gen_int_lit_raw(F, n, nat, neg);
        if (!is_err(v.t)) v.t = expect;
        return v;
    }
    return gen_int_lit_raw(F, n, expect, neg);
}

static Val gen_float_lit(FnCtx *F, Node *n, Type *expect, bool neg) {
    Type *nat = (!n->suffix && expect) ? c_native(expect) : NULL;
    if (nat && ty_is_float(nat)) {
        Val v = gen_float_lit_raw(F, n, nat, neg);
        if (!is_err(v.t)) v.t = expect;
        return v;
    }
    return gen_float_lit_raw(F, n, expect, neg);
}

/* ---- runtime helpers ---- */

static void rt_decl(FnCtx *F, const char *name, const char *line) {
    declare(F->g, name, line);
}

static Val str_val(char *constant) {
    Val v = {ty_string, constant};
    return v;
}

static void str_parts(FnCtx *F, Val s, char **p, char **n) {
    if (strncmp(s.v, "{ ptr ", 6) == 0 && strchr(s.v, '@')) {
        /* a constant: take the pieces apart textually */
        char *comma = strchr(s.v, ',');
        *p = zstrndup(s.v + 6, (size_t)(comma - (s.v + 6)));
        char *len = strstr(s.v, "i64 ");
        *n = zstrndup(len + 4, strlen(len + 4) - 2);
        return;
    }
    *p = tmp(F);
    *n = tmp(F);
    emit(F, "%s = extractvalue { ptr, i64 } %s, 0", *p, s.v);
    emit(F, "%s = extractvalue { ptr, i64 } %s, 1", *n, s.v);
}

/* Call a runtime function that returns a String through an out pointer. */
static Val call_str_out(FnCtx *F, const char *fn, const char *args) {
    char *slot = new_slot(F, ty_string);
    emit(F, "call void @%s(ptr %s%s%s)", fn, slot, args[0] ? ", " : "", args);
    char *t = tmp(F);
    emit(F, "%s = load { ptr, i64 }, ptr %s", t, slot);
    Val v = {ty_string, t};
    return v;
}

/* x.ToString() for the built-in types. */
static Val gen_to_string(FnCtx *F, Val v, Pos pos) {
    Type *t = v.t;
    if (is_err(t)) return str_val("{ ptr null, i64 0 }");
    if (t == ty_string) return v;
    if (ty_is_int(t)) {
        int bits = ty_bits(t);
        char *x = v.v;
        if (bits < 64) {
            x = tmp(F);
            emit(F, "%s = %s i%d %s to i64", x, ty_is_signed(t) ? "sext" : "zext", bits, v.v);
        }
        if (ty_is_signed(t)) {
            rt_decl(F, "zevic_i64_to_str", "declare void @zevic_i64_to_str(ptr, i64)");
            return call_str_out(F, "zevic_i64_to_str", zsprintf("i64 %s", x));
        }
        rt_decl(F, "zevic_u64_to_str", "declare void @zevic_u64_to_str(ptr, i64)");
        return call_str_out(F, "zevic_u64_to_str", zsprintf("i64 %s", x));
    }
    if (ty_is_float(t)) {
        char *x = v.v;
        if (t == ty_f32) {
            x = tmp(F);
            emit(F, "%s = fpext float %s to double", x, v.v);
        }
        rt_decl(F, "zevic_f64_to_str", "declare void @zevic_f64_to_str(ptr, double)");
        return call_str_out(F, "zevic_f64_to_str", zsprintf("double %s", x));
    }
    if (t == ty_bool) {
        char *x = tmp(F);
        emit(F, "%s = zext i1 %s to i32", x, v.v);
        rt_decl(F, "zevic_bool_to_str", "declare void @zevic_bool_to_str(ptr, i32)");
        return call_str_out(F, "zevic_bool_to_str", zsprintf("i32 %s", x));
    }
    if (t == ty_char) {
        rt_decl(F, "zevic_char_to_str", "declare void @zevic_char_to_str(ptr, i32)");
        return call_str_out(F, "zevic_char_to_str", zsprintf("i32 %s", v.v));
    }
    if (t == ty_unit) return str_val(str_const(F->g, "()", 2));
    if (strcmp(F->inst->decl->mod->name, "Zevic.Test") == 0) {
        /* the assertions show any value; types without a text form show their name */
        char *name = zsprintf("<%s>", t->name);
        return str_val(str_const(F->g, name, (int)strlen(name)));
    }
    diag_error(pos, "`ToString` is not available for type `%s` yet", t->name);
    return str_val("{ ptr null, i64 0 }");
}

static Val gen_string_lit(FnCtx *F, Node *n) {
    if (n->list.n == 0) return str_val(str_const(F->g, "", 0));
    if (n->list.n == 1 && ((Node *)n->list.v[0])->kind == N_STR_LIT) {
        Node *lit = n->list.v[0];
        return str_val(str_const(F->g, lit->name, lit->slen));
    }
    /* interpolation: build the result in a runtime string builder */
    rt_decl(F, "zevic_sb_new", "declare ptr @zevic_sb_new()");
    rt_decl(F, "zevic_sb_add", "declare void @zevic_sb_add(ptr, ptr, i64)");
    rt_decl(F, "zevic_sb_finish", "declare void @zevic_sb_finish(ptr, ptr)");
    char *sb = tmp(F);
    emit(F, "%s = call ptr @zevic_sb_new()", sb);
    for (int i = 0; i < n->list.n; i++) {
        Node *part = n->list.v[i];
        Val s;
        if (part->kind == N_STR_LIT) {
            s = str_val(str_const(F->g, part->name, part->slen));
        } else {
            Val v = gen_expr(F, part, NULL);
            if (v.t == ty_never) return never_val(ty_string);
            s = gen_to_string(F, v, part->pos);
        }
        char *p, *len;
        str_parts(F, s, &p, &len);
        emit(F, "call void @zevic_sb_add(ptr %s, ptr %s, i64 %s)", sb, p, len);
    }
    return call_str_out(F, "zevic_sb_finish", zsprintf("ptr %s", sb));
}

/* =========================================================================
 * Operators
 * ========================================================================= */

static char *int_intrinsic(FnCtx *F, const char *op, Type *t) {
    int bits = ty_bits(t);
    char *name = zsprintf("llvm.%s.with.overflow.i%d", op, bits);
    declare(F->g, name, zsprintf("declare { i%d, i1 } @%s(i%d, i%d)", bits, name, bits, bits));
    return name;
}

static Val gen_int_arith(FnCtx *F, TokKind op, Val a, Val b, Pos pos) {
    Type *t = a.t;
    int bits = ty_bits(t);
    bool sgn = ty_is_signed(t);
    char *ll = type_ll(t);
    if (op == TK_PLUS || op == TK_MINUS || op == TK_STAR) {
        const char *nm = op == TK_PLUS ? (sgn ? "sadd" : "uadd") : op == TK_MINUS ? (sgn ? "ssub" : "usub") : (sgn ? "smul" : "umul");
        char *fn = int_intrinsic(F, nm, t);
        char *r = tmp(F), *v = tmp(F), *o = tmp(F);
        emit(F, "%s = call { %s, i1 } @%s(%s %s, %s %s)", r, ll, fn, ll, a.v, ll, b.v);
        emit(F, "%s = extractvalue { %s, i1 } %s, 0", v, ll, r);
        emit(F, "%s = extractvalue { %s, i1 } %s, 1", o, ll, r);
        char *ok = newlabel(F, "ok");
        condbr(F, o, cold_panic(F, "integer overflow", pos), ok);
        start_block(F, ok);
        Val res = {t, v};
        return res;
    }
    if (op == TK_SLASH || op == TK_PERCENT) {
        char *z = tmp(F);
        emit(F, "%s = icmp eq %s %s, 0", z, ll, b.v);
        char *ok = newlabel(F, "ok");
        condbr(F, z, cold_panic(F, op == TK_SLASH ? "division by zero" : "remainder by zero", pos), ok);
        start_block(F, ok);
        if (sgn) {
            /* MIN / -1 overflows */
            char *min = zsprintf("%lld", bits == 64 ? (long long)INT64_MIN : -((long long)1 << (bits - 1)));
            char *c1 = tmp(F), *c2 = tmp(F), *both = tmp(F);
            emit(F, "%s = icmp eq %s %s, %s", c1, ll, a.v, min);
            emit(F, "%s = icmp eq %s %s, -1", c2, ll, b.v);
            emit(F, "%s = and i1 %s, %s", both, c1, c2);
            char *ok2 = newlabel(F, "ok");
            condbr(F, both, cold_panic(F, "integer overflow", pos), ok2);
            start_block(F, ok2);
        }
        char *r = tmp(F);
        emit(F, "%s = %s %s %s, %s", r, op == TK_SLASH ? (sgn ? "sdiv" : "udiv") : (sgn ? "srem" : "urem"), ll, a.v, b.v);
        Val res = {t, r};
        return res;
    }
    const char *ins = NULL;
    switch (op) {
    case TK_AMP: ins = "and"; break;
    case TK_PIPE: ins = "or"; break;
    case TK_CARET: ins = "xor"; break;
    default: break;
    }
    char *r = tmp(F);
    emit(F, "%s = %s %s %s, %s", r, ins, ll, a.v, b.v);
    Val res = {t, r};
    return res;
}

/* a << n and a >> n: the amount is an I32 and must be below the width. */
static Val gen_shift(FnCtx *F, TokKind op, Val a, Val amt, Pos pos) {
    Type *t = a.t;
    int bits = ty_bits(t);
    char *ll = type_ll(t);
    char *bad = tmp(F);
    emit(F, "%s = icmp uge i32 %s, %d", bad, amt.v, bits);
    char *ok = newlabel(F, "ok");
    condbr(F, bad, cold_panic(F, "shift amount is out of range", pos), ok);
    start_block(F, ok);
    char *x = amt.v;
    if (bits != 32) {
        x = tmp(F);
        emit(F, "%s = %s i32 %s to %s", x, bits < 32 ? "trunc" : "zext", amt.v, ll);
    }
    char *r = tmp(F);
    emit(F, "%s = %s %s %s, %s", r, op == TK_SHL ? "shl" : (ty_is_signed(t) ? "ashr" : "lshr"), ll, a.v, x);
    Val res = {t, r};
    return res;
}

static Val gen_float_arith(FnCtx *F, TokKind op, Val a, Val b) {
    const char *ins = op == TK_PLUS ? "fadd" : op == TK_MINUS ? "fsub" : op == TK_STAR ? "fmul" : op == TK_SLASH ? "fdiv" : "frem";
    char *r = tmp(F);
    emit(F, "%s = %s %s %s, %s", r, ins, type_ll(a.t), a.v, b.v);
    Val res = {a.t, r};
    return res;
}

static Val gen_eq(FnCtx *F, Val a, Val b, Pos pos);
static Place spill(FnCtx *F, Val v);
static Node *find_member_fn(Type *t, const char *name);
static Val call_method_vals(FnCtx *F, Type *t, Node *fn, Val recv_obj, Val *args, int nargs, Pos pos, Type *expect);
/* A struct's methods take `this` by pointer: put the value in memory and pass its address. */
static Val method_receiver(FnCtx *F, Val a) {
    if (a.t->k != T_STRUCT) return a;
    Place pl = spill(F, a);
    Val v = {a.t, pl.ptr};
    return v;
}

static Val gen_compare(FnCtx *F, TokKind op, Val a, Val b, Pos pos) {
    Type *t = a.t;
    char *r = tmp(F);
    Val res = {ty_bool, r};
    if (ty_is_int(t) || t == ty_char || t == ty_bool) {
        bool sgn = ty_is_signed(t);
        const char *p = op == TK_EQEQ ? "eq" : op == TK_NEQ ? "ne"
                      : op == TK_LT ? (sgn ? "slt" : "ult") : op == TK_LE ? (sgn ? "sle" : "ule")
                      : op == TK_GT ? (sgn ? "sgt" : "ugt") : (sgn ? "sge" : "uge");
        emit(F, "%s = icmp %s %s %s, %s", r, p, type_ll(t), a.v, b.v);
        return res;
    }
    if (t->k == T_CTYPE && strcmp(t->name, "CVoid") != 0) {
        bool is_f = strcmp(t->ll, "float") == 0 || strcmp(t->ll, "double") == 0;
        bool is_p = strcmp(t->ll, "ptr") == 0;
        if (is_f) {
            const char *p = op == TK_EQEQ ? "oeq" : op == TK_NEQ ? "une" : op == TK_LT ? "olt" : op == TK_LE ? "ole" : op == TK_GT ? "ogt" : "oge";
            emit(F, "%s = fcmp %s %s %s, %s", r, p, t->ll, a.v, b.v);
            return res;
        }
        if (is_p && op != TK_EQEQ && op != TK_NEQ) {
            diag_error(pos, "`CString` values can only be compared with `==` and `!=`");
            return err_val();
        }
        bool sgn = t->payload_words == 1;
        const char *p = op == TK_EQEQ ? "eq" : op == TK_NEQ ? "ne"
                      : op == TK_LT ? (sgn ? "slt" : "ult") : op == TK_LE ? (sgn ? "sle" : "ule")
                      : op == TK_GT ? (sgn ? "sgt" : "ugt") : (sgn ? "sge" : "uge");
        emit(F, "%s = icmp %s %s %s, %s", r, p, t->ll, a.v, b.v);
        return res;
    }
    if (ty_is_float(t)) {
        const char *p = op == TK_EQEQ ? "oeq" : op == TK_NEQ ? "une" : op == TK_LT ? "olt" : op == TK_LE ? "ole" : op == TK_GT ? "ogt" : "oge";
        emit(F, "%s = fcmp %s %s %s, %s", r, p, type_ll(t), a.v, b.v);
        return res;
    }
    if (t == ty_string) {
        char *ap, *an, *bp, *bn;
        str_parts(F, a, &ap, &an);
        str_parts(F, b, &bp, &bn);
        char *c = tmp(F);
        if (op == TK_EQEQ || op == TK_NEQ) {
            rt_decl(F, "zevic_str_eq", "declare i32 @zevic_str_eq(ptr, i64, ptr, i64)");
            emit(F, "%s = call i32 @zevic_str_eq(ptr %s, i64 %s, ptr %s, i64 %s)", c, ap, an, bp, bn);
            emit(F, "%s = icmp %s i32 %s, 0", r, op == TK_EQEQ ? "ne" : "eq", c);
        } else {
            rt_decl(F, "zevic_str_cmp", "declare i32 @zevic_str_cmp(ptr, i64, ptr, i64)");
            emit(F, "%s = call i32 @zevic_str_cmp(ptr %s, i64 %s, ptr %s, i64 %s)", c, ap, an, bp, bn);
            const char *p = op == TK_LT ? "slt" : op == TK_LE ? "sle" : op == TK_GT ? "sgt" : "sge";
            emit(F, "%s = icmp %s i32 %s, 0", r, p, c);
        }
        return res;
    }
    if (t == ty_unit) {
        Val v = {ty_bool, op == TK_EQEQ ? "true" : "false"};
        return v;
    }
    if ((op == TK_EQEQ || op == TK_NEQ) &&
        (t->k == T_STRUCT || t->k == T_ENUM || t->k == T_TUPLE || t->k == T_CLASS || t->k == T_PTR)) {
        Val e = gen_eq(F, a, b, pos);
        if (is_err(e.t) || op == TK_EQEQ) return e;
        char *n = tmp(F);
        emit(F, "%s = xor i1 %s, true", n, e.v);
        Val res2 = {ty_bool, n};
        return res2;
    }
    if (t->k == T_STRUCT || t->k == T_CLASS) {
        /* a < b is a.Compare(b) < 0 for a type that implements Comparable */
        Node *cmp = find_member_fn(t, "Compare");
        if (cmp && !cmp->is_static) {
            Val args[1] = {b};
            Val c = call_method_vals(F, t, cmp, method_receiver(F, a), args, 1, pos, ty_i32);
            if (is_err(c.t)) return err_val();
            const char *p = op == TK_LT ? "slt" : op == TK_LE ? "sle" : op == TK_GT ? "sgt" : "sge";
            emit(F, "%s = icmp %s i32 %s, 0", r, p, c.v);
            return res;
        }
    }
    if (op == TK_EQEQ || op == TK_NEQ)
        diag_error(pos, "values of type `%s` cannot be compared with `%s`", t->name, tok_name(op));
    else
        diag_error(pos, "values of type `%s` cannot be ordered with `%s`", t->name, tok_name(op));
    return err_val();
}

/* Generate two operands; a bare number literal takes the other side's type. */
static void gen_pair(FnCtx *F, Node *na, Node *nb, Type *hint, Val *va, Val *vb) {
    if (is_untyped_lit(na) && !is_untyped_lit(nb)) {
        *vb = gen_expr(F, nb, hint);
        *va = gen_expr(F, na, (is_err(vb->t) || vb->t == ty_never) ? hint : vb->t);
    } else {
        *va = gen_expr(F, na, hint);
        *vb = gen_expr(F, nb, (is_err(va->t) || va->t == ty_never) ? hint : va->t);
    }
}

/* ---- `is` and smart casts ---- */

typedef struct {
    const char *name;
    Type *t;
} Narrow;

static Local *narrowable(FnCtx *F, Node *x) {
    if (x->kind != N_IDENT) return NULL;
    Local *l = find_local(F, x->name);
    if (!l || l->mut || l->t->k != T_UNION) return NULL;
    return l;
}

/* The type of `l` once it is known to be (truth) or not to be (!truth) `t`. */
static Type *narrowed_type(Type *u, Type *t, bool truth) {
    if (union_index(u, t) < 0) return NULL;
    if (truth) return t;
    Vec rest = {0};
    for (int i = 0; i < u->args.n; i++)
        if (u->args.v[i] != t) vec_push(&rest, u->args.v[i]);
    return ty_union(rest);
}

/* What the condition `c` tells about variables when it is true (truth) or false (!truth). */
static void collect_narrow(FnCtx *F, Node *c, bool truth, Vec *out) {
    if (c->kind == N_IS) {
        Local *l = narrowable(F, c->a);
        if (!l) return;
        Type *t = resolve_type(F->g, F->file, F->env, c->b);
        if (is_err(t)) return;
        Type *nt = narrowed_type(l->t, t, truth != (bool)c->flag);
        if (!nt) return;
        Narrow *nr = zalloc(sizeof *nr);
        nr->name = l->name;
        nr->t = nt;
        vec_push(out, nr);
    } else if (c->kind == N_UNARY && c->op == TK_BANG) {
        collect_narrow(F, c->a, !truth, out);
    } else if (c->kind == N_BINARY && ((c->op == TK_ANDAND && truth) || (c->op == TK_OROR && !truth))) {
        collect_narrow(F, c->a, truth, out);
        collect_narrow(F, c->b, truth, out);
    }
}

/* Open a scope in which the variables are known to have narrower types. Returns true when a scope was opened. */
static int push_narrow(FnCtx *F, Node *cond, bool truth, int errs) {
    if (zv_error_count != errs) return 0;
    int scopes = 0;
    Vec ns = {0};
    collect_narrow(F, cond, truth, &ns);
    for (int i = 0; i < ns.n; i++) {
        Narrow *nr = ns.v[i];
        Local *l = find_local(F, nr->name);
        if (!l || l->t->k != T_UNION) continue;
        if (union_index(l->t, nr->t->k == T_UNION ? nr->t->args.v[0] : nr->t) < 0) continue;
        push_scope(F); /* one scope for each name, so that a name can be narrowed again */
        scopes++;
        Val cur = load_local(F, l);
        Val nv = nr->t->k == T_UNION ? union_remap(F, cur, l->t, nr->t) : union_extract(F, cur, nr->t);
        Local *nl = declare_local(F, nr->name, nr->t, false, l->pos);
        if (ty_size(nr->t) > 0) emit(F, "store %s %s, ptr %s", type_ll(nr->t), nv.v, nl->ptr);
    }
    return scopes;
}

static Val gen_is(FnCtx *F, Node *n) {
    Val v = gen_expr(F, n->a, NULL);
    if (is_err(v.t) || v.t == ty_never) return is_err(v.t) ? err_val() : never_val(ty_bool);
    Type *t = resolve_type(F->g, F->file, F->env, n->b);
    if (is_err(t)) return err_val();
    if (v.t->k != T_UNION) {
        diag_error(n->pos, "`is` needs a union on the left, but this is `%s`", v.t->name);
        return err_val();
    }
    int idx = union_index(v.t, t);
    if (idx < 0) {
        diag_error(n->b->pos, "`%s` is not a member of `%s`", t->name, v.t->name);
        return err_val();
    }
    char *slot = new_slot(F, v.t);
    emit(F, "store %s %s, ptr %s", type_ll(v.t), v.v, slot);
    char *tg = tmp(F);
    emit(F, "%s = load i32, ptr %s", tg, tag_ptr(F, v.t, slot));
    char *r = tmp(F);
    emit(F, "%s = icmp %s i32 %s, %d", r, n->flag ? "ne" : "eq", tg, idx);
    Val out = {ty_bool, r};
    return out;
}

static Val gen_logic(FnCtx *F, Node *n) {
    bool is_and = n->op == TK_ANDAND;
    int errs = zv_error_count;
    Val a = gen_expr(F, n->a, ty_bool);
    a = coerce(F, n->a->pos, a, ty_bool, "the operand of a logical operator");
    if (a.t == ty_never) return never_val(ty_bool);
    char *from = F->cur;
    char *rhs = newlabel(F, is_and ? "and.rhs" : "or.rhs");
    char *end = newlabel(F, is_and ? "and.end" : "or.end");
    if (is_and) condbr(F, a.v, rhs, end);
    else condbr(F, a.v, end, rhs);
    start_block(F, rhs);
    int nsc = push_narrow(F, n->a, is_and, errs);
    Val b = gen_expr(F, n->b, ty_bool);
    b = coerce(F, n->b->pos, b, ty_bool, "the operand of a logical operator");
    while (nsc-- > 0) pop_scope(F);
    char *from2 = F->cur;
    bool b_live = !F->term;
    if (b_live) br(F, end);
    start_block(F, end);
    char *r = tmp(F);
    if (b_live)
        emit(F, "%s = phi i1 [ %s, %%%s ], [ %s, %%%s ]", r, is_and ? "false" : "true", from, b.v, from2);
    else
        emit(F, "%s = phi i1 [ %s, %%%s ]", r, is_and ? "false" : "true", from);
    Val res = {ty_bool, r};
    return res;
}

static Val gen_binary(FnCtx *F, Node *n, Type *expect) {
    TokKind op = n->op;
    if (op == TK_ANDAND || op == TK_OROR) return gen_logic(F, n);
    bool cmp = op == TK_EQEQ || op == TK_NEQ || op == TK_LT || op == TK_GT || op == TK_LE || op == TK_GE;
    bool shift = op == TK_SHL || op == TK_SHR;
    Val a, b;
    if (shift) {
        a = gen_expr(F, n->a, expect);
        b = gen_expr(F, n->b, ty_i32);
    } else {
        gen_pair(F, n->a, n->b, cmp ? NULL : expect, &a, &b);
    }
    if (a.t == ty_never || b.t == ty_never) return never_val(cmp ? ty_bool : expect);
    if (is_err(a.t) || is_err(b.t)) return err_val();
    if (shift) {
        if (!ty_is_int(a.t)) {
            diag_error(n->pos, "`%s` needs an integer on the left, found `%s`", tok_name(op), a.t->name);
            return err_val();
        }
        if (b.t != ty_i32) {
            diag_error(n->b->pos, "the shift amount must be `I32`, found `%s`", b.t->name);
            return err_val();
        }
        return gen_shift(F, op, a, b, n->pos);
    }
    if (a.t != b.t) {
        diag_error(n->pos, "the operands of `%s` have different types: `%s` and `%s` (convert one explicitly, e.g. with `ToI64()`)",
                   tok_name(op), a.t->name, b.t->name);
        return err_val();
    }
    if (cmp) return gen_compare(F, op, a, b, n->pos);
    Type *t = a.t;
    if (t == ty_string && op == TK_PLUS) {
        char *ap, *an, *bp, *bn;
        str_parts(F, a, &ap, &an);
        str_parts(F, b, &bp, &bn);
        rt_decl(F, "zevic_str_concat", "declare void @zevic_str_concat(ptr, ptr, i64, ptr, i64)");
        return call_str_out(F, "zevic_str_concat", zsprintf("ptr %s, i64 %s, ptr %s, i64 %s", ap, an, bp, bn));
    }
    if (ty_is_float(t)) {
        if (op == TK_PLUS || op == TK_MINUS || op == TK_STAR || op == TK_SLASH || op == TK_PERCENT)
            return gen_float_arith(F, op, a, b);
        diag_error(n->pos, "`%s` is only for integers, found `%s`", tok_name(op), t->name);
        return err_val();
    }
    if (ty_is_int(t)) return gen_int_arith(F, op, a, b, n->pos);
    diag_error(n->pos, "`%s` cannot be used with values of type `%s`", tok_name(op), t->name);
    return err_val();
}

static Val gen_unary(FnCtx *F, Node *n, Type *expect) {
    if (n->op == TK_MINUS) {
        Node *x = n->a;
        if (x->kind == N_INT) return gen_int_lit(F, x, expect, true);
        if (x->kind == N_FLOAT) return gen_float_lit(F, x, expect, true);
    }
    if (n->op == TK_AMP) {
        /* &x: the address of a `mut` variable, to hand to a C function */
        Node *x = n->a;
        if (x->kind != N_IDENT && x->kind != N_MEMBER) {
            diag_error(n->pos, "`&` can only be applied to a variable or a field");
            return err_val();
        }
        Place p = gen_place(F, x);
        if (p.t == ty_never) return never_val(expect);
        if (is_err(p.t)) return err_val();
        if (!p.mut) {
            diag_error(n->pos, "cannot take the address of `%s` because it is not `mut`", root_name(x));
            diag_note("declare it with `mut %s: %s = ...`", root_name(x), p.t->name);
            return err_val();
        }
        Val r = {ty_ptr(p.t), p.ptr};
        return r;
    }
    Val v = gen_expr(F, n->a, n->op == TK_BANG ? ty_bool : expect);
    if (v.t == ty_never || is_err(v.t)) return v;
    Type *t = v.t;
    char *r = tmp(F);
    Val res = {t, r};
    switch (n->op) {
    case TK_MINUS:
        if (ty_is_float(t)) {
            emit(F, "%s = fneg %s %s", r, type_ll(t), v.v);
            return res;
        }
        if (ty_is_signed(t)) {
            Val zero = {t, "0"};
            return gen_int_arith(F, TK_MINUS, zero, v, n->pos);
        }
        diag_error(n->pos, "cannot negate a value of type `%s`", t->name);
        return err_val();
    case TK_BANG:
        if (t != ty_bool) {
            diag_error(n->pos, "`!` needs a `Bool`, found `%s`", t->name);
            return err_val();
        }
        emit(F, "%s = xor i1 %s, true", r, v.v);
        return res;
    case TK_TILDE:
        if (!ty_is_int(t)) {
            diag_error(n->pos, "`~` needs an integer, found `%s`", t->name);
            return err_val();
        }
        emit(F, "%s = xor %s %s, -1", r, type_ll(t), v.v);
        return res;
    default:
        return err_val();
    }
}

/* =========================================================================
 * Expressions
 * ========================================================================= */

static Val gen_block(FnCtx *F, Node *blk, Type *expect);
static Val gen_call(FnCtx *F, Node *n, Type *expect);
static Val gen_member(FnCtx *F, Node *n, Type *expect);
static Val gen_match(FnCtx *F, Node *n, Type *expect);
static Val gen_try(FnCtx *F, Node *n, Type *expect);
static Val load_place(FnCtx *F, Place p);
static void gen_stmt(FnCtx *F, Node *s);

static Decl *prelude_variant_owner(FnCtx *F, const char *name);
static Val gen_variant(FnCtx *F, Node *call, Pos pos, Decl *d, Type *et, const char *vname, Type *expect);
static Type *static_type(FnCtx *F, Decl *d, Node *tnode, Type *expect);

static Val fn_value_of(FnCtx *F, Decl *d, Pos pos);
static Val gen_lambda(FnCtx *F, Node *n, Type *expect);
static Node *mk_ident(Pos pos, const char *name);
static Node *mk_method_call(Node *recv, const char *method, Pos pos, Node *a1, Node *a2);
static Val gen_call_decl(FnCtx *F, Node *call, Decl *d, Type *self, Vec *explicit, Type *expect, Val *recv);
static Val gen_call_value(FnCtx *F, Node *call, Val f, Type *expect);

static Val gen_ident(FnCtx *F, Node *n, Type *expect) {
    Local *l = find_local(F, n->name);
    if (l) return load_local(F, l);
    /* a constant */
    char *seg[1] = {n->name};
    int used = 0;
    Decl *d = find_decl(F->g, F->file, seg, 1, &used, n->pos);
    if (used < 0) return err_val();
    if (!d && strcmp(n->name, "Null") == 0) {
        if (expect && (expect->k == T_PTR || (expect->k == T_CTYPE && strcmp(expect->ll, "ptr") == 0))) {
            Val v = {expect, "null"};
            return v;
        }
        diag_error(n->pos, "`Null` is a pointer value; use it where a pointer type (`*T` or `CString`) is expected");
        return err_val();
    }
    if (d && d->node->kind == D_CONST) {
        Type *t = resolve_type(F->g, d->file, make_env(d, NULL, NULL), d->node->a);
        Val v = gen_expr(F, d->node->b, t);
        return coerce(F, d->node->b->pos, v, t, "a constant");
    }
    if (d && d->node->kind == D_FN && used == 1) return fn_value_of(F, d, n->pos);
    if (!d) {
        Decl *owner = prelude_variant_owner(F, n->name);
        if (owner) return gen_variant(F, NULL, n->pos, owner, static_type(F, owner, NULL, expect), n->name, expect);
    }
    diag_error(n->pos, "unknown name `%s`", n->name);
    return err_val();
}

static Val gen_tuple(FnCtx *F, Node *n, Type *expect) {
    Vec vals = {0}, tys = {0};
    for (int i = 0; i < n->list.n; i++) {
        Type *e = (expect && expect->k == T_TUPLE && i < expect->args.n) ? expect->args.v[i] : NULL;
        Val *v = zalloc(sizeof *v);
        *v = gen_expr(F, n->list.v[i], e);
        if (v->t == ty_never) return never_val(expect);
        vec_push(&vals, v);
        vec_push(&tys, v->t);
    }
    for (int i = 0; i < tys.n; i++)
        if (is_err(tys.v[i])) return err_val();
    Type *tt = ty_tuple(tys);
    char *acc = "undef";
    for (int i = 0; i < vals.n; i++) {
        Val *v = vals.v[i];
        char *r = tmp(F);
        emit(F, "%s = insertvalue %s %s, %s %s, %d", r, type_ll(tt), acc, type_ll(v->t), v->v, i);
        acc = r;
    }
    Val res = {tt, acc};
    return res;
}

static Val gen_if(FnCtx *F, Node *n, Type *expect) {
    int errs = zv_error_count;
    Val c = gen_expr(F, n->a, ty_bool);
    c = coerce(F, n->a->pos, c, ty_bool, "an `if` condition");
    if (c.t == ty_never) return never_val(expect);
    char *then_l = newlabel(F, "then"), *else_l = newlabel(F, "else"), *end_l = newlabel(F, "endif");
    condbr(F, c.v, then_l, n->c ? else_l : end_l);
    start_block(F, then_l);
    Type *want = (expect && expect != ty_unit) ? expect : NULL;
    int ns1 = push_narrow(F, n->a, true, errs);
    Val v1 = gen_block(F, n->b, n->c ? want : ty_unit);
    if (want && want->k == T_UNION && v1.t != want && v1.t != ty_never && !is_err(v1.t))
        v1 = coerce(F, n->b->pos, v1, want, "an `if` branch");
    while (ns1-- > 0) pop_scope(F);
    char *pred1 = F->cur;
    bool live1 = !F->term;
    if (live1) br(F, end_l);
    if (!n->c) {
        start_block(F, end_l);
        return unit_val();
    }
    start_block(F, else_l);
    Type *t1 = (v1.t == ty_never) ? want : v1.t;
    Val v2;
    int ns2 = push_narrow(F, n->a, false, errs);
    if (n->c->kind == N_IF) v2 = gen_if(F, n->c, t1);
    else v2 = gen_block(F, n->c, t1);
    if (t1 && t1->k == T_UNION && v2.t != t1 && v2.t != ty_never && !is_err(v2.t))
        v2 = coerce(F, n->c->pos, v2, t1, "an `if` branch");
    while (ns2-- > 0) pop_scope(F);
    char *pred2 = F->cur;
    bool live2 = !F->term;
    if (live2) br(F, end_l);
    start_block(F, end_l);
    if (!live1 && !live2) {
        emit(F, "unreachable");
        F->term = true;
        return never_val(want);
    }
    Type *rt = live1 ? v1.t : v2.t;
    if (live1 && live2 && v1.t != v2.t && !is_err(v1.t) && !is_err(v2.t)) {
        diag_error(n->c->pos, "the branches of this `if` have different types: `%s` and `%s`", v1.t->name, v2.t->name);
        return err_val();
    }
    if (is_err(rt)) return err_val();
    if (rt == ty_unit) return unit_val();
    char *r = tmp(F);
    if (live1 && live2)
        emit(F, "%s = phi %s [ %s, %%%s ], [ %s, %%%s ]", r, type_ll(rt), v1.v, pred1, v2.v, pred2);
    else if (live1)
        emit(F, "%s = phi %s [ %s, %%%s ]", r, type_ll(rt), v1.v, pred1);
    else
        emit(F, "%s = phi %s [ %s, %%%s ]", r, type_ll(rt), v2.v, pred2);
    Val res = {rt, r};
    return res;
}

/* Does this block always leave (return, break, continue)? Decided from the syntax only. */
static bool block_leaves(Node *b) {
    if (!b || b->kind != N_BLOCK) return false;
    Node *last = b->a;
    if (!last && b->list.n) last = b->list.v[b->list.n - 1];
    if (!last) return false;
    if (last->kind == N_EXPRSTMT) last = last->a;
    return last->kind == N_RETURN || last->kind == N_BREAK || last->kind == N_CONTINUE;
}

static Val gen_block(FnCtx *F, Node *blk, Type *expect) {
    push_scope(F);
    int extra = 0;
    for (int i = 0; i < blk->list.n; i++) {
        Node *st = blk->list.v[i];
        int errs = zv_error_count;
        gen_stmt(F, st);
        /* `if (x is E) { return ... }`: afterwards x is known not to be an E */
        Node *e = st->kind == N_EXPRSTMT ? st->a : st;
        if (e->kind == N_IF && !e->c && block_leaves(e->b)) extra += push_narrow(F, e->a, false, errs);
    }
    Val v = unit_val();
    if (blk->a) {
        v = gen_expr(F, blk->a, expect == ty_unit ? NULL : expect);
        if (expect == ty_unit && v.t != ty_never) v = unit_val();
    }
    for (int i = 0; i < extra; i++) pop_scope(F);
    pop_scope(F);
    if (F->term && v.t != ty_never) return never_val(expect ? expect : v.t);
    return v;
}

static Val gen_expr(FnCtx *F, Node *n, Type *expect) {
    switch (n->kind) {
    case N_INT: return gen_int_lit(F, n, expect, false);
    case N_FLOAT: return gen_float_lit(F, n, expect, false);
    case N_BOOL: {
        Val v = {ty_bool, n->ival ? "true" : "false"};
        return v;
    }
    case N_CHAR: {
        Val v = {ty_char, zsprintf("%llu", (unsigned long long)n->ival)};
        return v;
    }
    case N_BYTE: {
        Val v = {ty_u8, zsprintf("%llu", (unsigned long long)n->ival)};
        return v;
    }
    case N_STRING: return gen_string_lit(F, n);
    case N_UNIT: return unit_val();
    case N_IDENT: return gen_ident(F, n, expect);
    case N_TUPLE: return gen_tuple(F, n, expect);
    case N_INDEX: return gen_index_read(F, n, expect);
    case N_ARRAY: return gen_array_lit(F, n, expect);
    case N_STRUCTLIT: return gen_struct_lit(F, n, expect);
    case N_UNARY: return gen_unary(F, n, expect);
    case N_LAMBDA: return gen_lambda(F, n, expect);
    case N_ASYNC: {
        /* async { b }  ->  Zevic.Async.Spawn(() => { b }) */
        Module *am = prog_find_module(F->g->pg, "Zevic.Async");
        Decl *sp = am ? module_find(am, "Spawn") : NULL;
        if (!sp) {
            diag_error(n->pos, "internal error: `Zevic.Async.Spawn` is missing from the standard library");
            return err_val();
        }
        Node *lam = node_new(N_LAMBDA, n->pos);
        lam->b = n->b;
        lam->flag = true;
        Node *call = node_new(N_CALL, n->pos);
        call->a = mk_ident(n->pos, "Spawn");
        vec_push(&call->list, lam);
        return gen_call_decl(F, call, sp, NULL, NULL, expect, NULL);
    }
    case N_AWAIT: return gen_expr(F, mk_method_call(n->a, "Await", n->pos, NULL, NULL), expect);
    case N_BINARY: return gen_binary(F, n, expect);
    case N_IS: return gen_is(F, n);
    case N_IF: return gen_if(F, n, expect);
    case N_BLOCK: return gen_block(F, n, expect);
    case N_CALL: return gen_call(F, n, expect);
    case N_MEMBER: return gen_member(F, n, expect);
    case N_MATCH: return gen_match(F, n, expect);
    case N_TRY: return gen_try(F, n, expect);
    case N_THIS: {
        Place p = gen_place(F, n);
        if (is_err(p.t)) return err_val();
        return load_place(F, p);
    }
    case N_RETURN: {
        Val v = unit_val();
        if (!F->ret) {
            /* a lambda whose result type is not known yet takes the type of its first `return` */
            if (n->a) {
                v = gen_expr(F, n->a, NULL);
                if (v.t == ty_never || is_err(v.t)) return v;
                F->ret = v.t;
            } else {
                F->ret = ty_unit;
            }
        } else if (n->a) {
            v = gen_expr(F, n->a, F->ret);
            v = coerce(F, n->a->pos, v, F->ret, "a `return` value");
            if (v.t == ty_never) return v;
        } else if (F->ret != ty_unit && !is_err(F->ret)) {
            diag_error(n->pos, "this function returns `%s`, so `return` needs a value", F->ret->name);
        }
        for (Scope *s = F->scope; s; s = s->up) run_defers_in(F, s);
        if (F->ret == ty_unit || F->ret == ty_never) emit(F, "ret void");
        else emit(F, "ret %s %s", type_ll(F->ret), v.v);
        F->term = true;
        return never_val(expect);
    }
    case N_BREAK: case N_CONTINUE: {
        if (!F->loops.n) {
            diag_error(n->pos, "`%s` can only be used inside a loop", n->kind == N_BREAK ? "break" : "continue");
            return err_val();
        }
        Loop *lp = F->loops.v[F->loops.n - 1];
        if (n->name) {
            lp = NULL;
            for (int i = F->loops.n - 1; i >= 0; i--) {
                Loop *c = F->loops.v[i];
                if (c->label && strcmp(c->label, n->name) == 0) { lp = c; break; }
            }
            if (!lp) {
                diag_error(n->pos, "there is no loop labeled `%s` around this `%s` (write `%s@ for (...) { ... }` on the loop)",
                           n->name, n->kind == N_BREAK ? "break" : "continue", n->name);
                return err_val();
            }
        }
        for (Scope *s = F->scope; s && s != lp->scope; s = s->up) run_defers_in(F, s);
        if (n->kind == N_BREAK) lp->brk_used = true;
        br(F, n->kind == N_BREAK ? lp->brk : lp->cont);
        return never_val(expect);
    }
    default:
        diag_error(n->pos, "this kind of expression is not supported yet (%s)", node_kind_name(n->kind));
        return err_val();
    }
}

/* =========================================================================
 * Statements
 * ========================================================================= */

static void run_defers_in(FnCtx *F, Scope *s) {
    for (int i = s->defers.n - 1; i >= 0; i--) {
        Scope *saved = F->scope;
        Scope *t = zalloc(sizeof *t);
        t->up = s;
        F->scope = t;
        Node *d = s->defers.v[i];
        if (d->kind == N_BLOCK) {
            Val v = gen_block(F, d, ty_unit);
            (void)v;
        } else {
            gen_stmt(F, d);
        }
        F->scope = saved;
    }
}

/* Bind an irrefutable pattern to a value. */
static void bind_pattern(FnCtx *F, Node *pat, Val v, bool mut) {
    switch (pat->kind) {
    case P_WILD:
        return;
    case P_BIND: {
        if (is_err(v.t)) {
            declare_local(F, pat->name, ty_err, mut, pat->pos);
            return;
        }
        Local *l = declare_local(F, pat->name, v.t, mut, pat->pos);
        if (v.t != ty_unit) store_to(F, v.t, v, l->ptr);
        return;
    }
    case P_TUPLE: {
        if (v.t->k != T_TUPLE) {
            if (!is_err(v.t)) diag_error(pat->pos, "cannot destructure a value of type `%s` as a tuple", v.t->name);
            return;
        }
        if (v.t->args.n != pat->list.n) {
            diag_error(pat->pos, "the tuple pattern has %d element(s), but the value has %d", pat->list.n, v.t->args.n);
            return;
        }
        for (int i = 0; i < pat->list.n; i++) {
            Type *et = v.t->args.v[i];
            char *r = tmp(F);
            emit(F, "%s = extractvalue %s %s, %d", r, type_ll(v.t), v.v, i);
            Val ev = {et, r};
            bind_pattern(F, pat->list.v[i], ev, mut);
        }
        return;
    }
    default:
        diag_error(pat->pos, "this pattern can fail to match; only `_`, names and tuples can be used here");
    }
}

static TokKind base_op(TokKind k) {
    switch (k) {
    case TK_PLUSEQ: return TK_PLUS;
    case TK_MINUSEQ: return TK_MINUS;
    case TK_STAREQ: return TK_STAR;
    case TK_SLASHEQ: return TK_SLASH;
    case TK_PERCENTEQ: return TK_PERCENT;
    case TK_AMPEQ: return TK_AMP;
    case TK_PIPEEQ: return TK_PIPE;
    case TK_CARETEQ: return TK_CARET;
    case TK_SHLEQ: return TK_SHL;
    case TK_SHREQ: return TK_SHR;
    default: return TK_ASSIGN;
    }
}


/* The address of an assignable place; sets *t to its type. NULL after an error. */
static char *gen_lvalue(FnCtx *F, Node *n, Type **t) {
    if (n->kind == N_IDENT) {
        Local *l = find_local(F, n->name);
        if (!l) {
            diag_error(n->pos, "cannot assign to `%s`: it is not a variable", n->name);
            return NULL;
        }
        if (!l->mut) {
            diag_error(n->pos, "cannot assign to `%s` because it is not `mut`", n->name);
            diag_note("declare it with `mut %s: %s = ...` to allow changes", n->name, l->t->name);
            return NULL;
        }
        *t = l->t;
        return l->ptr;
    }
    if (n->kind == N_MEMBER && !is_static_path(F, n)) {
        Place p = gen_place(F, n);
        if (is_err(p.t) || p.t == ty_never || !p.ptr) return NULL;
        if (!p.mut) {
            const char *root = n->a->kind == N_THIS ? "this" : NULL;
            if (root)
                diag_error(n->pos, "cannot assign to field `%s` here: `this` can only be changed inside a `mut fn` (or `Init`)", n->name);
            else
                diag_error(n->pos, "cannot assign to field `%s` because `%s` is not `mut`", n->name, root_name(n));
            return NULL;
        }
        *t = p.t;
        return p.ptr;
    }
    diag_error(n->pos, "this expression cannot be assigned to");
    return NULL;
}

static void gen_assign_place(FnCtx *F, Node *n, Node **wb_elem, char **wb_name);

/* After changing a field of a struct element (`xs[i].F = v`), the changed copy goes back into the array. */
static void gen_assign(FnCtx *F, Node *n) {
    if (n->a->kind == N_INDEX) {
        gen_index_assign(F, n);
        return;
    }
    Node *el = NULL;
    char *name = NULL;
    gen_assign_place(F, n, &el, &name);
    if (el) {
        if (!is_pure_expr(el->a) || !is_pure_expr(el->b)) {
            diag_error(n->pos, "changing a field of an array element needs a simple array and index (put them in variables first)");
            return;
        }
        Node *call = mk_method_call(el->a, "SetIndex", n->pos, el->b, mk_ident(n->pos, name));
        call->flag3 = true;
        gen_expr(F, call, NULL);
    }
}

static void gen_assign_place(FnCtx *F, Node *n, Node **wb_elem, char **wb_name) {
    Type *t = NULL;
    F->wb_elem = NULL;
    char *ptr = gen_lvalue(F, n->a, &t);
    /* the right-hand side may read elements too, so the write-back is noted now */
    *wb_elem = F->wb_elem;
    *wb_name = F->wb_name;
    F->wb_elem = NULL;
    if (!ptr) {
        gen_expr(F, n->b, NULL);
        return;
    }
    if (n->op == TK_ASSIGN) {
        Val v = gen_expr(F, n->b, t);
        v = coerce(F, n->b->pos, v, t, "an assignment");
        if (v.t == ty_never || is_err(v.t)) return;
        store_to(F, t, v, ptr);
        return;
    }
    TokKind op = base_op(n->op);
    char *cur = tmp(F);
    emit(F, "%s = load %s, ptr %s", cur, type_ll(t), ptr);
    Val a = {t, cur};
    bool shift = op == TK_SHL || op == TK_SHR;
    Val b = gen_expr(F, n->b, shift ? ty_i32 : t);
    if (b.t == ty_never || is_err(b.t) || is_err(t)) return;
    Val r;
    if (shift) {
        if (!ty_is_int(t) || b.t != ty_i32) {
            diag_error(n->pos, "`%s` needs an integer variable and an `I32` amount", tok_name(n->op));
            return;
        }
        r = gen_shift(F, op, a, b, n->pos);
    } else {
        b = coerce(F, n->b->pos, b, t, "a compound assignment");
        if (is_err(b.t)) return;
        if (t == ty_string && op == TK_PLUS) {
            char *ap, *an, *bp, *bn;
            str_parts(F, a, &ap, &an);
            str_parts(F, b, &bp, &bn);
            rt_decl(F, "zevic_str_concat", "declare void @zevic_str_concat(ptr, ptr, i64, ptr, i64)");
            r = call_str_out(F, "zevic_str_concat", zsprintf("ptr %s, i64 %s, ptr %s, i64 %s", ap, an, bp, bn));
        } else if (ty_is_float(t) && op != TK_AMP && op != TK_PIPE && op != TK_CARET) {
            r = gen_float_arith(F, op, a, b);
        } else if (ty_is_int(t)) {
            r = gen_int_arith(F, op, a, b, n->pos);
        } else {
            diag_error(n->pos, "`%s` cannot be used with values of type `%s`", tok_name(n->op), t->name);
            return;
        }
    }
    store_to(F, t, r, ptr);
}

static void gen_while(FnCtx *F, Node *n) {
    char *head = newlabel(F, "while.cond"), *body = newlabel(F, "while.body"), *end = newlabel(F, "while.end");
    goto_block(F, head);
    Val c = gen_expr(F, n->a, ty_bool);
    c = coerce(F, n->a->pos, c, ty_bool, "a `while` condition");
    if (c.t != ty_never) condbr(F, c.v, body, end);
    start_block(F, body);
    Loop lp = {end, head, F->scope, false, n->name};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    if (!F->term) br(F, head);
    start_block(F, end);
}

static void gen_loop(FnCtx *F, Node *n) {
    char *body = newlabel(F, "loop.body"), *end = newlabel(F, "loop.end");
    goto_block(F, body);
    Loop lp = {end, body, F->scope, false, n->name};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    if (!F->term) br(F, body);
    start_block(F, end);
    if (!lp.brk_used) {
        /* a `loop` with no `break` never finishes, so what follows it is unreachable */
        emit(F, "unreachable");
        F->term = true;
    }
}

static void gen_for_range(FnCtx *F, Node *n) {
    Node *r = n->d;
    Val a, b;
    gen_pair(F, r->a, r->b, NULL, &a, &b);
    if (a.t == ty_never || b.t == ty_never || is_err(a.t) || is_err(b.t)) return;
    if (!ty_is_int(a.t) || a.t != b.t) {
        diag_error(r->pos, "the bounds of a range must be integers of the same type, found `%s` and `%s`", a.t->name, b.t->name);
        return;
    }
    Type *t = a.t;
    char *ll = type_ll(t);
    bool sgn = ty_is_signed(t);
    push_scope(F);
    char *ctr = new_slot(F, t);
    store_to(F, t, a, ctr);
    char *head = newlabel(F, "for.cond"), *body = newlabel(F, "for.body"), *cont = newlabel(F, "for.next"),
         *end = newlabel(F, "for.end");
    goto_block(F, head);
    char *cur = tmp(F), *c = tmp(F);
    emit(F, "%s = load %s, ptr %s", cur, ll, ctr);
    const char *pred = r->flag ? (sgn ? "sle" : "ule") : (sgn ? "slt" : "ult");
    emit(F, "%s = icmp %s %s %s, %s", c, pred, ll, cur, b.v);
    condbr(F, c, body, end);
    start_block(F, body);
    /* the loop variable is an immutable alias of the counter */
    if (n->a->kind == P_BIND && F->heap.cap && dict_has(&F->heap, n->a->name)) {
        /* a lambda may capture it: every iteration gets its own cell */
        Local *l = declare_local(F, n->a->name, t, false, n->a->pos);
        store_to(F, t, (Val){t, cur}, l->ptr);
    } else if (n->a->kind == P_BIND) {
        Local *l = zalloc(sizeof *l);
        l->name = n->a->name;
        l->t = t;
        l->ptr = ctr;
        l->mut = false;
        l->pos = n->a->pos;
        vec_push(&F->scope->locals, l);
    } else if (n->a->kind != P_WILD) {
        diag_error(n->a->pos, "a range loop variable must be a name or `_`");
    }
    if (n->c) diag_error(n->c->pos, "a range has no index; use a single loop variable");
    Loop lp = {end, cont, F->scope->up, false, n->name};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    if (!F->term) br(F, cont);
    start_block(F, cont);
    char *cur2 = tmp(F);
    emit(F, "%s = load %s, ptr %s", cur2, ll, ctr);
    if (r->flag) {
        /* inclusive: stop after the upper bound without overflowing */
        char *last = tmp(F);
        char *step = newlabel(F, "for.step");
        emit(F, "%s = icmp eq %s %s, %s", last, ll, cur2, b.v);
        condbr(F, last, end, step);
        start_block(F, step);
    }
    char *nx = tmp(F);
    emit(F, "%s = add %s %s, 1", nx, ll, cur2);
    store_to(F, t, (Val){t, nx}, ctr);
    br(F, head);
    start_block(F, end);
    pop_scope(F);
}

static void gen_for(FnCtx *F, Node *n) {
    if (n->d->kind == N_RANGE) {
        gen_for_range(F, n);
        return;
    }
    gen_for_iter(F, n);
}

static void gen_let(FnCtx *F, Node *n) {
    /* `n->b == NULL` only happens for lets the compiler writes itself: the type comes from the value */
    Type *t = n->b ? resolve_type(F->g, F->file, F->env, n->b) : NULL;
    Val v = gen_expr(F, n->c, t);
    if (!t) t = (v.t == ty_never || is_err(v.t)) ? ty_err : v.t;
    v = coerce(F, n->c->pos, v, t, "a variable declaration");
    if (v.t == ty_never) {
        /* the initialiser never completes; still declare the names so later code can be checked */
        v = (Val){t, "undef"};
    }
    if (is_err(v.t)) v = (Val){t, "undef"};
    bind_pattern(F, n->a, v, n->flag);
}

static void check_loop_label(FnCtx *F, Node *s) {
    if (!s->name) return;
    for (int i = 0; i < F->loops.n; i++) {
        Loop *c = F->loops.v[i];
        if (c->label && strcmp(c->label, s->name) == 0)
            diag_error(s->pos, "the label `%s` is already used by a loop around this one", s->name);
    }
}

static void gen_stmt(FnCtx *F, Node *s) {
    if (s->kind == N_WHILE || s->kind == N_LOOP || s->kind == N_FOR) check_loop_label(F, s);
    switch (s->kind) {
    case N_LET: gen_let(F, s); break;
    case N_ASSIGN: gen_assign(F, s); break;
    case N_EXPRSTMT: gen_expr(F, s->a, NULL); break;
    case N_WHILE: gen_while(F, s); break;
    case N_LOOP: gen_loop(F, s); break;
    case N_FOR: gen_for(F, s); break;
    case N_DEFER: vec_push(&F->scope->defers, s->a); break;
    default: gen_expr(F, s, NULL); break;
    }
}

/* =========================================================================
 * Calls
 * ========================================================================= */

static bool flatten_path(Node *n, Vec *segs, Vec *nodes) {
    if (n->kind == N_IDENT) {
        vec_push(segs, n->name);
        vec_push(nodes, n);
        return true;
    }
    if (n->kind == N_MEMBER && flatten_path(n->a, segs, nodes)) {
        vec_push(segs, n->name);
        vec_push(nodes, n);
        return true;
    }
    return false;
}

static bool is_fn_tparam(Node *fn, const char *name) {
    for (int i = 0; i < fn->list2.n; i++)
        if (strcmp(((Node *)fn->list2.v[i])->name, name) == 0) return true;
    return false;
}

/* Does this type syntax mention a type parameter of `fn` that is not bound yet? */
static bool syntax_uses_unbound(Node *te, Node *fn, Env *env) {
    switch (te->kind) {
    case N_TYPE_NAME:
        if (te->path.n == 1 && is_fn_tparam(fn, te->path.v[0]) && !env_get(env, te->path.v[0])) return true;
        for (int i = 0; i < te->list.n; i++)
            if (syntax_uses_unbound(te->list.v[i], fn, env)) return true;
        return false;
    case N_TYPE_TUPLE: case N_TYPE_UNION:
        for (int i = 0; i < te->list.n; i++)
            if (syntax_uses_unbound(te->list.v[i], fn, env)) return true;
        return false;
    case N_TYPE_FN:
        for (int i = 0; i < te->list.n; i++)
            if (syntax_uses_unbound(te->list.v[i], fn, env)) return true;
        return syntax_uses_unbound(te->a, fn, env);
    case N_TYPE_PTR:
        return syntax_uses_unbound(te->a, fn, env);
    default:
        return false;
    }
}

/* Bind the type parameters in `te` by matching it against `actual`. */
static void unify(Node *te, Type *actual, Node *fn, Env *env) {
    if (is_err(actual) || actual == ty_never) return;
    switch (te->kind) {
    case N_TYPE_NAME:
        if (te->path.n == 1 && is_fn_tparam(fn, te->path.v[0])) {
            if (!env_get(env, te->path.v[0])) env_add(env, te->path.v[0], actual);
            return;
        }
        if (actual->decl && te->list.n == actual->args.n)
            for (int i = 0; i < te->list.n; i++) unify(te->list.v[i], actual->args.v[i], fn, env);
        return;
    case N_TYPE_TUPLE:
        if (actual->k == T_TUPLE && actual->args.n == te->list.n)
            for (int i = 0; i < te->list.n; i++) unify(te->list.v[i], actual->args.v[i], fn, env);
        return;
    case N_TYPE_FN:
        if (actual->k == T_FN && actual->args.n == te->list.n) {
            for (int i = 0; i < te->list.n; i++) unify(te->list.v[i], actual->args.v[i], fn, env);
            unify(te->a, actual->ret, fn, env);
        }
        return;
    case N_TYPE_PTR:
        if (actual->k == T_PTR) unify(te->a, actual->args.v[0], fn, env);
        return;
    default:
        return;
    }
}

/* =========================================================================
 * User types: members, places, construction
 * ========================================================================= */

static Val gen_member(FnCtx *F, Node *n, Type *expect);
static Val gen_call_decl(FnCtx *F, Node *call, Decl *d, Type *self, Vec *explicit, Type *expect, Val *recv);

static bool is_user_type(Type *t) {
    return t->k == T_STRUCT || t->k == T_CLASS || t->k == T_ENUM;
}


static bool name_is_pub(const char *n) {
    return n[0] >= 'A' && n[0] <= 'Z';
}

static Decl *method_decl(Type *t, Node *fn) {
    if (fn->aux) return fn->aux;
    Decl *d = zalloc(sizeof *d);
    d->node = fn;
    d->mod = t->decl->mod;
    d->file = t->decl->file;
    d->name = fn->name;
    d->is_pub = name_is_pub(fn->name);
    fn->aux = d;
    return d;
}

static Node *find_member_fn(Type *t, const char *name) {
    if (!t->decl) return NULL;
    Node *td = t->decl->node;
    for (int i = 0; i < td->list.n; i++) {
        Node *m = td->list.v[i];
        if (m->kind == D_FN && strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}


/* =========================================================================
 * Interfaces
 * ========================================================================= */

typedef struct {
    Node *fn;    /* the method as declared in the interface */
    Type *owner; /* the interface type (with type arguments) that declares it */
} IMethod;

static Env *iface_env(Type *it, Type *self) {
    Env *env = env_new();
    env->self = self;
    Node *n = it->decl->node;
    for (int i = 0; i < n->list2.n && i < it->args.n; i++) env_add(env, ((Node *)n->list2.v[i])->name, it->args.v[i]);
    return env;
}

/* The interfaces a type implements (for an interface: the ones it extends). */
static Vec *type_supers(Gen *g, Type *t) {
    if (t->supers_done) return &t->supers;
    t->supers_done = true;
    if (!t->decl) return &t->supers;
    Node *n = t->decl->node;
    Env *env = env_new();
    env->self = t;
    for (int i = 0; i < n->list2.n && i < t->args.n; i++) env_add(env, ((Node *)n->list2.v[i])->name, t->args.v[i]);
    bool saved = g_allow_selfy_iface;
    g_allow_selfy_iface = true;
    for (int i = 0; i < n->list3.n; i++) {
        Node *te = n->list3.v[i];
        Type *s = resolve_type(g, t->decl->file, env, te);
        if (is_err(s)) continue;
        if (s->k != T_IFACE) {
            diag_error(te->pos, "`%s` is not an interface, so `%s` cannot implement it", s->name, t->name);
            continue;
        }
        vec_push(&t->supers, s);
    }
    g_allow_selfy_iface = saved;
    return &t->supers;
}

static bool type_implements_d(Gen *g, Type *t, Type *it, int depth) {
    if (t == it) return true;
    if (!t->decl || depth > 20) return false;
    Vec *s = type_supers(g, t);
    for (int i = 0; i < s->n; i++)
        if (type_implements_d(g, s->v[i], it, depth + 1)) return true;
    return false;
}

static bool type_implements(Gen *g, Type *t, Type *it) {
    return type_implements_d(g, t, it, 0);
}

/* Every instance method of an interface and the ones it extends, parents first. */
static void collect_iface_methods(Gen *g, Type *it, Vec *out, int depth) {
    if (depth > 20 || !it->decl) return;
    Vec *sup = type_supers(g, it);
    for (int i = 0; i < sup->n; i++) collect_iface_methods(g, sup->v[i], out, depth + 1);
    Node *n = it->decl->node;
    for (int i = 0; i < n->list.n; i++) {
        Node *m = n->list.v[i];
        if (m->kind != D_FN || m->is_static) continue;
        bool dup = false;
        for (int k = 0; k < out->n; k++)
            if (strcmp(((IMethod *)out->v[k])->fn->name, m->name) == 0) dup = true;
        if (dup) continue;
        IMethod *im = zalloc(sizeof *im);
        im->fn = m;
        im->owner = it;
        vec_push(out, im);
    }
}

/* A default method (one with a body) that `t` gets from its interfaces. `*conflict` is set when two interfaces
 * both supply a default with this name. */
static bool find_default(Gen *g, Type *t, const char *name, IMethod *out, bool *conflict) {
    if (!t->decl) return false;
    Vec all = {0};
    Vec *sup = type_supers(g, t);
    for (int i = 0; i < sup->n; i++) collect_iface_methods(g, sup->v[i], &all, 0);
    bool found = false;
    for (int i = 0; i < all.n; i++) {
        IMethod *m = all.v[i];
        if (strcmp(m->fn->name, name) != 0 || !m->fn->b) continue;
        if (found && out->fn != m->fn) {
            if (conflict) *conflict = true;
            continue;
        }
        *out = *m;
        found = true;
    }
    return found;
}

static Decl *default_decl(Gen *g, Type *t, IMethod *m) {
    char *key = zsprintf("%s|%s|%p", t->name, m->fn->name, (void *)m->fn);
    Decl *d = dict_get(&g->defaults, key);
    if (d) return d;
    d = zalloc(sizeof *d);
    d->node = m->fn;
    d->mod = m->owner->decl->mod;
    d->file = m->owner->decl->file;
    d->name = m->fn->name;
    d->is_pub = name_is_pub(m->fn->name);
    d->iface = m->owner;
    dict_put(&g->defaults, key, d);
    return d;
}

static char *sig_string(Gen *g, Vec *ptypes, Type *ret) {
    (void)g;
    SB sb = {0};
    sb_puts(&sb, "(");
    for (int i = 0; i < ptypes->n; i++) sb_printf(&sb, "%s%s", i ? ", " : "", ((Type *)ptypes->v[i])->name);
    sb_printf(&sb, ") -> %s", ret->name);
    return sb_str(&sb);
}

static void fn_sig(Gen *g, SourceFile *f, Env *env, Node *fn, Vec *ptypes, Type **ret) {
    for (int i = 0; i < fn->list.n; i++) {
        Node *p = fn->list.v[i];
        if (p->flag3) continue;
        vec_push(ptypes, resolve_type(g, f, env, p->a));
    }
    *ret = fn->a ? resolve_type(g, f, env, fn->a) : ty_unit;
}

/* Does `t` provide everything its interfaces ask for? */
static void check_conformance(Gen *g, Type *t) {
    Node *tn = t->decl->node;
    Vec *sup = type_supers(g, t);
    Vec seen_defaults = {0};
    for (int si = 0; si < sup->n; si++) {
        Type *s = sup->v[si];
        Vec all = {0};
        collect_iface_methods(g, s, &all, 0);
        for (int i = 0; i < all.n; i++) {
            IMethod *m = all.v[i];
            Node *impl = find_member_fn(t, m->fn->name);
            if (!impl) {
                if (m->fn->b) {
                    bool dup = false;
                    for (int k = 0; k < seen_defaults.n; k++) {
                        IMethod *o = seen_defaults.v[k];
                        if (strcmp(o->fn->name, m->fn->name) == 0 && o->fn != m->fn) {
                            diag_error(tn->pos, "`%s` gets a default `%s` from both `%s` and `%s`; write its own `%s` to choose", t->name,
                                       m->fn->name, o->owner->name, m->owner->name, m->fn->name);
                        }
                        if (o->fn == m->fn) dup = true;
                    }
                    if (!dup) vec_push(&seen_defaults, m);
                } else {
                    diag_error(tn->pos, "`%s` does not implement `%s`, which `%s` requires", t->name, m->fn->name, s->name);
                }
                continue;
            }
            if (impl->list2.n || m->fn->list2.n) continue; /* generic methods are not compared */
            Vec ip = {0}, mp = {0};
            Type *iret, *mret;
            Decl *md = method_decl(t, impl);
            fn_sig(g, md->file, make_env(md, t, NULL), impl, &ip, &iret);
            fn_sig(g, m->owner->decl->file, iface_env(m->owner, t), m->fn, &mp, &mret);
            bool ok = ip.n == mp.n && iret == mret && impl->is_mut_fn == m->fn->is_mut_fn && impl->is_static == m->fn->is_static;
            for (int k = 0; ok && k < ip.n; k++)
                if (ip.v[k] != mp.v[k] && !is_err(ip.v[k]) && !is_err(mp.v[k])) ok = false;
            if (!ok && !is_err(iret) && !is_err(mret))
                diag_error(impl->pos, "`%s.%s` does not match the declaration in `%s`: it should be `%sfn %s%s`", t->name, impl->name, m->owner->name,
                           m->fn->is_mut_fn ? "mut " : "", m->fn->name, sig_string(g, &mp, mret));
        }
    }
}


/* The built-in types satisfy Equatable, Comparable and Hashable without declaring them. */
static bool builtin_satisfies(Type *t, const char *iface) {
    bool num = ty_is_int(t) || t == ty_char || t == ty_bool;
    if (strcmp(iface, "Equatable") == 0) return num || ty_is_float(t) || t == ty_string;
    if (strcmp(iface, "Comparable") == 0) return (ty_is_int(t) || t == ty_char || ty_is_float(t) || t == ty_string);
    if (strcmp(iface, "Hashable") == 0) return num || t == ty_string;
    return false;
}

static bool is_builtin_value(Type *t) {
    return ty_is_numeric(t) || t == ty_char || t == ty_bool || t == ty_string;
}

static bool satisfies(Gen *g, Type *t, Type *bound) {
    if (is_err(t) || t->k == T_PARAM || is_err(bound)) return true;
    if (is_builtin_value(t) && bound->decl && bound->decl->mod->is_std) return builtin_satisfies(t, bound->decl->name);
    return type_implements(g, t, bound);
}

/* Check the `<T: Bound + Bound>` constraints of a generic declaration against the type arguments. */
static void check_bounds(Gen *g, SourceFile *f, Node *owner, Vec *targs, Env *base, const char *what, Pos pos) {
    for (int i = 0; i < owner->list2.n && i < targs->n; i++) {
        Node *tp = owner->list2.v[i];
        if (!tp->list.n) continue;
        Type *arg = targs->v[i];
        if (is_err(arg) || arg->k == T_PARAM) continue;
        Env *env = env_new();
        if (base) {
            env->self = base->self;
            for (int k = 0; k < base->names.n; k++) env_add(env, base->names.v[k], base->types.v[k]);
        }
        for (int k = 0; k < owner->list2.n && k < targs->n; k++) env_add(env, ((Node *)owner->list2.v[k])->name, targs->v[k]);
        Type *saved_self = env->self;
        env->self = arg; /* `Self` in a bound is the constrained type */
        bool saved = g_allow_selfy_iface;
        g_allow_selfy_iface = true;
        for (int b = 0; b < tp->list.n; b++) {
            Type *bound = resolve_type(g, f, env, tp->list.v[b]);
            if (is_err(bound)) continue;
            if (bound->k != T_IFACE) {
                diag_error(((Node *)tp->list.v[b])->pos, "`%s` is not an interface, so it cannot be a constraint", bound->name);
                continue;
            }
            if (!satisfies(g, arg, bound)) {
                diag_error(pos, "`%s` does not satisfy the constraint `%s: %s` of `%s`", arg->name, tp->name, bound->name, what);
                diag_note("`%s` would have to implement `%s`", arg->name, bound->name);
            }
        }
        g_allow_selfy_iface = saved;
        env->self = saved_self;
    }
}


/* The method table of `t` for interface `it`: one entry per method, in `collect_iface_methods` order. */
static char *vtable_for(Gen *g, Type *t, Type *it) {
    char *key = zsprintf("%s|%s", t->name, it->name);
    char *old = dict_get(&g->vtables, key);
    if (old) return old;
    char *name = zsprintf("@.vt.%d", g->nvt++);
    dict_put(&g->vtables, key, name);
    Vec all = {0};
    collect_iface_methods(g, it, &all, 0);
    SB e = {0};
    for (int i = 0; i < all.n; i++) {
        IMethod *m = all.v[i];
        if (i) sb_puts(&e, ", ");
        if (m->fn->list2.n) {
            sb_puts(&e, "ptr null"); /* a generic method has no table slot */
            continue;
        }
        Node *impl = find_member_fn(t, m->fn->name);
        Decl *d = NULL;
        if (impl) {
            d = method_decl(t, impl);
        } else {
            IMethod dm;
            if (find_default(g, t, m->fn->name, &dm, NULL)) d = default_decl(g, t, &dm);
        }
        if (!d) {
            sb_puts(&e, "ptr null");
            continue;
        }
        Inst *in = get_inst(g, d, t, NULL);
        sb_printf(&e, "ptr @\"%s\"", in->name);
    }
    sb_printf(&g->globals, "%s = private unnamed_addr constant [%d x ptr] [%s]\n", name, all.n, sb_str(&e));
    return name;
}

/* A value of a type that implements `it`, as an interface value { object, method table }. */
static Val to_iface(FnCtx *F, Pos pos, Val v, Type *it, const char *what) {
    Gen *g = F->g;
    if (!type_implements(g, v.t, it)) {
        type_error(pos, it, v.t, what);
        diag_note("`%s` does not implement `%s`", v.t->name, it->name);
        return err_val();
    }
    char *obj;
    if (v.t->k == T_CLASS) {
        obj = v.v;
    } else {
        /* a struct or enum goes to the heap, so the interface value can point at it */
        rt_decl(F, "zevic_alloc", "declare ptr @zevic_alloc(i64)");
        obj = tmp(F);
        emit(F, "%s = call ptr @zevic_alloc(i64 %lld)", obj, (long long)ty_size(v.t));
        store_to(F, v.t, v, obj);
    }
    char *vt = vtable_for(g, v.t, it);
    char *a = tmp(F), *b = tmp(F);
    emit(F, "%s = insertvalue { ptr, ptr } undef, ptr %s, 0", a, obj);
    emit(F, "%s = insertvalue { ptr, ptr } %s, ptr %s, 1", b, a, vt);
    Val r = {it, b};
    return r;
}

/* iface.Method(args): look the function up in the method table. */
static Val gen_iface_method(FnCtx *F, Node *call, Place p, Node *recv_node, const char *name, Type *expect) {
    Gen *g = F->g;
    Type *it = p.t;
    Vec all = {0};
    collect_iface_methods(g, it, &all, 0);
    int idx = -1;
    for (int i = 0; i < all.n; i++)
        if (strcmp(((IMethod *)all.v[i])->fn->name, name) == 0) idx = i;
    if (idx < 0) {
        diag_error(call->pos, "interface `%s` has no method `%s`", it->name, name);
        for (int i = 0; i < call->list.n; i++) gen_expr(F, call->list.v[i], NULL);
        return err_val();
    }
    IMethod *m = all.v[idx];
    Node *fn = m->fn;
    if (fn->list2.n) {
        diag_error(call->pos, "`%s` is generic, so it cannot be called on an interface value of type `%s`", name, it->name);
        return err_val();
    }
    if (fn->is_mut_fn && !p.mut) {
        diag_error(call->pos, "cannot call `mut fn %s` because `%s` is not `mut`", name, root_name(recv_node));
        return err_val();
    }
    int nparams = 0;
    for (int i = 0; i < fn->list.n; i++)
        if (!((Node *)fn->list.v[i])->flag3) nparams++;
    if (call->list.n != nparams) {
        diag_error(call->pos, "`%s` takes %d argument(s), but %d were given", name, nparams, call->list.n);
        return err_val();
    }
    Env *env = iface_env(m->owner, NULL);
    SB args = {0};
    bool never = false;
    for (int i = 0; i < nparams; i++) {
        Node *pn = fn->list.v[i];
        Type *pt = resolve_type(g, m->owner->decl->file, env, pn->a);
        Val v = gen_expr(F, call->list.v[i], pt);
        if (v.t == ty_never) {
            never = true;
            continue;
        }
        v = coerce(F, ((Node *)call->list.v[i])->pos, v, pt, zsprintf("argument %d of `%s`", i + 1, name));
        if (is_err(v.t)) return err_val();
        sb_printf(&args, ", %s %s", type_ll(pt), v.v);
    }
    if (never) return never_val(expect);
    Type *ret = fn->a ? resolve_type(g, m->owner->decl->file, env, fn->a) : ty_unit;
    if (is_err(ret)) return err_val();
    Val recv = load_place(F, p);
    char *obj = tmp(F), *vt = tmp(F), *slot = tmp(F), *fp = tmp(F);
    emit(F, "%s = extractvalue { ptr, ptr } %s, 0", obj, recv.v);
    emit(F, "%s = extractvalue { ptr, ptr } %s, 1", vt, recv.v);
    emit(F, "%s = getelementptr inbounds ptr, ptr %s, i64 %d", slot, vt, idx);
    emit(F, "%s = load ptr, ptr %s", fp, slot);
    if (ret == ty_unit || ret == ty_never) {
        emit(F, "call void %s(ptr %s%s)", fp, obj, sb_str(&args));
        if (ret == ty_never) {
            emit(F, "unreachable");
            F->term = true;
            return never_val(NULL);
        }
        return unit_val();
    }
    char *r = tmp(F);
    emit(F, "%s = call %s %s(ptr %s%s)", r, type_ll(ret), fp, obj, sb_str(&args));
    Val res = {ret, r};
    return res;
}

static Node *find_static_field(Type *t, const char *name) {
    if (!t->decl) return NULL;
    Node *td = t->decl->node;
    for (int i = 0; i < td->list.n; i++) {
        Node *m = td->list.v[i];
        if (m->kind == N_FIELD && m->flag && strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

static Field *find_field(Type *t, const char *name, int *idx) {
    for (int i = 0; i < t->fields.n; i++) {
        Field *f = t->fields.v[i];
        if (strcmp(f->name, name) == 0) {
            if (idx) *idx = i;
            return f;
        }
    }
    return NULL;
}

static Variant *find_variant(Type *t, const char *name) {
    for (int i = 0; i < t->variants.n; i++) {
        Variant *v = t->variants.v[i];
        if (strcmp(v->name, name) == 0) return v;
    }
    return NULL;
}

/* A class object is laid out like a tuple of its fields. */
static Type *obj_tuple(Type *cls) {
    Vec ts = {0};
    for (int i = 0; i < cls->fields.n; i++) vec_push(&ts, ((Field *)cls->fields.v[i])->t);
    return ty_tuple(ts);
}

static bool check_vis(FnCtx *F, Type *t, const char *name, Pos pos, const char *what) {
    if (name_is_pub(name) || !t->decl || t->decl->mod == F->file->mod) return true;
    diag_error(pos, "%s `%s` of `%s` is private (names starting with a lowercase letter are only visible inside module `%s`)",
               what, name, t->name, t->decl->mod->name);
    return false;
}

static Place place_err(void) {
    Place p = {NULL, ty_err, false};
    return p;
}

static Val load_place(FnCtx *F, Place p) {
    if (is_err(p.t)) return err_val();
    if (p.t == ty_unit) return unit_val();
    char *t = tmp(F);
    emit(F, "%s = load %s, ptr %s", t, type_ll(p.t), p.ptr);
    Val v = {p.t, t};
    return v;
}

static Place spill(FnCtx *F, Val v) {
    Place p = {NULL, v.t, false};
    if (is_err(v.t) || v.t == ty_never) {
        p.t = is_err(v.t) ? ty_err : ty_never;
        return p;
    }
    p.ptr = new_slot(F, v.t);
    if (v.t != ty_unit) store_to(F, v.t, v, p.ptr);
    return p;
}

/* Is this a dotted path whose first name is not a variable (a module or type)? */
static bool is_static_path(FnCtx *F, Node *n) {
    Vec segs = {0}, nodes = {0};
    if (!flatten_path(n, &segs, &nodes)) return false;
    return find_local(F, segs.v[0]) == NULL;
}

static Place gen_place(FnCtx *F, Node *n) {
    switch (n->kind) {
    case N_IDENT: {
        Local *l = find_local(F, n->name);
        if (l) {
            Place p = {l->ptr, l->t, l->mut};
            return p;
        }
        break;
    }
    case N_THIS: {
        if (!F->has_this) {
            diag_error(n->pos, "`this` can only be used inside a method");
            return place_err();
        }
        Place tp = {F->this_slot, F->this_t, F->this_mut};
        return tp;
    }
    case N_INDEX: {
        /* an element of an array: a class element is a reference, so its fields can be changed in place; a struct
           element is copied into a local and stored back after the assignment (see gen_assign). Either way the
           array itself must be `mut`. */
        bool amut = false;
        if (n->a->kind == N_IDENT || n->a->kind == N_MEMBER || n->a->kind == N_THIS || n->a->kind == N_INDEX) {
            Place ap = gen_place(F, n->a);
            if (is_err(ap.t) || ap.t == ty_never) return place_err();
            amut = ap.mut;
        }
        Val v = gen_expr(F, n, NULL);
        if (v.t == ty_never || is_err(v.t)) return place_err();
        if (v.t->k == T_STRUCT) {
            static int nel = 0;
            char *name = zsprintf("$el%d", nel++);
            Local *l = declare_local(F, name, v.t, true, n->pos);
            store_to(F, v.t, v, l->ptr);
            F->wb_elem = n;
            F->wb_name = name;
            Place p = {l->ptr, v.t, amut};
            return p;
        }
        Place p = spill(F, v);
        p.mut = amut;
        return p;
    }
    case N_MEMBER: {
        if (is_static_path(F, n)) break;
        Place b = gen_place(F, n->a);
        if (is_err(b.t)) return place_err();
        if (b.t == ty_never) return b;
        Type *bt = b.t;
        if (bt->k != T_STRUCT && bt->k != T_CLASS) {
            diag_error(n->pos, "type `%s` has no field `%s`", bt->name, n->name);
            return place_err();
        }
        int idx = 0;
        Field *f = find_field(bt, n->name, &idx);
        if (!f) {
            if (find_member_fn(bt, n->name))
                diag_error(n->pos, "`%s` is a method of `%s`; call it with `()`", n->name, bt->name);
            else
                diag_error(n->pos, "type `%s` has no field `%s`", bt->name, n->name);
            return place_err();
        }
        if (!check_vis(F, bt, n->name, n->pos, "field")) return place_err();
        char *base = b.ptr;
        const char *sll;
        if (bt->k == T_CLASS) {
            base = tmp(F);
            emit(F, "%s = load ptr, ptr %s", base, b.ptr);
            sll = type_ll(obj_tuple(bt));
        } else {
            sll = type_ll(bt);
        }
        char *gep = tmp(F);
        emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", gep, sll, base, idx);
        Place p = {gep, f->t, b.mut};
        return p;
    }
    default:
        break;
    }
    Val v = gen_expr(F, n, NULL);
    return spill(F, v);
}

/* The name of the variable a path starts from (for messages). */
static const char *root_name(Node *n) {
    while (n && (n->kind == N_MEMBER || n->kind == N_INDEX)) n = n->a;
    if (n && n->kind == N_IDENT) return n->name;
    if (n && n->kind == N_THIS) return "this";
    return "this value";
}

/* Build an aggregate value from already generated parts. */
static Val make_aggregate(FnCtx *F, Type *tt, Vec *vals) {
    char *acc = "undef";
    for (int i = 0; i < vals->n; i++) {
        Val *v = vals->v[i];
        char *r = tmp(F);
        emit(F, "%s = insertvalue %s %s, %s %s, %d", r, type_ll(tt), acc, type_ll(v->t), v->v, i);
        acc = r;
    }
    if (vals->n == 0) acc = "zeroinitializer";
    Val res = {tt, acc};
    return res;
}

/* ---- enums ---- */

static char *tag_ptr(FnCtx *F, Type *et, char *slot) {
    char *p = tmp(F);
    emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 0", p, type_ll(et), slot);
    return p;
}

static char *payload_ptr(FnCtx *F, Type *et, char *slot) {
    if (et->payload_words == 0) return slot; /* every payload is empty (like Option<Unit>): the layout has no payload part */
    char *p = tmp(F);
    emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 1", p, type_ll(et), slot);
    return p;
}

static Val build_variant(FnCtx *F, Type *et, Variant *v, Vec *fieldvals) {
    char *slot = new_slot(F, et);
    emit(F, "store i32 %d, ptr %s", v->tag, tag_ptr(F, et, slot));
    if (v->payload && fieldvals->n) {
        Val pv = make_aggregate(F, v->payload, fieldvals);
        emit(F, "store %s %s, ptr %s", type_ll(v->payload), pv.v, payload_ptr(F, et, slot));
    }
    char *t = tmp(F);
    emit(F, "%s = load %s, ptr %s", t, type_ll(et), slot);
    Val r = {et, t};
    return r;
}

static int union_index(Type *u, Type *m) {
    for (int i = 0; i < u->args.n; i++)
        if (u->args.v[i] == m) return i;
    return -1;
}

/* A member value becomes a union value, or a union value becomes a wider union (a subset of its members). */
/* The same value in another union: every member of `from` that `to` has keeps its payload (members `to` lacks
   never reach here at run time, their tag becomes 0). */
static Val union_remap(FnCtx *F, Val v, Type *from, Type *to) {
    char *src = new_slot(F, from);
    emit(F, "store %s %s, ptr %s", type_ll(from), v.v, src);
    char *tg = tmp(F);
    emit(F, "%s = load i32, ptr %s", tg, tag_ptr(F, from, src));
    char *dst = new_slot(F, to);
    char *base = tg;
    for (int i = 0; i < from->args.n; i++) {
        int di = union_index(to, from->args.v[i]);
        if (di < 0) di = 0;
        if (di == i) continue;
        char *c = tmp(F);
        emit(F, "%s = icmp eq i32 %s, %d", c, tg, i);
        char *nx = tmp(F);
        emit(F, "%s = select i1 %s, i32 %d, i32 %s", nx, c, di, base);
        base = nx;
    }
    emit(F, "store i32 %s, ptr %s", base, tag_ptr(F, to, dst));
    int words = from->payload_words < to->payload_words ? from->payload_words : to->payload_words;
    if (words > 0) {
        char *pv = tmp(F);
        emit(F, "%s = load [%d x i64], ptr %s", pv, words, payload_ptr(F, from, src));
        emit(F, "store [%d x i64] %s, ptr %s", words, pv, payload_ptr(F, to, dst));
    }
    char *r = tmp(F);
    emit(F, "%s = load %s, ptr %s", r, type_ll(to), dst);
    Val out = {to, r};
    return out;
}

/* The member `m` of a union value. */
static Val union_extract(FnCtx *F, Val v, Type *m) {
    if (ty_size(m) == 0) {
        Val z = {m, m == ty_unit ? "zeroinitializer" : "zeroinitializer"};
        return z;
    }
    char *slot = new_slot(F, v.t);
    emit(F, "store %s %s, ptr %s", type_ll(v.t), v.v, slot);
    char *r = tmp(F);
    emit(F, "%s = load %s, ptr %s", r, type_ll(m), payload_ptr(F, v.t, slot));
    Val out = {m, r};
    return out;
}

static Val union_coerce(FnCtx *F, Pos pos, Val v, Type *want, const char *what) {
    if (v.t->k == T_UNION) {
        for (int i = 0; i < v.t->args.n; i++)
            if (union_index(want, v.t->args.v[i]) < 0) {
                type_error(pos, want, v.t, what);
                diag_note("`%s` is not a member of `%s`", ((Type *)v.t->args.v[i])->name, want->name);
                return err_val();
            }
        return union_remap(F, v, v.t, want);
    }
    int idx = union_index(want, v.t);
    if (idx < 0) {
        type_error(pos, want, v.t, what);
        diag_note("`%s` is not a member of `%s`", v.t->name, want->name);
        return err_val();
    }
    char *slot = new_slot(F, want);
    emit(F, "store i32 %d, ptr %s", idx, tag_ptr(F, want, slot));
    if (ty_size(v.t) > 0) emit(F, "store %s %s, ptr %s", type_ll(v.t), v.v, payload_ptr(F, want, slot));
    char *r = tmp(F);
    emit(F, "%s = load %s, ptr %s", r, type_ll(want), slot);
    Val out = {want, r};
    return out;
}

/* `Some` and `None` belong to Option, `Ok` and `Err` to Result (both live in Zevic.Core). */
static Decl *prelude_variant_owner(FnCtx *F, const char *name) {
    const char *owner = NULL;
    if (strcmp(name, "Some") == 0 || strcmp(name, "None") == 0) owner = "Option";
    else if (strcmp(name, "Ok") == 0 || strcmp(name, "Err") == 0) owner = "Result";
    if (!owner) return NULL;
    return lookup_in_prelude(F->g, owner);
}

static Node *find_variant_node(Node *enum_decl, const char *name) {
    for (int i = 0; i < enum_decl->list.n; i++) {
        Node *m = enum_decl->list.v[i];
        if (m->kind == N_VARIANT && strcmp(m->name, name) == 0) return m;
    }
    return NULL;
}

/* The type of a generic type's instance when it is known without looking at the arguments. */
static Type *static_type(FnCtx *F, Decl *d, Node *tnode, Type *expect) {
    Node *td = d->node;
    if (td->list2.n == 0) {
        Vec none = {0};
        return instantiate(F->g, d, none, td->pos);
    }
    if (tnode && tnode->list2.n) {
        Vec targs = {0};
        for (int i = 0; i < tnode->list2.n; i++) vec_push(&targs, resolve_type(F->g, F->file, F->env, tnode->list2.v[i]));
        return instantiate(F->g, d, targs, tnode->pos);
    }
    if (expect && expect->decl == d) return expect;
    return NULL;
}

/* Shape.Circle(1.0), Some(x), None ... `call` may be NULL for a variant without data. */
static Val gen_variant(FnCtx *F, Node *call, Pos pos, Decl *d, Type *et, const char *vname, Type *expect) {
    Node *vn = find_variant_node(d->node, vname);
    if (!vn) {
        diag_error(pos, "enum `%s` has no variant `%s`", d->name, vname);
        return err_val();
    }
    int nf = vn->flag ? vn->list2.n : vn->list.n;
    int nargs = call ? call->list.n : 0;
    if (nf != nargs) {
        if (nf == 0)
            diag_error(pos, "variant `%s.%s` has no data, so it is written without `(...)`", d->name, vname);
        else if (!call)
            diag_error(pos, "variant `%s.%s` needs %d value(s): write `%s.%s(...)`", d->name, vname, nf, d->name, vname);
        else
            diag_error(pos, "variant `%s.%s` takes %d value(s), but %d were given", d->name, vname, nf, nargs);
        return err_val();
    }
    if (vn->flag && !(call && call->flag2)) {
        diag_error(pos, "variant `%s.%s` has named fields: write `%s.%s { %s: ... }`", d->name, vname, d->name, vname,
                   vn->list2.n ? ((Node *)vn->list2.v[0])->name : "Field");
        return err_val();
    }
    Vec vals = {0};
    if (!et) {
        Env *env = env_new();
        for (int i = 0; i < nargs; i++) {
            Node *fte = vn->flag ? ((Node *)vn->list2.v[i])->a : vn->list.v[i];
            Type *pexp = syntax_uses_unbound(fte, d->node, env) ? NULL : resolve_type(F->g, d->file, env, fte);
            Val *v = zalloc(sizeof *v);
            *v = gen_expr(F, call->list.v[i], pexp);
            if (v->t == ty_never) return never_val(expect);
            if (!pexp) unify(fte, v->t, d->node, env);
            vec_push(&vals, v);
        }
        Vec targs = {0};
        for (int i = 0; i < d->node->list2.n; i++) {
            Node *tp = d->node->list2.v[i];
            Type *t = env_get(env, tp->name);
            if (!t && expect && expect->decl == d) t = expect->args.v[i];
            if (!t) {
                diag_error(pos, "cannot infer the type parameter `%s` of `%s` (add a type annotation)", tp->name, d->name);
                return err_val();
            }
            vec_push(&targs, t);
        }
        et = instantiate(F->g, d, targs, pos);
    } else {
        Variant *v0 = find_variant(et, vname);
        for (int i = 0; i < nargs; i++) {
            Val *v = zalloc(sizeof *v);
            *v = gen_expr(F, call->list.v[i], ((Field *)v0->fields.v[i])->t);
            if (v->t == ty_never) return never_val(expect);
            vec_push(&vals, v);
        }
    }
    Variant *var = find_variant(et, vname);
    for (int i = 0; i < nargs; i++) {
        Val *v = vals.v[i];
        char *what = call->flag2 ? zsprintf("field `%s` of `%s.%s`", ((Field *)var->fields.v[i])->name, d->name, vname)
                                 : zsprintf("argument %d of `%s.%s`", i + 1, d->name, vname);
        *v = coerce(F, ((Node *)call->list.v[i])->pos, *v, ((Field *)var->fields.v[i])->t, what);
        if (is_err(v->t)) return err_val();
    }
    return build_variant(F, et, var, &vals);
}

static Val gen_buf_new(FnCtx *F, Node *call, Type *t);

/* A field of an `extern struct` is a C type, a pointer, or another extern struct. */
static bool is_extern_struct(Type *t) {
    return t->k == T_STRUCT && t->decl && t->decl->abi;
}

/* Point.New(x, y) for an `extern struct`: every field, in order. */
static Val gen_extern_struct_new(FnCtx *F, Node *call, Decl *d, Type *t) {
    ensure_members(F->g, t);
    bool ok = true;
    for (int i = 0; i < t->fields.n; i++) {
        Field *f = t->fields.v[i];
        if (is_err(f->t) || is_c_value_type(f->t) || is_extern_struct(f->t)) continue;
        diag_error(d->node->pos, "field `%s` of the extern struct `%s` has type `%s`; its fields must be C types, pointers or other extern structs",
                   f->name, d->name, f->t->name);
        ok = false;
    }
    if (!ok) return err_val();
    if (call->list.n != t->fields.n) {
        diag_error(call->pos, "`%s.New` takes %d argument(s) (one per field), but %d were given", d->name, (int)t->fields.n,
                   call->list.n);
        return err_val();
    }
    char *slot = new_slot(F, t);
    emit(F, "store %s zeroinitializer, ptr %s", type_ll(t), slot);
    for (int i = 0; i < t->fields.n; i++) {
        Field *f = t->fields.v[i];
        Val v = gen_expr(F, call->list.v[i], f->t);
        v = coerce(F, ((Node *)call->list.v[i])->pos, v, f->t, zsprintf("field `%s` of `%s`", f->name, d->name));
        if (v.t == ty_never) return never_val(NULL);
        if (is_err(v.t)) return err_val();
        char *gep = tmp(F);
        emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", gep, type_ll(t), slot, i);
        store_to(F, f->t, v, gep);
    }
    char *r = tmp(F);
    emit(F, "%s = load %s, ptr %s", r, type_ll(t), slot);
    Val res = {t, r};
    return res;
}

/* Type.New(args) */
static Val gen_construct(FnCtx *F, Node *call, Decl *d, Type *t, Type *expect) {
    (void)expect;
    Gen *g = F->g;
    Node *td = d->node;
    if (t->k == T_BUF) return gen_buf_new(F, call, t);
    if (td->kind == D_ENUM) {
        diag_error(call->pos, "an enum value is built with one of its variants, like `%s.%s(...)`", d->name,
                   td->list.n ? ((Node *)td->list.v[0])->name : "Variant");
        return err_val();
    }
    if (d->abi && t->k == T_STRUCT) return gen_extern_struct_new(F, call, d, t);
    ensure_members(g, t);
    bool is_class = t->k == T_CLASS;
    char *obj;
    Type *vt = is_class ? obj_tuple(t) : t;
    if (is_class) {
        rt_decl(F, "zevic_alloc", "declare ptr @zevic_alloc(i64)");
        obj = tmp(F);
        emit(F, "%s = call ptr @zevic_alloc(i64 %lld)", obj, (long long)ty_size(vt));
    } else {
        obj = new_slot(F, t);
        emit(F, "store %s zeroinitializer, ptr %s", type_ll(t), obj);
    }
    /* field initial values, evaluated as the type's own code would */
    Env *tenv = env_new();
    tenv->self = t;
    for (int i = 0; i < td->list2.n; i++) env_add(tenv, ((Node *)td->list2.v[i])->name, t->args.v[i]);
    Node *init = find_member_fn(t, "Init");
    for (int i = 0; i < t->fields.n; i++) {
        Field *f = t->fields.v[i];
        Node *fn = NULL;
        for (int k = 0; k < td->list.n; k++) {
            Node *m = td->list.v[k];
            if (m->kind == N_FIELD && !m->flag && strcmp(m->name, f->name) == 0) fn = m;
        }
        if (!fn || !fn->b) {
            if (!init && !fn->b) {
                diag_error(call->pos, "`%s` has no `Init`, so every field needs an initial value (`%s` has none)", t->name,
                           f->name);
                return err_val();
            }
            continue;
        }
        SourceFile *sf = F->file;
        Env *se = F->env;
        Scope *ss = F->scope;
        F->file = d->file;
        F->env = tenv;
        F->scope = zalloc(sizeof(Scope));
        Val v = gen_expr(F, fn->b, f->t);
        v = coerce(F, fn->b->pos, v, f->t, "a field initial value");
        F->file = sf;
        F->env = se;
        F->scope = ss;
        if (is_err(v.t) || v.t == ty_never) continue;
        char *gep = tmp(F);
        emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", gep, type_ll(vt), obj, i);
        store_to(F, f->t, v, gep);
    }
    if (init) {
        Val recv = {ty_ptr(ty_u8), obj};
        Decl *md = method_decl(t, init);
        if (init->is_static) {
            diag_error(init->pos, "`Init` cannot be static");
            return err_val();
        }
        Val r = gen_call_decl(F, call, md, t, NULL, NULL, &recv);
        if (is_err(r.t)) return r;
        if (r.t == ty_never) return r;
    } else if (call->list.n) {
        diag_error(call->pos, "`%s` has no `Init`, so `New` takes no arguments", t->name);
        return err_val();
    }
    if (is_class) {
        Val r = {t, obj};
        return r;
    }
    char *v = tmp(F);
    emit(F, "%s = load %s, ptr %s", v, type_ll(t), obj);
    Val r = {t, v};
    return r;
}

/* `Type.member` without a call: unit variants, static fields; and `Module.CONST`. */
static Val gen_static_member(FnCtx *F, Node *n, Type *expect) {
    Vec segs = {0}, nodes = {0};
    flatten_path(n, &segs, &nodes);
    int used = 0;
    Decl *d = NULL;
    if (strcmp(segs.v[0], "Self") == 0 && F->env && F->env->self && F->env->self->decl) {
        d = F->env->self->decl;
        used = 1;
    } else {
        d = find_decl(F->g, F->file, (char **)segs.v, segs.n, &used, n->pos);
    }
    if (used < 0) return err_val();
    if (!d) {
        if (strcmp(segs.v[0], "None") == 0) {
            /* handled by gen_ident */
        }
        diag_error(n->pos, "unknown name `%s`", path_join(&segs, "."));
        return err_val();
    }
    Node *dn = d->node;
    if (dn->kind == D_CONST && used == segs.n) {
        Type *t = resolve_type(F->g, d->file, make_env(d, NULL, NULL), dn->a);
        Val v = gen_expr(F, dn->b, t);
        return coerce(F, dn->b->pos, v, t, "a constant");
    }
    if ((dn->kind == D_ENUM || dn->kind == D_STRUCT || dn->kind == D_CLASS) && used == segs.n - 1) {
        const char *member = segs.v[used];
        Node *tnode = nodes.v[used - 1];
        Type *t = (strcmp(segs.v[0], "Self") == 0 && F->env->self) ? F->env->self : static_type(F, d, tnode, expect);
        if (dn->kind == D_ENUM && find_variant_node(dn, member)) return gen_variant(F, NULL, n->pos, d, t, member, expect);
        if (t) {
            Node *sf = find_static_field(t, member);
            if (sf) {
                Env *tenv = env_new();
                tenv->self = t;
                for (int i = 0; i < dn->list2.n; i++) env_add(tenv, ((Node *)dn->list2.v[i])->name, t->args.v[i]);
                Type *ft = resolve_type(F->g, d->file, tenv, sf->a);
                SourceFile *osf = F->file;
                Env *oe = F->env;
                Scope *os = F->scope;
                F->file = d->file;
                F->env = tenv;
                F->scope = zalloc(sizeof(Scope));
                Val v = sf->b ? gen_expr(F, sf->b, ft) : err_val();
                v = coerce(F, n->pos, v, ft, "a static field");
                F->file = osf;
                F->env = oe;
                F->scope = os;
                return v;
            }
            if (find_member_fn(t, member)) {
                diag_error(n->pos, "`%s.%s` is a function; call it with `()`", d->name, member);
                return err_val();
            }
        }
        diag_error(n->pos, "type `%s` has no member `%s`", d->name, member);
        return err_val();
    }
    if (dn->kind == D_FN && used == segs.n) return fn_value_of(F, d, n->pos);
    diag_error(n->pos, "`%s` cannot be used as a value here", path_join(&segs, "."));
    return err_val();
}

static Val gen_member(FnCtx *F, Node *n, Type *expect) {
    if (is_static_path(F, n)) return gen_static_member(F, n, expect);
    Place p = gen_place(F, n);
    if (p.t == ty_never) return never_val(expect);
    return load_place(F, p);
}

/* =========================================================================
 * match
 * ========================================================================= */

static Node g_wild_node = {.kind = P_WILD};

static Place field_place(FnCtx *F, char *base, Type *agg, int idx, Type *ft, bool mut) {
    char *gep = tmp(F);
    emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", gep, type_ll(agg), base, idx);
    Place p = {gep, ft, mut};
    return p;
}

static const char *variant_pat_name(Node *pat) {
    return pat->path.v[pat->path.n - 1];
}

static bool is_prelude_variant(const char *n) {
    return strcmp(n, "Some") == 0 || strcmp(n, "None") == 0 || strcmp(n, "Ok") == 0 || strcmp(n, "Err") == 0;
}

/* Resolve the variant a pattern names, checking its shape. NULL after an error. */
static Variant *pat_variant(Type *et, Node *pat) {
    if (et->k != T_ENUM) {
        diag_error(pat->pos, "cannot match a value of type `%s` against the variant pattern `%s`", et->name,
                   path_join(&pat->path, "."));
        return NULL;
    }
    const char *vname = variant_pat_name(pat);
    if (pat->path.n >= 2) {
        const char *tn = pat->path.v[pat->path.n - 2];
        if (strcmp(tn, et->decl->name) != 0) {
            diag_error(pat->pos, "the pattern `%s` belongs to a different type than the matched value (`%s`)",
                       path_join(&pat->path, "."), et->name);
            return NULL;
        }
    } else if (!is_prelude_variant(vname)) {
        Variant *maybe = find_variant(et, vname);
        if (maybe)
            diag_error(pat->pos, "write the variant with its type name, like `%s.%s` (only `Some`, `None`, `Ok` and `Err` can be written alone)",
                       et->decl->name, vname);
        else
            diag_error(pat->pos, "unknown variant `%s` of `%s`", vname, et->name);
        return NULL;
    }
    Variant *v = find_variant(et, vname);
    if (!v) {
        diag_error(pat->pos, "type `%s` has no variant `%s`", et->name, vname);
        return NULL;
    }
    int nf = v->fields.n;
    if (pat->flag2) {
        if (!v->named) {
            diag_error(pat->pos, "variant `%s` has no named fields; write `%s(...)`", v->name, v->name);
            return NULL;
        }
    } else if (v->named) {
        diag_error(pat->pos, "variant `%s` has named fields; match it with `%s { Name: pattern, .. }`", v->name, v->name);
        return NULL;
    } else if (pat->list.n != nf) {
        if (nf == 0)
            diag_error(pat->pos, "variant `%s` has no data, so the pattern is written without `(...)`", v->name);
        else
            diag_error(pat->pos, "variant `%s` has %d value(s), but the pattern has %d", v->name, nf, pat->list.n);
        return NULL;
    }
    return v;
}

static void pat_test(FnCtx *F, Node *pat, Place pl, const char *fail) {
    if (is_err(pl.t)) return;
    switch (pat->kind) {
    case P_WILD: case P_BIND:
        return;
    case P_TYPE: {
        if (pl.t->k != T_UNION) {
            diag_error(pat->pos, "a type pattern needs a union to match on, but this is `%s`", pl.t->name);
            return;
        }
        Type *mt = pat->aux ? (Type *)pat->aux : resolve_type(F->g, F->file, F->env, pat->a);
        if (is_err(mt)) return;
        int idx = union_index(pl.t, mt);
        if (idx < 0) {
            diag_error(pat->a->pos, "`%s` is not a member of `%s`", mt->name, pl.t->name);
            pat->aux = ty_err;
            return;
        }
        pat->aux = mt;
        char *tg = tmp(F), *eq = tmp(F);
        emit(F, "%s = load i32, ptr %s", tg, tag_ptr(F, pl.t, pl.ptr));
        emit(F, "%s = icmp eq i32 %s, %d", eq, tg, idx);
        char *ok = newlabel(F, "pat.type");
        condbr(F, eq, ok, fail);
        start_block(F, ok);
        return;
    }
    case P_LIT: {
        Val v = gen_expr(F, pat->a, pl.t);
        if (is_err(v.t) || v.t == ty_never) return;
        if (v.t != pl.t) {
            type_error(pat->pos, pl.t, v.t, "a pattern");
            return;
        }
        Val cur = load_place(F, pl);
        Val eq = gen_compare(F, TK_EQEQ, cur, v, pat->pos);
        if (is_err(eq.t)) return;
        char *ok = newlabel(F, "pat.ok");
        condbr(F, eq.v, ok, fail);
        start_block(F, ok);
        return;
    }
    case P_RANGE: {
        Val lo = gen_expr(F, pat->a, pl.t), hi = gen_expr(F, pat->b, pl.t);
        if (is_err(lo.t) || is_err(hi.t) || lo.t == ty_never || hi.t == ty_never) return;
        if (lo.t != pl.t || hi.t != pl.t || !(ty_is_int(pl.t) || pl.t == ty_char)) {
            diag_error(pat->pos, "a range pattern needs integers or characters of the matched type (`%s`)", pl.t->name);
            return;
        }
        Val cur = load_place(F, pl);
        Val c1 = gen_compare(F, TK_GE, cur, lo, pat->pos);
        char *ok1 = newlabel(F, "pat.lo");
        condbr(F, c1.v, ok1, fail);
        start_block(F, ok1);
        Val c2 = gen_compare(F, pat->flag ? TK_LE : TK_LT, cur, hi, pat->pos);
        char *ok2 = newlabel(F, "pat.hi");
        condbr(F, c2.v, ok2, fail);
        start_block(F, ok2);
        return;
    }
    case P_TUPLE: {
        if (pl.t->k != T_TUPLE) {
            diag_error(pat->pos, "cannot match a value of type `%s` against a tuple pattern", pl.t->name);
            return;
        }
        if (pl.t->args.n != pat->list.n) {
            diag_error(pat->pos, "the tuple pattern has %d element(s), but the value has %d", pat->list.n, pl.t->args.n);
            return;
        }
        for (int i = 0; i < pat->list.n; i++)
            pat_test(F, pat->list.v[i], field_place(F, pl.ptr, pl.t, i, pl.t->args.v[i], pl.mut), fail);
        return;
    }
    case P_OR: {
        char *done = newlabel(F, "pat.or");
        for (int i = 0; i < pat->list.n; i++) {
            bool last = i == pat->list.n - 1;
            char *next = last ? (char *)fail : newlabel(F, "pat.alt");
            pat_test(F, pat->list.v[i], pl, next);
            if (!last) {
                br(F, done);
                start_block(F, next);
            }
        }
        goto_block(F, done);
        return;
    }
    case P_VARIANT: {
        Variant *v = pat_variant(pl.t, pat);
        if (!v) return;
        char *tg = tmp(F), *eq = tmp(F);
        emit(F, "%s = load i32, ptr %s", tg, tag_ptr(F, pl.t, pl.ptr));
        emit(F, "%s = icmp eq i32 %s, %d", eq, tg, v->tag);
        char *ok = newlabel(F, "pat.tag");
        condbr(F, eq, ok, fail);
        start_block(F, ok);
        if (!v->fields.n) return;
        char *pp = payload_ptr(F, pl.t, pl.ptr);
        if (pat->flag2) {
            for (int i = 0; i < pat->list2.n; i++) {
                Node *fp = pat->list2.v[i];
                int idx = -1;
                for (int k = 0; k < v->fields.n; k++)
                    if (strcmp(((Field *)v->fields.v[k])->name, fp->name) == 0) idx = k;
                if (idx < 0) {
                    diag_error(fp->pos, "variant `%s` has no field `%s`", v->name, fp->name);
                    continue;
                }
                pat_test(F, fp->a, field_place(F, pp, v->payload, idx, ((Field *)v->fields.v[idx])->t, pl.mut), fail);
            }
            if (!pat->flag3 && pat->list2.n < v->fields.n)
                diag_error(pat->pos, "the pattern does not mention every field of `%s`; add `..` to ignore the rest", v->name);
        } else {
            for (int i = 0; i < pat->list.n; i++)
                pat_test(F, pat->list.v[i], field_place(F, pp, v->payload, i, ((Field *)v->fields.v[i])->t, pl.mut), fail);
        }
        return;
    }
    default:
        diag_error(pat->pos, "this pattern is not supported here");
    }
}

static bool pat_has_binding(Node *pat) {
    switch (pat->kind) {
    case P_BIND: return true;
    case P_TYPE: return strcmp(pat->name, "_") != 0;
    case P_TUPLE: case P_OR: case P_VARIANT:
        for (int i = 0; i < pat->list.n; i++)
            if (pat_has_binding(pat->list.v[i])) return true;
        for (int i = 0; i < pat->list2.n; i++)
            if (pat_has_binding(((Node *)pat->list2.v[i])->a)) return true;
        return false;
    default:
        return false;
    }
}

static void pat_bind(FnCtx *F, Node *pat, Place pl, bool mut) {
    if (is_err(pl.t)) return;
    switch (pat->kind) {
    case P_BIND: {
        Local *l = declare_local(F, pat->name, pl.t, mut, pat->pos);
        if (pl.t != ty_unit) {
            Val v = load_place(F, pl);
            store_to(F, pl.t, v, l->ptr);
        }
        return;
    }
    case P_TYPE: {
        Type *mt = (Type *)pat->aux;
        if (pl.t->k != T_UNION || !mt || strcmp(pat->name, "_") == 0) return;
        Local *l = declare_local(F, pat->name, mt, mut, pat->pos);
        if (ty_size(mt) > 0) {
            Place mp = {payload_ptr(F, pl.t, pl.ptr), mt, pl.mut};
            Val v = load_place(F, mp);
            store_to(F, mt, v, l->ptr);
        }
        return;
    }
    case P_TUPLE:
        if (pl.t->k != T_TUPLE || pl.t->args.n != pat->list.n) return;
        for (int i = 0; i < pat->list.n; i++)
            pat_bind(F, pat->list.v[i], field_place(F, pl.ptr, pl.t, i, pl.t->args.v[i], pl.mut), mut);
        return;
    case P_OR:
        for (int i = 0; i < pat->list.n; i++)
            if (pat_has_binding(pat->list.v[i])) {
                diag_error(pat->pos, "an `|` pattern cannot bind names (each alternative would bind different values)");
                return;
            }
        return;
    case P_VARIANT: {
        if (pl.t->k != T_ENUM) return;
        Variant *v = find_variant(pl.t, variant_pat_name(pat));
        if (!v || !v->fields.n) return;
        char *pp = payload_ptr(F, pl.t, pl.ptr);
        if (pat->flag2) {
            for (int i = 0; i < pat->list2.n; i++) {
                Node *fp = pat->list2.v[i];
                for (int k = 0; k < v->fields.n; k++)
                    if (strcmp(((Field *)v->fields.v[k])->name, fp->name) == 0)
                        pat_bind(F, fp->a, field_place(F, pp, v->payload, k, ((Field *)v->fields.v[k])->t, pl.mut), mut);
            }
        } else {
            for (int i = 0; i < pat->list.n && i < v->fields.n; i++)
                pat_bind(F, pat->list.v[i], field_place(F, pp, v->payload, i, ((Field *)v->fields.v[i])->t, pl.mut), mut);
        }
        return;
    }
    default:
        return;
    }
}

/* ---- exhaustiveness: does this set of pattern rows cover every value? ---- */

static bool pat_is_wild(Node *p) {
    return p->kind == P_WILD || p->kind == P_BIND;
}

static Vec *row_new(void) {
    return zalloc(sizeof(Vec));
}

static Vec *row_with(Vec *first_part, Vec *rest, int skip) {
    Vec *r = row_new();
    for (int i = 0; i < first_part->n; i++) vec_push(r, first_part->v[i]);
    for (int i = skip; i < rest->n; i++) vec_push(r, rest->v[i]);
    return r;
}

static bool rows_cover(Vec *rows, Vec *tys);

static bool rows_cover(Vec *rows_in, Vec *tys) {
    if (rows_in->n == 0) return false;
    if (tys->n == 0) return true;
    Type *t = tys->v[0];
    /* expand `a | b` in the first column into separate rows */
    Vec rows = {0};
    for (int i = 0; i < rows_in->n; i++) {
        Vec *r = rows_in->v[i];
        Node *p = r->v[0];
        if (p->kind == P_OR) {
            for (int k = 0; k < p->list.n; k++) {
                Vec first = {0};
                vec_push(&first, p->list.v[k]);
                vec_push(&rows, row_with(&first, r, 1));
            }
        } else {
            vec_push(&rows, r);
        }
    }
    Vec rest_tys = {0};
    for (int i = 1; i < tys->n; i++) vec_push(&rest_tys, tys->v[i]);
    if (t->k == T_ENUM) {
        for (int vi = 0; vi < t->variants.n; vi++) {
            Variant *v = t->variants.v[vi];
            Vec nrows = {0};
            for (int i = 0; i < rows.n; i++) {
                Vec *r = rows.v[i];
                Node *p = r->v[0];
                Vec sub = {0};
                if (pat_is_wild(p)) {
                    for (int k = 0; k < v->fields.n; k++) vec_push(&sub, &g_wild_node);
                } else if (p->kind == P_VARIANT && strcmp(variant_pat_name(p), v->name) == 0) {
                    if (p->flag2) {
                        for (int k = 0; k < v->fields.n; k++) {
                            Node *sp = &g_wild_node;
                            for (int q = 0; q < p->list2.n; q++) {
                                Node *fp = p->list2.v[q];
                                if (strcmp(((Field *)v->fields.v[k])->name, fp->name) == 0) sp = fp->a;
                            }
                            vec_push(&sub, sp);
                        }
                    } else {
                        for (int k = 0; k < v->fields.n; k++)
                            vec_push(&sub, k < p->list.n ? p->list.v[k] : &g_wild_node);
                    }
                } else {
                    continue;
                }
                vec_push(&nrows, row_with(&sub, r, 1));
            }
            Vec ntys = {0};
            for (int k = 0; k < v->fields.n; k++) vec_push(&ntys, ((Field *)v->fields.v[k])->t);
            for (int k = 0; k < rest_tys.n; k++) vec_push(&ntys, rest_tys.v[k]);
            if (!rows_cover(&nrows, &ntys)) return false;
        }
        return true;
    }
    if (t->k == T_UNION) {
        for (int mi = 0; mi < t->args.n; mi++) {
            Vec nrows = {0};
            for (int i = 0; i < rows.n; i++) {
                Vec *r = rows.v[i];
                Node *p = r->v[0];
                if (pat_is_wild(p) || (p->kind == P_TYPE && p->aux == t->args.v[mi])) vec_push(&nrows, row_with(&(Vec){0}, r, 1));
            }
            if (!rows_cover(&nrows, &rest_tys)) return false;
        }
        return true;
    }
    if (t->k == T_TUPLE) {
        Vec nrows = {0};
        for (int i = 0; i < rows.n; i++) {
            Vec *r = rows.v[i];
            Node *p = r->v[0];
            Vec sub = {0};
            if (pat_is_wild(p)) {
                for (int k = 0; k < t->args.n; k++) vec_push(&sub, &g_wild_node);
            } else if (p->kind == P_TUPLE) {
                for (int k = 0; k < t->args.n; k++) vec_push(&sub, k < p->list.n ? p->list.v[k] : &g_wild_node);
            } else {
                continue;
            }
            vec_push(&nrows, row_with(&sub, r, 1));
        }
        Vec ntys = {0};
        for (int k = 0; k < t->args.n; k++) vec_push(&ntys, t->args.v[k]);
        for (int k = 0; k < rest_tys.n; k++) vec_push(&ntys, rest_tys.v[k]);
        return rows_cover(&nrows, &ntys);
    }
    if (t == ty_bool) {
        for (int b = 0; b < 2; b++) {
            Vec nrows = {0};
            for (int i = 0; i < rows.n; i++) {
                Vec *r = rows.v[i];
                Node *p = r->v[0];
                bool take = pat_is_wild(p);
                if (p->kind == P_LIT && p->a->kind == N_BOOL && (int)p->a->ival == b) take = true;
                if (take) vec_push(&nrows, row_with(&(Vec){0}, r, 1));
            }
            if (!rows_cover(&nrows, &rest_tys)) return false;
        }
        return true;
    }
    /* numbers, characters, strings: only a catch-all row covers everything */
    Vec nrows = {0};
    for (int i = 0; i < rows.n; i++) {
        Vec *r = rows.v[i];
        if (pat_is_wild(r->v[0])) vec_push(&nrows, row_with(&(Vec){0}, r, 1));
    }
    return rows_cover(&nrows, &rest_tys);
}

static const char *missing_hint(Type *t, Vec *rows) {
    if (t->k == T_UNION) {
        for (int mi = 0; mi < t->args.n; mi++) {
            bool seen = false;
            for (int i = 0; i < rows->n && !seen; i++) {
                Node *p = ((Vec *)rows->v[i])->v[0];
                if (pat_is_wild(p) || (p->kind == P_TYPE && p->aux == t->args.v[mi])) seen = true;
                else if (p->kind == P_OR)
                    for (int k = 0; k < p->list.n; k++)
                        if (((Node *)p->list.v[k])->kind == P_TYPE && ((Node *)p->list.v[k])->aux == t->args.v[mi]) seen = true;
            }
            if (!seen) return zsprintf("`%s` is not handled", ((Type *)t->args.v[mi])->name);
        }
    }
    /* name one enum variant that no arm mentions, to make the message concrete */
    if (t->k == T_ENUM) {
        for (int vi = 0; vi < t->variants.n; vi++) {
            Variant *v = t->variants.v[vi];
            Vec sub = {0};
            Vec tys = {0};
            vec_push(&tys, t);
            for (int i = 0; i < rows->n; i++) vec_push(&sub, rows->v[i]);
            /* a variant is missing when the rows restricted to it cover nothing */
            Vec only = {0};
            for (int i = 0; i < rows->n; i++) {
                Vec *r = rows->v[i];
                Node *p = r->v[0];
                if (pat_is_wild(p) || (p->kind == P_VARIANT && strcmp(variant_pat_name(p), v->name) == 0)) vec_push(&only, r);
                else if (p->kind == P_OR) {
                    for (int k = 0; k < p->list.n; k++) {
                        Node *q = p->list.v[k];
                        if (q->kind == P_VARIANT && strcmp(variant_pat_name(q), v->name) == 0) vec_push(&only, r);
                    }
                }
            }
            if (only.n == 0) return zsprintf("`%s.%s` is not handled", t->decl->name, v->name);
        }
    }
    return NULL;
}

/* Resolve the types of the type patterns first: the exhaustiveness check looks at them before the arms are generated. */
static void resolve_type_pats(FnCtx *F, Node *pat) {
    if (!pat) return;
    if (pat->kind == P_TYPE) pat->aux = resolve_type(F->g, F->file, F->env, pat->a);
    for (int i = 0; i < pat->list.n; i++) resolve_type_pats(F, pat->list.v[i]);
    for (int i = 0; i < pat->list2.n; i++) resolve_type_pats(F, ((Node *)pat->list2.v[i])->a);
}

static Val gen_match(FnCtx *F, Node *n, Type *expect) {
    Place sp = gen_place(F, n->a);
    if (sp.t == ty_never) return never_val(expect);
    bool bad = is_err(sp.t);
    for (int i = 0; i < n->list.n; i++) resolve_type_pats(F, ((Node *)n->list.v[i])->a);
    if (!bad) {
        Vec rows = {0}, tys = {0};
        for (int i = 0; i < n->list.n; i++) {
            Node *arm = n->list.v[i];
            if (arm->b) continue; /* a guarded arm may fail, so it does not count */
            Vec *r = row_new();
            vec_push(r, arm->a);
            vec_push(&rows, r);
        }
        vec_push(&tys, sp.t);
        if (!rows_cover(&rows, &tys)) {
            const char *hint = missing_hint(sp.t, &rows);
            diag_error(n->pos, "this `match` does not cover every case%s%s", hint ? ": " : "",
                       hint ? hint : " (add a `_ =>` arm)");
            if (hint) diag_note("add an arm for it or a `_ =>` arm");
        }
    }
    char *end_l = newlabel(F, "match.end");
    Vec rvals = {0}, rlabels = {0};
    Type *want = (expect && expect != ty_unit) ? expect : NULL;
    Type *rt = NULL;
    for (int i = 0; i < n->list.n; i++) {
        Node *arm = n->list.v[i];
        char *next = newlabel(F, "arm.next");
        push_scope(F);
        if (!bad) {
            pat_test(F, arm->a, sp, next);
            pat_bind(F, arm->a, sp, false);
        }
        if (arm->b) {
            Val g = gen_expr(F, arm->b, ty_bool);
            g = coerce(F, arm->b->pos, g, ty_bool, "a match guard");
            if (g.t != ty_never && !is_err(g.t)) {
                char *ok = newlabel(F, "guard.ok");
                condbr(F, g.v, ok, next);
                start_block(F, ok);
            }
        }
        Type *arm_want = rt ? rt : want;
        Val v = arm->c->kind == N_BLOCK ? gen_block(F, arm->c, arm_want) : gen_expr(F, arm->c, arm_want);
        bool live = !F->term;
        if (live && want && want->k == T_UNION && v.t != want && !is_err(v.t) && v.t != ty_never)
            v = coerce(F, arm->c->pos, v, want, "a `match` arm");
        if (live) {
            if (is_err(v.t)) {
                /* keep going: the error is already reported */
            } else if (!rt) {
                rt = v.t;
            } else if (v.t != rt && !is_err(rt)) {
                diag_error(arm->c->pos, "the arms of this `match` have different types: `%s` and `%s`", rt->name, v.t->name);
                v = err_val();
            }
        }
        pop_scope(F);
        if (live && !F->term) {
            Val *pv = zalloc(sizeof *pv);
            *pv = v;
            vec_push(&rvals, pv);
            vec_push(&rlabels, F->cur);
            br(F, end_l);
        }
        start_block(F, next);
    }
    emit(F, "unreachable");
    F->term = true;
    start_block(F, end_l);
    if (rvals.n == 0) {
        emit(F, "unreachable");
        F->term = true;
        return never_val(expect);
    }
    for (int i = 0; i < rvals.n; i++)
        if (is_err(((Val *)rvals.v[i])->t)) return err_val();
    if (!rt || rt == ty_unit) return unit_val();
    char *r = tmp(F);
    SB sb = {0};
    for (int i = 0; i < rvals.n; i++) {
        Val *v = rvals.v[i];
        sb_printf(&sb, "%s[ %s, %%%s ]", i ? ", " : "", v->v, (char *)rlabels.v[i]);
    }
    emit(F, "%s = phi %s %s", r, type_ll(rt), sb_str(&sb));
    Val res = {rt, r};
    return res;
}

/* =========================================================================
 * The `?` operator
 * ========================================================================= */

static bool is_result_type(Type *t) {
    return t->k == T_ENUM && t->decl && strcmp(t->decl->name, "Result") == 0 && t->decl->mod->is_std &&
           strcmp(t->decl->mod->name, "Zevic.Core") == 0;
}

static bool is_option_type(Type *t) {
    return t->k == T_ENUM && t->decl && strcmp(t->decl->name, "Option") == 0 && t->decl->mod->is_std &&
           strcmp(t->decl->mod->name, "Zevic.Core") == 0;
}

static bool is_error_type(Type *t) {
    return t->k == T_CLASS && t->decl && strcmp(t->decl->name, "Error") == 0 && t->decl->mod->is_std &&
           strcmp(t->decl->mod->name, "Zevic.Core") == 0;
}

static Val gen_try(FnCtx *F, Node *n, Type *expect) {
    Val v = gen_expr(F, n->a, NULL);
    if (v.t == ty_never) return never_val(expect);
    if (is_err(v.t)) return err_val();
    if (!is_result_type(v.t)) {
        if (v.t->k == T_ENUM && v.t->decl && strcmp(v.t->decl->name, "Option") == 0)
            diag_error(n->pos, "`?` works on a `Result`, not an `Option` (turn it into a `Result` with `OkOr(error)` first)");
        else
            diag_error(n->pos, "`?` can only be used on a `Result`, found `%s`", v.t->name);
        return err_val();
    }
    if (!is_error_type(v.t->args.v[1])) {
        diag_error(n->pos, "`?` needs the error type to be `Error`, found `Result<%s, %s>`", ((Type *)v.t->args.v[0])->name,
                   ((Type *)v.t->args.v[1])->name);
        return err_val();
    }
    if (!F->ret) {
        diag_error(n->pos, "`?` needs to know the result type of the function; write the lambda's expected type (`let f: fn(...) -> Result<...> = ...`)");
        return err_val();
    }
    if (!is_result_type(F->ret) || F->ret->args.v[1] != v.t->args.v[1]) {
        diag_error(n->pos, "`?` can only be used in a function that returns `Result<_, Error>` (this one returns `%s`)",
                   F->ret->name);
        return err_val();
    }
    Type *T = v.t->args.v[0];
    Variant *ok = find_variant(v.t, "Ok"), *er = find_variant(v.t, "Err");
    Place sp = spill(F, v);
    char *tg = tmp(F), *isok = tmp(F);
    emit(F, "%s = load i32, ptr %s", tg, tag_ptr(F, v.t, sp.ptr));
    emit(F, "%s = icmp eq i32 %s, %d", isok, tg, ok->tag);
    char *ok_l = newlabel(F, "try.ok"), *err_l = newlabel(F, "try.err");
    condbr(F, isok, ok_l, err_l);
    /* Err: return it from this function (running the defers of every open scope) */
    start_block(F, err_l);
    {
        Place ep = field_place(F, payload_ptr(F, v.t, sp.ptr), er->payload, 0, v.t->args.v[1], false);
        Val ev = load_place(F, ep);
        Vec vals = {0};
        Val *pv = zalloc(sizeof *pv);
        *pv = ev;
        vec_push(&vals, pv);
        Val rv = build_variant(F, F->ret, find_variant(F->ret, "Err"), &vals);
        for (Scope *s = F->scope; s; s = s->up) run_defers_in(F, s);
        emit(F, "ret %s %s", type_ll(F->ret), rv.v);
        F->term = true;
    }
    start_block(F, ok_l);
    if (T == ty_unit) return unit_val();
    Place op = field_place(F, payload_ptr(F, v.t, sp.ptr), ok->payload, 0, T, false);
    return load_place(F, op);
}

/* =========================================================================
 * Intrinsics.Buf<T>
 * ========================================================================= */

/* The size of one T in bytes, computed by LLVM so that it always matches the layout. */
static char *elem_size(FnCtx *F, Type *et) {
    char *g1 = tmp(F), *sz = tmp(F);
    emit(F, "%s = getelementptr %s, ptr null, i32 1", g1, type_ll(et));
    emit(F, "%s = ptrtoint ptr %s to i64", sz, g1);
    return sz;
}

static char *elem_ptr(FnCtx *F, Type *et, const char *base, const char *idx) {
    char *p = tmp(F);
    emit(F, "%s = getelementptr inbounds %s, ptr %s, i64 %s", p, type_ll(et), base, idx);
    return p;
}

static Val gen_i64_arg(FnCtx *F, Node *a, const char *what) {
    Val v = gen_expr(F, a, ty_i64);
    return coerce(F, a->pos, v, ty_i64, what);
}

/* Buf<T>.New(capacity) */
static Val gen_buf_new(FnCtx *F, Node *call, Type *t) {
    if (call->list.n != 1) {
        diag_error(call->pos, "`Buf.New` takes the capacity");
        return err_val();
    }
    Val cap = gen_i64_arg(F, call->list.v[0], "the capacity of `Buf.New`");
    if (cap.t == ty_never) return never_val(t);
    if (is_err(cap.t)) return err_val();
    Type *et = t->args.v[0];
    char *bytes = tmp(F), *p = tmp(F);
    emit(F, "%s = mul i64 %s, %s", bytes, cap.v, elem_size(F, et));
    rt_decl(F, "zevic_alloc", "declare ptr @zevic_alloc(i64)");
    emit(F, "%s = call ptr @zevic_alloc(i64 %s)", p, bytes);
    Val r = {t, p};
    return r;
}

static Val gen_buf_method(FnCtx *F, Node *call, Val recv, const char *name) {
    Type *et = recv.t->args.v[0];
    int nargs = call->list.n;
    if (strcmp(name, "Get") == 0 && nargs == 1) {
        Val i = gen_i64_arg(F, call->list.v[0], "the index of `Buf.Get`");
        if (i.t == ty_never) return i;
        if (is_err(i.t)) return err_val();
        if (et == ty_unit) return unit_val();
        char *gp = elem_ptr(F, et, recv.v, i.v), *r = tmp(F);
        emit(F, "%s = load %s, ptr %s", r, type_ll(et), gp);
        Val v = {et, r};
        return v;
    }
    if (strcmp(name, "Set") == 0 && nargs == 2) {
        Val i = gen_i64_arg(F, call->list.v[0], "the index of `Buf.Set`");
        Val v = gen_expr(F, call->list.v[1], et);
        v = coerce(F, ((Node *)call->list.v[1])->pos, v, et, "the value of `Buf.Set`");
        if (i.t == ty_never || v.t == ty_never) return never_val(NULL);
        if (is_err(i.t) || is_err(v.t)) return err_val();
        if (et != ty_unit) {
            char *gp = elem_ptr(F, et, recv.v, i.v);
            store_to(F, et, v, gp);
        }
        return unit_val();
    }
    if (strcmp(name, "Copy") == 0 && nargs == 4) {
        Val src = gen_expr(F, call->list.v[0], recv.t);
        src = coerce(F, ((Node *)call->list.v[0])->pos, src, recv.t, "the source of `Buf.Copy`");
        Val from = gen_i64_arg(F, call->list.v[1], "an argument of `Buf.Copy`");
        Val to = gen_i64_arg(F, call->list.v[2], "an argument of `Buf.Copy`");
        Val n = gen_i64_arg(F, call->list.v[3], "an argument of `Buf.Copy`");
        if (src.t == ty_never || from.t == ty_never || to.t == ty_never || n.t == ty_never) return never_val(NULL);
        if (is_err(src.t) || is_err(from.t) || is_err(to.t) || is_err(n.t)) return err_val();
        char *sp = elem_ptr(F, et, src.v, from.v), *dp = elem_ptr(F, et, recv.v, to.v);
        char *bytes = tmp(F);
        emit(F, "%s = mul i64 %s, %s", bytes, n.v, elem_size(F, et));
        declare(F->g, "llvm.memmove.p0.p0.i64", "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1 immarg)");
        emit(F, "call void @llvm.memmove.p0.p0.i64(ptr %s, ptr %s, i64 %s, i1 false)", dp, sp, bytes);
        return unit_val();
    }
    if (strcmp(name, "Ptr") == 0 && nargs == 0) {
        Val v = {ty_ptr(et), recv.v};
        return v;
    }
    diag_error(call->pos, "`Buf` has no method `%s` that takes %d argument(s)", name, nargs);
    return err_val();
}

/* Shape.Named { Name: "x", Size: 3 }: check the field names, order the values as declared, build the variant. */
static Val gen_struct_lit(FnCtx *F, Node *n, Type *expect) {
    Vec segs = {0}, nodes = {0};
    if (!flatten_path(n->a, &segs, &nodes) || segs.n < 2) {
        diag_error(n->pos, "`{ name: value }` can only follow an enum variant like `Shape.Named`");
        return err_val();
    }
    int used = 0;
    Decl *d = find_decl(F->g, F->file, (char **)segs.v, segs.n, &used, n->a->pos);
    if (used < 0) return err_val();
    Node *vn = NULL;
    if (d && d->node->kind == D_ENUM && used == segs.n - 1) vn = find_variant_node(d->node, segs.v[used]);
    if (!vn) {
        diag_error(n->pos, "`%s` is not an enum variant, so it cannot be built with `{ name: value }`", path_join(&segs, "."));
        return err_val();
    }
    const char *vname = vn->name;
    if (!vn->flag) {
        diag_error(n->pos, "variant `%s.%s` has no named fields; write `%s.%s(...)`", d->name, vname, d->name, vname);
        return err_val();
    }
    Node *call = node_new(N_CALL, n->pos);
    call->flag2 = true;
    call->a = n->a;
    bool ok = true;
    for (int i = 0; i < vn->list2.n; i++) {
        Node *decl_f = vn->list2.v[i];
        Node *given = NULL;
        for (int k = 0; k < n->list.n; k++) {
            Node *g = n->list.v[k];
            if (strcmp(g->name, decl_f->name) == 0) {
                if (given) {
                    diag_error(g->pos, "field `%s` is given twice", g->name);
                    ok = false;
                }
                given = given ? given : g;
            }
        }
        if (!given) {
            diag_error(n->pos, "variant `%s.%s` needs the field `%s`", d->name, vname, decl_f->name);
            ok = false;
            continue;
        }
        vec_push(&call->list, given->b);
    }
    for (int k = 0; k < n->list.n; k++) {
        Node *g = n->list.v[k];
        bool known = false;
        for (int i = 0; i < vn->list2.n; i++)
            if (strcmp(((Node *)vn->list2.v[i])->name, g->name) == 0) known = true;
        if (!known) {
            diag_error(g->pos, "variant `%s.%s` has no field `%s`", d->name, vname, g->name);
            ok = false;
        }
    }
    if (!ok) return err_val();
    Type *t = static_type(F, d, nodes.v[used - 1], expect);
    return gen_variant(F, call, n->pos, d, t, vname, expect);
}

/* =========================================================================
 * Arrays: syntax that turns into calls of the Array class in Zevic.Core
 * ========================================================================= */

static bool is_array_type(FnCtx *F, Type *t) {
    if (t->k != T_CLASS || !t->decl) return false;
    Decl *d = lookup_in_prelude(F->g, "Array");
    return d && t->decl == d;
}

static Node *mk_ident(Pos pos, const char *name) {
    Node *n = node_new(N_IDENT, pos);
    n->name = zstrdup(name);
    return n;
}

static Node *mk_int(Pos pos, uint64_t v) {
    Node *n = node_new(N_INT, pos);
    n->ival = v;
    return n;
}

/* recv.method(args...) where the call comes from `[]` (flag3 makes the messages talk about indexing) */
static Node *mk_method_call(Node *recv, const char *method, Pos pos, Node *a1, Node *a2) {
    Node *m = node_new(N_MEMBER, pos);
    m->a = recv;
    m->name = zstrdup(method);
    Node *c = node_new(N_CALL, pos);
    c->a = m;
    if (a1) vec_push(&c->list, a1);
    if (a2) vec_push(&c->list, a2);
    return c;
}

/* Call function `fname` of standard library module `module` with values that are already computed.
 * They are parked in hidden variables so that the ordinary call code can be reused. */
static Val call_std(FnCtx *F, const char *module, const char *fname, Pos pos, Val *args, int nargs, Type *expect) {
    Module *m = prog_find_module(F->g->pg, module);
    Decl *d = m ? module_find(m, fname) : NULL;
    if (!d) {
        diag_error(pos, "internal error: `%s.%s` is missing from the standard library", module, fname);
        return err_val();
    }
    Node *call = node_new(N_CALL, pos);
    call->a = mk_ident(pos, fname);
    for (int i = 0; i < nargs; i++) {
        char *name = zsprintf("$c%d", F->ntmp++);
        Local *l = declare_local(F, name, args[i].t, false, pos);
        if (args[i].t != ty_unit) store_to(F, args[i].t, args[i], l->ptr);
        vec_push(&call->list, mk_ident(pos, name));
    }
    return gen_call_decl(F, call, d, NULL, NULL, expect, NULL);
}

/* a[i]  ->  a.Index(i) */
static Val gen_index_read(FnCtx *F, Node *n, Type *expect) {
    Node *call = mk_method_call(n->a, "Index", n->pos, n->b, NULL);
    call->flag3 = true;
    return gen_expr(F, call, expect);
}

/* An expression that can be evaluated twice without changing the result. */
static bool is_pure_expr(Node *n) {
    switch (n->kind) {
    case N_IDENT: case N_INT: case N_THIS: case N_BOOL: case N_CHAR: case N_BYTE: return true;
    case N_MEMBER: return is_pure_expr(n->a);
    case N_UNARY: return is_pure_expr(n->a);
    case N_BINARY: return is_pure_expr(n->a) && is_pure_expr(n->b);
    case N_INDEX: return is_pure_expr(n->a) && is_pure_expr(n->b);
    default: return false;
    }
}

/* a[i] = v  ->  a.SetIndex(i, v);   a[i] += v  ->  a.SetIndex(i, a[i] + v) */
static void gen_index_assign(FnCtx *F, Node *n) {
    Node *target = n->a;
    Node *value = n->b;
    if (n->op != TK_ASSIGN) {
        if (!is_pure_expr(target->a) || !is_pure_expr(target->b)) {
            diag_error(n->pos, "`%s` on an element needs a simple array and index (put them in variables first)", tok_name(n->op));
            return;
        }
        Node *bin = node_new(N_BINARY, n->pos);
        bin->op = base_op(n->op);
        bin->a = target;
        bin->b = n->b;
        value = bin;
    }
    Node *call = mk_method_call(target->a, "SetIndex", n->pos, target->b, value);
    call->flag3 = true;
    gen_expr(F, call, NULL);
}

/* [a, b, c]: fill a new buffer and wrap it in an Array object. */
static Val gen_array_lit(FnCtx *F, Node *n, Type *expect) {
    Gen *g = F->g;
    int cnt = n->list.n;
    Type *at = NULL, *et = NULL;
    Val first = {0};
    bool have_first = false;
    if (expect && is_array_type(F, expect)) {
        at = expect;
        et = expect->args.v[0];
    } else {
        if (cnt == 0) {
            diag_error(n->pos, "an empty array `[]` needs a type from its context (write `let a: Array<I32> = []`)");
            return err_val();
        }
        first = gen_expr(F, n->list.v[0], NULL);
        if (first.t == ty_never) return first;
        if (is_err(first.t)) {
            for (int i = 1; i < cnt; i++) gen_expr(F, n->list.v[i], NULL);
            return err_val();
        }
        have_first = true;
        et = first.t;
        at = array_type(g, et);
        if (is_err(at)) return err_val();
    }
    rt_decl(F, "zevic_alloc", "declare ptr @zevic_alloc(i64)");
    char *bytes = tmp(F), *buf = tmp(F);
    emit(F, "%s = mul i64 %d, %s", bytes, cnt, elem_size(F, et));
    emit(F, "%s = call ptr @zevic_alloc(i64 %s)", buf, bytes);
    bool failed = false;
    for (int i = 0; i < cnt; i++) {
        Node *en = n->list.v[i];
        Val v = (i == 0 && have_first) ? first : gen_expr(F, en, et);
        v = coerce(F, en->pos, v, et, "an array element");
        if (v.t == ty_never) return never_val(at);
        if (is_err(v.t)) {
            failed = true;
            continue;
        }
        if (et != ty_unit) store_to(F, et, v, elem_ptr(F, et, buf, zsprintf("%d", i)));
    }
    if (failed) return err_val();
    Type *vt = obj_tuple(at);
    char *obj = tmp(F);
    emit(F, "%s = call ptr @zevic_alloc(i64 %lld)", obj, (long long)ty_size(vt));
    int bi = -1, li = -1, ci = -1;
    if (!find_field(at, "buf", &bi) || !find_field(at, "len", &li) || !find_field(at, "cap", &ci)) {
        diag_error(n->pos, "internal error: Array in Core.zev must have the fields `buf`, `len` and `cap`");
        return err_val();
    }
    char *gb = tmp(F), *gl = tmp(F), *gc = tmp(F);
    emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", gb, type_ll(vt), obj, bi);
    emit(F, "store ptr %s, ptr %s", buf, gb);
    emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", gl, type_ll(vt), obj, li);
    emit(F, "store i64 %d, ptr %s", cnt, gl);
    emit(F, "%s = getelementptr inbounds %s, ptr %s, i32 0, i32 %d", gc, type_ll(vt), obj, ci);
    emit(F, "store i64 %d, ptr %s", cnt, gc);
    Val r = {at, obj};
    return r;
}

/* for (x in arr) / for (i, x in arr):
 *   let $it = arr;  mut $i = 0
 *   while ($i < $it.Len()) { let x = $it.Index($i); let i = $i; $i += 1; body }
 * The counter moves before the body, so `continue` needs no special care. */
/* Does the type have an `Iter()` method (its own, a default one, or one of an interface value)? */
static bool has_iter_method(Gen *g, Type *t) {
    if (t->k == T_IFACE) {
        Vec all = {0};
        collect_iface_methods(g, t, &all, 0);
        for (int i = 0; i < all.n; i++)
            if (strcmp(((IMethod *)all.v[i])->fn->name, "Iter") == 0) return true;
        return false;
    }
    if (!is_user_type(t)) return false;
    if (find_member_fn(t, "Iter")) return true;
    IMethod dm;
    return find_default(g, t, "Iter", &dm, NULL);
}

/* for (x in v) over anything with Iter(): it = v.Iter(); loop { o = it.Next(); if None, stop; x = o.Unwrap(); body } */
static void gen_for_iterable(FnCtx *F, Node *n, Val src) {
    Pos pos = n->pos;
    Local *sl = declare_local(F, "$src", src.t, false, pos);
    store_to(F, src.t, src, sl->ptr);
    Val iter = gen_expr(F, mk_method_call(mk_ident(pos, "$src"), "Iter", pos, NULL, NULL), NULL);
    if (iter.t == ty_never || is_err(iter.t)) {
        pop_scope(F);
        return;
    }
    Local *itl = declare_local(F, "$iter", iter.t, true, pos); /* `Next` is a `mut fn` */
    store_to(F, iter.t, iter, itl->ptr);
    /* Map is the one exception: `for (k, v in m)` destructures the (key, value) element */
    bool map_pairs = n->c && src.t->k == T_CLASS && src.t->decl && strcmp(src.t->decl->name, "Map") == 0 &&
                     src.t->decl->mod->is_std;
    Local *il = NULL;
    if (n->c && !map_pairs) {
        il = declare_local(F, "$i", ty_i64, true, pos);
        store_to(F, ty_i64, (Val){ty_i64, "0"}, il->ptr);
    }
    char *head = newlabel(F, "for.cond"), *body = newlabel(F, "for.body"), *end = newlabel(F, "for.end");
    goto_block(F, head);
    Val o = gen_expr(F, mk_method_call(mk_ident(pos, "$iter"), "Next", pos, NULL, NULL), NULL);
    if (o.t == ty_never || is_err(o.t) || !is_option_type(o.t)) {
        if (!is_err(o.t) && o.t != ty_never) diag_error(pos, "`Next()` must return an `Option`, found `%s`", o.t->name);
        pop_scope(F);
        return;
    }
    Local *ol = declare_local(F, "$o", o.t, false, pos);
    store_to(F, o.t, o, ol->ptr);
    Val some = gen_expr(F, mk_method_call(mk_ident(pos, "$o"), "IsSome", pos, NULL, NULL), ty_bool);
    condbr(F, some.v, body, end);
    start_block(F, body);
    push_scope(F);
    Node *let_elem = node_new(N_LET, pos);
    if (map_pairs) {
        Node *tp = node_new(P_TUPLE, pos);
        vec_push(&tp->list, n->c);
        vec_push(&tp->list, n->a);
        let_elem->a = tp;
    } else {
        let_elem->a = n->a;
    }
    let_elem->c = mk_method_call(mk_ident(pos, "$o"), "Unwrap", pos, NULL, NULL);
    gen_let(F, let_elem);
    if (n->c && !map_pairs) {
        Node *let_idx = node_new(N_LET, pos);
        let_idx->a = n->c;
        let_idx->c = mk_ident(pos, "$i");
        gen_let(F, let_idx);
        Node *inc = node_new(N_ASSIGN, pos);
        inc->op = TK_PLUSEQ;
        inc->a = mk_ident(pos, "$i");
        inc->b = mk_int(pos, 1);
        gen_assign(F, inc);
    }
    Loop lp = {end, head, F->scope, false, n->name};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    pop_scope(F);
    if (!F->term) br(F, head);
    start_block(F, end);
    pop_scope(F);
}

static void gen_for_iter(FnCtx *F, Node *n) {
    Pos pos = n->pos;
    push_scope(F);
    Val it = gen_expr(F, n->d, NULL);
    if (it.t == ty_never || is_err(it.t)) {
        pop_scope(F);
        return;
    }
    if (!is_array_type(F, it.t)) {
        if (has_iter_method(F->g, it.t)) {
            gen_for_iterable(F, n, it);
            return;
        }
        diag_error(n->d->pos, "cannot loop over a value of type `%s` (`for` works on ranges like `0..10`, on arrays and on anything that is `Iterable`)",
                   it.t->name);
        pop_scope(F);
        return;
    }
    Local *itl = declare_local(F, "$it", it.t, false, pos);
    store_to(F, it.t, it, itl->ptr);
    Local *il = declare_local(F, "$i", ty_i64, true, pos);
    store_to(F, ty_i64, (Val){ty_i64, "0"}, il->ptr);

    Node *cond = node_new(N_BINARY, pos);
    cond->op = TK_LT;
    cond->a = mk_ident(pos, "$i");
    cond->b = mk_method_call(mk_ident(pos, "$it"), "Len", pos, NULL, NULL);

    Node *get = mk_method_call(mk_ident(pos, "$it"), "Index", pos, mk_ident(pos, "$i"), NULL);
    Node *let_elem = node_new(N_LET, pos);
    let_elem->a = n->a;
    let_elem->c = get;
    Node *let_idx = NULL;
    if (n->c) {
        let_idx = node_new(N_LET, pos);
        let_idx->a = n->c;
        let_idx->c = mk_ident(pos, "$i");
    }
    Node *inc = node_new(N_ASSIGN, pos);
    inc->op = TK_PLUSEQ;
    inc->a = mk_ident(pos, "$i");
    inc->b = mk_int(pos, 1);

    char *head = newlabel(F, "for.cond"), *body = newlabel(F, "for.body"), *end = newlabel(F, "for.end");
    goto_block(F, head);
    Val c = gen_expr(F, cond, ty_bool);
    if (c.t != ty_never && !is_err(c.t)) condbr(F, c.v, body, end);
    start_block(F, body);
    push_scope(F);
    gen_let(F, let_elem);
    if (let_idx) gen_let(F, let_idx);
    gen_assign(F, inc);
    Loop lp = {end, head, F->scope, false, n->name};
    vec_push(&F->loops, &lp);
    gen_block(F, n->b, ty_unit);
    F->loops.n--;
    pop_scope(F);
    if (!F->term) br(F, head);
    start_block(F, end);
    pop_scope(F);
}

/* =========================================================================
 * Equality of structs, tuples, enums and classes
 * ========================================================================= */

/* Call method `fn` of type `t` with an already computed receiver and arguments. */
static Val call_method_vals(FnCtx *F, Type *t, Node *fn, Val recv_obj, Val *args, int nargs, Pos pos, Type *expect) {
    Node *call = node_new(N_CALL, pos);
    call->a = mk_ident(pos, fn->name);
    for (int i = 0; i < nargs; i++) {
        char *name = zsprintf("$c%d", F->ntmp++);
        Local *l = declare_local(F, name, args[i].t, false, pos);
        if (args[i].t != ty_unit) store_to(F, args[i].t, args[i], l->ptr);
        vec_push(&call->list, mk_ident(pos, name));
    }
    Val recv = {ty_ptr(ty_u8), recv_obj.v};
    return gen_call_decl(F, call, method_decl(t, fn), t, NULL, expect, &recv);
}

/* Compare the parts of two LLVM aggregates (a struct's fields or a tuple's elements). */
static Val gen_eq_parts(FnCtx *F, Vec *types, Val a, Val b, Pos pos) {
    char *acc = "true";
    for (int i = 0; i < types->n; i++) {
        Type *ft = types->v[i];
        Val x = {ft, tmp(F)}, y = {ft, tmp(F)};
        emit(F, "%s = extractvalue %s %s, %d", x.v, type_ll(a.t), a.v, i);
        emit(F, "%s = extractvalue %s %s, %d", y.v, type_ll(b.t), b.v, i);
        Val e = gen_eq(F, x, y, pos);
        if (is_err(e.t)) return e;
        if (i == 0) {
            acc = e.v;
        } else {
            char *r = tmp(F);
            emit(F, "%s = and i1 %s, %s", r, acc, e.v);
            acc = r;
        }
    }
    Val res = {ty_bool, acc};
    return res;
}

static Val gen_eq_enum(FnCtx *F, Val a, Val b, Pos pos) {
    Type *t = a.t;
    ty_layout(t);
    Place pa = spill(F, a), pb = spill(F, b);
    char *ta = tmp(F), *tb = tmp(F), *tagok = tmp(F);
    emit(F, "%s = load i32, ptr %s", ta, tag_ptr(F, t, pa.ptr));
    emit(F, "%s = load i32, ptr %s", tb, tag_ptr(F, t, pb.ptr));
    emit(F, "%s = icmp eq i32 %s, %s", tagok, ta, tb);
    bool any_payload = false;
    for (int i = 0; i < t->variants.n; i++)
        if (((Variant *)t->variants.v[i])->payload) any_payload = true;
    if (!any_payload) {
        Val res = {ty_bool, tagok};
        return res;
    }
    char *res = new_slot(F, ty_bool);
    emit(F, "store i1 false, ptr %s", res);
    char *cmp = newlabel(F, "eq.cmp"), *same = newlabel(F, "eq.same"), *done = newlabel(F, "eq.done");
    condbr(F, tagok, cmp, done);
    start_block(F, cmp);
    Vec labels = {0};
    SB sw = {0};
    for (int i = 0; i < t->variants.n; i++) {
        Variant *v = t->variants.v[i];
        char *l = v->payload ? newlabel(F, "eq.variant") : NULL;
        vec_push(&labels, l);
        if (l) sb_printf(&sw, " i32 %d, label %%%s", v->tag, l);
    }
    emit(F, "switch i32 %s, label %%%s [%s ]", ta, same, sb_str(&sw));
    F->term = true;
    for (int i = 0; i < t->variants.n; i++) {
        Variant *v = t->variants.v[i];
        if (!labels.v[i]) continue;
        start_block(F, labels.v[i]);
        Val x = {v->payload, tmp(F)}, y = {v->payload, tmp(F)};
        emit(F, "%s = load %s, ptr %s", x.v, type_ll(v->payload), payload_ptr(F, t, pa.ptr));
        emit(F, "%s = load %s, ptr %s", y.v, type_ll(v->payload), payload_ptr(F, t, pb.ptr));
        Val e = gen_eq_parts(F, &v->payload->args, x, y, pos);
        if (is_err(e.t)) return e;
        emit(F, "store i1 %s, ptr %s", e.v, res);
        br(F, done);
    }
    start_block(F, same);
    emit(F, "store i1 true, ptr %s", res);
    br(F, done);
    start_block(F, done);
    char *r = tmp(F);
    emit(F, "%s = load i1, ptr %s", r, res);
    Val out = {ty_bool, r};
    return out;
}

/* a == b for any type that has a value-style equality. */
static Val gen_eq(FnCtx *F, Val a, Val b, Pos pos) {
    Type *t = a.t;
    if (is_err(t) || is_err(b.t)) return err_val();
    if (t->k == T_STRUCT) {
        /* a struct that implements Equatable compares with its own Equals */
        Node *eq = find_member_fn(t, "Equals");
        if (eq && !eq->is_static) {
            Val args[1] = {b};
            return call_method_vals(F, t, eq, method_receiver(F, a), args, 1, pos, ty_bool);
        }
        return gen_eq_parts(F, &obj_tuple(t)->args, a, b, pos);
    }
    if (t->k == T_TUPLE) return gen_eq_parts(F, &t->args, a, b, pos);
    if (t->k == T_ENUM) return gen_eq_enum(F, a, b, pos);
    if (t->k == T_CLASS) {
        Node *eq = find_member_fn(t, "Equals");
        if (eq && !eq->is_static) {
            Val args[1] = {b};
            return call_method_vals(F, t, eq, a, args, 1, pos, ty_bool);
        }
        char *r = tmp(F);
        emit(F, "%s = icmp eq ptr %s, %s", r, a.v, b.v);
        Val res = {ty_bool, r};
        return res;
    }
    if (t->k == T_PTR) {
        char *r = tmp(F);
        emit(F, "%s = icmp eq ptr %s, %s", r, a.v, b.v);
        Val res = {ty_bool, r};
        return res;
    }
    return gen_compare(F, TK_EQEQ, a, b, pos);
}

/* The pointer part of a string constant `{ ptr @x, i64 n }`. */
static char *const_ptr(const char *c) {
    return zstrndup(c + 6, (size_t)(strchr(c, ',') - (c + 6)));
}

static void emit_panic_call(FnCtx *F, Val msg, Pos pos) {
    declare(F->g, "zevic_panic", "declare void @zevic_panic(ptr, i64, ptr, i32) noreturn");
    char *p, *n;
    str_parts(F, msg, &p, &n);
    const char *file = src_name(pos.file);
    char *fc = str_const(F->g, file, (int)strlen(file));
    emit(F, "call void @zevic_panic(ptr %s, i64 %s, ptr %s, i32 %d)", p, n, const_ptr(fc), pos.line);
    emit(F, "unreachable");
    F->term = true;
}

/* Attribute that makes a narrow C integer cross the call boundary the way a C compiler would. */
static const char *c_ext_attr(Type *t) {
    if (t->k != T_CTYPE || t->ll[0] != 'i') return "";
    if (t->size >= 4) return "";
    if (strcmp(t->name, "CBool") == 0) return " zeroext";
    return t->payload_words == 1 ? " signext" : " zeroext";
}

/* A Zevic function value as a C function pointer: a wrapper with the plain C signature that calls the function
 * with a null environment. Only a named function or a lambda that captures nothing can be passed. */
static char *c_callback(FnCtx *F, Val *v) {
    Gen *g = F->g;
    char *target = NULL;
    const char *pre = "{ ptr @\"thunk.";
    if (strncmp(v->v, pre, strlen(pre)) == 0 && strstr(v->v, "\", ptr null }")) {
        target = zstrndup(v->v + 8, (size_t)(strstr(v->v, "\", ptr null }") - (v->v + 8)));
    } else {
        target = dict_get(&g->noenv, zsprintf("%p:%s", (void *)F, v->v));
    }
    if (!target) {
        diag_error(g_call_pos, "only a named function, or a lambda that captures nothing, can be passed to C as a callback");
        diag_note("wrap the data a callback needs in a `Handle` and pass its number instead");
        return NULL;
    }
    char *wrapper = zsprintf("cb.%s", target);
    if (!dict_has(&g->thunks, wrapper)) {
        dict_put(&g->thunks, wrapper, target);
        Type *ft = v->t;
        SB ps = {0}, as = {0};
        for (int i = 0; i < ft->args.n; i++) {
            Type *pt = ft->args.v[i];
            sb_printf(&ps, "%s%s%s %%p%d", i ? ", " : "", type_ll(pt), c_ext_attr(pt), i);
            sb_printf(&as, ", %s %%p%d", type_ll(pt), i);
        }
        Type *r = ft->ret;
        bool is_void = r == ty_unit || r == ty_never || (r->k == T_CTYPE && strcmp(r->name, "CVoid") == 0);
        const char *rl = is_void ? "void" : type_ll(r);
        const char *ra = is_void ? "" : c_ext_attr(r);
        if (is_void) {
            sb_printf(&g->funcs, "define void @\"%s\"(%s) {\nentry:\n  call void @\"%s\"(ptr null%s)\n  ret void\n}\n\n", wrapper, sb_str(&ps), target,
                      sb_str(&as));
        } else {
            sb_printf(&g->funcs, "define%s %s @\"%s\"(%s) {\nentry:\n  %%r = call %s @\"%s\"(ptr null%s)\n  ret %s %%r\n}\n\n", ra, rl, wrapper,
                      sb_str(&ps), rl, target, sb_str(&as), rl);
        }
    }
    return zsprintf("@\"%s\"", wrapper);
}

/* A call to an `extern "C"` function. */
static Val emit_c_call(FnCtx *F, Inst *in, Vec *args) {
    SB a = {0}, ptypes = {0};
    Type *ret = in->ret;
    bool variadic = false;
    for (int i = 0; i < in->decl->node->list.n; i++)
        if (((Node *)in->decl->node->list.v[i])->flag3) variadic = true;
    for (int i = 0; i < args->n; i++) {
        Val *v = args->v[i];
        if (i) sb_puts(&a, ", ");
        const char *attr = c_ext_attr(v->t);
        const char *ll = type_ll(v->t);
        char *vv = v->v;
        if (v->t->k == T_FN) {
            vv = c_callback(F, v);
            if (!vv) return err_val();
            ll = "ptr";
        }
        sb_printf(&a, "%s%s %s", ll, attr, vv);
        if (i < in->ptypes.n) {
            if (i) sb_puts(&ptypes, ", ");
            sb_printf(&ptypes, "%s%s", ll, attr);
        }
    }
    if (variadic) sb_puts(&ptypes, ptypes.len ? ", ..." : "...");
    bool is_void = ret == ty_unit || ret == ty_never;
    const char *rl = is_void ? "void" : type_ll(ret);
    const char *rattr = is_void ? "" : c_ext_attr(ret);
    declare(F->g, in->name, zsprintf("declare%s %s @%s(%s)", rattr, rl, in->name, sb_str(&ptypes)));
    /* a variadic callee is called through its full function type */
    const char *fty = variadic ? zsprintf(" %s (%s)", rl, sb_str(&ptypes)) : zsprintf(" %s", rl);
    if (is_void) {
        emit(F, "call%s @%s(%s)", fty, in->name, sb_str(&a));
        return unit_val();
    }
    char *t = tmp(F);
    emit(F, "%s = call%s%s @%s(%s)", t, rattr, fty, in->name, sb_str(&a));
    Val r = {ret, t};
    return r;
}

static Val emit_c_call(FnCtx *F, Inst *in, Vec *args);

/* Emit a direct call to a function instance. */
static Val emit_call(FnCtx *F, Inst *in, Vec *args) {
    Gen *g = F->g;
    SB a = {0};
    Type *ret = in->ret;
    char *slot = NULL;
    bool zevic_abi = in->is_extern && in->decl->abi && strcmp(in->decl->abi, "zevic") == 0;
    bool c_abi = in->is_extern && in->decl->abi && strcmp(in->decl->abi, "C") == 0;
    if (c_abi) return emit_c_call(F, in, args);
    if (!in->is_extern && strcmp(in->decl->mod->name, "Zevic.Test") == 0 &&
        strcmp(F->inst->decl->mod->name, "Zevic.Test") != 0) {
        /* an assertion reports the line of the call, not a line inside the library */
        rt_decl(F, "zevic_test_at", "declare void @zevic_test_at(ptr, i32)");
        const char *file = src_name(g_call_pos.file);
        char *fc = str_const(g, file, (int)strlen(file));
        emit(F, "call void @zevic_test_at(ptr %s, i32 %d)", const_ptr(fc), g_call_pos.line);
    }
    if (in->is_extern && !zevic_abi) {
        diag_error(in->decl->node->pos, "`extern \"%s\"` functions are not supported yet", in->decl->abi);
        return err_val();
    }
    if (zevic_abi) {
        SB ptypes = {0};
        if (ret == ty_string) {
            slot = new_slot(F, ty_string);
            sb_printf(&a, "ptr %s", slot);
            sb_puts(&ptypes, "ptr");
        }
        for (int i = 0; i < args->n; i++) {
            Val *v = args->v[i];
            if (v->t == ty_unit) continue;
            if (a.len) sb_puts(&a, ", ");
            if (ptypes.len) sb_puts(&ptypes, ", ");
            if (v->t == ty_string) {
                char *p, *n;
                str_parts(F, *v, &p, &n);
                sb_printf(&a, "ptr %s, i64 %s", p, n);
                sb_puts(&ptypes, "ptr, i64");
            } else {
                sb_printf(&a, "%s %s", type_ll(v->t), v->v);
                sb_puts(&ptypes, type_ll(v->t));
            }
        }
        const char *rl = (ret == ty_string || ret == ty_unit || ret == ty_never) ? "void" : type_ll(ret);
        declare(g, in->name, zsprintf("declare %s @%s(%s)", rl, in->name, sb_str(&ptypes)));
        if (ret == ty_string || ret == ty_unit || ret == ty_never) {
            emit(F, "call void @%s(%s)", in->name, sb_str(&a));
            if (ret == ty_never) {
                emit(F, "unreachable");
                F->term = true;
                return never_val(NULL);
            }
            if (ret == ty_unit) return unit_val();
            char *t = tmp(F);
            emit(F, "%s = load { ptr, i64 }, ptr %s", t, slot);
            Val r = {ty_string, t};
            return r;
        }
        char *t = tmp(F);
        emit(F, "%s = call %s @%s(%s)", t, type_ll(ret), in->name, sb_str(&a));
        Val r = {ret, t};
        return r;
    }
    for (int i = 0; i < args->n; i++) {
        Val *v = args->v[i];
        if (i) sb_puts(&a, ", ");
        sb_printf(&a, "%s %s", type_ll(v->t), v->v);
    }
    if (ret == ty_unit || ret == ty_never) {
        emit(F, "call void @\"%s\"(%s)", in->name, sb_str(&a));
        if (ret == ty_never) {
            emit(F, "unreachable");
            F->term = true;
            return never_val(NULL);
        }
        return unit_val();
    }
    char *t = tmp(F);
    emit(F, "%s = call %s @\"%s\"(%s)", t, type_ll(ret), in->name, sb_str(&a));
    Val r = {ret, t};
    return r;
}

/* Call function `d` with the arguments of `call`, inferring generic arguments. */
static Val gen_call_decl(FnCtx *F, Node *call, Decl *d, Type *self, Vec *explicit, Type *expect, Val *recv) {
    Gen *g = F->g;
    Node *fn = d->node;
    int ntp = fn->list2.n;
    Env *env = make_env(d, self, NULL);
    if (explicit && explicit->n) {
        if (explicit->n != ntp) {
            diag_error(call->pos, "`%s` takes %d type argument(s), found %d", fn->name, ntp, explicit->n);
            return err_val();
        }
        for (int i = 0; i < ntp; i++)
            env_add(env, ((Node *)fn->list2.v[i])->name, resolve_type(g, F->file, F->env, explicit->v[i]));
    }
    /* `params xs: Array<T>`: the values at the end of the call are collected into an array */
    for (int i = 0; i < fn->list.n; i++) {
        Node *pm = fn->list.v[i];
        if (pm->flag2 && i != fn->list.n - 1) {
            diag_error(pm->pos, "only the last parameter can be `params`");
            return err_val();
        }
    }
    if (fn->list.n && ((Node *)fn->list.v[fn->list.n - 1])->flag2) {
        int fixed = fn->list.n - 1;
        if (call->list.n < fixed) {
            diag_error(call->pos, "`%s` takes at least %d argument(s), but %d were given", fn->name, fixed, call->list.n);
            return err_val();
        }
        Node *packed = zalloc(sizeof *packed);
        *packed = *call;
        memset(&packed->list, 0, sizeof packed->list);
        for (int i = 0; i < fixed; i++) vec_push(&packed->list, call->list.v[i]);
        Node *arr = node_new(N_ARRAY, call->pos);
        for (int i = fixed; i < call->list.n; i++) vec_push(&arr->list, call->list.v[i]);
        vec_push(&packed->list, arr);
        call = packed;
    }
    int nparams = 0;
    for (int i = 0; i < fn->list.n; i++)
        if (!((Node *)fn->list.v[i])->flag3) nparams++;
    bool variadic = nparams < fn->list.n;
    if (variadic ? call->list.n < nparams : call->list.n != nparams) {
        diag_error(call->pos, "`%s` takes %s%d argument(s), but %d were given", fn->name, variadic ? "at least " : "", nparams,
                   call->list.n);
        return err_val();
    }
    Vec vals = {0};
    bool never = false;
    for (int i = 0; i < nparams; i++) vec_push(&vals, zalloc(sizeof(Val)));
    /* a bare number literal for a parameter whose type is still unknown (`Reduce(0, f)`) waits until the other
     * arguments have fixed the type parameters; it has no side effects, so the order does not matter */
    bool *postponed = zalloc((size_t)(nparams + 1));
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nparams; i++) {
            Node *pn = fn->list.v[i];
            Node *an = call->list.v[i];
            if (pass == 0 && syntax_uses_unbound(pn->a, fn, env) && is_untyped_lit(an)) {
                postponed[i] = true;
                continue;
            }
            if (pass == 1 && !postponed[i]) continue;
            Type *pexp = NULL;
            if (!syntax_uses_unbound(pn->a, fn, env)) pexp = resolve_type(g, d->file, env, pn->a);
            Val *v = vals.v[i];
            *v = gen_expr(F, an, pexp);
            if (v->t == ty_never) never = true;
            else if (!pexp) unify(pn->a, v->t, fn, env);
        }
    }
    if (never) return never_val(expect);
    Vec targs = {0};
    for (int i = 0; i < ntp; i++) {
        Node *tp = fn->list2.v[i];
        Type *t = env_get(env, tp->name);
        if (!t && expect && fn->a) {
            unify(fn->a, expect, fn, env);
            t = env_get(env, tp->name);
        }
        if (!t) {
            diag_error(call->pos, "cannot infer the type parameter `%s` of `%s` (write it explicitly, like `%s<I32>(...)`)",
                       tp->name, fn->name, fn->name);
            return err_val();
        }
        vec_push(&targs, t);
    }
    if (ntp) check_bounds(g, d->file, fn, &targs, env, fn->name, call->pos);
    Inst *in = get_inst(g, d, self, &targs);
    for (int i = 0; i < nparams; i++) {
        Val *v = vals.v[i];
        char *what = zsprintf("argument %d of `%s`", i + 1, fn->name);
        if (call->flag3) /* written as `a[i]` or `a[i] = v` */
            what = strcmp(fn->name, "SetIndex") == 0 && i == 1 ? "an assignment" : "an index";
        *v = coerce(F, ((Node *)call->list.v[i])->pos, *v, in->ptypes.v[i], what);
        if (is_err(v->t)) return err_val();
    }
    for (int i = nparams; i < call->list.n; i++) {
        /* the `...` part: C types and pointers, promoted the way C does */
        Node *an = call->list.v[i];
        Val *v = zalloc(sizeof *v);
        *v = gen_expr(F, an, NULL);
        if (v->t == ty_never) return never_val(expect);
        if (is_err(v->t)) return err_val();
        if (!is_c_value_type(v->t)) {
            diag_error(an->pos, "argument %d of `%s` has type `%s`; the `...` part of a C function takes only C types and pointers",
                       i + 1, fn->name, v->t->name);
            if (ty_is_numeric(v->t)) diag_note("convert it with a method such as `.ToCInt()` or `.ToCDouble()`");
            else if (v->t == ty_string) diag_note("convert a String with `.ToC()`");
            return err_val();
        }
        if (v->t->k == T_CTYPE && strcmp(v->t->ll, "float") == 0) {
            char *r = tmp(F);
            emit(F, "%s = fpext float %s to double", r, v->v);
            v->v = r;
            v->t = ty_c_by_name("CDouble");
        } else if (v->t->k == T_CTYPE && v->t->ll[0] == 'i' && v->t->size < 4) {
            char *r = tmp(F);
            bool sgn = v->t->payload_words == 1 && strcmp(v->t->name, "CBool") != 0;
            emit(F, "%s = %s %s %s to i32", r, sgn ? "sext" : "zext", v->t->ll, v->v);
            v->v = r;
            v->t = ty_c_by_name(sgn ? "CInt" : "CUInt");
        }
        vec_push(&vals, v);
    }
    g_call_pos = call->pos;
    if (recv) {
        /* the receiver goes first */
        Vec all = {0};
        vec_push(&all, recv);
        for (int i = 0; i < vals.n; i++) vec_push(&all, vals.v[i]);
        return emit_call(F, in, &all);
    }
    return emit_call(F, in, &vals);
}

/* ---- Printf ---- */

typedef struct {
    bool is_spec;
    char *text; /* literal text, or the C format fragment (e.g. "%5lld") */
    int tlen;
    char conv;
} FmtPiece;

/* mode 0: Printf (stdout), 1: Eprintf (stderr), 2: Text.Format (returns a String) */
static Val gen_printf(FnCtx *F, Node *n, int mode) {
    const char *who = mode == 2 ? "Format" : "Printf";
    if (n->list.n < 1 || ((Node *)n->list.v[0])->kind != N_STRING) {
        diag_error(n->pos, "`%s` needs a string literal as its format", who);
        return err_val();
    }
    Node *fs = n->list.v[0];
    SB fmt = {0};
    for (int i = 0; i < fs->list.n; i++) {
        Node *part = fs->list.v[i];
        if (part->kind != N_STR_LIT) {
            diag_error(fs->pos, "the format of `%s` cannot contain `${...}`; pass the values as arguments", who);
            return err_val();
        }
        sb_putn(&fmt, part->name, (size_t)part->slen);
    }
    Vec pieces = {0};
    SB lit = {0};
    char *f = sb_str(&fmt);
    size_t len = fmt.len;
    for (size_t i = 0; i < len; i++) {
        if (f[i] != '%') {
            sb_putc(&lit, f[i]);
            continue;
        }
        if (i + 1 < len && f[i + 1] == '%') {
            sb_putc(&lit, '%');
            i++;
            continue;
        }
        if (lit.len) {
            FmtPiece *p = zalloc(sizeof *p);
            p->text = zstrndup(lit.p, lit.len);
            p->tlen = (int)lit.len;
            vec_push(&pieces, p);
            lit.len = 0;
        }
        SB flags = {0}, width = {0}, prec = {0};
        size_t j = i + 1;
        while (j < len && strchr("-+ 0#", f[j])) sb_putc(&flags, f[j++]);
        while (j < len && f[j] >= '0' && f[j] <= '9') sb_putc(&width, f[j++]);
        bool has_prec = false;
        if (j < len && f[j] == '.') {
            has_prec = true;
            j++;
            while (j < len && f[j] >= '0' && f[j] <= '9') sb_putc(&prec, f[j++]);
        }
        if (j >= len || !strchr("duoxXfeEgGscDi", f[j])) {
            diag_error(fs->pos, "invalid format specifier in `%s` (use %%d %%u %%o %%x %%f %%e %%g %%s %%c or %%%%)", who);
            return err_val();
        }
        char conv = f[j] == 'i' ? 'd' : f[j];
        FmtPiece *p = zalloc(sizeof *p);
        p->is_spec = true;
        p->conv = conv;
        const char *length = (conv == 'd' || conv == 'u' || conv == 'o' || conv == 'x' || conv == 'X') ? "ll" : "";
        char cconv = (conv == 'c') ? 's' : conv;
        p->text = zsprintf("%%%s%s%s%s%s%c", sb_str(&flags), sb_str(&width), has_prec ? "." : "",
                           has_prec ? (prec.len ? sb_str(&prec) : "0") : "", length, cconv);
        p->tlen = (int)strlen(p->text);
        vec_push(&pieces, p);
        i = j;
    }
    if (lit.len) {
        FmtPiece *p = zalloc(sizeof *p);
        p->text = zstrndup(lit.p, lit.len);
        p->tlen = (int)lit.len;
        vec_push(&pieces, p);
    }
    int nspecs = 0;
    for (int i = 0; i < pieces.n; i++)
        if (((FmtPiece *)pieces.v[i])->is_spec) nspecs++;
    if (nspecs != n->list.n - 1) {
        diag_error(n->pos, "the format has %d placeholder(s), but %d value(s) were given", nspecs, n->list.n - 1);
        return err_val();
    }
    /* evaluate the arguments first, in order */
    Vec vals = {0};
    int ai = 1;
    for (int i = 0; i < pieces.n; i++) {
        FmtPiece *p = pieces.v[i];
        if (!p->is_spec) continue;
        Node *an = n->list.v[ai++];
        Type *hint = strchr("feEgG", p->conv) ? ty_f64 : NULL;
        Val *v = zalloc(sizeof *v);
        *v = gen_expr(F, an, hint);
        if (v->t == ty_never) return never_val(NULL);
        bool ok = true;
        const char *need = "";
        switch (p->conv) {
        case 'd': case 'u': case 'o': case 'x': case 'X': ok = ty_is_int(v->t); need = "an integer"; break;
        case 'f': case 'e': case 'E': case 'g': case 'G': ok = ty_is_float(v->t); need = "a decimal number"; break;
        case 's': ok = v->t == ty_string; need = "a `String`"; break;
        case 'c': ok = v->t == ty_char; need = "a `Char`"; break;
        default: break;
        }
        if (!ok && !is_err(v->t)) {
            diag_error(an->pos, "`%%%c` needs %s, found `%s`", p->conv, need, v->t->name);
            return err_val();
        }
        vec_push(&vals, v);
    }
    int stream = mode == 2 ? 3 : (mode == 1 ? 2 : 1);
    if (mode == 2) {
        rt_decl(F, "zevic_fmt_begin", "declare void @zevic_fmt_begin()");
        rt_decl(F, "zevic_fmt_end", "declare void @zevic_fmt_end(ptr)");
        emit(F, "call void @zevic_fmt_begin()");
    }
    rt_decl(F, "zevic_printf_lit", "declare void @zevic_printf_lit(i32, ptr, i64)");
    rt_decl(F, "zevic_printf_i", "declare void @zevic_printf_i(i32, ptr, i64)");
    rt_decl(F, "zevic_printf_f", "declare void @zevic_printf_f(i32, ptr, double)");
    rt_decl(F, "zevic_printf_s", "declare void @zevic_printf_s(i32, ptr, ptr, i64)");
    rt_decl(F, "zevic_printf_c", "declare void @zevic_printf_c(i32, ptr, i32)");
    int vi = 0;
    for (int i = 0; i < pieces.n; i++) {
        FmtPiece *p = pieces.v[i];
        char *c = str_const(F->g, p->text, p->tlen);
        if (!p->is_spec) {
            emit(F, "call void @zevic_printf_lit(i32 %d, ptr %s, i64 %d)", stream, const_ptr(c), p->tlen);
            continue;
        }
        Val *v = vals.v[vi++];
        if (is_err(v->t)) continue;
        if (p->conv == 'd' || p->conv == 'u' || p->conv == 'o' || p->conv == 'x' || p->conv == 'X') {
            char *x = v->v;
            int bits = ty_bits(v->t);
            if (bits < 64) {
                x = tmp(F);
                emit(F, "%s = %s i%d %s to i64", x, ty_is_signed(v->t) ? "sext" : "zext", bits, v->v);
            }
            emit(F, "call void @zevic_printf_i(i32 %d, ptr %s, i64 %s)", stream, const_ptr(c), x);
        } else if (strchr("feEgG", p->conv)) {
            char *x = v->v;
            if (v->t == ty_f32) {
                x = tmp(F);
                emit(F, "%s = fpext float %s to double", x, v->v);
            }
            emit(F, "call void @zevic_printf_f(i32 %d, ptr %s, double %s)", stream, const_ptr(c), x);
        } else if (p->conv == 's') {
            char *sp, *sn;
            str_parts(F, *v, &sp, &sn);
            emit(F, "call void @zevic_printf_s(i32 %d, ptr %s, ptr %s, i64 %s)", stream, const_ptr(c), sp, sn);
        } else {
            emit(F, "call void @zevic_printf_c(i32 %d, ptr %s, i32 %s)", stream, const_ptr(c), v->v);
        }
    }
    if (mode == 2) {
        char *slot = new_slot(F, ty_string);
        emit(F, "call void @zevic_fmt_end(ptr %s)", slot);
        char *r = tmp(F);
        emit(F, "%s = load { ptr, i64 }, ptr %s", r, slot);
        Val res = {ty_string, r};
        return res;
    }
    return unit_val();
}

/* ---- Panic and Assert ---- */

static Val gen_panic_builtin(FnCtx *F, Node *n) {
    if (n->list.n != 1) {
        diag_error(n->pos, "`Panic` takes one argument, the message");
        return err_val();
    }
    Val m = gen_expr(F, n->list.v[0], ty_string);
    m = coerce(F, ((Node *)n->list.v[0])->pos, m, ty_string, "the message of `Panic`");
    if (m.t == ty_never || is_err(m.t)) return m.t == ty_never ? m : err_val();
    emit_panic_call(F, m, n->pos);
    return never_val(NULL);
}

static Val gen_assert_builtin(FnCtx *F, Node *n) {
    if (n->list.n != 2) {
        diag_error(n->pos, "`Assert` takes two arguments: the condition and the message");
        return err_val();
    }
    Val c = gen_expr(F, n->list.v[0], ty_bool);
    c = coerce(F, ((Node *)n->list.v[0])->pos, c, ty_bool, "the condition of `Assert`");
    if (c.t == ty_never) return c;
    if (is_err(c.t)) return err_val();
    char *fail = newlabel(F, "assert.fail"), *ok = newlabel(F, "assert.ok");
    condbr(F, c.v, ok, fail);
    start_block(F, fail);
    Val m = gen_expr(F, n->list.v[1], ty_string);
    m = coerce(F, ((Node *)n->list.v[1])->pos, m, ty_string, "the message of `Assert`");
    if (m.t != ty_never && !is_err(m.t)) emit_panic_call(F, m, n->pos);
    else if (!F->term) {
        emit(F, "unreachable");
        F->term = true;
    }
    start_block(F, ok);
    return unit_val();
}

/* ---- methods on built-in types ---- */

static bool int_fits(Type *from, Type *to) {
    int fb = ty_bits(from), tb = ty_bits(to);
    if (ty_is_signed(from) == ty_is_signed(to)) return tb >= fb;
    if (!ty_is_signed(from) && ty_is_signed(to)) return tb > fb;
    return false;
}

static Val cast_int(FnCtx *F, Val v, Type *to) {
    int fb = ty_bits(v.t), tb = ty_bits(to);
    if (fb == tb) {
        Val r = {to, v.v};
        return r;
    }
    char *r = tmp(F);
    if (tb < fb) emit(F, "%s = trunc %s %s to %s", r, type_ll(v.t), v.v, type_ll(to));
    else emit(F, "%s = %s %s %s to %s", r, ty_is_signed(v.t) ? "sext" : "zext", type_ll(v.t), v.v, type_ll(to));
    Val res = {to, r};
    return res;
}

static Val cast_float_to_int(FnCtx *F, Val v, Type *to, Pos pos) {
    int bits = ty_bits(to);
    bool sgn = ty_is_signed(to);
    char *x = v.v;
    if (v.t == ty_f32) {
        x = tmp(F);
        emit(F, "%s = fpext float %s to double", x, v.v);
    }
    double lo = sgn ? -ldexp(1.0, bits - 1) : 0.0;
    double hi = ldexp(1.0, sgn ? bits - 1 : bits);
    char *c1 = tmp(F), *c2 = tmp(F), *both = tmp(F);
    emit(F, "%s = fcmp oge double %s, %s", c1, x, double_hex(lo));
    emit(F, "%s = fcmp olt double %s, %s", c2, x, double_hex(hi));
    emit(F, "%s = and i1 %s, %s", both, c1, c2);
    char *ok = newlabel(F, "ok"), *bad = cold_panic(F, "conversion from a decimal number is out of range (or NaN)", pos);
    condbr(F, both, ok, bad);
    start_block(F, ok);
    char *r = tmp(F);
    emit(F, "%s = %s double %s to %s", r, sgn ? "fptosi" : "fptoui", x, type_ll(to));
    Val res = {to, r};
    return res;
}

static Val cast_to_float(FnCtx *F, Val v, Type *to) {
    char *r = tmp(F);
    if (ty_is_int(v.t)) emit(F, "%s = %s %s %s to %s", r, ty_is_signed(v.t) ? "sitofp" : "uitofp", type_ll(v.t), v.v, type_ll(to));
    else if (v.t == to) return v;
    else emit(F, "%s = %s %s %s to %s", r, to == ty_f64 ? "fpext" : "fptrunc", type_ll(v.t), v.v, type_ll(to));
    Val res = {to, r};
    return res;
}

/* The Zevic type that has the same representation as a C number type (NULL if `t` is not one). */
static Type *c_native(Type *t) {
    if (t->k != T_CTYPE) return NULL;
    if (strcmp(t->ll, "float") == 0) return ty_f32;
    if (strcmp(t->ll, "double") == 0) return ty_f64;
    if (t->ll[0] != 'i' || t == ty_c_by_name("CBool")) return NULL;
    bool sgn = t->payload_words == 1;
    switch ((int)t->size) {
    case 1: return sgn ? ty_i8 : ty_u8;
    case 2: return sgn ? ty_i16 : ty_u16;
    case 4: return sgn ? ty_i32 : ty_u32;
    default: return sgn ? ty_i64 : ty_u64;
    }
}

/* Result<A, E> with the same payload shape, relabelled as Result<B, E>. */
static Type *retag_result(Gen *g, Type *res, Type *ok) {
    Vec targs = {0};
    vec_push(&targs, ok);
    vec_push(&targs, res->args.v[1]);
    return instantiate(g, res->decl, targs, (Pos){0});
}

static Val gen_prim_method(FnCtx *F, Node *call, Val recv, const char *name, Type *expect);

/* x.ToCInt(), c.ToI32(), c.ToCLong() ...: conversions between Zevic numbers and C numbers.
 * Both sides are mapped to their Zevic equivalents, converted there, and the result relabelled. */
static bool gen_c_conversion(FnCtx *F, Node *call, Val recv, const char *name, Type *expect, Val *out) {
    if (name[0] != 'T' || name[1] != 'o') return false;
    Type *src = c_native(recv.t);
    if (!src && !ty_is_numeric(recv.t)) return false;
    char *base = zstrdup(name + 2);
    bool wrap = false;
    size_t bl = strlen(base);
    if (bl > 4 && strcmp(base + bl - 4, "Wrap") == 0) {
        wrap = true;
        base[bl - 4] = 0;
    }
    Type *to_c = ty_c_by_name(base);
    Type *to_native = to_c ? c_native(to_c) : NULL;
    if (!src && !to_native) return false; /* plain Zevic conversion */
    if (to_c && !to_native) return false;
    Type *to = to_native ? to_native : ty_prim_by_name(base);
    if (!to || !ty_is_numeric(to)) return false;
    Val r = recv;
    if (src) r.t = src;
    const char *tn = to->name;
    Val res = gen_prim_method(F, call, r, zsprintf("To%s%s", tn, wrap ? "Wrap" : ""), expect);
    if (is_err(res.t) || res.t == ty_never) {
        *out = res;
        return true;
    }
    if (to_c) {
        if (is_result_type(res.t)) res.t = retag_result(F->g, res.t, to_c);
        else res.t = to_c;
    }
    *out = res;
    return true;
}

/* x.Equals(y), x.Compare(y), x.Hash() on the built-in types. */
static bool gen_builtin_iface_method(FnCtx *F, Node *call, Val recv, const char *name, Val *out) {
    Type *t = recv.t;
    int nargs = call->list.n;
    if (strcmp(name, "Hash") == 0) {
        if (nargs || !builtin_satisfies(t, "Hashable")) return false;
        char *x;
        if (t == ty_string) {
            char *p, *n;
            str_parts(F, recv, &p, &n);
            declare(F->g, "zevic_str_hash", "declare i64 @zevic_str_hash(ptr, i64)");
            x = tmp(F);
            emit(F, "%s = call i64 @zevic_str_hash(ptr %s, i64 %s)", x, p, n);
        } else {
            Val w = recv;
            if (t == ty_bool) {
                char *z = tmp(F);
                emit(F, "%s = zext i1 %s to i64", z, recv.v);
                x = z;
            } else if (t == ty_char) {
                char *z = tmp(F);
                emit(F, "%s = zext i32 %s to i64", z, recv.v);
                x = z;
            } else if (ty_bits(t) == 64) {
                x = w.v;
            } else {
                char *z = tmp(F);
                emit(F, "%s = %s %s %s to i64", z, ty_is_signed(t) ? "sext" : "zext", type_ll(t), recv.v);
                x = z;
            }
            declare(F->g, "zevic_hash_u64", "declare i64 @zevic_hash_u64(i64)");
            char *h = tmp(F);
            emit(F, "%s = call i64 @zevic_hash_u64(i64 %s)", h, x);
            x = h;
        }
        Val r = {ty_u64, x};
        *out = r;
        return true;
    }
    if (nargs != 1) return false;
    Val o = gen_expr(F, call->list.v[0], t);
    o = coerce(F, ((Node *)call->list.v[0])->pos, o, t, "the argument");
    if (o.t == ty_never || is_err(o.t)) {
        *out = o.t == ty_never ? o : err_val();
        return true;
    }
    if (strcmp(name, "Equals") == 0) {
        *out = gen_compare(F, TK_EQEQ, recv, o, call->pos);
        return true;
    }
    if (!builtin_satisfies(t, "Comparable")) return false;
    Val lt = gen_compare(F, TK_LT, recv, o, call->pos);
    Val gt = gen_compare(F, TK_GT, recv, o, call->pos);
    char *a = tmp(F), *b = tmp(F);
    emit(F, "%s = select i1 %s, i32 1, i32 0", a, gt.v);
    emit(F, "%s = select i1 %s, i32 -1, i32 %s", b, lt.v, a);
    Val r = {ty_i32, b};
    *out = r;
    return true;
}

static Val gen_prim_method(FnCtx *F, Node *call, Val recv, const char *name, Type *expect) {
    Type *t = recv.t;
    int nargs = call->list.n;
    {
        Val cv;
        if (gen_c_conversion(F, call, recv, name, expect, &cv)) return cv;
    }
    if (is_builtin_value(t) && (strcmp(name, "Equals") == 0 || strcmp(name, "Compare") == 0 || strcmp(name, "Hash") == 0)) {
        Val r;
        if (gen_builtin_iface_method(F, call, recv, name, &r)) return r;
    }
    if (t == ty_string && strcmp(name, "ToC") == 0) {
        if (nargs) {
            diag_error(call->pos, "`ToC` takes no arguments");
            return err_val();
        }
        /* a String is always NUL-terminated, so its data pointer is a valid C string */
        char *p, *n;
        str_parts(F, recv, &p, &n);
        Val r = {ty_c_by_name("CString"), p};
        return r;
    }
    if (strcmp(name, "ToString") == 0) {
        if (nargs) {
            diag_error(call->pos, "`ToString` takes no arguments");
            return err_val();
        }
        return gen_to_string(F, recv, call->pos);
    }
    /* Char <-> U32 */
    if (t == ty_char && strcmp(name, "ToU32") == 0 && !nargs) {
        Val r = {ty_u32, recv.v};
        return r;
    }
    if (t == ty_u32 && strcmp(name, "ToCharUnchecked") == 0 && !nargs) {
        Val r = {ty_char, recv.v};
        return r;
    }
    if (t == ty_u32 && strcmp(name, "ToChar") == 0 && !nargs)
        return call_std(F, "Zevic.Convert", "U32ToChar", call->pos, &recv, 1, expect);
    /* numeric conversions: ToI32, ToU8Wrap, ToF64, ... */
    if (name[0] == 'T' && name[1] == 'o' && (ty_is_numeric(t))) {
        char *base = zstrdup(name + 2);
        bool wrap = false;
        size_t bl = strlen(base);
        if (bl > 4 && strcmp(base + bl - 4, "Wrap") == 0) {
            wrap = true;
            base[bl - 4] = 0;
        }
        Type *to = ty_prim_by_name(base);
        if (to && ty_is_numeric(to)) {
            if (nargs) {
                diag_error(call->pos, "`%s` takes no arguments", name);
                return err_val();
            }
            if (ty_is_float(to)) {
                if (wrap) {
                    diag_error(call->pos, "`%s` does not exist (only integer conversions have a `Wrap` form)", name);
                    return err_val();
                }
                return cast_to_float(F, recv, to);
            }
            if (ty_is_int(t)) {
                if (wrap || int_fits(t, to)) return cast_int(F, recv, to);
                /* may not fit: the answer is a Result, computed by Zevic.Convert */
                return call_std(F, "Zevic.Convert", zsprintf("%sTo%s", t->name, to->name), call->pos, &recv, 1, expect);
            }
            if (wrap) {
                diag_error(call->pos, "`%s` does not exist for `%s`", name, t->name);
                return err_val();
            }
            return cast_float_to_int(F, recv, to, call->pos);
        }
    }
    /* WrapAdd / WrapSub / WrapMul: arithmetic that wraps around instead of panicking */
    if (ty_is_int(t) && (strcmp(name, "WrapAdd") == 0 || strcmp(name, "WrapSub") == 0 || strcmp(name, "WrapMul") == 0)) {
        if (nargs != 1) {
            diag_error(call->pos, "`%s` takes one argument", name);
            return err_val();
        }
        Val o = gen_expr(F, call->list.v[0], t);
        o = coerce(F, ((Node *)call->list.v[0])->pos, o, t, "the argument");
        if (o.t == ty_never || is_err(o.t)) return o.t == ty_never ? o : err_val();
        const char *op = name[4] == 'A' ? "add" : name[4] == 'S' ? "sub" : "mul";
        char *r = tmp(F);
        emit(F, "%s = %s %s %s, %s", r, op, type_ll(t), recv.v, o.v);
        Val res = {t, r};
        return res;
    }
    if (ty_is_int(t) && (strcmp(name, "Min") == 0 || strcmp(name, "Max") == 0)) {
        if (nargs != 1) {
            diag_error(call->pos, "`%s` takes one argument", name);
            return err_val();
        }
        Val o = gen_expr(F, call->list.v[0], t);
        o = coerce(F, ((Node *)call->list.v[0])->pos, o, t, "the argument");
        if (o.t == ty_never || is_err(o.t)) return o.t == ty_never ? o : err_val();
        char *c = tmp(F), *r = tmp(F);
        bool sgn = ty_is_signed(t);
        const char *pred = strcmp(name, "Min") == 0 ? (sgn ? "slt" : "ult") : (sgn ? "sgt" : "ugt");
        emit(F, "%s = icmp %s %s %s, %s", c, pred, type_ll(t), recv.v, o.v);
        emit(F, "%s = select i1 %s, %s %s, %s %s", r, c, type_ll(t), recv.v, type_ll(t), o.v);
        Val res = {t, r};
        return res;
    }
    /* Min / Max / Clamp on decimal numbers (integers are handled above) */
    if (ty_is_float(t) && (strcmp(name, "Min") == 0 || strcmp(name, "Max") == 0) && nargs == 1) {
        Val o = gen_expr(F, call->list.v[0], t);
        o = coerce(F, ((Node *)call->list.v[0])->pos, o, t, "the argument");
        if (o.t == ty_never || is_err(o.t)) return o.t == ty_never ? o : err_val();
        char *c = tmp(F), *r = tmp(F);
        emit(F, "%s = fcmp %s %s %s, %s", c, strcmp(name, "Min") == 0 ? "olt" : "ogt", type_ll(t), recv.v, o.v);
        emit(F, "%s = select i1 %s, %s %s, %s %s", r, c, type_ll(t), recv.v, type_ll(t), o.v);
        Val res = {t, r};
        return res;
    }
    if (ty_is_numeric(t) && strcmp(name, "Clamp") == 0) {
        if (nargs != 2) {
            diag_error(call->pos, "`Clamp` takes two arguments: the lowest and the highest value");
            return err_val();
        }
        Val lo = gen_expr(F, call->list.v[0], t);
        lo = coerce(F, ((Node *)call->list.v[0])->pos, lo, t, "the lowest value of `Clamp`");
        Val hi = gen_expr(F, call->list.v[1], t);
        hi = coerce(F, ((Node *)call->list.v[1])->pos, hi, t, "the highest value of `Clamp`");
        if (lo.t == ty_never || hi.t == ty_never) return never_val(t);
        if (is_err(lo.t) || is_err(hi.t)) return err_val();
        bool fl = ty_is_float(t), sgn = ty_is_signed(t);
        const char *lt = fl ? "fcmp olt" : sgn ? "icmp slt" : "icmp ult";
        const char *gt = fl ? "fcmp ogt" : sgn ? "icmp sgt" : "icmp ugt";
        char *bad = tmp(F);
        emit(F, "%s = %s %s %s, %s", bad, gt, type_ll(t), lo.v, hi.v);
        char *ok = newlabel(F, "ok");
        condbr(F, bad, cold_panic(F, "Clamp: the lowest value is greater than the highest", call->pos), ok);
        start_block(F, ok);
        char *c1 = tmp(F), *m1 = tmp(F), *c2 = tmp(F), *m2 = tmp(F);
        emit(F, "%s = %s %s %s, %s", c1, lt, type_ll(t), recv.v, lo.v);
        emit(F, "%s = select i1 %s, %s %s, %s %s", m1, c1, type_ll(t), lo.v, type_ll(t), recv.v);
        emit(F, "%s = %s %s %s, %s", c2, gt, type_ll(t), m1, hi.v);
        emit(F, "%s = select i1 %s, %s %s, %s %s", m2, c2, type_ll(t), hi.v, type_ll(t), m1);
        Val res = {t, m2};
        return res;
    }
    if (ty_is_int(t) && strcmp(name, "Abs") == 0 && !nargs) {
        if (!ty_is_signed(t)) return recv;
        char *neg = tmp(F), *r = tmp(F);
        Val zero = {t, "0"};
        Val n = gen_int_arith(F, TK_MINUS, zero, recv, call->pos);
        neg = n.v;
        emit(F, "%s = icmp slt %s %s, 0", r, type_ll(t), recv.v);
        char *sel = tmp(F);
        emit(F, "%s = select i1 %s, %s %s, %s %s", sel, r, type_ll(t), neg, type_ll(t), recv.v);
        Val res = {t, sel};
        return res;
    }
    if (ty_is_float(t) && strcmp(name, "Abs") == 0 && !nargs) {
        char *r = tmp(F);
        const char *intr = t == ty_f64 ? "llvm.fabs.f64" : "llvm.fabs.f32";
        declare(F->g, intr, zsprintf("declare %s @%s(%s)", type_ll(t), intr, type_ll(t)));
        emit(F, "%s = call %s @%s(%s %s)", r, type_ll(t), intr, type_ll(t), recv.v);
        Val res = {t, r};
        return res;
    }
    if (ty_is_float(t) && (strcmp(name, "IsNaN") == 0 || strcmp(name, "IsInf") == 0) && !nargs) {
        char *r = tmp(F);
        if (name[2] == 'N') {
            emit(F, "%s = fcmp uno %s %s, %s", r, type_ll(t), recv.v, recv.v);
        } else {
            const char *intr = t == ty_f64 ? "llvm.fabs.f64" : "llvm.fabs.f32";
            declare(F->g, intr, zsprintf("declare %s @%s(%s)", type_ll(t), intr, type_ll(t)));
            char *a = tmp(F);
            emit(F, "%s = call %s @%s(%s %s)", a, type_ll(t), intr, type_ll(t), recv.v);
            emit(F, "%s = fcmp oeq %s %s, 0x7FF0000000000000", r, type_ll(t), a);
        }
        Val res = {ty_bool, r};
        return res;
    }
    if (t == ty_string) {
        if (name[0] == 'T' && name[1] == 'o' && !nargs) {
            Type *to = ty_prim_by_name(name + 2);
            if (to && ty_is_numeric(to))
                return call_std(F, "Zevic.Convert", zsprintf("StringTo%s", to->name), call->pos, &recv, 1, expect);
        }
        if (strcmp(name, "Len") == 0 && !nargs) {
            char *p, *n;
            str_parts(F, recv, &p, &n);
            Val res = {ty_i64, n};
            return res;
        }
        if (strcmp(name, "IsEmpty") == 0 && !nargs) {
            char *p, *n;
            str_parts(F, recv, &p, &n);
            char *r = tmp(F);
            emit(F, "%s = icmp eq i64 %s, 0", r, n);
            Val res = {ty_bool, r};
            return res;
        }
        if (strcmp(name, "CharCount") == 0 && !nargs) {
            char *p, *n;
            str_parts(F, recv, &p, &n);
            rt_decl(F, "zevic_str_char_count", "declare i64 @zevic_str_char_count(ptr, i64)");
            char *r = tmp(F);
            emit(F, "%s = call i64 @zevic_str_char_count(ptr %s, i64 %s)", r, p, n);
            Val res = {ty_i64, r};
            return res;
        }
        if (strcmp(name, "Slice") == 0 && nargs == 2) {
            /* s.Slice(from, to): a copy of the bytes [from, to); both ends must be on character boundaries */
            Val a = gen_expr(F, call->list.v[0], ty_i64);
            a = coerce(F, ((Node *)call->list.v[0])->pos, a, ty_i64, "the start of `Slice`");
            Val b = gen_expr(F, call->list.v[1], ty_i64);
            b = coerce(F, ((Node *)call->list.v[1])->pos, b, ty_i64, "the end of `Slice`");
            if (a.t == ty_never || b.t == ty_never) return never_val(NULL);
            if (is_err(a.t) || is_err(b.t)) return err_val();
            char *p, *n;
            str_parts(F, recv, &p, &n);
            rt_decl(F, "zevic_str_slice_ok", "declare i32 @zevic_str_slice_ok(ptr, i64, i64, i64)");
            rt_decl(F, "zevic_str_slice", "declare void @zevic_str_slice(ptr, ptr, i64, i64, i64)");
            char *ok = tmp(F), *bad = tmp(F);
            emit(F, "%s = call i32 @zevic_str_slice_ok(ptr %s, i64 %s, i64 %s, i64 %s)", ok, p, n, a.v, b.v);
            emit(F, "%s = icmp eq i32 %s, 0", bad, ok);
            char *go = newlabel(F, "ok");
            condbr(F, bad, cold_panic(F, "string slice out of range or not on character boundaries", call->pos), go);
            start_block(F, go);
            char *slot = new_slot(F, ty_string);
            emit(F, "call void @zevic_str_slice(ptr %s, ptr %s, i64 %s, i64 %s, i64 %s)", slot, p, n, a.v, b.v);
            char *r = tmp(F);
            emit(F, "%s = load { ptr, i64 }, ptr %s", r, slot);
            Val res = {ty_string, r};
            return res;
        }
        if (strcmp(name, "ByteAt") == 0 && nargs == 1) {
            Val i = gen_expr(F, call->list.v[0], ty_i64);
            i = coerce(F, ((Node *)call->list.v[0])->pos, i, ty_i64, "the argument of `ByteAt`");
            if (i.t == ty_never || is_err(i.t)) return i.t == ty_never ? i : err_val();
            char *p, *n;
            str_parts(F, recv, &p, &n);
            char *bad = tmp(F);
            emit(F, "%s = icmp uge i64 %s, %s", bad, i.v, n);
            char *ok = newlabel(F, "ok");
            condbr(F, bad, cold_panic(F, "string index out of range", call->pos), ok);
            start_block(F, ok);
            char *gp = tmp(F), *r = tmp(F);
            emit(F, "%s = getelementptr inbounds i8, ptr %s, i64 %s", gp, p, i.v);
            emit(F, "%s = load i8, ptr %s", r, gp);
            Val res = {ty_u8, r};
            return res;
        }
    }
    diag_error(call->pos, "type `%s` has no method `%s` (or it takes different arguments)", t->name, name);
    return err_val();
}

/* x.Method(args) where x is a struct, class or enum. */
static Val gen_user_method(FnCtx *F, Node *call, Place p, Node *recv_node, const char *name, Type *expect) {
    Type *t = p.t;
    Node *fn = find_member_fn(t, name);
    Decl *dflt = NULL;
    if (!fn) {
        IMethod dm;
        bool conflict = false;
        if (find_default(F->g, t, name, &dm, &conflict)) {
            fn = dm.fn;
            dflt = default_decl(F->g, t, &dm);
        }
    }
    if (!fn && call->flag3) {
        diag_error(call->pos, "a value of type `%s` cannot be indexed with `[]`", t->name);
        for (int i = 0; i < call->list.n; i++) gen_expr(F, call->list.v[i], NULL);
        return err_val();
    }
    if (!fn) {
        Field *ff = find_field(t, name, NULL);
        if (ff && ff->t->k == T_FN) {
            Node *fm = node_new(N_MEMBER, call->pos);
            fm->a = recv_node;
            fm->name = (char *)name;
            Val fv = gen_expr(F, fm, NULL);
            if (fv.t == ty_never || is_err(fv.t)) return fv.t == ty_never ? fv : err_val();
            return gen_call_value(F, call, fv, expect);
        }
        if (ff)
            diag_error(call->pos, "`%s` is a field of `%s`, not a method", name, t->name);
        else
            diag_error(call->pos, "type `%s` has no method `%s`", t->name, name);
        for (int i = 0; i < call->list.n; i++) gen_expr(F, call->list.v[i], NULL);
        return err_val();
    }
    if (strcmp(name, "Init") == 0) {
        diag_error(call->pos, "`Init` cannot be called directly; create values with `%s.New(...)`", t->decl->name);
        return err_val();
    }
    if (!check_vis(F, t, name, call->pos, "method")) return err_val();
    if (fn->is_static) {
        diag_error(call->pos, "`%s` is a static function; call it as `%s.%s(...)`", name, t->decl->name, name);
        return err_val();
    }
    if (fn->is_mut_fn && !p.mut && call->flag3) {
        diag_error(call->pos, "cannot assign to an element of `%s` because it is not `mut`", root_name(recv_node));
        diag_note("declare it with `mut %s: %s = ...` to allow changes", root_name(recv_node), t->name);
        return err_val();
    }
    if (fn->is_mut_fn && !p.mut) {
        if (recv_node->kind == N_THIS)
            diag_error(call->pos, "`mut fn %s` cannot be called here: `this` can only be changed inside a `mut fn` (or `Init`)", name);
        else
            diag_error(call->pos, "cannot call `mut fn %s` because `%s` is not `mut`", name, root_name(recv_node));
        return err_val();
    }
    Val recv = {ty_ptr(ty_u8), p.ptr};
    if (t->k == T_CLASS) {
        char *o = tmp(F);
        emit(F, "%s = load ptr, ptr %s", o, p.ptr);
        recv.v = o;
    }
    Decl *md = dflt ? dflt : method_decl(t, fn);
    return gen_call_decl(F, call, md, t, &call->a->list2, expect, &recv);
}

/* `42.ToCInt()`: an unsuffixed literal receiver takes the C type's own width, so the conversion cannot fail. */
static Type *literal_c_target(Node *recv, const char *name) {
    Node *lit = recv;
    if (lit->kind == N_UNARY && lit->a && lit->a->kind == N_INT) lit = lit->a;
    if (lit->kind != N_INT && lit->kind != N_FLOAT) return NULL;
    if (lit->suffix) return NULL;
    if (name[0] != 'T' || name[1] != 'o') return NULL;
    Type *c = ty_c_by_name(name + 2);
    return c ? c_native(c) : NULL;
}

static Val gen_method_call(FnCtx *F, Node *call, Node *recv_node, const char *name, Type *expect) {
    Type *lit_t = literal_c_target(recv_node, name);
    if (lit_t) {
        Val v = gen_expr(F, recv_node, lit_t);
        if (v.t == ty_never || is_err(v.t)) return v;
        return gen_prim_method(F, call, v, name, expect);
    }
    Place p = gen_place(F, recv_node);
    if (p.t == ty_never) return never_val(expect);
    if (is_err(p.t)) {
        for (int i = 0; i < call->list.n; i++) gen_expr(F, call->list.v[i], NULL);
        return err_val();
    }
    if (p.t->k == T_IFACE) return gen_iface_method(F, call, p, recv_node, name, expect);
    if (is_user_type(p.t)) return gen_user_method(F, call, p, recv_node, name, expect);
    Val recv = load_place(F, p);
    if (recv.t->k == T_BUF) return gen_buf_method(F, call, recv, name);
    if (call->flag3) {
        diag_error(call->pos, "a value of type `%s` cannot be indexed with `[]`", recv.t->name);
        for (int i = 0; i < call->list.n; i++) gen_expr(F, call->list.v[i], NULL);
        return err_val();
    }
    return gen_prim_method(F, call, recv, name, expect);
}

/* String.FromC(c) / String.FromCLossy(c): copy a C string into a String. */
static Val gen_string_from_c(FnCtx *F, Node *call, bool lossy) {
    const char *fname = lossy ? "FromCLossy" : "FromC";
    if (call->list.n != 1) {
        diag_error(call->pos, "`String.%s` takes one argument (a `CString`)", fname);
        return err_val();
    }
    Type *cs = ty_c_by_name("CString");
    Val c = gen_expr(F, call->list.v[0], cs);
    if (c.t == ty_never || is_err(c.t)) return c.t == ty_never ? c : err_val();
    c = coerce(F, ((Node *)call->list.v[0])->pos, c, cs, "the argument");
    if (is_err(c.t)) return err_val();
    declare(F->g, "zevic_str_from_c", "declare i32 @zevic_str_from_c(ptr, ptr, i32)");
    char *slot = new_slot(F, ty_string);
    char *ok = tmp(F);
    emit(F, "%s = call i32 @zevic_str_from_c(ptr %s, ptr %s, i32 %d)", ok, slot, c.v, lossy ? 1 : 0);
    if (!lossy) {
        char *bad = tmp(F);
        char *lb = newlabel(F, "fromc.bad"), *lg = newlabel(F, "fromc.ok");
        emit(F, "%s = icmp eq i32 %s, 0", bad, ok);
        emit(F, "br i1 %s, label %%%s, label %%%s", bad, lb, lg);
        start_block(F, lb);
        const char *m = "String.FromC: the C string is not valid UTF-8 (use String.FromCLossy)";
        Val msg = str_val(str_const(F->g, m, (int)strlen(m)));
        emit_panic_call(F, msg, call->pos);
        start_block(F, lg);
    }
    char *r = tmp(F);
    emit(F, "%s = load { ptr, i64 }, ptr %s", r, slot);
    Val res = {ty_string, r};
    return res;
}

/* NewString(s): a copy of `s` in malloc'd memory that C may keep (and must free). */
static Val gen_new_string(FnCtx *F, Node *call) {
    if (call->list.n != 1) {
        diag_error(call->pos, "`NewString` takes one argument (a `String`)");
        return err_val();
    }
    Val s = gen_expr(F, call->list.v[0], ty_string);
    s = coerce(F, ((Node *)call->list.v[0])->pos, s, ty_string, "the argument of `NewString`");
    if (s.t == ty_never || is_err(s.t)) return s.t == ty_never ? s : err_val();
    declare(F->g, "zevic_new_cstring", "declare ptr @zevic_new_cstring(ptr, i64)");
    char *p, *n;
    str_parts(F, s, &p, &n);
    char *r = tmp(F);
    emit(F, "%s = call ptr @zevic_new_cstring(ptr %s, i64 %s)", r, p, n);
    Val res = {ty_c_by_name("CString"), r};
    return res;
}

/* Free(p): C's free, for memory from C or from NewString. */
static Val gen_free(FnCtx *F, Node *call) {
    if (call->list.n != 1) {
        diag_error(call->pos, "`Free` takes one argument (a pointer or a `CString`)");
        return err_val();
    }
    Val p = gen_expr(F, call->list.v[0], NULL);
    if (p.t == ty_never || is_err(p.t)) return p.t == ty_never ? p : err_val();
    if (p.t->k != T_PTR && p.t != ty_c_by_name("CString")) {
        diag_error(((Node *)call->list.v[0])->pos, "`Free` needs a pointer or a `CString`, found `%s`", p.t->name);
        return err_val();
    }
    declare(F->g, "free", "declare void @free(ptr)");
    emit(F, "call void @free(ptr %s)", p.v);
    return unit_val();
}


/* =========================================================================
 * Function values and lambdas
 * ========================================================================= */

static Type *fn_type_of(Vec *ptypes, Type *ret) {
    Vec ps = {0};
    for (int i = 0; i < ptypes->n; i++) vec_push(&ps, ptypes->v[i]);
    return ty_fn(ps, ret);
}

/* A named function as a value: { thunk, null }. The thunk takes the (unused) environment first. */
static Val fn_value_of(FnCtx *F, Decl *d, Pos pos) {
    Gen *g = F->g;
    Node *fn = d->node;
    if (fn->list2.n) {
        diag_error(pos, "the generic function `%s` cannot be used as a value (call it, or wrap it in a lambda)", d->name);
        return err_val();
    }
    if (d->abi) {
        diag_error(pos, "the %s function `%s` cannot be used as a value here (wrap it in a Zevic function)", d->abi, d->name);
        return err_val();
    }
    Inst *in = get_inst(g, d, NULL, NULL);
    for (int i = 0; i < in->ptypes.n; i++)
        if (is_err(in->ptypes.v[i])) return err_val();
    if (is_err(in->ret)) return err_val();
    Type *ft = fn_type_of(&in->ptypes, in->ret);
    char *thunk = zsprintf("thunk.%s", in->name);
    if (!dict_has(&g->thunks, thunk)) {
        dict_put(&g->thunks, thunk, in->name);
        SB ps = {0}, as = {0};
        for (int i = 0; i < in->ptypes.n; i++) {
            sb_printf(&ps, ", %s %%p%d", type_ll(in->ptypes.v[i]), i);
            sb_printf(&as, "%s%s %%p%d", i ? ", " : "", type_ll(in->ptypes.v[i]), i);
        }
        bool is_void = in->ret == ty_unit || in->ret == ty_never;
        const char *rl = is_void ? "void" : type_ll(in->ret);
        sb_printf(&g->funcs, "define %s @\"%s\"(ptr %%env%s) {\nentry:\n", rl, thunk, sb_str(&ps));
        if (in->ret == ty_never) sb_printf(&g->funcs, "  call void @\"%s\"(%s)\n  unreachable\n}\n\n", in->name, sb_str(&as));
        else if (is_void) sb_printf(&g->funcs, "  call void @\"%s\"(%s)\n  ret void\n}\n\n", in->name, sb_str(&as));
        else sb_printf(&g->funcs, "  %%r = call %s @\"%s\"(%s)\n  ret %s %%r\n}\n\n", rl, in->name, sb_str(&as), rl);
    }
    Val v = {ft, zsprintf("{ ptr @\"%s\", ptr null }", thunk)};
    return v;
}

/* f(args) where `f` is a value of a function type. */
static Val gen_call_value(FnCtx *F, Node *call, Val f, Type *expect) {
    Type *ft = f.t;
    if (call->list.n != ft->args.n) {
        diag_error(call->pos, "this function value takes %d argument(s), but %d were given", ft->args.n, call->list.n);
        for (int i = 0; i < call->list.n; i++) gen_expr(F, call->list.v[i], NULL);
        return err_val();
    }
    SB args = {0};
    bool never = false;
    for (int i = 0; i < ft->args.n; i++) {
        Type *pt = ft->args.v[i];
        Val v = gen_expr(F, call->list.v[i], pt);
        if (v.t == ty_never) {
            never = true;
            continue;
        }
        v = coerce(F, ((Node *)call->list.v[i])->pos, v, pt, zsprintf("argument %d of a function value", i + 1));
        if (is_err(v.t)) return err_val();
        sb_printf(&args, ", %s %s", type_ll(pt), v.v);
    }
    if (never) return never_val(expect);
    char *fp = tmp(F), *env = tmp(F);
    emit(F, "%s = extractvalue { ptr, ptr } %s, 0", fp, f.v);
    emit(F, "%s = extractvalue { ptr, ptr } %s, 1", env, f.v);
    Type *ret = ft->ret;
    if (ret == ty_unit || ret == ty_never) {
        emit(F, "call void %s(ptr %s%s)", fp, env, sb_str(&args));
        if (ret == ty_never) {
            emit(F, "unreachable");
            F->term = true;
            return never_val(expect);
        }
        return unit_val();
    }
    char *r = tmp(F);
    emit(F, "%s = call %s %s(ptr %s%s)", r, type_ll(ret), fp, env, sb_str(&args));
    Val res = {ret, r};
    return res;
}

/* (x: I32) => body: a function plus an environment holding the variables it captures. */
static Val gen_lambda(FnCtx *F, Node *n, Type *expect) {
    Gen *g = F->g;
    Pos pos = n->pos;
    Type *want = (expect && expect->k == T_FN) ? expect : NULL;
    if (want && want->args.n != n->list.n) {
        diag_error(pos, "this lambda takes %d parameter(s), but a function with %d is expected", n->list.n, want->args.n);
        return err_val();
    }
    Vec ptypes = {0};
    for (int i = 0; i < n->list.n; i++) {
        Node *pm = n->list.v[i];
        Type *pt = NULL;
        if (pm->a) pt = resolve_type(g, F->file, F->env, pm->a);
        else if (want) pt = want->args.v[i];
        else {
            diag_error(pm->pos, "the type of the lambda parameter `%s` cannot be inferred here; write `%s: Type`", pm->name, pm->name);
            return err_val();
        }
        if (is_err(pt)) return err_val();
        if (want && pt != want->args.v[i]) {
            diag_error(pm->pos, "parameter `%s` has type `%s`, but `%s` is expected here", pm->name, pt->name, ((Type *)want->args.v[i])->name);
            return err_val();
        }
        vec_push(&ptypes, pt);
    }
    /* what the body uses from the enclosing function */
    Dict used = {0};
    bool uses_this = false;
    collect_idents(n->b, &used, &uses_this);
    Vec cap_names = {0};
    Vec cap_locals = {0};
    for (size_t k = 0; k < used.cap; k++) {
        if (!used.keys[k]) continue;
        bool is_param = false;
        for (int i = 0; i < n->list.n; i++)
            if (strcmp(((Node *)n->list.v[i])->name, used.keys[k]) == 0) is_param = true;
        if (is_param) continue;
        Local *l = find_local(F, used.keys[k]);
        if (!l) continue;
        if (n->flag && l->mut && l->t->k != T_CLASS) {
            diag_error(n->pos, "an `async` block cannot use the `mut` variable `%s` of the surrounding function (the two would race); "
                       "send the value through a `Channel`, or keep it in a class that is guarded by a `Mutex`", l->name);
            return err_val();
        }
        vec_push(&cap_names, (void *)l->name);
        vec_push(&cap_locals, l);
    }
    if (uses_this && !F->has_this) uses_this = false;
    int nslots = cap_locals.n + (uses_this ? 1 : 0);

    /* the lambda's own function */
    FnCtx L = {0};
    L.g = g;
    L.file = F->file;
    L.env = F->env;
    L.cur = "entry";
    L.ret = want ? want->ret : NULL;
    collect_lambda_names(n->b, &L.heap);
    push_scope(&L);
    int slot = 0;
    if (uses_this) {
        char *gp = tmp(&L), *tp = tmp(&L);
        emit(&L, "%s = getelementptr inbounds ptr, ptr %%env, i64 %d", gp, slot++);
        emit(&L, "%s = load ptr, ptr %s", tp, gp);
        L.has_this = true;
        L.this_t = F->this_t;
        L.this_mut = F->this_mut;
        if (F->this_t->k == T_CLASS) {
            L.this_slot = new_slot(&L, F->this_t);
            emit(&L, "store ptr %s, ptr %s", tp, L.this_slot);
        } else {
            L.this_slot = tp;
        }
    }
    for (int i = 0; i < cap_locals.n; i++) {
        Local *cl = cap_locals.v[i];
        char *gp = tmp(&L), *cp = tmp(&L);
        emit(&L, "%s = getelementptr inbounds ptr, ptr %%env, i64 %d", gp, slot++);
        emit(&L, "%s = load ptr, ptr %s", cp, gp);
        bind_local_ptr(&L, cl->name, cl->t, cl->mut, cp, cl->pos);
    }
    SB params = {0};
    sb_puts(&params, "ptr %env");
    for (int i = 0; i < n->list.n; i++) {
        Node *pm = n->list.v[i];
        Type *pt = ptypes.v[i];
        sb_printf(&params, ", %s %%p%d", type_ll(pt), i);
        Local *pl = declare_local(&L, pm->name, pt, false, pm->pos);
        Val pv = {pt, zsprintf("%%p%d", i)};
        if (pt != ty_unit) store_to(&L, pt, pv, pl->ptr);
    }
    Val v = n->b->kind == N_BLOCK ? gen_block(&L, n->b, L.ret) : gen_expr(&L, n->b, L.ret);
    Type *ret = L.ret;
    if (!ret) ret = (v.t == ty_never || is_err(v.t)) ? ty_unit : v.t;
    if (!L.term) {
        if (ret == ty_unit) {
            emit(&L, "ret void");
        } else if (ret == ty_never) {
            emit(&L, "unreachable");
        } else {
            v = coerce(&L, n->b->pos, v, ret, "the result of the lambda");
            if (is_err(v.t)) emit(&L, "unreachable");
            else emit(&L, "ret %s %s", type_ll(ret), v.v);
        }
        L.term = true;
    }
    char *lname = zsprintf("lambda.%d", g->nlambda++);
    const char *rl = (ret == ty_unit || ret == ty_never) ? "void" : type_ll(ret);
    sb_printf(&g->funcs, "define %s @\"%s\"(%s) {\nentry:\n%s%s%s}\n\n", rl, lname, sb_str(&params), sb_str(&L.allocas),
              sb_str(&L.code), sb_str(&L.cold));

    /* the environment: the cells of the captured variables (and `this`) */
    char *env = "null";
    if (nslots) {
        rt_decl(F, "zevic_alloc", "declare ptr @zevic_alloc(i64)");
        env = tmp(F);
        emit(F, "%s = call ptr @zevic_alloc(i64 %d)", env, nslots * 8);
        int si = 0;
        if (uses_this) {
            char *tv = F->this_slot;
            if (F->this_t->k == T_CLASS) {
                tv = tmp(F);
                emit(F, "%s = load ptr, ptr %s", tv, F->this_slot);
            }
            char *gp = tmp(F);
            emit(F, "%s = getelementptr inbounds ptr, ptr %s, i64 %d", gp, env, si++);
            emit(F, "store ptr %s, ptr %s", tv, gp);
        }
        for (int i = 0; i < cap_locals.n; i++) {
            Local *cl = cap_locals.v[i];
            char *gp = tmp(F);
            emit(F, "%s = getelementptr inbounds ptr, ptr %s, i64 %d", gp, env, si++);
            emit(F, "store ptr %s, ptr %s", cl->ptr, gp);
        }
    }
    Type *ft = fn_type_of(&ptypes, ret);
    char *a = tmp(F), *b = tmp(F);
    emit(F, "%s = insertvalue { ptr, ptr } undef, ptr @\"%s\", 0", a, lname);
    emit(F, "%s = insertvalue { ptr, ptr } %s, ptr %s, 1", b, a, env);
    if (strcmp(env, "null") == 0) dict_put(&g->noenv, zsprintf("%p:%s", (void *)F, b), (void *)lname);
    Val res = {ft, b};
    return res;
}

/* Intrinsics.ObjPtr(x) and Intrinsics.PtrObj<T>(p): a class reference and its address are the same bits. */
static Val gen_obj_ptr(FnCtx *F, Node *call, Decl *d, Node *last) {
    bool to_ptr = strcmp(d->name, "ObjPtr") == 0;
    if (call->list.n != 1) {
        diag_error(call->pos, "`%s` takes one argument", d->name);
        return err_val();
    }
    Type *voidp = ty_ptr(ty_c_by_name("CVoid"));
    if (to_ptr) {
        Val x = gen_expr(F, call->list.v[0], NULL);
        if (x.t == ty_never || is_err(x.t)) return x.t == ty_never ? x : err_val();
        if (x.t->k != T_CLASS) {
            diag_error(call->pos, "`ObjPtr` needs a class value, found `%s`", x.t->name);
            return err_val();
        }
        Val r = {voidp, x.v};
        return r;
    }
    Val p = gen_expr(F, call->list.v[0], voidp);
    p = coerce(F, ((Node *)call->list.v[0])->pos, p, voidp, "the argument of `PtrObj`");
    if (p.t == ty_never || is_err(p.t)) return p.t == ty_never ? p : err_val();
    Type *t = last->list2.n == 1 ? resolve_type(F->g, F->file, F->env, last->list2.v[0]) : NULL;
    if (!t || is_err(t)) {
        if (!t) diag_error(call->pos, "`PtrObj` needs its type: `PtrObj<T>(p)`");
        return err_val();
    }
    if (t->k != T_CLASS) {
        diag_error(call->pos, "`PtrObj` can only produce a class, not `%s`", t->name);
        return err_val();
    }
    Val r = {t, p.v};
    return r;
}

static Val gen_call(FnCtx *F, Node *n, Type *expect) {
    Node *callee = n->a;
    if (callee->kind == N_IDENT) {
        Local *fl = find_local(F, callee->name);
        if (fl && fl->t->k == T_FN) return gen_call_value(F, n, load_local(F, fl), expect);
    }
    Vec segs = {0}, nodes = {0};
    if (flatten_path(callee, &segs, &nodes)) {
        char *head = segs.v[0];
        bool value_head = find_local(F, head) != NULL;
        if (!value_head) {
            char **sg = (char **)segs.v;
            int used = 0;
            Decl *d = NULL;
            if (strcmp(head, "Self") == 0 && F->env && F->env->self && F->env->self->decl) {
                d = F->env->self->decl;
                used = 1;
            } else {
                d = find_decl(F->g, F->file, sg, segs.n, &used, callee->pos);
            }
            if (used < 0) return err_val();
            if (d && (d->node->kind == D_STRUCT || d->node->kind == D_CLASS || d->node->kind == D_ENUM) &&
                used == segs.n - 1) {
                const char *member = segs.v[used];
                Node *tnode = nodes.v[used - 1];
                Type *t = (strcmp(head, "Self") == 0) ? F->env->self : static_type(F, d, tnode, expect);
                Node *lastn = nodes.v[nodes.n - 1];
                if (!t && strcmp(d->name, "Handle") == 0 && d->mod->is_std && strcmp(member, "Get") == 0 && lastn->list2.n == 1) {
                    /* Handle.Get<T>(id): the type argument written after `Get` is the class's */
                    Vec targs = {0};
                    vec_push(&targs, resolve_type(F->g, F->file, F->env, lastn->list2.v[0]));
                    t = instantiate(F->g, d, targs, n->pos);
                    Node *gfn = find_member_fn(t, "Get");
                    if (is_err(t) || !gfn) return err_val();
                    return gen_call_decl(F, n, method_decl(t, gfn), t, NULL, expect, NULL);
                }
                if (d->node->kind == D_ENUM && find_variant_node(d->node, member))
                    return gen_variant(F, n, n->pos, d, t, member, expect);
                if (strcmp(member, "New") == 0) {
                    if (!t) {
                        diag_error(n->pos, "cannot infer the type arguments of `%s` (write `%s<...>.New(...)` or give the variable a type)",
                                   d->name, d->name);
                        return err_val();
                    }
                    return gen_construct(F, n, d, t, expect);
                }
                Node *fn = t ? find_member_fn(t, member) : NULL;
                if (fn && fn->is_static) {
                    if (!check_vis(F, t, member, n->pos, "function")) return err_val();
                    Node *last = nodes.v[nodes.n - 1];
                    return gen_call_decl(F, n, method_decl(t, fn), t, &last->list2, expect, NULL);
                }
                if (fn) {
                    diag_error(n->pos, "`%s` is a method; call it on a value, like `value.%s(...)`", member, member);
                    return err_val();
                }
                if (!t) {
                    diag_error(n->pos, "cannot infer the type arguments of `%s`; write them like `%s<I32>.%s(...)`", d->name,
                               d->name, member);
                    return err_val();
                }
                diag_error(n->pos, "type `%s` has no function `%s`", d->name, member);
                return err_val();
            }
            if (d && d->node->kind == D_FN && used == segs.n) {
                Node *last = nodes.v[nodes.n - 1];
                if (d->abi && strcmp(d->abi, "intrinsic") == 0) {
                    if (strcmp(d->name, "Printf") == 0) return gen_printf(F, n, 0);
                    if (strcmp(d->name, "Eprintf") == 0) return gen_printf(F, n, 1);
                    if (strcmp(d->name, "Format") == 0) return gen_printf(F, n, 2);
                    if (strcmp(d->name, "ObjPtr") == 0 || strcmp(d->name, "PtrObj") == 0) return gen_obj_ptr(F, n, d, last);
                    diag_error(n->pos, "unknown intrinsic `%s`", d->name);
                    return err_val();
                }
                return gen_call_decl(F, n, d, NULL, &last->list2, expect, NULL);
            }
            if (d && d->node->kind == D_FN) {
                diag_error(callee->pos, "function `%s` has no member `%s`", d->name, segs.v[used]);
                return err_val();
            }
            if (!d && segs.n == 2 && strcmp(head, "String") == 0 &&
                (strcmp(segs.v[1], "FromC") == 0 || strcmp(segs.v[1], "FromCLossy") == 0))
                return gen_string_from_c(F, n, strcmp(segs.v[1], "FromCLossy") == 0);
            if (!d && segs.n == 1) {
                Decl *owner = prelude_variant_owner(F, head);
                if (owner) return gen_variant(F, n, n->pos, owner, static_type(F, owner, NULL, expect), head, expect);
                if (strcmp(head, "NewString") == 0) return gen_new_string(F, n);
                if (strcmp(head, "Free") == 0) return gen_free(F, n);
                if (strcmp(head, "Panic") == 0) return gen_panic_builtin(F, n);
                if (strcmp(head, "Assert") == 0) return gen_assert_builtin(F, n);
            }
            if (d && (d->node->kind == D_INTERFACE || d->node->kind == D_EXTERN)) {
                diag_error(callee->pos, "calling `%s` this way is not supported yet", path_join(&segs, "."));
                return err_val();
            }
            /* a type or constant followed by more names (`Shape.Empty.Area()`): a method call on a value */
            if (callee->kind == N_IDENT) {
                diag_error(callee->pos, "unknown function `%s`", head);
                return err_val();
            }
            /* a.b(...) where `a` is not a module or type: fall through to a method call on a value */
        }
    }
    if (callee->kind == N_MEMBER) return gen_method_call(F, n, callee->a, callee->name, expect);
    if (callee->kind == N_CALL || callee->kind == N_INDEX || callee->kind == N_LAMBDA) {
        Val f = gen_expr(F, callee, NULL);
        if (f.t == ty_never || is_err(f.t)) return f.t == ty_never ? f : err_val();
        if (f.t->k == T_FN) return gen_call_value(F, n, f, expect);
        diag_error(callee->pos, "a value of type `%s` cannot be called", f.t->name);
        return err_val();
    }
    diag_error(callee->pos, "this expression cannot be called");
    return err_val();
}

/* =========================================================================
 * Functions and the whole program
 * ========================================================================= */

static void emit_function(Gen *g, Inst *in) {
    if (in->emitted) return;
    in->emitted = true;
    Node *fn = in->decl->node;
    FnCtx F = {0};
    F.g = g;
    F.inst = in;
    F.file = in->decl->file;
    F.env = in->env;
    F.ret = in->ret;
    F.cur = "entry";
    collect_lambda_names(fn->b, &F.heap);
    push_scope(&F);
    SB params = {0};
    if (in->has_this) {
        sb_puts(&params, "ptr %this");
        F.has_this = true;
        F.this_t = in->self;
        F.this_mut = fn->is_mut_fn || strcmp(fn->name, "Init") == 0;
        if (in->self->k == T_CLASS) {
            /* a class `this` is an object pointer; keep it in a slot so it can be used like a variable */
            F.this_slot = new_slot(&F, in->self);
            emit(&F, "store ptr %%this, ptr %s", F.this_slot);
        } else {
            F.this_slot = "%this";
        }
    }
    for (int i = 0; i < in->ptypes.n; i++) {
        Node *pn = fn->list.v[i];
        Type *pt = in->ptypes.v[i];
        if (params.len) sb_puts(&params, ", ");
        sb_printf(&params, "%s%s %%p%d", type_ll(pt), in->is_export ? c_ext_attr(pt) : "", i);
        Local *l = declare_local(&F, pn->name, pt, pn->flag, pn->pos);
        char *pv = zsprintf("%%p%d", i);
        Val v = {pt, pv};
        if (pt != ty_unit) store_to(&F, pt, v, l->ptr);
    }
    if (!fn->b) {
        diag_error(fn->pos, "function `%s` has no body", fn->name);
        return;
    }
    Val v = gen_block(&F, fn->b, in->ret);
    if (!F.term) {
        if (in->ret == ty_unit) {
            emit(&F, "ret void");
        } else if (in->ret == ty_never) {
            emit(&F, "unreachable");
        } else {
            Pos end = fn->b->pos;
            v = coerce(&F, end, v, in->ret, "the function result (the body must end with a value or `return`)");
            if (is_err(v.t)) emit(&F, "unreachable");
            else emit(&F, "ret %s %s", type_ll(in->ret), v.v);
        }
        F.term = true;
    }
    const char *rl = (in->ret == ty_unit || in->ret == ty_never) ? "void" : type_ll(in->ret);
    if (in->is_export && rl[0] != 'v') {
        const char *ra = c_ext_attr(in->ret);
        if (ra[0]) rl = zsprintf("%s %s", ra + 1, rl);
    }
    sb_printf(&g->funcs, "define %s @\"%s\"(%s) {\nentry:\n%s%s%s}\n\n", rl, in->name, sb_str(&params),
              sb_str(&F.allocas), sb_str(&F.code), sb_str(&F.cold));
}

static Decl *find_main(Gen *g) {
    Module *m = prog_find_module(g->pg, "Main");
    Decl *d = m ? module_find(m, "Main") : NULL;
    if (!d || d->node->kind != D_FN) {
        Pos none = {0};
        diag_error(none, "no entry point: a program needs `module Main` with a public `fn Main()`");
        return NULL;
    }
    return d;
}

void gen_set_test_mode(Gen *g) { g->test_mode = true; }
void gen_set_lib_mode(Gen *g) { g->lib_mode = true; }

bool gen_program(Gen *g, SB *out) {
    Decl *md = (g->test_mode || g->lib_mode) ? NULL : find_main(g);
    Inst *main_in = NULL, *args_in = NULL;
    if (md) {
        Node *fn = md->node;
        if (fn->list2.n) diag_error(fn->pos, "`Main` cannot have type parameters");
        else if (fn->list.n > 1) diag_error(fn->pos, "`Main` takes no parameters or one `args: Array<String>`");
        else {
            main_in = get_inst(g, md, NULL, NULL);
            if (fn->list.n == 1) {
                Module *env = prog_find_module(g->pg, "Zevic.Env");
                Decl *ra = env ? module_find(env, "RawArgs") : NULL;
                if (!ra) {
                    diag_error(fn->pos, "cannot find `Zevic.Env.RawArgs` for the arguments of `Main`");
                } else {
                    args_in = get_inst(g, ra, NULL, NULL);
                    if (strcmp(((Type *)main_in->ptypes.v[0])->name, args_in->ret->name) != 0)
                        diag_error(fn->pos, "the parameter of `Main` must be `args: Array<String>`");
                }
            }
        }
        if (main_in && main_in->ret != ty_unit && !is_err(main_in->ret))
            diag_error(fn->pos, "`Main` must not return a value (use `Env.Exit(code)` to set the exit code)");
    }
    /* check every non-generic function of the user's modules, even if unused */
    for (int i = 0; i < g->pg->modules.n; i++) {
        Module *m = g->pg->modules.v[i];
        if (m->is_std) continue;
        for (size_t k = 0; k < m->decls.cap; k++) {
            if (!m->decls.keys[k]) continue;
            Decl *d = m->decls.vals[k];
            if (d->node->kind == D_FN && !d->abi && d->node->list2.n == 0) get_inst(g, d, NULL, NULL);
            NodeKind dk = d->node->kind;
            if ((dk == D_STRUCT || dk == D_CLASS || dk == D_ENUM) && d->node->list2.n == 0) {
                /* check every method of a non-generic type, even one that is never called */
                Vec none = {0};
                Type *t = instantiate(g, d, none, d->node->pos);
                for (int j = 0; j < d->node->list.n; j++) {
                    Node *m = d->node->list.v[j];
                    if (m->kind == D_FN && m->list2.n == 0) get_inst(g, method_decl(t, m), t, NULL);
                }
            }
        }
    }
    Vec tests = {0}; /* Inst* of every test, in module order */
    Vec tnames = {0}; /* the string constant of each test's name (made before the constants are written out) */
    if (g->test_mode) {
        for (int i = 0; i < g->pg->modules.n; i++) {
            Module *m = g->pg->modules.v[i];
            if (m->is_std) continue;
            for (int k = 0; k < m->tests.n; k++) {
                Decl *td = m->tests.v[k];
                vec_push(&tests, get_inst(g, td, NULL, NULL));
                vec_push(&tnames, const_ptr(str_const(g, td->test_name, (int)strlen(td->test_name))));
            }
        }
    }
    for (int i = 0; i < g->worklist.n; i++) emit_function(g, g->worklist.v[i]);
    if (zv_error_count) return false;
    sb_puts(out, "; generated by zevic0\nsource_filename = \"zevic\"\n\n");
    sb_puts(out, sb_str(&g->globals));
    sb_puts(out, "\n");
    sb_puts(out, sb_str(&g->decls));
    sb_puts(out, "declare void @zevic_init(i32, ptr)\n\n");
    sb_puts(out, sb_str(&g->funcs));
    if (g->test_mode) {
        sb_puts(out, "declare void @zevic_test_add(ptr, i64, ptr)\ndeclare i32 @zevic_test_main()\n\n");
        sb_puts(out, "define i32 @main(i32 %argc, ptr %argv) {\n  call void @zevic_init(i32 %argc, ptr %argv)\n");
        for (int i = 0; i < tests.n; i++) {
            Inst *in = tests.v[i];
            const char *nm = in->decl->test_name;
            sb_printf(out, "  call void @zevic_test_add(ptr %s, i64 %d, ptr @\"%s\")\n", (char *)tnames.v[i], (int)strlen(nm), in->name);
        }
        sb_puts(out, "  %rc = call i32 @zevic_test_main()\n  ret i32 %rc\n}\n");
        return true;
    }
    if (g->lib_mode) return true;
    sb_puts(out, "define i32 @main(i32 %argc, ptr %argv) {\n  call void @zevic_init(i32 %argc, ptr %argv)\n");
    if (args_in)
        sb_printf(out, "  %%args = call %s @\"%s\"()\n  call void @\"%s\"(%s %%args)\n", type_ll(args_in->ret), args_in->name,
                  main_in->name, type_ll(args_in->ret));
    else
        sb_printf(out, "  call void @\"%s\"()\n", main_in->name);
    sb_puts(out, "  ret i32 0\n}\n");
    return true;
}

/* ---- the C header ---- */

static const char *c_spelling(Type *t) {
    static const char *tab[][2] = {
        {"CChar", "char"}, {"CSChar", "signed char"}, {"CUChar", "unsigned char"}, {"CShort", "short"},
        {"CUShort", "unsigned short"}, {"CInt", "int"}, {"CUInt", "unsigned int"}, {"CLong", "long"},
        {"CULong", "unsigned long"}, {"CLongLong", "long long"}, {"CULongLong", "unsigned long long"},
        {"CSizeT", "size_t"}, {"CSSizeT", "intptr_t"}, {"CPtrDiff", "ptrdiff_t"}, {"CIntPtr", "intptr_t"},
        {"CUIntPtr", "uintptr_t"}, {"CFloat", "float"}, {"CDouble", "double"}, {"CBool", "_Bool"},
        {"CString", "const char *"}, {"CVoid", "void"}, {NULL, NULL}};
    if (t == ty_unit) return "void";
    if (t->k == T_PTR) {
        const char *in = c_spelling(t->args.v[0]);
        return zsprintf("%s%s*", in, in[strlen(in) - 1] == '*' ? "" : " ");
    }
    if (t->k == T_STRUCT) return zsprintf("struct %s", t->decl->name);
    for (int i = 0; tab[i][0]; i++)
        if (strcmp(tab[i][0], t->name) == 0) return tab[i][1];
    return "int";
}

static void header_struct(Gen *g, Type *t, Dict *seen, SB *out) {
    if (t->k == T_PTR) {
        header_struct(g, t->args.v[0], seen, out);
        return;
    }
    if (t->k != T_STRUCT || dict_get(seen, t->name)) return;
    dict_put(seen, t->name, t);
    ensure_members(g, t);
    for (int i = 0; i < t->fields.n; i++) header_struct(g, ((Field *)t->fields.v[i])->t, seen, out);
    sb_printf(out, "struct %s {\n", t->decl->name);
    for (int i = 0; i < t->fields.n; i++) {
        Field *f = t->fields.v[i];
        sb_printf(out, "    %s %s;\n", c_spelling(f->t), f->name);
    }
    sb_puts(out, "};\n\n");
}

void gen_header(Gen *g, const char *guard, SB *out) {
    sb_printf(out, "/* generated by zevic0: the `export \"C\"` functions of this program */\n#ifndef %s\n#define %s\n\n", guard, guard);
    sb_puts(out, "#include <stddef.h>\n#include <stdint.h>\n\n#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n");
    if (g->lib_mode)
        sb_puts(out, "/* Call this once before any other function of the library. */\nvoid zevic_init(int argc, char **argv);\n\n");
    Dict seen = {0};
    for (int i = 0; i < g->exports.n; i++) {
        Inst *in = g->exports.v[i];
        for (int k = 0; k < in->ptypes.n; k++) header_struct(g, in->ptypes.v[k], &seen, out);
        header_struct(g, in->ret, &seen, out);
    }
    for (int i = 0; i < g->exports.n; i++) {
        Inst *in = g->exports.v[i];
        sb_printf(out, "%s %s(", c_spelling(in->ret), in->name);
        if (in->ptypes.n == 0) sb_puts(out, "void");
        for (int k = 0; k < in->ptypes.n; k++) {
            Node *pn = in->decl->node->list.v[k];
            sb_printf(out, "%s%s %s", k ? ", " : "", c_spelling(in->ptypes.v[k]), pn->name);
        }
        sb_puts(out, ");\n");
    }
    sb_puts(out, "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n");
}
