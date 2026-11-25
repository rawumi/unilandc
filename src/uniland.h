#ifndef UNILAND_H
#define UNILAND_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

void *xmalloc(size_t n);
char *xstrndup(const char *s, size_t n);
char *xstrdup(const char *s);
void *xrealloc(void *p, size_t n);

typedef struct BigInt BigInt;

BigInt *bi_from_i64(int64_t v);
BigInt *bi_from_str(const char *s);
BigInt *bi_copy(const BigInt *n);
char *bi_to_str(const BigInt *n);
int bi_cmp(const BigInt *a, const BigInt *b);
int bi_sign(const BigInt *n);
int bi_is_zero(const BigInt *n);
int bi_is_i64(const BigInt *n, int64_t *out);
int bi_bit(const BigInt *n, int i);
uint64_t bi_bitlen(const BigInt *n);
double bi_to_f64(const BigInt *n);
BigInt *bi_add(const BigInt *a, const BigInt *b);
BigInt *bi_sub(const BigInt *a, const BigInt *b);
BigInt *bi_mul(const BigInt *a, const BigInt *b);
BigInt *bi_neg(const BigInt *a);
BigInt *bi_abs(const BigInt *a);
BigInt *bi_rem(const BigInt *a, const BigInt *b);
BigInt *bi_pow(const BigInt *a, const BigInt *b);

typedef struct Value Value;
typedef struct Str Str;
typedef struct List List;
typedef struct Map Map;
typedef struct Func Func;
typedef struct Env Env;
typedef struct Node Node;
typedef struct Block Block;

typedef enum {
    V_NULL, V_BOOL, V_INT, V_FLOAT, V_STR, V_LIST, V_MAP, V_FUNC
} VType;

struct Str {
    size_t len;
    char *s;
};

struct List {
    int n, cap;
    Value *items;
};

struct Value {
    VType t;
    union {
        int b;
        double f;
        BigInt *i;
        Str *s;
        List *l;
        Map *m;
        Func *fn;
    } as;
};

typedef struct {
    char *k;
    Value v;
} KV;

struct Map {
    int n, cap;
    KV *pairs;
};

typedef Value (*NativeFn)(int argc, Value *argv);

struct Func {
    const char *name;
    int nparams;
    char **params;
    Block *body;
    Env *closure;
    NativeFn native;
};

Str *str_new(const char *s, size_t len);
Str *str_from_cstr(const char *s);
const char *str_cstr(const Str *s);
int str_eq(const Str *a, const Str *b);
size_t u8_count(const char *s, size_t len);
const char *u8_at(const char *s, size_t len, size_t idx, size_t *clen);
char *u8_upper(const char *s, size_t len);
char *u8_lower(const char *s, size_t len);
char *u8_trim(const char *s, size_t len);
char *u8_encode(uint32_t cp);

Value v_null(void);
Value v_bool(int b);
Value v_int_i64(int64_t n);
Value v_int(BigInt *n);
Value v_float(double f);
Value v_str_take(char *s, size_t len);
Value v_str(const char *s);
Value v_list(void);
Value v_map(void);
void list_push(List *l, Value v);
Value list_get(List *l, int i);
void map_set(Map *m, const char *k, Value v);
Value map_get(Map *m, const char *k);
int map_has(Map *m, const char *k);
Value v_func(Func *f);

typedef struct {
    char *buf;
    size_t len, cap;
} SB;
void sb_init(SB *b);
void sb_add(SB *b, const char *s, size_t n);
void sb_adds(SB *b, const char *s);
void sb_addch(SB *b, char c);
void sb_addf(SB *b, const char *fmt, ...);
char *sb_take(SB *b, size_t *len);

typedef struct {
    int set;
    char msg[2048];
    int line;
    int has_line;
    int is_exit;
    int exit_code;
} UErr;

extern UErr g_err;
void err_clear(void);
void err_msg(const char *msg);
void err_msgf(const char *fmt, ...);
void err_line(int line, const char *msg);
void err_linef(int line, const char *fmt, ...);
void err_exit(int code);

typedef enum {
    T_KEYWORD, T_IDENT, T_OP, T_NUMBER, T_NEWLINE, T_PUNCT,
    T_TEMPLATE, T_STRING, T_PYBLOCK, T_GOBLOCK, T_EOF
} TokType;

typedef struct {
    TokType t;
    char *v;
    int line, col;
} Token;

Token *lex(const char *src, int *out_count);
const char *tok_typename(TokType t);
const char *tok_dump(const Token *t);
char *py_repr(const char *s);

typedef enum {
    N_PROGRAM, N_VARDECL, N_FUNCDECL, N_FUNCEXPR, N_RETURN, N_IF, N_FOR,
    N_FORIN, N_WHILE, N_BREAK, N_CONTINUE, N_BLOCK, N_EXPRSTMT, N_ASSIGN,
    N_TRYCATCH, N_THROW, N_PYBLOCK, N_GOBLOCK, N_BINARY, N_UNARY, N_CALL,
    N_IDENT, N_LITERAL, N_TEMPLATE, N_ARRAY, N_OBJECT, N_INDEX, N_MEMBER
} NType;

typedef struct {
    char *str;
    Node *expr;
} TplPart;

struct Block {
    Node **stmts;
    int n;
    int line;
};

typedef struct {
    char *key;
    Node *value;
} KeyValue;

struct Node {
    NType t;
    int line;
    union {
        struct { Node **stmts; int n; } program;
        struct { char *name; Node *value; int is_const; } var;
        struct { char *name; char **params; int nparams; Block *body; } funcdecl;
        struct { char **params; int nparams; Block *body; } funcexpr;
        struct { Node *value; } ret;
        struct { Node *cond; Block *then_b; Node *else_b; } ifs;
        struct { Node *init, *cond, *update; Block *body; } fors;
        struct { char *var; Node *iterable; Block *body; } forin;
        struct { Node *cond; Block *body; } whiles;
        Block block;
        struct { Node *expr; } exprstmt;
        struct { Node *target, *value; char *op; } assign;
        struct { Block *try_b, *catch_b, *finally_b; char *catch_var; } tc;
        struct { Node *value; } thr;
        struct { char *code; } bridge;
        struct { Node *left, *right; char *op; } binary;
        struct { Node *expr; char *op; } unary;
        struct { Node *callee; Node **args; int nargs; } call;
        struct { char *name; } ident;
        struct { Value value; } literal;
        struct { TplPart *parts; int nparts; } tpl;
        struct { Node **elems; int n; } arr;
        struct { KeyValue *pairs; int n; } obj;
        struct { Node *obj, *index; } index;
        struct { Node *obj; char *name; } member;
    } u;
};

Node *parse_program(Token *tokens);

typedef enum {
    F_OK, F_RETURN, F_BREAK, F_CONTINUE, F_ERROR, F_EXIT
} Flow;

typedef struct {
    FILE *out;
    FILE *errw;
    FILE *in;
    char **args;
    int nargs;
    int exit_code;
} Interp;

Value eval(Node *n);
Flow exec(Node *n);
Value call_value(Value fn, int argc, Value *argv, int line);
void interp_init(FILE *out, FILE *errw, FILE *in, char **args, int nargs);
Env *env_global(void);
Value value_str(Value v);
Value value_py_str(Value v);
int value_truthy(Value v);
const char *type_of(Value v);
const char *type_name(Value v);
Value value_inner(Value v);

Value interp_global_get(const char *name);
void interp_global_set(const char *name, Value v);
Value interp_call_named(const char *name, int argc, Value *argv);

int py_eval(const char *code, Value *out, char *err, size_t cap);
int py_exec(const char *code, char *err, size_t cap);
void py_bridge_close(void);
char *py_dedent(const char *s);
int go_eval(const char *expr, Value *out, char *err, size_t cap);
int go_exec(const char *code, char *err, size_t cap);
void go_bridge_close(void);

extern Interp g_it;
void env_define_public(Env *e, const char *name, Value v, int is_const);
void std_install(Env *env);
void c_install(Env *env);
void http_install(Env *env);
void gui_install(Env *env);
char *unifmt_format(const char *src, char *err, size_t cap);
int lsp_serve(void);
Flow interp_interpret(Node *program);
char *interp_eval_program(Node *program);
Value json_parse_value(const char *src);
char *json_dumps(Value v, int indent, int *err);
int value_equal(Value a, Value b);
int value_less(Value a, Value b);
Value op_add(Value a, Value b);
Value op_sub(Value a, Value b);
Value op_mul(Value a, Value b);
Value op_div(Value a, Value b);
Value op_mod(Value a, Value b);
Value op_pow(Value a, Value b);
Value op_neg(Value a);
int value_is_number(Value v);
BigInt *int_of(Value v);
char *nformat(double f);
char *nformat_repr(double f);

#endif
