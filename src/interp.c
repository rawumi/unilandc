
#include "uniland.h"

#include <stdlib.h>
#include <string.h>

typedef struct Binding {
    char *name;
    Value v;
    int is_const;
} Binding;

struct Env {
    Env *parent;
    Binding *b;
    int n, cap;
};

static Env *g_global;
static Env *g_env;
static Value g_ret;
Interp g_it;

static Env *env_new(Env *parent);
static void env_define(Env *e, const char *name, Value v, int is_const);

Env *env_global(void) { return g_global; }

void env_define_public(Env *e, const char *name, Value v, int is_const) { env_define(e, name, v, is_const); }

static Env *env_new(Env *parent) {
    Env *e = xmalloc(sizeof(Env));
    e->parent = parent;
    e->b = NULL;
    e->n = e->cap = 0;
    return e;
}

static void env_define(Env *e, const char *name, Value v, int is_const) {
    for (int i = 0; i < e->n; i++) {
        if (strcmp(e->b[i].name, name) == 0) {
            e->b[i].v = v;
            e->b[i].is_const = is_const;
            return;
        }
    }
    if (e->n == e->cap) {
        e->cap = e->cap ? e->cap * 2 : 8;
        e->b = xrealloc(e->b, sizeof(Binding) * (size_t)e->cap);
    }
    e->b[e->n].name = xstrdup(name);
    e->b[e->n].v = v;
    e->b[e->n].is_const = is_const;
    e->n++;
}

static Binding *env_find(Env *e, const char *name) {
    for (Env *p = e; p; p = p->parent) {
        for (int i = 0; i < p->n; i++)
            if (strcmp(p->b[i].name, name) == 0) return &p->b[i];
    }
    return NULL;
}

Value interp_global_get(const char *name) {
    Binding *b = g_global ? env_find(g_global, name) : NULL;
    if (!b) {
        err_msgf("Undefined variable: %s", name);
        return v_null();
    }
    return b->v;
}

void interp_global_set(const char *name, Value v) { env_define(g_global, name, v, 0); }

Value interp_call_named(const char *name, int argc, Value *argv) {
    Binding *b = g_global ? env_find(g_global, name) : NULL;
    if (!b) {
        err_msgf("Undefined variable: %s", name);
        return v_null();
    }
    return call_value(b->v, argc, argv, -1);
}

static Value env_get(Env *e, const char *name) {
    Binding *b = env_find(e, name);
    if (!b) {
        err_msgf("Undefined variable: %s", name);
        return v_null();
    }
    return b->v;
}

static void env_set(Env *e, const char *name, Value v) {
    Binding *b = env_find(e, name);
    if (!b) {
        err_msgf("Undefined variable: %s", name);
        return;
    }
    if (b->is_const) {
        err_msgf("Cannot reassign constant: %s", name);
        return;
    }
    b->v = v;
}

static Flow exec_block(Block *b) {
    Env *old = g_env;
    g_env = env_new(old);
    Flow fl = F_OK;
    for (int i = 0; i < b->n; i++) {
        fl = exec(b->stmts[i]);
        if (fl != F_OK) break;
    }
    g_env = old;
    return fl;
}

static Value apply_binary(const char *op, Value left, Value right, int line);

static Value index_value(Value obj, Value index, int line) {
    if (obj.t == V_LIST) {
        int64_t idx;
        if (index.t != V_INT) {
            err_line(line, "List index must be an integer");
            return v_null();
        }
        if (!bi_is_i64(index.as.i, &idx)) idx = bi_sign(index.as.i) > 0 ? (1LL << 62) : -(1LL << 62);
        int64_t n = obj.as.l->n;
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) {
            err_linef(line, "List index out of range (length %d)", obj.as.l->n);
            return v_null();
        }
        return obj.as.l->items[idx];
    }
    if (obj.t == V_STR) {
        int64_t idx;
        if (index.t != V_INT) {
            err_line(line, "String index must be an integer");
            return v_null();
        }
        if (!bi_is_i64(index.as.i, &idx)) idx = bi_sign(index.as.i) > 0 ? (1LL << 62) : -(1LL << 62);
        int64_t n = (int64_t)u8_count(obj.as.s->s, obj.as.s->len);
        if (idx < 0) idx += n;
        if (idx < 0 || idx >= n) {
            err_line(line, "String index out of range");
            return v_null();
        }
        size_t clen = 0;
        const char *p = u8_at(obj.as.s->s, obj.as.s->len, (size_t)idx, &clen);
        return v_str_take(xstrndup(p, clen), clen);
    }
    if (obj.t == V_MAP) {
        Value k = value_py_str(index);
        return map_get(obj.as.m, k.as.s->s);
    }
    err_linef(line, "Cannot index %s", type_of(obj));
    return v_null();
}

Value eval(Node *n) {
    if (!n) return v_null();
    switch (n->t) {
    case N_LITERAL:
        return n->u.literal.value;
    case N_TEMPLATE: {
        SB b;
        sb_init(&b);
        for (int i = 0; i < n->u.tpl.nparts; i++) {
            TplPart *p = &n->u.tpl.parts[i];
            if (!p->expr) {
                sb_adds(&b, p->str);
                continue;
            }
            Value v = eval(p->expr);
            if (g_err.set) return v_null();
            Value s = value_str(v);
            sb_add(&b, s.as.s->s, s.as.s->len);
        }
        return v_str_take(b.buf, b.len);
    }
    case N_IDENT:
        return env_get(g_env, n->u.ident.name);
    case N_BINARY: {
        if (strcmp(n->u.binary.op, "&&") == 0) {
            Value l = eval(n->u.binary.left);
            if (g_err.set) return v_null();
            if (!value_truthy(l)) return l;
            return eval(n->u.binary.right);
        }
        if (strcmp(n->u.binary.op, "||") == 0) {
            Value l = eval(n->u.binary.left);
            if (g_err.set) return v_null();
            if (value_truthy(l)) return l;
            return eval(n->u.binary.right);
        }
        Value l = eval(n->u.binary.left);
        if (g_err.set) return v_null();
        Value r = eval(n->u.binary.right);
        if (g_err.set) return v_null();
        return apply_binary(n->u.binary.op, l, r, n->line);
    }
    case N_UNARY: {
        Value v = eval(n->u.unary.expr);
        if (g_err.set) return v_null();
        if (strcmp(n->u.unary.op, "-") == 0) return op_neg(v);
        if (strcmp(n->u.unary.op, "!") == 0) return v_bool(!value_truthy(v));
        err_linef(n->line, "Unknown unary operator %s", n->u.unary.op);
        return v_null();
    }
    case N_INDEX: {
        Value obj = eval(n->u.index.obj);
        if (g_err.set) return v_null();
        Value idx = eval(n->u.index.index);
        if (g_err.set) return v_null();
        return index_value(obj, idx, n->line);
    }
    case N_MEMBER: {
        Value obj = eval(n->u.member.obj);
        if (g_err.set) return v_null();
        if (obj.t == V_MAP) return map_get(obj.as.m, n->u.member.name);
        err_linef(n->line, "Cannot read field '%s' of %s", n->u.member.name, type_of(obj));
        return v_null();
    }
    case N_CALL: {
        Value fn = eval(n->u.call.callee);
        if (g_err.set) return v_null();
        Value *args = xmalloc(sizeof(Value) * (size_t)(n->u.call.nargs ? n->u.call.nargs : 1));
        for (int i = 0; i < n->u.call.nargs; i++) {
            args[i] = eval(n->u.call.args[i]);
            if (g_err.set) return v_null();
        }
        return call_value(fn, n->u.call.nargs, args, n->line);
    }
    case N_FUNCEXPR: {
        Func *f = xmalloc(sizeof(Func));
        f->name = "<anonymous>";
        f->nparams = n->u.funcexpr.nparams;
        f->params = n->u.funcexpr.params;
        f->body = n->u.funcexpr.body;
        f->closure = g_env;
        f->native = NULL;
        return v_func(f);
    }
    case N_ARRAY: {
        Value l = v_list();
        for (int i = 0; i < n->u.arr.n; i++) {
            Value v = eval(n->u.arr.elems[i]);
            if (g_err.set) return v_null();
            list_push(l.as.l, v);
        }
        return l;
    }
    case N_OBJECT: {
        Value m = v_map();
        for (int i = 0; i < n->u.obj.n; i++) {
            Value v = eval(n->u.obj.pairs[i].value);
            if (g_err.set) return v_null();
            map_set(m.as.m, n->u.obj.pairs[i].key, v);
        }
        return m;
    }
    default:
        err_msgf("Cannot evaluate node");
        return v_null();
    }
}

static int int_index(Value v, int64_t *out) {
    if (v.t != V_INT) return 0;
    if (!bi_is_i64(v.as.i, out)) *out = bi_sign(v.as.i) > 0 ? (1LL << 62) : -(1LL << 62);
    return 1;
}

static void assign_to(Node *target, Value value, int line) {
    if (target->t == N_IDENT) {
        if (!env_find(g_env, target->u.ident.name)) {
            env_define(g_env, target->u.ident.name, value, 0);
            return;
        }
        env_set(g_env, target->u.ident.name, value);
        return;
    }
    if (target->t == N_INDEX) {
        Value obj = eval(target->u.index.obj);
        if (g_err.set) return;
        Value idx = eval(target->u.index.index);
        if (g_err.set) return;
        if (obj.t == V_LIST) {
            int64_t i;
            if (!int_index(idx, &i)) {
                err_line(line, "List index must be an integer");
                return;
            }
            if (i < 0 || i >= obj.as.l->n) {
                Value is = idx.t == V_INT ? v_str(bi_to_str(idx.as.i)) : value_py_str(idx);
                err_linef(line, "List index %s out of range", is.as.s->s);
                return;
            }
            obj.as.l->items[i] = value;
            return;
        }
        if (obj.t == V_MAP) {
            Value k = value_py_str(idx);
            map_set(obj.as.m, k.as.s->s, value);
            return;
        }
        err_linef(line, "Cannot assign by index into %s", type_of(obj));
        return;
    }
    if (target->t == N_MEMBER) {
        Value obj = eval(target->u.member.obj);
        if (g_err.set) return;
        if (obj.t == V_MAP) {
            map_set(obj.as.m, target->u.member.name, value);
            return;
        }
        err_linef(line, "Cannot assign field on %s", type_of(obj));
        return;
    }
    err_line(line, "Invalid assignment target");
}

static Flow exec_assignment(Node *n) {
    Value new_value = v_null();
    if (strcmp(n->u.assign.op, "++") == 0 || strcmp(n->u.assign.op, "--") == 0) {
        Value cur = eval(n->u.assign.target);
        if (g_err.set) return F_ERROR;
        Value step = v_int_i64(strcmp(n->u.assign.op, "++") == 0 ? 1 : -1);
        new_value = op_add(cur, step);
        if (g_err.set) return F_ERROR;
    } else if (strcmp(n->u.assign.op, "=") == 0) {
        new_value = eval(n->u.assign.value);
        if (g_err.set) return F_ERROR;
    } else {
        Value cur = eval(n->u.assign.target);
        if (g_err.set) return F_ERROR;
        Value rhs = eval(n->u.assign.value);
        if (g_err.set) return F_ERROR;
        char op[2] = {n->u.assign.op[0], 0};
        new_value = apply_binary(op, cur, rhs, n->line);
        if (g_err.set) return F_ERROR;
    }
    assign_to(n->u.assign.target, new_value, n->line);
    if (g_err.set) return F_ERROR;
    return F_OK;
}

static Value *iterable_items(Value v, int *count) {
    if (v.t == V_LIST) {
        Value *items = xmalloc(sizeof(Value) * (size_t)(v.as.l->n ? v.as.l->n : 1));
        for (int i = 0; i < v.as.l->n; i++) items[i] = v.as.l->items[i];
        *count = v.as.l->n;
        return items;
    }
    if (v.t == V_STR) {
        size_t n = u8_count(v.as.s->s, v.as.s->len);
        Value *items = xmalloc(sizeof(Value) * (n ? n : 1));
        for (size_t i = 0; i < n; i++) {
            size_t clen = 0;
            const char *p = u8_at(v.as.s->s, v.as.s->len, i, &clen);
            items[i] = v_str_take(xstrndup(p, clen), clen);
        }
        *count = (int)n;
        return items;
    }
    if (v.t == V_MAP) {
        Value *items = xmalloc(sizeof(Value) * (size_t)(v.as.m->n ? v.as.m->n : 1));
        for (int i = 0; i < v.as.m->n; i++) items[i] = v_str(v.as.m->pairs[i].k);
        *count = v.as.m->n;
        return items;
    }
    err_msgf("Cannot iterate over %s", type_of(v));
    *count = 0;
    return NULL;
}

static Flow exec_try(Node *n) {
    Flow try_err = exec_block(n->u.tc.try_b);
    if (try_err == F_OK) {
        if (n->u.tc.finally_b) return exec_block(n->u.tc.finally_b);
        return F_OK;
    }
    if (try_err != F_ERROR || !g_err.set) {
        if (n->u.tc.finally_b) {
            Flow f = exec_block(n->u.tc.finally_b);
            if (f != F_OK) return f;
        }
        return try_err;
    }
    if (g_err.is_exit) {
        if (n->u.tc.finally_b) {
            UErr saved = g_err;
            err_clear();
            exec_block(n->u.tc.finally_b);
            g_err = saved;
        }
        return F_EXIT;
    }
    if (n->u.tc.catch_b) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "%s", g_err.msg);
        err_clear();
        Env *old = g_env;
        g_env = env_new(old);
        if (n->u.tc.catch_var) env_define(g_env, n->u.tc.catch_var, v_str(msg), 0);
        Flow catch_err = exec_block(n->u.tc.catch_b);
        g_env = old;
        if (n->u.tc.finally_b) {
            Flow f = exec_block(n->u.tc.finally_b);
            if (f != F_OK) return f;
        }
        return catch_err;
    }
    if (n->u.tc.finally_b) {
        UErr saved = g_err;
        err_clear();
        Flow f = exec_block(n->u.tc.finally_b);
        if (g_err.set) return f;
        g_err = saved;
        if (f != F_OK) return f;
    }
    return try_err;
}

Flow exec(Node *n) {
    if (!n) return F_OK;
    switch (n->t) {
    case N_BLOCK:
        return exec_block(&n->u.block);
    case N_VARDECL: {
        Value v = eval(n->u.var.value);
        if (g_err.set) return F_ERROR;
        env_define(g_env, n->u.var.name, v, n->u.var.is_const);
        return F_OK;
    }
    case N_FUNCDECL: {
        Func *f = xmalloc(sizeof(Func));
        f->name = n->u.funcdecl.name;
        f->nparams = n->u.funcdecl.nparams;
        f->params = n->u.funcdecl.params;
        f->body = n->u.funcdecl.body;
        f->closure = g_env;
        f->native = NULL;
        env_define(g_env, n->u.funcdecl.name, v_func(f), 0);
        return F_OK;
    }
    case N_RETURN: {
        if (!n->u.ret.value) {
            g_ret = v_null();
            return F_RETURN;
        }
        Value v = eval(n->u.ret.value);
        if (g_err.set) return F_ERROR;
        g_ret = v;
        return F_RETURN;
    }
    case N_IF: {
        Value c = eval(n->u.ifs.cond);
        if (g_err.set) return F_ERROR;
        if (value_truthy(c)) return exec_block(n->u.ifs.then_b);
        if (n->u.ifs.else_b) return exec(n->u.ifs.else_b);
        return F_OK;
    }
    case N_FOR: {
        if (n->u.fors.init) {
            Flow f = exec(n->u.fors.init);
            if (f != F_OK) return f;
        }
        for (;;) {
            Value c = eval(n->u.fors.cond);
            if (g_err.set) return F_ERROR;
            if (!value_truthy(c)) break;
            Flow f = exec_block(n->u.fors.body);
            if (f == F_BREAK) break;
            if (f != F_OK && f != F_CONTINUE) {
                if (f == F_ERROR || f == F_RETURN) return f;
                return f;
            }
            if (n->u.fors.update) {
                Flow u = exec(n->u.fors.update);
                if (u != F_OK) return u;
            }
        }
        return F_OK;
    }
    case N_FORIN: {
        Value iter = eval(n->u.forin.iterable);
        if (g_err.set) return F_ERROR;
        int count = 0;
        Value *items = iterable_items(iter, &count);
        if (!items) return F_ERROR;
        Env *old = g_env;
        for (int i = 0; i < count; i++) {
            g_env = env_new(old);
            env_define(g_env, n->u.forin.var, items[i], 0);
            Flow f = exec_block(n->u.forin.body);
            g_env = old;
            if (f == F_BREAK) break;
            if (f == F_CONTINUE) continue;
            if (f != F_OK) return f;
        }
        g_env = old;
        return F_OK;
    }
    case N_WHILE: {
        for (;;) {
            Value c = eval(n->u.whiles.cond);
            if (g_err.set) return F_ERROR;
            if (!value_truthy(c)) break;
            Flow f = exec_block(n->u.whiles.body);
            if (f == F_BREAK) break;
            if (f == F_CONTINUE) continue;
            if (f != F_OK) return f;
        }
        return F_OK;
    }
    case N_BREAK:
        return F_BREAK;
    case N_CONTINUE:
        return F_CONTINUE;
    case N_ASSIGN:
        return exec_assignment(n);
    case N_EXPRSTMT: {
        eval(n->u.exprstmt.expr);
        return g_err.set ? F_ERROR : F_OK;
    }
    case N_PYBLOCK: {
        char err[2048];
        char *code = py_dedent(n->u.bridge.code);
        if (!py_exec(code, err, sizeof(err))) {
            err_linef(n->line, "Python error: %s", err);
            return F_ERROR;
        }
        return F_OK;
    }
    case N_GOBLOCK: {
        char err[2048];
        char *code = py_dedent(n->u.bridge.code);
        if (!go_exec(code, err, sizeof(err))) {
            err_linef(n->line, "Go error: %s", err);
            return F_ERROR;
        }
        return F_OK;
    }
    case N_TRYCATCH:
        return exec_try(n);
    case N_THROW: {
        Value v = eval(n->u.thr.value);
        if (g_err.set) return F_ERROR;
        Value s = value_str(v);
        err_line(n->line, s.as.s->s);
        return F_ERROR;
    }
    default:
        err_msg("Cannot execute node");
        return F_ERROR;
    }
}

Value call_value(Value fn, int argc, Value *argv, int line) {
    if (fn.t != V_FUNC) {
        const char *msg_type = type_of(fn);
        if (line >= 0) err_linef(line, "Value of type %s is not callable", msg_type);
        else err_msgf("Value of type %s is not callable", msg_type);
        return v_null();
    }
    Func *f = fn.as.fn;
    if (f->native) return f->native(argc, argv);
    if (argc != f->nparams) {
        if (line >= 0)
            err_linef(line, "%s expects %d argument(s), got %d", f->name, f->nparams, argc);
        else
            err_msgf("%s expects %d argument(s), got %d", f->name, f->nparams, argc);
        return v_null();
    }
    Env *call_env = env_new(f->closure);
    for (int i = 0; i < f->nparams; i++) env_define(call_env, f->params[i], argv[i], 0);
    Env *old = g_env;
    g_env = call_env;
    Flow fl = exec_block(f->body);
    g_env = old;
    if (fl == F_RETURN) return g_ret;
    if (fl == F_ERROR) return v_null();
    if (fl == F_EXIT) {
        err_exit(0);
        return v_null();
    }
    if (fl == F_BREAK) {
        err_msg("break вне цикла");
        return v_null();
    }
    if (fl == F_CONTINUE) {
        err_msg("continue вне цикла");
        return v_null();
    }
    return v_null();
}

static Value apply_binary(const char *op, Value left, Value right, int line) {
    if (strcmp(op, "..") == 0) {
        if (left.t != V_INT || right.t != V_INT) {
            err_line(line, "Range bounds must be integers");
            return v_null();
        }
        int64_t a = 0, b = 0;
        if (!bi_is_i64(left.as.i, &a) || !bi_is_i64(right.as.i, &b)) {
            err_line(line, "Range bounds too large");
            return v_null();
        }
        Value out = v_list();
        for (int64_t i = a; i <= b && out.as.l->n < 1000000; i++) list_push(out.as.l, v_int_i64(i));
        return out;
    }
    if (strcmp(op, "+") == 0) {
        if (left.t == V_STR || right.t == V_STR) {
            Value a = value_str(left);
            Value b = value_str(right);
            SB s;
            sb_init(&s);
            sb_add(&s, a.as.s->s, a.as.s->len);
            sb_add(&s, b.as.s->s, b.as.s->len);
            return v_str_take(s.buf, s.len);
        }
        if (left.t == V_LIST && right.t == V_LIST) {
            Value l = v_list();
            for (int i = 0; i < left.as.l->n; i++) list_push(l.as.l, left.as.l->items[i]);
            for (int i = 0; i < right.as.l->n; i++) list_push(l.as.l, right.as.l->items[i]);
            return l;
        }
        return op_add(left, right);
    }
    if (strcmp(op, "-") == 0) return op_sub(left, right);
    if (strcmp(op, "*") == 0) return op_mul(left, right);
    if (strcmp(op, "/") == 0) {
        int zero = 0;
        if (right.t == V_INT) zero = bi_is_zero(right.as.i);
        else if (right.t == V_FLOAT) zero = right.as.f == 0;
        else if (right.t == V_BOOL) zero = !right.as.b;
        if (zero) {
            err_line(line, "Division by zero");
            return v_null();
        }
        return op_div(left, right);
    }
    if (strcmp(op, "%") == 0) return op_mod(left, right);
    if (strcmp(op, "^") == 0) return op_pow(left, right);
    if (strcmp(op, "==") == 0) return v_bool(value_equal(left, right));
    if (strcmp(op, "!=") == 0) return v_bool(!value_equal(left, right));
    if (strcmp(op, "<") == 0) {
        int r = value_less(left, right);
        if (g_err.set) return v_null();
        return v_bool(r);
    }
    if (strcmp(op, ">") == 0) {
        int r = value_less(right, left);
        if (g_err.set) return v_null();
        return v_bool(r);
    }
    if (strcmp(op, "<=") == 0) {
        int r = value_less(left, right);
        if (g_err.set) return v_null();
        return v_bool(r || value_equal(left, right));
    }
    if (strcmp(op, ">=") == 0) {
        int r = value_less(right, left);
        if (g_err.set) return v_null();
        return v_bool(r || value_equal(left, right));
    }
    err_linef(line, "Unknown operator %s", op);
    return v_null();
}

Flow interp_interpret(Node *program) {
    for (int i = 0; i < program->u.program.n; i++) {
        Flow f = exec(program->u.program.stmts[i]);
        if (f == F_EXIT) return F_EXIT;
        if (f == F_ERROR) return g_err.is_exit ? F_EXIT : F_ERROR;
        if (f == F_RETURN) {
            err_msg("return вне функции");
            return F_ERROR;
        }
        if (f == F_BREAK) {
            err_msg("break вне цикла");
            return F_ERROR;
        }
        if (f == F_CONTINUE) {
            err_msg("continue вне цикла");
            return F_ERROR;
        }
    }
    Binding *mb = env_find(g_global, "main");
    if (mb && mb->v.t == V_FUNC && mb->v.as.fn->native == NULL && mb->v.as.fn->nparams == 0) {
        Value r = call_value(mb->v, 0, NULL, -1);
        if (g_err.set) return g_err.is_exit ? F_EXIT : F_ERROR;
        (void)r;
    }
    return F_OK;
}

char *interp_eval_program(Node *program) {
    SB echo;
    sb_init(&echo);
    for (int i = 0; i < program->u.program.n; i++) {
        Node *st = program->u.program.stmts[i];
        if (st->t == N_EXPRSTMT) {
            Value v = eval(st->u.exprstmt.expr);
            if (g_err.set) return NULL;
            if (v.t != V_NULL) {
                Value s = value_str(v);
                if (echo.len > 0) sb_addch(&echo, '\n');
                sb_add(&echo, s.as.s->s, s.as.s->len);
            }
            continue;
        }
        Flow f = exec(st);
        if (f == F_ERROR) return NULL;
        if (f == F_EXIT) {
            err_exit(0);
            return NULL;
        }
        if (f != F_OK) {
            err_msg("return вне функции");
            return NULL;
        }
    }
    return sb_take(&echo, NULL);
}

void interp_init(FILE *out, FILE *errw, FILE *in, char **args, int nargs) {
    err_clear();
    g_it.out = out;
    g_it.errw = errw;
    g_it.in = in;
    g_it.args = args;
    g_it.nargs = nargs;
    g_it.exit_code = 0;
    g_global = env_new(NULL);
    g_env = g_global;
    g_ret = v_null();
    std_install(g_global);
}
