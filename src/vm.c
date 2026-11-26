#include "uniland.h"

#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OP_HALT, OP_CONST, OP_POP, OP_GETVAR, OP_SETVAR, OP_DEFVAR, OP_CLOSURE,
    OP_ECHO, OP_TEMPLATE, OP_LIST, OP_DICT, OP_INDEX_GET, OP_INDEX_SET,
    OP_MEMBER_GET, OP_MEMBER_SET, OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_POW, OP_EQ, OP_NEQ, OP_CMP, OP_NEG, OP_NOT, OP_INCR, OP_COMPOUND,
    OP_AND, OP_OR, OP_JMP, OP_JMP_F, OP_RET, OP_TRY, OP_TEND, OP_CEND,
    OP_FEND, OP_PYBLOCK, OP_GOBLOCK, OP_THROW, OP_BREAK, OP_CONTINUE,
    OP_FORIN_START, OP_FORIN_NEXT, OP_FORIN_END, OP_BLOCK_ENTER,
    OP_BLOCK_EXIT, OP_LOOP_PUSH, OP_LOOP_POP, OP_CALL, OP_PUTREG, OP_GETREG,
    OP_IMPORT
} VOp;

typedef struct VmMeta {
    int catch_pc;
    int finally_pc;
    int after_pc;
    const char *catch_var;
} VmMeta;

typedef struct VmIns {
    unsigned char op;
    unsigned short a;
    unsigned short b;
    int line;
    VmMeta *meta;
} VmIns;

typedef struct VmCode {
    char *name;
    char **params;
    int nparams;
    Value *consts;
    int nconsts;
    int cconsts;
    struct VmCode **codes;
    int ncodes;
    int ccodes;
    VmIns *ins;
    int nins;
    int cins;
    int regn;
    Block *body;
} VmCode;

typedef struct {
    VmCode *c;
    int nregs;
} Vmc;

static jmp_buf g_jmp;
static int g_trace_on = 0;

static const char *const k_arith[6] = {"+", "-", "*", "/", "%", "^"};
static const char *const k_cmp[4] = {"<", ">", "<=", ">="};
static const char *const k_compound[6] = {"+", "-", "*", "/", "%", "^"};

static void vm_fail(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    err_msg(buf);
    longjmp(g_jmp, 1);
}

static VmCode *code_new(const char *name, char **params, int nparams, Block *body) {
    VmCode *c = xmalloc(sizeof(VmCode));
    c->name = xstrdup(name ? name : "<anonymous>");
    c->params = params;
    c->nparams = nparams;
    c->consts = NULL;
    c->nconsts = 0;
    c->cconsts = 0;
    c->codes = NULL;
    c->ncodes = 0;
    c->ccodes = 0;
    c->ins = NULL;
    c->nins = 0;
    c->cins = 0;
    c->regn = 0;
    c->body = body;
    return c;
}

static int code_add_const(VmCode *c, Value v, int dedup) {
    if (dedup) {
        for (int i = 0; i < c->nconsts; i++)
            if (c->consts[i].t == v.t && value_equal(c->consts[i], v)) return i;
    }
    if (c->nconsts == c->cconsts) {
        c->cconsts = c->cconsts ? c->cconsts * 2 : 16;
        c->consts = xrealloc(c->consts, sizeof(Value) * (size_t)c->cconsts);
    }
    c->consts[c->nconsts] = v;
    c->nconsts++;
    if (c->nconsts > 60000) vm_fail("vm: константный пул переполнен (%s)", c->name);
    return c->nconsts - 1;
}

static int code_add_sub(VmCode *c, VmCode *sub) {
    if (c->ncodes == c->ccodes) {
        c->ccodes = c->ccodes ? c->ccodes * 2 : 8;
        c->codes = xrealloc(c->codes, sizeof(VmCode *) * (size_t)c->ccodes);
    }
    c->codes[c->ncodes] = sub;
    c->ncodes++;
    return c->ncodes - 1;
}

static int emit(Vmc *k, int op, int a, int b, int line, VmMeta *meta) {
    VmCode *c = k->c;
    if (c->nins == c->cins) {
        c->cins = c->cins ? c->cins * 2 : 64;
        c->ins = xrealloc(c->ins, sizeof(VmIns) * (size_t)c->cins);
    }
    VmIns *in = &c->ins[c->nins];
    in->op = (unsigned char)op;
    in->a = (unsigned short)a;
    in->b = (unsigned short)b;
    in->line = line;
    in->meta = meta;
    c->nins++;
    if (c->nins > 0xFFFF) vm_fail("vm: тело %s слишком большое", c->name);
    return c->nins - 1;
}

static int c_str(Vmc *k, const char *s) { return code_add_const(k->c, v_str(s), 1); }

static int c_val(Vmc *k, Value v) { return code_add_const(k->c, v, 1); }

static int c_raw(Vmc *k, Value v) { return code_add_const(k->c, v, 0); }

static void patch(Vmc *k, int pc, int target) {
    if (target < 0 || target > 0xFFFF) vm_fail("vm: переход за пределы кода (%s)", k->c->name);
    k->c->ins[pc].a = (unsigned short)target;
}

static int alloc_reg(Vmc *k) {
    k->nregs++;
    if (k->nregs > 0xFFF0) vm_fail("vm: переполнение регистров");
    if (k->c->regn < k->nregs) k->c->regn = k->nregs;
    return k->nregs - 1;
}

static void c_stmt(Vmc *k, Node *n, int top);
static void c_expr(Vmc *k, Node *n);
static void c_block(Vmc *k, Block *b);

static int block_needs_env(Block *b) {
    for (int i = 0; i < b->n; i++) {
        Node *s = b->stmts[i];
        if (!s) continue;
        if (s->t == N_VARDECL || s->t == N_FUNCDECL || s->t == N_IMPORT) return 1;
    }
    return 0;
}

static void c_block(Vmc *k, Block *b) {
    if (!b) return;
    int env = block_needs_env(b);
    if (env) emit(k, OP_BLOCK_ENTER, 0, 0, b->line, NULL);
    for (int i = 0; i < b->n; i++) c_stmt(k, b->stmts[i], 0);
    if (env) emit(k, OP_BLOCK_EXIT, 0, 0, b->line, NULL);
}

static VmCode *c_func(const char *name, char **params, int nparams, Block *body) {
    VmCode *sub = code_new(name, params, nparams, body);
    Vmc k = {sub, 0};
    c_block(&k, body);
    int line = body ? body->line : 0;
    emit(&k, OP_CONST, c_val(&k, v_null()), 0, line, NULL);
    emit(&k, OP_RET, 0, 0, line, NULL);
    sub->regn = k.nregs;
    return sub;
}

static void c_binary(Vmc *k, const char *op, int line) {
    if (strcmp(op, "+") == 0) { emit(k, OP_ADD, 0, 0, line, NULL); return; }
    if (strcmp(op, "-") == 0) { emit(k, OP_SUB, 0, 0, line, NULL); return; }
    if (strcmp(op, "*") == 0) { emit(k, OP_MUL, 0, 0, line, NULL); return; }
    if (strcmp(op, "/") == 0) { emit(k, OP_DIV, 0, 0, line, NULL); return; }
    if (strcmp(op, "%") == 0) { emit(k, OP_MOD, 0, 0, line, NULL); return; }
    if (strcmp(op, "^") == 0) { emit(k, OP_POW, 0, 0, line, NULL); return; }
    if (strcmp(op, "==") == 0) { emit(k, OP_EQ, 0, 0, line, NULL); return; }
    if (strcmp(op, "!=") == 0) { emit(k, OP_NEQ, 0, 0, line, NULL); return; }
    if (strcmp(op, "<") == 0) { emit(k, OP_CMP, 0, 0, line, NULL); return; }
    if (strcmp(op, ">") == 0) { emit(k, OP_CMP, 1, 0, line, NULL); return; }
    if (strcmp(op, "<=") == 0) { emit(k, OP_CMP, 2, 0, line, NULL); return; }
    if (strcmp(op, ">=") == 0) { emit(k, OP_CMP, 3, 0, line, NULL); return; }
    vm_fail("vm: неизвестный оператор %s", op);
}

static int compound_index(const char *op) {
    if (strcmp(op, "+=") == 0) return 0;
    if (strcmp(op, "-=") == 0) return 1;
    if (strcmp(op, "*=") == 0) return 2;
    if (strcmp(op, "/=") == 0) return 3;
    if (strcmp(op, "%=") == 0) return 4;
    if (strcmp(op, "^=") == 0) return 5;
    vm_fail("vm: неизвестный compound-op %s", op);
    return 0;
}

static void c_assign(Vmc *k, Node *n) {
    const char *op = n->u.assign.op;
    Node *target = n->u.assign.target;
    int line = n->line;
    if (strcmp(op, "++") == 0 || strcmp(op, "--") == 0) {
        int inc = strcmp(op, "++") == 0 ? 1 : 0;
        if (target->t == N_IDENT) {
            int name = c_str(k, target->u.ident.name);
            emit(k, OP_GETVAR, name, 0, line, NULL);
            emit(k, OP_INCR, inc, 0, line, NULL);
            emit(k, OP_SETVAR, name, 0, line, NULL);
            return;
        }
        if (target->t == N_INDEX) {
            c_expr(k, target->u.index.obj);
            c_expr(k, target->u.index.index);
            emit(k, OP_INDEX_GET, 0, 0, line, NULL);
            emit(k, OP_INCR, inc, 0, line, NULL);
            c_expr(k, target->u.index.obj);
            c_expr(k, target->u.index.index);
            emit(k, OP_INDEX_SET, 0, 0, line, NULL);
            return;
        }
        if (target->t == N_MEMBER) {
            int name = c_str(k, target->u.member.name);
            c_expr(k, target->u.member.obj);
            emit(k, OP_MEMBER_GET, name, 0, line, NULL);
            emit(k, OP_INCR, inc, 0, line, NULL);
            c_expr(k, target->u.member.obj);
            emit(k, OP_MEMBER_SET, name, 0, line, NULL);
            return;
        }
        vm_fail("vm: ++/-- для этого выражения не поддерживается");
    }
    if (strcmp(op, "=") == 0) {
        if (target->t == N_IDENT) {
            c_expr(k, n->u.assign.value);
            emit(k, OP_SETVAR, c_str(k, target->u.ident.name), 0, line, NULL);
            return;
        }
        if (target->t == N_INDEX) {
            c_expr(k, n->u.assign.value);
            c_expr(k, target->u.index.obj);
            c_expr(k, target->u.index.index);
            emit(k, OP_INDEX_SET, 0, 0, line, NULL);
            return;
        }
        if (target->t == N_MEMBER) {
            int name = c_str(k, target->u.member.name);
            c_expr(k, n->u.assign.value);
            c_expr(k, target->u.member.obj);
            emit(k, OP_MEMBER_SET, name, 0, line, NULL);
            return;
        }
        vm_fail("vm: неверная цель присваивания");
    }
    int opidx = compound_index(op);
    if (target->t == N_IDENT) {
        int name = c_str(k, target->u.ident.name);
        emit(k, OP_GETVAR, name, 0, line, NULL);
        c_expr(k, n->u.assign.value);
        emit(k, OP_COMPOUND, opidx, 0, line, NULL);
        emit(k, OP_SETVAR, name, 0, line, NULL);
        return;
    }
    if (target->t == N_INDEX) {
        c_expr(k, target->u.index.obj);
        c_expr(k, target->u.index.index);
        emit(k, OP_INDEX_GET, 0, 0, line, NULL);
        int reg = alloc_reg(k);
        emit(k, OP_PUTREG, reg, 0, line, NULL);
        emit(k, OP_GETREG, reg, 0, line, NULL);
        c_expr(k, n->u.assign.value);
        emit(k, OP_COMPOUND, opidx, 0, line, NULL);
        c_expr(k, target->u.index.obj);
        c_expr(k, target->u.index.index);
        emit(k, OP_INDEX_SET, 0, 0, line, NULL);
        return;
    }
    if (target->t == N_MEMBER) {
        int name = c_str(k, target->u.member.name);
        c_expr(k, target->u.member.obj);
        emit(k, OP_MEMBER_GET, name, 0, line, NULL);
        int reg = alloc_reg(k);
        emit(k, OP_PUTREG, reg, 0, line, NULL);
        emit(k, OP_GETREG, reg, 0, line, NULL);
        c_expr(k, n->u.assign.value);
        emit(k, OP_COMPOUND, opidx, 0, line, NULL);
        c_expr(k, target->u.member.obj);
        emit(k, OP_MEMBER_SET, name, 0, line, NULL);
        return;
    }
    vm_fail("vm: неверная цель присваивания");
}

static void c_try(Vmc *k, Node *n) {
    VmMeta *meta = xmalloc(sizeof(VmMeta));
    meta->catch_pc = -1;
    meta->finally_pc = -1;
    meta->after_pc = -1;
    meta->catch_var = n->u.tc.catch_var;
    emit(k, OP_TRY, 0, 0, n->line, meta);
    c_block(k, n->u.tc.try_b);
    emit(k, OP_TEND, 0, 0, n->line, meta);
    if (n->u.tc.catch_b) {
        meta->catch_pc = k->c->nins;
        c_block(k, n->u.tc.catch_b);
        emit(k, OP_CEND, 0, 0, n->line, meta);
    }
    if (n->u.tc.finally_b) {
        meta->finally_pc = k->c->nins;
        c_block(k, n->u.tc.finally_b);
        emit(k, OP_FEND, 0, 0, n->line, meta);
    }
    meta->after_pc = k->c->nins;
}

static void c_stmt(Vmc *k, Node *n, int top) {
    if (!n) return;
    switch (n->t) {
    case N_BLOCK:
        c_block(k, &n->u.block);
        return;

    case N_VARDECL:
        c_expr(k, n->u.var.value);
        emit(k, OP_DEFVAR, c_str(k, n->u.var.name), n->u.var.is_const ? 1 : 0, n->line, NULL);
        return;

    case N_FUNCDECL: {
        VmCode *sub = c_func(n->u.funcdecl.name, n->u.funcdecl.params, n->u.funcdecl.nparams,
                             n->u.funcdecl.body);
        int idx = code_add_sub(k->c, sub);
        emit(k, OP_CLOSURE, idx, 0, n->line, NULL);
        emit(k, OP_DEFVAR, c_str(k, n->u.funcdecl.name), 0, n->line, NULL);
        return;
    }

    case N_RETURN:
        if (!n->u.ret.value) emit(k, OP_CONST, c_val(k, v_null()), 0, n->line, NULL);
        else c_expr(k, n->u.ret.value);
        emit(k, OP_RET, 0, 0, n->line, NULL);
        return;

    case N_IF: {
        c_expr(k, n->u.ifs.cond);
        int jf = emit(k, OP_JMP_F, 0, 0, n->line, NULL);
        c_block(k, n->u.ifs.then_b);
        if (n->u.ifs.else_b) {
            int jmp = emit(k, OP_JMP, 0, 0, n->line, NULL);
            patch(k, jf, k->c->nins);
            c_stmt(k, n->u.ifs.else_b, 0);
            patch(k, jmp, k->c->nins);
        } else {
            patch(k, jf, k->c->nins);
        }
        return;
    }

    case N_FOR: {
        if (n->u.fors.init) c_stmt(k, n->u.fors.init, 0);
        int push = emit(k, OP_LOOP_PUSH, 0, 0, n->line, NULL);
        int top_pc = k->c->nins;
        if (n->u.fors.cond) c_expr(k, n->u.fors.cond);
        else emit(k, OP_CONST, c_val(k, v_bool(1)), 0, n->line, NULL);
        int jf = emit(k, OP_JMP_F, 0, 0, n->line, NULL);
        c_block(k, n->u.fors.body);
        int cont = top_pc;
        if (n->u.fors.update) {
            cont = k->c->nins;
            c_stmt(k, n->u.fors.update, 0);
        }
        emit(k, OP_JMP, top_pc, 0, n->line, NULL);
        int end = k->c->nins;
        k->c->ins[push].a = (unsigned short)end;
        k->c->ins[push].b = (unsigned short)cont;
        patch(k, jf, end);
        emit(k, OP_LOOP_POP, 0, 0, n->line, NULL);
        return;
    }

    case N_WHILE: {
        int push = emit(k, OP_LOOP_PUSH, 0, 0, n->line, NULL);
        int top_pc = k->c->nins;
        c_expr(k, n->u.whiles.cond);
        int jf = emit(k, OP_JMP_F, 0, 0, n->line, NULL);
        c_block(k, n->u.whiles.body);
        emit(k, OP_JMP, top_pc, 0, n->line, NULL);
        int end = k->c->nins;
        k->c->ins[push].a = (unsigned short)end;
        k->c->ins[push].b = (unsigned short)top_pc;
        patch(k, jf, end);
        emit(k, OP_LOOP_POP, 0, 0, n->line, NULL);
        return;
    }

    case N_FORIN: {
        c_expr(k, n->u.forin.iterable);
        emit(k, OP_FORIN_START, 0, 0, n->line, NULL);
        int push = emit(k, OP_LOOP_PUSH, 0, 0, n->line, NULL);
        int next_pc = k->c->nins;
        int fin = emit(k, OP_FORIN_NEXT, 0, 0, n->line, NULL);
        emit(k, OP_BLOCK_ENTER, 0, 0, n->line, NULL);
        emit(k, OP_DEFVAR, c_str(k, n->u.forin.var), 0, n->line, NULL);
        c_block(k, n->u.forin.body);
        emit(k, OP_BLOCK_EXIT, 0, 0, n->line, NULL);
        emit(k, OP_JMP, next_pc, 0, n->line, NULL);
        int end = k->c->nins;
        k->c->ins[push].a = (unsigned short)end;
        k->c->ins[push].b = (unsigned short)next_pc;
        patch(k, fin, end);
        emit(k, OP_LOOP_POP, 0, 0, n->line, NULL);
        emit(k, OP_FORIN_END, 0, 0, n->line, NULL);
        return;
    }

    case N_BREAK:
        emit(k, OP_BREAK, 0, 0, n->line, NULL);
        return;

    case N_CONTINUE:
        emit(k, OP_CONTINUE, 0, 0, n->line, NULL);
        return;

    case N_ASSIGN:
        c_assign(k, n);
        return;

    case N_EXPRSTMT:
        c_expr(k, n->u.exprstmt.expr);
        emit(k, top ? OP_ECHO : OP_POP, 0, 0, n->line, NULL);
        return;

    case N_PYBLOCK:
        emit(k, OP_PYBLOCK, c_str(k, n->u.bridge.code), 0, n->line, NULL);
        return;

    case N_GOBLOCK:
        emit(k, OP_GOBLOCK, c_str(k, n->u.bridge.code), 0, n->line, NULL);
        return;

    case N_TRYCATCH:
        c_try(k, n);
        return;

    case N_THROW:
        c_expr(k, n->u.thr.value);
        emit(k, OP_THROW, 0, 0, n->line, NULL);
        return;

    case N_IMPORT: {
        int path = c_str(k, n->u.imports.path);
        int alias = n->u.imports.alias ? c_str(k, n->u.imports.alias) : 0xFFFF;
        emit(k, OP_IMPORT, path, alias, n->line, NULL);
        return;
    }

    default:
        vm_fail("vm: не умею компилировать оператор (тип %d)", (int)n->t);
    }
}

static void c_expr(Vmc *k, Node *n) {
    if (!n) {
        emit(k, OP_CONST, c_val(k, v_null()), 0, 0, NULL);
        return;
    }
    switch (n->t) {
    case N_LITERAL:
        emit(k, OP_CONST, c_val(k, n->u.literal.value), 0, n->line, NULL);
        return;

    case N_TEMPLATE:
        for (int i = 0; i < n->u.tpl.nparts; i++) {
            if (n->u.tpl.parts[i].expr) c_expr(k, n->u.tpl.parts[i].expr);
            else emit(k, OP_CONST, c_val(k, v_str(n->u.tpl.parts[i].str)), 0, n->line, NULL);
        }
        emit(k, OP_TEMPLATE, n->u.tpl.nparts, 0, n->line, NULL);
        return;

    case N_IDENT:
        emit(k, OP_GETVAR, c_str(k, n->u.ident.name), 0, n->line, NULL);
        return;

    case N_BINARY: {
        const char *op = n->u.binary.op;
        if (strcmp(op, "&&") == 0) {
            c_expr(k, n->u.binary.left);
            int j = emit(k, OP_AND, 0, 0, n->line, NULL);
            c_expr(k, n->u.binary.right);
            patch(k, j, k->c->nins);
            return;
        }
        if (strcmp(op, "||") == 0) {
            c_expr(k, n->u.binary.left);
            int j = emit(k, OP_OR, 0, 0, n->line, NULL);
            c_expr(k, n->u.binary.right);
            patch(k, j, k->c->nins);
            return;
        }
        c_expr(k, n->u.binary.left);
        c_expr(k, n->u.binary.right);
        c_binary(k, op, n->line);
        return;
    }

    case N_UNARY:
        c_expr(k, n->u.unary.expr);
        if (strcmp(n->u.unary.op, "-") == 0) emit(k, OP_NEG, 0, 0, n->line, NULL);
        else if (strcmp(n->u.unary.op, "!") == 0) emit(k, OP_NOT, 0, 0, n->line, NULL);
        else vm_fail("vm: неизвестный унарный оператор %s", n->u.unary.op);
        return;

    case N_INDEX:
        c_expr(k, n->u.index.obj);
        c_expr(k, n->u.index.index);
        emit(k, OP_INDEX_GET, 0, 0, n->line, NULL);
        return;

    case N_MEMBER:
        c_expr(k, n->u.member.obj);
        emit(k, OP_MEMBER_GET, c_str(k, n->u.member.name), 0, n->line, NULL);
        return;

    case N_CALL:
        c_expr(k, n->u.call.callee);
        for (int i = 0; i < n->u.call.nargs; i++) c_expr(k, n->u.call.args[i]);
        emit(k, OP_CALL, n->u.call.nargs, 0, n->line, NULL);
        return;

    case N_FUNCEXPR: {
        VmCode *sub = c_func("<anonymous>", n->u.funcexpr.params, n->u.funcexpr.nparams,
                             n->u.funcexpr.body);
        emit(k, OP_CLOSURE, code_add_sub(k->c, sub), 0, n->line, NULL);
        return;
    }

    case N_ARRAY:
        for (int i = 0; i < n->u.arr.n; i++) c_expr(k, n->u.arr.elems[i]);
        emit(k, OP_LIST, n->u.arr.n, 0, n->line, NULL);
        return;

    case N_OBJECT: {
        int base = k->c->nconsts;
        for (int i = 0; i < n->u.obj.n; i++) c_raw(k, v_str(n->u.obj.pairs[i].key));
        for (int i = 0; i < n->u.obj.n; i++) c_expr(k, n->u.obj.pairs[i].value);
        emit(k, OP_DICT, n->u.obj.n, base, n->line, NULL);
        return;
    }

    default:
        vm_fail("vm: не умею компилировать выражение (тип %d)", (int)n->t);
    }
}

static VmCode *compile_program(Node *prog) {
    if (setjmp(g_jmp)) return NULL;
    VmCode *c = code_new("main", NULL, 0, NULL);
    Vmc k = {c, 0};
    for (int i = 0; i < prog->u.program.n; i++) c_stmt(&k, prog->u.program.stmts[i], 0);
    emit(&k, OP_HALT, 0, 0, 0, NULL);
    c->regn = k.nregs;
    return c;
}

typedef struct {
    Value *items;
    int n;
    int pos;
} VmIter;

typedef struct {
    Env *saved_env;
    int env_depth;
    int end_pc;
    int cont_pc;
} VmLoop;

typedef struct {
    VmMeta *meta;
    Env *saved_env;
    int env_depth;
    int stack_len;
    int in_catch;
} VmTry;

typedef struct {
    int kind;
    Value value;
    UErr err;
} Sig;

typedef struct {
    VmCode *code;
    int ip;
    Env *env;
    Value *st;
    int sp;
    int scap;
    Value st_inline[64];
    Value *regs;
    Value regs_inline[8];
    int *vc_bi;
    Env **vc_env;
    int nvc;
    Env **envs;
    int nenvs;
    int cenvs;
    VmLoop *loops;
    int nloops;
    int cloops;
    VmTry *tries;
    int ntries;
    int ctries;
    VmIter *iters;
    int niters;
    int citers;
    Sig pending;
    int has_pending;
} Frame;

static void frame_init(Frame *f, VmCode *code, Env *env) {
    memset(f, 0, sizeof(*f));
    f->code = code;
    f->env = env;
    f->st = f->st_inline;
    f->scap = 64;
    f->regs = code->regn <= 8 ? f->regs_inline : xmalloc(sizeof(Value) * (size_t)code->regn);
    for (int i = 0; i < code->regn; i++) f->regs[i] = v_null();
    f->nvc = code->nconsts;
    f->vc_bi = xmalloc(sizeof(int) * (size_t)(f->nvc ? f->nvc : 1));
    f->vc_env = xmalloc(sizeof(Env *) * (size_t)(f->nvc ? f->nvc : 1));
    for (int i = 0; i < f->nvc; i++) {
        f->vc_bi[i] = -1;
        f->vc_env[i] = NULL;
    }
}

static void frame_free(Frame *f) {
    if (f->st != f->st_inline) free(f->st);
    if (f->regs != f->regs_inline) free(f->regs);
    free(f->vc_bi);
    free(f->vc_env);
    free(f->envs);
    free(f->loops);
    free(f->tries);
    free(f->iters);
}

static void f_push(Frame *f, Value v) {
    if (f->sp == f->scap) {
        int ncap = f->scap * 2;
        if (f->st == f->st_inline) {
            Value *n = xmalloc(sizeof(Value) * (size_t)ncap);
            memcpy(n, f->st, sizeof(Value) * (size_t)f->sp);
            f->st = n;
        } else {
            f->st = xrealloc(f->st, sizeof(Value) * (size_t)ncap);
        }
        f->scap = ncap;
    }
    f->st[f->sp++] = v;
}

static Value f_pop(Frame *f) {
    if (f->sp <= 0) return v_null();
    return f->st[--f->sp];
}

static void f_env_push(Frame *f, Env *e) {
    if (f->nenvs == f->cenvs) {
        f->cenvs = f->cenvs ? f->cenvs * 2 : 16;
        f->envs = xrealloc(f->envs, sizeof(Env *) * (size_t)f->cenvs);
    }
    f->envs[f->nenvs++] = e;
}

static void f_loop_push(Frame *f, VmLoop l) {
    if (f->nloops == f->cloops) {
        f->cloops = f->cloops ? f->cloops * 2 : 8;
        f->loops = xrealloc(f->loops, sizeof(VmLoop) * (size_t)f->cloops);
    }
    f->loops[f->nloops++] = l;
}

static void f_try_push(Frame *f, VmTry t) {
    if (f->ntries == f->ctries) {
        f->ctries = f->ctries ? f->ctries * 2 : 8;
        f->tries = xrealloc(f->tries, sizeof(VmTry) * (size_t)f->ctries);
    }
    f->tries[f->ntries++] = t;
}

static int f_iter_add(Frame *f, Value *items, int n) {
    if (f->niters == f->citers) {
        f->citers = f->citers ? f->citers * 2 : 8;
        f->iters = xrealloc(f->iters, sizeof(VmIter) * (size_t)f->citers);
    }
    f->iters[f->niters].items = items;
    f->iters[f->niters].n = n;
    f->iters[f->niters].pos = 0;
    return f->niters++;
}

static Value call_any(Value fn, int argc, Value *argv, int line);

static int run_frame(Frame *f, Value *out);

static int handle_sig(Frame *f, Sig *s, Value *out) {
    while (f->ntries > 0) {
        VmTry *st = &f->tries[f->ntries - 1];
        if (!st->in_catch && s->kind == 1 && st->meta->catch_pc >= 0) {
            f->sp = st->stack_len;
            f->nenvs = st->env_depth;
            f->env = env_public_new(st->saved_env);
            if (st->meta->catch_var)
                env_public_define(f->env, st->meta->catch_var, v_str(s->err.msg), 0);
            st->in_catch = 1;
            f->ip = st->meta->catch_pc;
            return 1;
        }
        f->ntries--;
        f->sp = st->stack_len;
        f->nenvs = st->env_depth;
        f->env = st->saved_env;
        if (st->meta->finally_pc >= 0) {
            f->pending = *s;
            f->has_pending = 1;
            f->ip = st->meta->finally_pc;
            return 1;
        }
    }
    if ((s->kind == 3 || s->kind == 4) && f->nloops > 0) {
        VmLoop *t = &f->loops[f->nloops - 1];
        f->env = t->saved_env;
        f->nenvs = t->env_depth;
        f->ip = s->kind == 3 ? t->end_pc : t->cont_pc;
        return 1;
    }
    if (s->kind == 2) {
        *out = s->value;
        return 3;
    }
    if (s->kind == 3) {
        err_msg("break вне цикла");
        return 2;
    }
    if (s->kind == 4) {
        err_msg("continue вне цикла");
        return 2;
    }
    g_err = s->err;
    return 2;
}

static Value str_of(Value v) { return value_str(v); }

static int run_frame(Frame *f, Value *out) {
    VmCode *code = f->code;
    for (;;) {
        VmIns *in = &code->ins[f->ip];
        int jumped = 0;
        if (g_trace_on && in->line > 0) fprintf(stderr, "[trace] строка %d\n", in->line);
        Sig sig;
        sig.kind = 0;
        sig.value = v_null();

        switch ((VOp)in->op) {
        case OP_HALT:
            *out = v_null();
            return 0;

        case OP_CONST:
            f_push(f, code->consts[in->a]);
            break;

        case OP_POP:
            (void)f_pop(f);
            break;

        case OP_GETVAR: {
            int ci = in->a;
            const char *name = code->consts[ci].as.s->s;
            if (f->vc_env[ci] == f->env && f->vc_bi[ci] >= 0) {
                f_push(f, env_public_index_value(f->env, f->vc_bi[ci]));
                break;
            }
            int bi = env_public_index(f->env, name);
            if (bi >= 0) {
                f->vc_bi[ci] = bi;
                f->vc_env[ci] = f->env;
                f_push(f, env_public_index_value(f->env, bi));
            } else {
                f_push(f, env_public_get(f->env, name));
            }
            break;
        }

        case OP_SETVAR: {
            int ci = in->a;
            const char *name = code->consts[ci].as.s->s;
            Value val = f_pop(f);
            if (f->vc_env[ci] == f->env && f->vc_bi[ci] >= 0) {
                env_public_index_assign(f->env, f->vc_bi[ci], val);
                break;
            }
            env_public_assign(f->env, name, val);
            if (!g_err.set) {
                int bi = env_public_index(f->env, name);
                if (bi >= 0) {
                    f->vc_bi[ci] = bi;
                    f->vc_env[ci] = f->env;
                }
            }
            break;
        }

        case OP_DEFVAR:
            env_public_define(f->env, code->consts[in->a].as.s->s, f_pop(f), in->b == 1);
            break;

        case OP_CLOSURE: {
            VmCode *sub = code->codes[in->a];
            Func *g = xmalloc(sizeof(Func));
            g->name = sub->name;
            g->nparams = sub->nparams;
            g->params = sub->params;
            g->body = sub->body;
            g->closure = f->env;
            g->native = NULL;
            g->native_ctx = NULL;
            g->data = sub;
            f_push(f, v_func(g));
            break;
        }

        case OP_ECHO: {
            Value v = f_pop(f);
            if (v.t != V_NULL) {
                Value s = str_of(v);
                fprintf(g_it.out, "%s\n", s.as.s->s);
            }
            break;
        }

        case OP_TEMPLATE: {
            int n = in->a;
            int start = f->sp - n;
            SB b;
            sb_init(&b);
            for (int i = start; i < f->sp; i++) {
                Value s = str_of(f->st[i]);
                sb_add(&b, s.as.s->s, s.as.s->len);
            }
            f->sp = start;
            f_push(f, v_str_take(b.buf, b.len));
            break;
        }

        case OP_LIST: {
            int n = in->a;
            int start = f->sp - n;
            Value l = v_list();
            for (int i = 0; i < n; i++) list_push(l.as.l, f->st[start + i]);
            f->sp = start;
            f_push(f, l);
            break;
        }

        case OP_DICT: {
            int n = in->a;
            int base = in->b;
            int start = f->sp - n;
            Value m = v_map();
            for (int i = 0; i < n; i++)
                map_set(m.as.m, code->consts[base + i].as.s->s, f->st[start + i]);
            f->sp = start;
            f_push(f, m);
            break;
        }

        case OP_INDEX_GET: {
            Value idx = f_pop(f);
            Value obj = f_pop(f);
            f_push(f, vm_index_get(obj, idx, in->line));
            break;
        }

        case OP_INDEX_SET: {
            Value idx = f_pop(f);
            Value obj = f_pop(f);
            Value v = f_pop(f);
            vm_index_set(obj, idx, v, in->line);
            break;
        }

        case OP_MEMBER_GET: {
            Value obj = f_pop(f);
            f_push(f, vm_member_get(obj, code->consts[in->a].as.s->s, in->line));
            break;
        }

        case OP_MEMBER_SET: {
            Value obj = f_pop(f);
            Value v = f_pop(f);
            vm_member_set(obj, code->consts[in->a].as.s->s, v, in->line);
            break;
        }

        case OP_ADD:
        case OP_SUB:
        case OP_MUL:
        case OP_DIV:
        case OP_MOD:
        case OP_POW: {
            Value rhs = f_pop(f);
            Value l = f_pop(f);
            const char *op = k_arith[in->op - OP_ADD];
            f_push(f, vm_apply_binary(op, l, rhs, in->line));
            break;
        }

        case OP_EQ: {
            Value rhs = f_pop(f);
            Value l = f_pop(f);
            f_push(f, v_bool(value_equal(l, rhs)));
            break;
        }

        case OP_NEQ: {
            Value rhs = f_pop(f);
            Value l = f_pop(f);
            f_push(f, v_bool(!value_equal(l, rhs)));
            break;
        }

        case OP_CMP: {
            Value rhs = f_pop(f);
            Value l = f_pop(f);
            f_push(f, vm_apply_binary(k_cmp[in->a], l, rhs, in->line));
            break;
        }

        case OP_NEG:
            f_push(f, vm_negate(f_pop(f)));
            break;

        case OP_NOT:
            f_push(f, v_bool(!value_truthy(f_pop(f))));
            break;

        case OP_INCR: {
            Value v = f_pop(f);
            f_push(f, vm_add_step(v, in->a == 0 ? -1 : 1));
            break;
        }

        case OP_COMPOUND: {
            Value rhs = f_pop(f);
            Value cur = f_pop(f);
            f_push(f, vm_apply_binary(k_compound[in->a], cur, rhs, in->line));
            break;
        }

        case OP_AND: {
            Value v = f_pop(f);
            if (!value_truthy(v)) {
                f_push(f, v);
                f->ip = in->a;
                jumped = 1;
            }
            break;
        }

        case OP_OR: {
            Value v = f_pop(f);
            if (value_truthy(v)) {
                f_push(f, v);
                f->ip = in->a;
                jumped = 1;
            }
            break;
        }

        case OP_JMP:
            f->ip = in->a;
            jumped = 1;
            break;

        case OP_JMP_F:
            if (!value_truthy(f_pop(f))) {
                f->ip = in->a;
                jumped = 1;
            }
            break;

        case OP_RET:
            sig.kind = 2;
            sig.value = f_pop(f);
            break;

        case OP_PUTREG:
            f->regs[in->a] = f_pop(f);
            break;

        case OP_GETREG:
            f_push(f, f->regs[in->a]);
            f->regs[in->a] = v_null();
            break;

        case OP_CALL: {
            int n = in->a;
            Value small[8];
            Value *args = n <= 8 ? small : xmalloc(sizeof(Value) * (size_t)n);
            for (int i = n - 1; i >= 0; i--) args[i] = f_pop(f);
            Value callee = f_pop(f);
            Value r = call_any(callee, n, args, in->line);
            if (args != small) free(args);
            f_push(f, r);
            break;
        }

        case OP_LOOP_PUSH: {
            VmLoop l;
            l.saved_env = f->env;
            l.env_depth = f->nenvs;
            l.end_pc = in->a;
            l.cont_pc = in->b;
            f_loop_push(f, l);
            break;
        }

        case OP_LOOP_POP:
            if (f->nloops > 0) f->nloops--;
            break;

        case OP_BREAK:
            sig.kind = 3;
            break;

        case OP_CONTINUE:
            sig.kind = 4;
            break;

        case OP_TRY: {
            VmTry t;
            t.meta = in->meta;
            t.saved_env = f->env;
            t.env_depth = f->nenvs;
            t.stack_len = f->sp;
            t.in_catch = 0;
            f_try_push(f, t);
            break;
        }

        case OP_TEND: {
            if (f->ntries > 0) f->ntries--;
            f->ip = in->meta->finally_pc >= 0 ? in->meta->finally_pc : in->meta->after_pc;
            jumped = 1;
            break;
        }

        case OP_CEND: {
            if (f->ntries > 0) {
                VmTry *st = &f->tries[f->ntries - 1];
                f->env = st->saved_env;
                f->nenvs = st->env_depth;
                f->ntries--;
            }
            f->ip = in->meta->finally_pc >= 0 ? in->meta->finally_pc : in->meta->after_pc;
            jumped = 1;
            break;
        }

        case OP_FEND:
            if (f->has_pending) {
                sig = f->pending;
                f->has_pending = 0;
            } else {
                f->ip = in->meta->after_pc;
                jumped = 1;
            }
            break;

        case OP_PYBLOCK: {
            char err[2048];
            char *src = py_dedent(code->consts[in->a].as.s->s);
            if (!py_exec(src, err, sizeof(err)))
                err_linef(in->line, "Python error: %s", err);
            break;
        }

        case OP_GOBLOCK: {
            char err[2048];
            char *src = py_dedent(code->consts[in->a].as.s->s);
            if (!go_exec(src, err, sizeof(err)))
                err_linef(in->line, "Go error: %s", err);
            break;
        }

        case OP_THROW: {
            Value s = str_of(f_pop(f));
            err_line(in->line, s.as.s->s);
            break;
        }

        case OP_FORIN_START: {
            Value it = f_pop(f);
            int n = 0;
            Value *items = vm_iterable(it, &n);
            if (!items) break;
            int id = f_iter_add(f, items, n);
            f_push(f, v_int_i64(id + 1));
            break;
        }

        case OP_FORIN_NEXT: {
            int64_t id = 0;
            Value marker = f->sp > 0 ? f->st[f->sp - 1] : v_null();
            if (marker.t != V_INT || !bi_is_i64(marker.as.i, &id) || id <= 0 || id > f->niters) {
                err_msg("vm: внутренняя ошибка итератора");
                break;
            }
            VmIter *cur = &f->iters[id - 1];
            if (cur->pos >= cur->n) {
                f->ip = in->a;
                jumped = 1;
            } else {
                f_push(f, cur->items[cur->pos++]);
            }
            break;
        }

        case OP_FORIN_END: {
            int64_t id = 0;
            Value marker = f_pop(f);
            if (marker.t == V_INT && bi_is_i64(marker.as.i, &id) && id > 0 && id <= f->niters)
                f->iters[id - 1].n = 0;
            break;
        }

        case OP_BLOCK_ENTER:
            f_env_push(f, f->env);
            f->env = env_public_new(f->env);
            break;

        case OP_BLOCK_EXIT:
            if (f->nenvs > 0) f->env = f->envs[--f->nenvs];
            break;

        case OP_IMPORT: {
            Node tmp;
            memset(&tmp, 0, sizeof(tmp));
            tmp.t = N_IMPORT;
            tmp.line = in->line;
            tmp.u.imports.path = code->consts[in->a].as.s->s;
            tmp.u.imports.alias = in->b == 0xFFFF ? NULL : code->consts[in->b].as.s->s;
            vm_import(&tmp);
            break;
        }

        default:
            err_msg("vm: неизвестная инструкция");
            break;
        }

        if (sig.kind == 0 && g_err.set) {
            sig.kind = 1;
            sig.err = g_err;
            err_clear();
        }
        if (sig.kind) {
            int r = handle_sig(f, &sig, out);
            if (r == 2) return 1;
            if (r == 3) return 0;
            continue;
        }
        if (!jumped) f->ip++;
    }
}

static Value call_any(Value fn, int argc, Value *argv, int line) {
    if (fn.t == V_FUNC) {
        Func *g = fn.as.fn;
        if (!g->native && !g->native_ctx && g->data && g->nparams == argc) {
            VmCode *code = (VmCode *)g->data;
            Env *ce = env_public_new(g->closure);
            for (int i = 0; i < g->nparams; i++) env_public_define(ce, g->params[i], argv[i], 0);
            Frame nf;
            frame_init(&nf, code, ce);
            Value out = v_null();
            if (g_trace_on) fprintf(stderr, "[trace] -> %s() строка %d\n", g->name, line);
            interp_frame_push(g->name, line);
            int r = run_frame(&nf, &out);
            interp_snapshot_error();
            interp_frame_pop();
            if (g_trace_on) fprintf(stderr, "[trace] <- %s()\n", g->name);
            frame_free(&nf);
            if (r != 0) return v_null();
            return out;
        }
    }
    return call_value(fn, argc, argv, line);
}

int vm_enabled(void) {
    const char *v = getenv("UNILAND_VM");
    return v && strcmp(v, "1") == 0;
}

Flow vm_interpret(Node *program) {
    g_trace_on = interp_trace_on();
    VmCode *code = compile_program(program);
    if (!code) {
        err_clear();
        return interp_tree_interpret(program);
    }
    Frame f;
    frame_init(&f, code, env_global());
    Value out = v_null();
    int r = run_frame(&f, &out);
    frame_free(&f);
    if (r != 0) return g_err.is_exit ? F_EXIT : F_ERROR;
    if (env_public_has(env_global(), "main")) {
        Value mv = env_public_get(env_global(), "main");
        if (mv.t == V_FUNC && mv.as.fn->native == NULL && mv.as.fn->nparams == 0) {
            Value res = call_any(mv, 0, NULL, -1);
            (void)res;
            if (g_err.set) return g_err.is_exit ? F_EXIT : F_ERROR;
        }
    }
    return F_OK;
}
