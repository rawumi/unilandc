
#include "uniland.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    Token *t;
    int n, pos;
} P;

static Node *nd(NType t, int line) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(*n));
    n->t = t;
    n->line = line;
    return n;
}

static Token eof_token(void) {
    Token t;
    t.t = T_EOF;
    t.v = "";
    t.line = 0;
    t.col = 0;
    return t;
}

static Token peek(P *p, int k) {
    int idx = p->pos + k;
    if (idx < p->n) return p->t[idx];
    return eof_token();
}

static Token cur(P *p) { return peek(p, 0); }

static Token take(P *p) {
    Token t = cur(p);
    if (p->pos < p->n) p->pos++;
    return t;
}

static int expect(P *p, TokType type, const char *value, Token *out) {
    Token t = cur(p);
    if (t.t != type || (value && strcmp(t.v, value) != 0)) {
        const char *want = value ? value : tok_typename(type);
        err_linef(t.line, "Expected %s, got %s('%s')", want, tok_typename(t.t), t.v);
        return 0;
    }
    take(p);
    if (out) *out = t;
    return 1;
}

static void skip_newlines(P *p) {
    while (cur(p).t == T_NEWLINE) take(p);
}

static int is_punct(P *p, const char *v) {
    Token t = cur(p);
    return t.t == T_PUNCT && strcmp(t.v, v) == 0;
}

static int is_kw(P *p, const char *v) {
    Token t = cur(p);
    return t.t == T_KEYWORD && strcmp(t.v, v) == 0;
}

static int end_statement(P *p) {
    Token t = cur(p);
    if (t.t == T_PUNCT && strcmp(t.v, ";") == 0) {
        take(p);
        skip_newlines(p);
        return 1;
    }
    if (t.t == T_NEWLINE) {
        skip_newlines(p);
        return 1;
    }
    if (t.t == T_EOF || (t.t == T_PUNCT && strcmp(t.v, "}") == 0)) return 1;
    err_linef(t.line, "Expected end of statement, got %s('%s')", tok_typename(t.t), t.v);
    return 0;
}

static Node *expression(P *p, int min_prec);
static Node *small(P *p);
static Node *statement(P *p);

static int prec_of(const char *op) {
    if (strcmp(op, "||") == 0 || strcmp(op, "&&") == 0) return -1;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) return 0;
    if (strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 || strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) return 1;
    if (strcmp(op, "..") == 0) return 1;
    if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0) return 2;
    if (strcmp(op, "*") == 0 || strcmp(op, "/") == 0 || strcmp(op, "%") == 0) return 3;
    if (strcmp(op, "^") == 0) return 4;
    return -100;
}

static int is_assign_op(const char *op) {
    return strcmp(op, "=") == 0 || strcmp(op, "+=") == 0 || strcmp(op, "-=") == 0 ||
           strcmp(op, "*=") == 0 || strcmp(op, "/=") == 0 || strcmp(op, "%=") == 0;
}

static void push_node(Node ***arr, int *n, int *cap, Node *v) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = xrealloc(*arr, sizeof(Node *) * (size_t)*cap);
    }
    (*arr)[(*n)++] = v;
}

static void push_str(char ***arr, int *n, int *cap, char *v) {
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *arr = xrealloc(*arr, sizeof(char *) * (size_t)*cap);
    }
    (*arr)[(*n)++] = v;
}

static Node *block(P *p) {
    if (!expect(p, T_PUNCT, "{", NULL)) return NULL;
    skip_newlines(p);
    Node **stmts = NULL;
    int n = 0, cap = 0;
    while (!is_punct(p, "}") && cur(p).t != T_EOF) {
        Node *s = statement(p);
        if (!s) return NULL;
        push_node(&stmts, &n, &cap, s);
        skip_newlines(p);
    }
    if (!expect(p, T_PUNCT, "}", NULL)) return NULL;
    Node *node = nd(N_BLOCK, 0);
    node->u.block.stmts = stmts;
    node->u.block.n = n;
    return node;
}

static int param_list(P *p, char ***out, int *out_n) {
    if (!expect(p, T_PUNCT, "(", NULL)) return 0;
    skip_newlines(p);
    char **params = NULL;
    int n = 0, cap = 0;
    if (!is_punct(p, ")")) {
        for (;;) {
            Token id;
            if (!expect(p, T_IDENT, NULL, &id)) return 0;
            push_str(&params, &n, &cap, id.v);
            skip_newlines(p);
            if (is_punct(p, ",")) {
                take(p);
                skip_newlines(p);
                continue;
            }
            break;
        }
    }
    if (!expect(p, T_PUNCT, ")", NULL)) return 0;
    *out = params;
    *out_n = n;
    return 1;
}

static Node *arrow_body(P *p) {
    skip_newlines(p);
    if (is_punct(p, "{")) return block(p);
    Node *ex = expression(p, -2);
    if (!ex) return NULL;
    Node *ret = nd(N_RETURN, ex->line);
    ret->u.ret.value = ex;
    Node **st = NULL;
    int n = 0, cap = 0;
    push_node(&st, &n, &cap, ret);
    Node *b = nd(N_BLOCK, 0);
    b->u.block.stmts = st;
    b->u.block.n = n;
    return b;
}

static Node *make_arrow(int line, char **params, int nparams, Node *body) {
    Node *n = nd(N_FUNCEXPR, line);
    n->u.funcexpr.params = params;
    n->u.funcexpr.nparams = nparams;
    n->u.funcexpr.body = &body->u.block;
    return n;
}

static Node *build_template(const char *raw, int line) {
    size_t len = strlen(raw), i = 0;
    TplPart *parts = NULL;
    int np = 0, cp = 0;
    SB buf;
    sb_init(&buf);
    while (i < len) {
        char ch = raw[i];
        if (ch == '{' && i + 1 < len && raw[i + 1] == '{') {
            sb_addch(&buf, '{');
            i += 2;
            continue;
        }
        if (ch == '}' && i + 1 < len && raw[i + 1] == '}') {
            sb_addch(&buf, '}');
            i += 2;
            continue;
        }
        if (ch == '{') {
            if (buf.len > 0) {
                if (np == cp) {
                    cp = cp ? cp * 2 : 8;
                    parts = xrealloc(parts, sizeof(TplPart) * (size_t)cp);
                }
                parts[np].str = buf.buf;
                parts[np].expr = NULL;
                np++;
                sb_init(&buf);
            }
            int depth = 1;
            size_t j = i + 1;
            SB ex;
            sb_init(&ex);
            while (j < len && depth > 0) {
                char cj = raw[j];
                if (cj == '{') depth++;
                else if (cj == '}') {
                    depth--;
                    if (depth == 0) break;
                }
                sb_addch(&ex, cj);
                j++;
            }
            char *es = u8_trim(ex.buf, ex.len);
            int cnt = 0;
            Token *sub = lex(es, &cnt);
            if (!sub) return NULL;
            P sp;
            sp.t = sub;
            sp.n = cnt;
            sp.pos = 0;
            skip_newlines(&sp);
            Node *subnode = expression(&sp, -2);
            if (!subnode) return NULL;
            if (np == cp) {
                cp = cp ? cp * 2 : 8;
                parts = xrealloc(parts, sizeof(TplPart) * (size_t)cp);
            }
            parts[np].str = NULL;
            parts[np].expr = subnode;
            np++;
            i = j + 1;
        } else {
            sb_addch(&buf, ch);
            i++;
        }
    }
    if (buf.len > 0) {
        if (np == cp) {
            cp = cp ? cp * 2 : 8;
            parts = xrealloc(parts, sizeof(TplPart) * (size_t)cp);
        }
        parts[np].str = buf.buf;
        parts[np].expr = NULL;
        np++;
    }
    Node *node = nd(N_TEMPLATE, line);
    node->u.tpl.parts = parts;
    node->u.tpl.nparts = np;
    return node;
}

static Node *postfix(P *p);

static Node *primary(P *p) {
    Token pt = cur(p);
    Node *node;

    if (pt.t == T_IDENT && peek(p, 1).t == T_OP && strcmp(peek(p, 1).v, "=>") == 0) {
        take(p);
        take(p);
        Node *body = arrow_body(p);
        if (!body) return NULL;
        char **params = xmalloc(sizeof(char *));
        params[0] = pt.v;
        return make_arrow(pt.line, params, 1, body);
    }

    switch (pt.t) {
    case T_NUMBER:
        take(p);
        node = nd(N_LITERAL, pt.line);
        if (strchr(pt.v, '.')) node->u.literal.value = v_float(strtod(pt.v, NULL));
        else node->u.literal.value = v_int(bi_from_str(pt.v));
        return node;
    case T_STRING:
        take(p);
        node = nd(N_LITERAL, pt.line);
        node->u.literal.value = v_str(pt.v);
        return node;
    case T_TEMPLATE:
        take(p);
        return build_template(pt.v, pt.line);
    case T_IDENT:
        take(p);
        node = nd(N_IDENT, pt.line);
        node->u.ident.name = pt.v;
        return node;
    case T_PYBLOCK:
        err_line(pt.line, "python {} block cannot be used as an expression");
        return NULL;
    case T_KEYWORD:
        if (strcmp(pt.v, "true") == 0 || strcmp(pt.v, "false") == 0) {
            take(p);
            node = nd(N_LITERAL, pt.line);
            node->u.literal.value = v_bool(strcmp(pt.v, "true") == 0);
            return node;
        }
        if (strcmp(pt.v, "null") == 0) {
            take(p);
            node = nd(N_LITERAL, pt.line);
            node->u.literal.value = v_null();
            return node;
        }
        if (strcmp(pt.v, "func") == 0 || strcmp(pt.v, "fn") == 0) {
            take(p);
            char **params = NULL;
            int np = 0;
            if (!param_list(p, &params, &np)) return NULL;
            Node *b = block(p);
            if (!b) return NULL;
            return make_arrow(pt.line, params, np, b);
        }
        take(p);
        node = nd(N_IDENT, pt.line);
        node->u.ident.name = pt.v;
        return node;
    case T_PUNCT:
        if (strcmp(pt.v, "(") == 0) {
            int save = p->pos;
            take(p);
            char **params = NULL;
            int np = 0, cp = 0;
            if (!is_punct(p, ")")) {
                for (;;) {
                    if (cur(p).t != T_IDENT) break;
                    push_str(&params, &np, &cp, cur(p).v);
                    take(p);
                    if (is_punct(p, ",")) {
                        take(p);
                        continue;
                    }
                    break;
                }
            }
            if (is_punct(p, ")")) {
                take(p);
                if (cur(p).t == T_OP && strcmp(cur(p).v, "=>") == 0) {
                    take(p);
                    Node *body = arrow_body(p);
                    if (!body) return NULL;
                    return make_arrow(pt.line, params, np, body);
                }
            }
            p->pos = save;
            take(p);
            skip_newlines(p);
            node = expression(p, -2);
            if (!node) return NULL;
            skip_newlines(p);
            if (!expect(p, T_PUNCT, ")", NULL)) return NULL;
            return node;
        }
        if (strcmp(pt.v, "[") == 0) {
            take(p);
            skip_newlines(p);
            Node **elems = NULL;
            int n = 0, cap = 0;
            if (!is_punct(p, "]")) {
                for (;;) {
                    Node *e = expression(p, -2);
                    if (!e) return NULL;
                    push_node(&elems, &n, &cap, e);
                    skip_newlines(p);
                    if (is_punct(p, ",")) {
                        take(p);
                        skip_newlines(p);
                        continue;
                    }
                    break;
                }
            }
            if (!expect(p, T_PUNCT, "]", NULL)) return NULL;
            node = nd(N_ARRAY, pt.line);
            node->u.arr.elems = elems;
            node->u.arr.n = n;
            return node;
        }
        if (strcmp(pt.v, "{") == 0) {
            take(p);
            skip_newlines(p);
            KeyValue *pairs = NULL;
            int n = 0, cap = 0;
            if (!is_punct(p, "}")) {
                for (;;) {
                    Token kt = cur(p);
                    char *key = NULL;
                    if (kt.t == T_IDENT || kt.t == T_STRING) {
                        key = kt.v;
                        take(p);
                    } else {
                        err_line(kt.line, "Expected identifier or string as object key");
                        return NULL;
                    }
                    if (!expect(p, T_PUNCT, ":", NULL)) return NULL;
                    Node *v = expression(p, -2);
                    if (!v) return NULL;
                    if (n == cap) {
                        cap = cap ? cap * 2 : 8;
                        pairs = xrealloc(pairs, sizeof(KeyValue) * (size_t)cap);
                    }
                    pairs[n].key = key;
                    pairs[n].value = v;
                    n++;
                    skip_newlines(p);
                    if (is_punct(p, ",")) {
                        take(p);
                        skip_newlines(p);
                        continue;
                    }
                    break;
                }
            }
            skip_newlines(p);
            if (!expect(p, T_PUNCT, "}", NULL)) return NULL;
            node = nd(N_OBJECT, pt.line);
            node->u.obj.pairs = pairs;
            node->u.obj.n = n;
            return node;
        }
        break;
    default:
        break;
    }
    err_linef(pt.line, "Unexpected token %s('%s')", tok_typename(pt.t), pt.v);
    return NULL;
}

static Node *postfix(P *p) {
    Node *node = primary(p);
    if (!node) return NULL;
    for (;;) {
        Token t = cur(p);
        if (is_punct(p, "(")) {
            take(p);
            skip_newlines(p);
            Node **args = NULL;
            int n = 0, cap = 0;
            if (!is_punct(p, ")")) {
                for (;;) {
                    Node *a = expression(p, -2);
                    if (!a) return NULL;
                    push_node(&args, &n, &cap, a);
                    skip_newlines(p);
                    if (is_punct(p, ",")) {
                        take(p);
                        skip_newlines(p);
                        continue;
                    }
                    break;
                }
            }
            if (!expect(p, T_PUNCT, ")", NULL)) return NULL;
            Node *c = nd(N_CALL, t.line);
            c->u.call.callee = node;
            c->u.call.args = args;
            c->u.call.nargs = n;
            node = c;
        } else if (is_punct(p, "[")) {
            take(p);
            Node *idx = expression(p, -2);
            if (!idx) return NULL;
            if (!expect(p, T_PUNCT, "]", NULL)) return NULL;
            Node *ix = nd(N_INDEX, t.line);
            ix->u.index.obj = node;
            ix->u.index.index = idx;
            node = ix;
        } else if (is_punct(p, ".")) {
            take(p);
            Token name;
            if (!expect(p, T_IDENT, NULL, &name)) return NULL;
            Node *m = nd(N_MEMBER, t.line);
            m->u.member.obj = node;
            m->u.member.name = name.v;
            node = m;
        } else {
            break;
        }
    }
    return node;
}

static Node *unary(P *p) {
    Token t = cur(p);
    if (t.t == T_OP && (strcmp(t.v, "-") == 0 || strcmp(t.v, "!") == 0)) {
        take(p);
        Node *e = unary(p);
        if (!e) return NULL;
        Node *n = nd(N_UNARY, t.line);
        n->u.unary.expr = e;
        n->u.unary.op = t.v;
        return n;
    }
    return postfix(p);
}

static Node *expression(P *p, int min_prec) {
    Node *left = unary(p);
    if (!left) return NULL;
    for (;;) {
        Token t = cur(p);
        if (t.t != T_OP) break;
        int prec = prec_of(t.v);
        if (prec == -100 || prec < min_prec) break;
        take(p);
        skip_newlines(p);
        Node *right = expression(p, prec + 1);
        if (!right) return NULL;
        Node *b = nd(N_BINARY, t.line);
        b->u.binary.left = left;
        b->u.binary.right = right;
        b->u.binary.op = t.v;
        left = b;
    }
    return left;
}

static Node *small(P *p) {
    Token t = cur(p);
    if (t.t == T_KEYWORD) {
        if (strcmp(t.v, "let") == 0 || strcmp(t.v, "const") == 0) {
            int is_const = strcmp(t.v, "const") == 0;
            take(p);
            Token id;
            if (!expect(p, T_IDENT, NULL, &id)) return NULL;
            if (!expect(p, T_OP, "=", NULL)) return NULL;
            Node *v = expression(p, -2);
            if (!v) return NULL;
            Node *n = nd(N_VARDECL, id.line);
            n->u.var.name = id.v;
            n->u.var.value = v;
            n->u.var.is_const = is_const;
            return n;
        }
        if (strcmp(t.v, "return") == 0) {
            take(p);
            Token nt = cur(p);
            if (nt.t == T_NEWLINE || nt.t == T_EOF ||
                (nt.t == T_PUNCT && (strcmp(nt.v, ";") == 0 || strcmp(nt.v, "}") == 0))) {
                return nd(N_RETURN, t.line);
            }
            Node *v = expression(p, -2);
            if (!v) return NULL;
            Node *n = nd(N_RETURN, t.line);
            n->u.ret.value = v;
            return n;
        }
        if (strcmp(t.v, "break") == 0) {
            take(p);
            return nd(N_BREAK, t.line);
        }
        if (strcmp(t.v, "continue") == 0) {
            take(p);
            return nd(N_CONTINUE, t.line);
        }
        if (strcmp(t.v, "throw") == 0) {
            take(p);
            Node *v = expression(p, -2);
            if (!v) return NULL;
            Node *n = nd(N_THROW, t.line);
            n->u.thr.value = v;
            return n;
        }
    }

    Node *expr = expression(p, -2);
    if (!expr) return NULL;
    Token op = cur(p);
    if (op.t == T_OP && is_assign_op(op.v)) {
        take(p);
        Node *v = expression(p, -2);
        if (!v) return NULL;
        Node *n = nd(N_ASSIGN, op.line);
        n->u.assign.target = expr;
        n->u.assign.value = v;
        n->u.assign.op = op.v;
        return n;
    }
    if (op.t == T_OP && (strcmp(op.v, "++") == 0 || strcmp(op.v, "--") == 0)) {
        take(p);
        Node *n = nd(N_ASSIGN, op.line);
        n->u.assign.target = expr;
        Node *one = nd(N_LITERAL, op.line);
        one->u.literal.value = v_int_i64(1);
        n->u.assign.value = one;
        n->u.assign.op = op.v;
        return n;
    }
    Node *n = nd(N_EXPRSTMT, t.line);
    n->u.exprstmt.expr = expr;
    return n;
}

static Node *statement(P *p) {
    skip_newlines(p);
    Token t = cur(p);
    if (t.t == T_KEYWORD) {
        if (strcmp(t.v, "func") == 0 || strcmp(t.v, "fn") == 0) {
            take(p);
            Token name;
            if (!expect(p, T_IDENT, NULL, &name)) return NULL;
            char **params = NULL;
            int np = 0;
            if (!param_list(p, &params, &np)) return NULL;
            Node *b = block(p);
            if (!b) return NULL;
            Node *n = nd(N_FUNCDECL, t.line);
            n->u.funcdecl.name = name.v;
            n->u.funcdecl.params = params;
            n->u.funcdecl.nparams = np;
            n->u.funcdecl.body = &b->u.block;
            return n;
        }
        if (strcmp(t.v, "if") == 0) {
            int line = t.line;
            take(p);
            Node *cond;
            if (is_punct(p, "(")) {
                take(p);
                cond = expression(p, -2);
                if (!cond) return NULL;
                if (!expect(p, T_PUNCT, ")", NULL)) return NULL;
            } else {
                cond = expression(p, -2);
                if (!cond) return NULL;
            }
            Node *then_b = block(p);
            if (!then_b) return NULL;
            Node *else_b = NULL;
            skip_newlines(p);
            if (is_kw(p, "else")) {
                take(p);
                skip_newlines(p);
                if (is_kw(p, "if")) {
                    else_b = statement(p);
                } else {
                    else_b = block(p);
                }
                if (!else_b) return NULL;
            }
            Node *n = nd(N_IF, line);
            n->u.ifs.cond = cond;
            n->u.ifs.then_b = &then_b->u.block;
            n->u.ifs.else_b = else_b;
            return n;
        }
        if (strcmp(t.v, "for") == 0) {
            int line = t.line;
            take(p);
            int paren = 0;
            if (is_punct(p, "(")) {
                take(p);
                paren = 1;
            }

            int forin = 0;
            if (is_kw(p, "let") || is_kw(p, "const")) {
                forin = peek(p, 1).t == T_IDENT && peek(p, 2).t == T_KEYWORD && strcmp(peek(p, 2).v, "in") == 0;
            } else if (cur(p).t == T_IDENT) {
                forin = peek(p, 1).t == T_KEYWORD && strcmp(peek(p, 1).v, "in") == 0;
            }

            if (forin) {
                if (is_kw(p, "let") || is_kw(p, "const")) take(p);
                Token var;
                if (!expect(p, T_IDENT, NULL, &var)) return NULL;
                if (!expect(p, T_KEYWORD, "in", NULL)) return NULL;
                Node *iter = expression(p, -2);
                if (!iter) return NULL;
                if (paren && !expect(p, T_PUNCT, ")", NULL)) return NULL;
                Node *body = block(p);
                if (!body) return NULL;
                Node *n = nd(N_FORIN, line);
                n->u.forin.var = var.v;
                n->u.forin.iterable = iter;
                n->u.forin.body = &body->u.block;
                return n;
            }

            Node *init = NULL;
            if (!is_punct(p, ";")) {
                init = small(p);
                if (!init) return NULL;
            }
            if (!expect(p, T_PUNCT, ";", NULL)) return NULL;
            Node *cond = NULL;
            if (!is_punct(p, ";")) {
                cond = expression(p, -2);
                if (!cond) return NULL;
            } else {
                cond = nd(N_LITERAL, line);
                cond->u.literal.value = v_bool(1);
            }
            if (!expect(p, T_PUNCT, ";", NULL)) return NULL;
            Node *update = NULL;
            if (!is_punct(p, ")")) {
                update = small(p);
                if (!update) return NULL;
            }
            if (paren && !expect(p, T_PUNCT, ")", NULL)) return NULL;
            Node *body = block(p);
            if (!body) return NULL;
            Node *n = nd(N_FOR, line);
            n->u.fors.init = init;
            n->u.fors.cond = cond;
            n->u.fors.update = update;
            n->u.fors.body = &body->u.block;
            return n;
        }
        if (strcmp(t.v, "while") == 0) {
            int line = t.line;
            take(p);
            Node *cond;
            if (is_punct(p, "(")) {
                take(p);
                cond = expression(p, -2);
                if (!cond) return NULL;
                if (!expect(p, T_PUNCT, ")", NULL)) return NULL;
            } else {
                cond = expression(p, -2);
                if (!cond) return NULL;
            }
            Node *body = block(p);
            if (!body) return NULL;
            Node *n = nd(N_WHILE, line);
            n->u.whiles.cond = cond;
            n->u.whiles.body = &body->u.block;
            return n;
        }
        if (strcmp(t.v, "try") == 0) {
            int line = t.line;
            take(p);
            Node *tb = block(p);
            if (!tb) return NULL;
            char *catch_var = NULL;
            Block *catch_b = NULL;
            skip_newlines(p);
            if (is_kw(p, "catch")) {
                take(p);
                if (is_punct(p, "(")) {
                    take(p);
                    Token id;
                    if (!expect(p, T_IDENT, NULL, &id)) return NULL;
                    catch_var = id.v;
                    if (!expect(p, T_PUNCT, ")", NULL)) return NULL;
                }
                Node *cb = block(p);
                if (!cb) return NULL;
                catch_b = &cb->u.block;
            }
            skip_newlines(p);
            Block *finally_b = NULL;
            if (is_kw(p, "finally")) {
                take(p);
                Node *fb = block(p);
                if (!fb) return NULL;
                finally_b = &fb->u.block;
            }
            Node *n = nd(N_TRYCATCH, line);
            n->u.tc.try_b = &tb->u.block;
            n->u.tc.catch_var = catch_var;
            n->u.tc.catch_b = catch_b;
            n->u.tc.finally_b = finally_b;
            return n;
        }
    }
    if (t.t == T_PUNCT && strcmp(t.v, "{") == 0) return block(p);
    if (t.t == T_PYBLOCK) {
        take(p);
        Node *n = nd(N_PYBLOCK, t.line);
        n->u.bridge.code = t.v;
        if (!end_statement(p)) return NULL;
        return n;
    }
    if (t.t == T_GOBLOCK) {
        take(p);
        Node *n = nd(N_GOBLOCK, t.line);
        n->u.bridge.code = t.v;
        if (!end_statement(p)) return NULL;
        return n;
    }

    Node *n = small(p);
    if (!n) return NULL;
    if (!end_statement(p)) return NULL;
    return n;
}

Node *parse_program(Token *tokens) {
    P p;
    p.t = tokens;
    p.pos = 0;
    p.n = 0;
    while (tokens[p.n].t != T_EOF) p.n++;
    p.n++;

    skip_newlines(&p);
    Node **stmts = NULL;
    int n = 0, cap = 0;
    while (cur(&p).t != T_EOF) {
        Node *s = statement(&p);
        if (!s) return NULL;
        push_node(&stmts, &n, &cap, s);
        skip_newlines(&p);
    }
    Node *prog = nd(N_PROGRAM, 0);
    prog->u.program.stmts = stmts;
    prog->u.program.n = n;
    return prog;
}
