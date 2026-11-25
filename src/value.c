
#include "uniland.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n ? n : 1);
    if (!q) {
        fputs("out of memory\n", stderr);
        exit(1);
    }
    return q;
}

char *xstrndup(const char *s, size_t n) {
    char *p = xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }

UErr g_err;

void err_clear(void) { g_err.set = 0; g_err.has_line = 0; g_err.line = 0; g_err.is_exit = 0; g_err.exit_code = 0; g_err.msg[0] = 0; }

void err_msg(const char *msg) {
    g_err.set = 1;
    g_err.has_line = 0;
    g_err.is_exit = 0;
    snprintf(g_err.msg, sizeof(g_err.msg), "%s", msg);
}

void err_msgf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_err.set = 1;
    g_err.has_line = 0;
    g_err.is_exit = 0;
    vsnprintf(g_err.msg, sizeof(g_err.msg), fmt, ap);
    va_end(ap);
}

void err_line(int line, const char *msg) {
    g_err.set = 1;
    g_err.has_line = 1;
    g_err.is_exit = 0;
    g_err.line = line;
    snprintf(g_err.msg, sizeof(g_err.msg), "%s", msg);
}

void err_linef(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_err.set = 1;
    g_err.has_line = 1;
    g_err.is_exit = 0;
    g_err.line = line;
    vsnprintf(g_err.msg, sizeof(g_err.msg), fmt, ap);
    va_end(ap);
}

void err_exit(int code) {
    g_err.set = 1;
    g_err.has_line = 0;
    g_err.is_exit = 1;
    g_err.exit_code = code;
    snprintf(g_err.msg, sizeof(g_err.msg), "SystemExit(%d)", code);
}

Str *str_new(const char *s, size_t len) {
    Str *r = xmalloc(sizeof(Str));
    r->s = xstrndup(s, len);
    r->len = len;
    return r;
}

Str *str_from_cstr(const char *s) { return str_new(s, strlen(s)); }

const char *str_cstr(const Str *s) { return s->s; }

int str_eq(const Str *a, const Str *b) {
    return a->len == b->len && memcmp(a->s, b->s, a->len) == 0;
}

static int u8_seq_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

size_t u8_count(const char *s, size_t len) {
    size_t n = 0;
    for (size_t i = 0; i < len;) {
        i += (size_t)u8_seq_len((unsigned char)s[i]);
        n++;
    }
    return n;
}

const char *u8_at(const char *s, size_t len, size_t idx, size_t *clen) {
    size_t i = 0, k = 0;
    while (i < len) {
        int l = u8_seq_len((unsigned char)s[i]);
        if (k == idx) {
            *clen = (size_t)l;
            return s + i;
        }
        i += (size_t)l;
        k++;
    }
    *clen = 0;
    return NULL;
}

static uint32_t u8_decode(const unsigned char *p, int l) {
    switch (l) {
    case 1: return p[0];
    case 2: return ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    case 3: return ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    default: return ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
                    ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    }
}

char *u8_encode(uint32_t cp) {
    char *b = xmalloc(5);
    if (cp < 0x80) {
        b[0] = (char)cp;
        b[1] = 0;
    } else if (cp < 0x800) {
        b[0] = (char)(0xC0 | (cp >> 6));
        b[1] = (char)(0x80 | (cp & 0x3F));
        b[2] = 0;
    } else if (cp < 0x10000) {
        b[0] = (char)(0xE0 | (cp >> 12));
        b[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[2] = (char)(0x80 | (cp & 0x3F));
        b[3] = 0;
    } else {
        b[0] = (char)(0xF0 | (cp >> 18));
        b[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        b[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        b[3] = (char)(0x80 | (cp & 0x3F));
        b[4] = 0;
    }
    return b;
}

static uint32_t cp_upper(uint32_t c) {
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 0x430 && c <= 0x44F) return c - 32;
    if (c == 0x451) return 0x401;
    return c;
}

static uint32_t cp_lower(uint32_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 0x410 && c <= 0x42F) return c + 32;
    if (c == 0x401) return 0x451;
    return c;
}

static char *u8_map_case(const char *s, size_t len, int upper) {
    SB b;
    sb_init(&b);
    for (size_t i = 0; i < len;) {
        int l = u8_seq_len((unsigned char)s[i]);
        uint32_t cp = u8_decode((const unsigned char *)s + i, l);
        uint32_t m = upper ? cp_upper(cp) : cp_lower(cp);
        if (m == cp) {
            sb_add(&b, s + i, (size_t)l);
        } else {
            char *e = u8_encode(m);
            sb_adds(&b, e);
        }
        i += (size_t)l;
    }
    return sb_take(&b, NULL);
}

char *u8_upper(const char *s, size_t len) { return u8_map_case(s, len, 1); }
char *u8_lower(const char *s, size_t len) { return u8_map_case(s, len, 0); }

static int is_space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

char *u8_trim(const char *s, size_t len) {
    size_t a = 0, b = len;
    while (a < b && is_space((unsigned char)s[a])) a++;
    while (b > a && is_space((unsigned char)s[b - 1])) b--;
    return xstrndup(s + a, b - a);
}

Value v_null(void) {
    Value v;
    memset(&v, 0, sizeof(v));
    v.t = V_NULL;
    return v;
}

Value v_bool(int b) {
    Value v = v_null();
    v.t = V_BOOL;
    v.as.b = b != 0;
    return v;
}

Value v_int_i64(int64_t n) {
    Value v = v_null();
    v.t = V_INT;
    v.as.i = bi_from_i64(n);
    return v;
}

Value v_int(BigInt *n) {
    Value v = v_null();
    v.t = V_INT;
    v.as.i = n;
    return v;
}

Value v_float(double f) {
    Value v = v_null();
    v.t = V_FLOAT;
    v.as.f = f;
    return v;
}

Value v_str_take(char *s, size_t len) {
    Value v = v_null();
    v.t = V_STR;
    v.as.s = xmalloc(sizeof(Str));
    v.as.s->s = s;
    v.as.s->len = len;
    return v;
}

Value v_str(const char *s) {
    Value v = v_null();
    v.t = V_STR;
    v.as.s = str_from_cstr(s);
    return v;
}

Value v_list(void) {
    Value v = v_null();
    v.t = V_LIST;
    v.as.l = xmalloc(sizeof(List));
    v.as.l->n = v.as.l->cap = 0;
    v.as.l->items = NULL;
    return v;
}

Value v_map(void) {
    Value v = v_null();
    v.t = V_MAP;
    v.as.m = xmalloc(sizeof(Map));
    v.as.m->n = v.as.m->cap = 0;
    v.as.m->pairs = NULL;
    return v;
}

Value v_func(Func *f) {
    Value v = v_null();
    v.t = V_FUNC;
    v.as.fn = f;
    return v;
}

void list_push(List *l, Value v) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = xrealloc(l->items, sizeof(Value) * (size_t)l->cap);
    }
    l->items[l->n++] = v;
}

Value list_get(List *l, int i) { return l->items[i]; }

void map_set(Map *m, const char *k, Value v) {
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->pairs[i].k, k) == 0) {
            m->pairs[i].v = v;
            return;
        }
    }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->pairs = xrealloc(m->pairs, sizeof(KV) * (size_t)m->cap);
    }
    m->pairs[m->n].k = xstrdup(k);
    m->pairs[m->n].v = v;
    m->n++;
}

Value map_get(Map *m, const char *k) {
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->pairs[i].k, k) == 0) return m->pairs[i].v;
    }
    return v_null();
}

int map_has(Map *m, const char *k) {
    for (int i = 0; i < m->n; i++) {
        if (strcmp(m->pairs[i].k, k) == 0) return 1;
    }
    return 0;
}

void sb_init(SB *b) {
    b->cap = 64;
    b->len = 0;
    b->buf = xmalloc(b->cap);
    b->buf[0] = 0;
}

static void sb_grow(SB *b, size_t need) {
    while (b->len + need + 1 > b->cap) b->cap *= 2;
    b->buf = xrealloc(b->buf, b->cap);
}

void sb_add(SB *b, const char *s, size_t n) {
    sb_grow(b, n);
    memcpy(b->buf + b->len, s, n);
    b->len += n;
    b->buf[b->len] = 0;
}

void sb_adds(SB *b, const char *s) { sb_add(b, s, strlen(s)); }

void sb_addch(SB *b, char c) { sb_add(b, &c, 1); }

void sb_addf(SB *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (need > 0) {
        sb_grow(b, (size_t)need);
        vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap2);
        b->len += (size_t)need;
    }
    va_end(ap2);
}

char *sb_take(SB *b, size_t *len) {
    if (len) *len = b->len;
    return b->buf;
}

static void fmt_shortest_e(double f, char *out, size_t cap) {
    for (int p = 0; p <= 16; p++) {
        snprintf(out, cap, "%.*e", p, f);
        if (strtod(out, NULL) == f) return;
    }
}

static char *float_str_prec(double f, int compact) {
    if (isnan(f)) return xstrdup("nan");
    if (isinf(f)) return xstrdup(f > 0 ? "inf" : "-inf");
    if (compact) {
        if (f == 0) return xstrdup("0");
        char buf[64];
        snprintf(buf, sizeof(buf), "%.0f", f);

        return xstrdup(buf);
    }
    char buf[64];
    fmt_shortest_e(f, buf, sizeof(buf));
    char *e = strchr(buf, 'e');
    if (!e) return xstrdup(buf);
    *e = 0;
    int exp = atoi(e + 1);
    int neg = buf[0] == '-';
    const char *mant = buf + (neg ? 1 : 0);
    char digits[64];
    int nd = 0;
    for (const char *p = mant; *p; p++) {
        if (*p >= '0' && *p <= '9') digits[nd++] = *p;
    }
    while (nd > 1 && digits[nd - 1] == '0') nd--;
    digits[nd] = 0;
    SB b;
    sb_init(&b);
    if (neg) sb_addch(&b, '-');
    if (exp < -4 || exp >= 16) {
        sb_addch(&b, digits[0]);
        if (nd > 1) {
            sb_addch(&b, '.');
            sb_add(&b, digits + 1, (size_t)(nd - 1));
        }
        if (exp < 0) sb_addf(&b, "e-%02d", -exp);
        else sb_addf(&b, "e+%02d", exp);
    } else if (exp >= nd) {
        sb_add(&b, digits, (size_t)nd);
        for (int i = 0; i < exp - nd; i++) sb_addch(&b, '0');
        sb_adds(&b, ".0");
    } else if (exp >= 0) {
        sb_add(&b, digits, (size_t)(exp + 1));
        sb_addch(&b, '.');
        sb_add(&b, digits + exp + 1, (size_t)(nd - exp - 1));
    } else {
        sb_adds(&b, "0.");
        for (int i = 0; i < -exp - 1; i++) sb_addch(&b, '0');
        sb_add(&b, digits, (size_t)nd);
    }
    return sb_take(&b, NULL);
}

char *nformat(double f) {
    if (isnan(f) || isinf(f) || f != trunc(f)) return float_str_prec(f, 0);
    return float_str_prec(f, 1);
}

char *nformat_repr(double f) { return float_str_prec(f, 0); }

static char *quo(const char *s) {
    SB b;
    sb_init(&b);
    sb_addch(&b, '"');
    for (const char *p = s; *p; p++) {
        if (*p == '\\') sb_adds(&b, "\\\\");
        else if (*p == '"') sb_adds(&b, "\\\"");
        else sb_addch(&b, *p);
    }
    sb_addch(&b, '"');
    return sb_take(&b, NULL);
}

static void inner_to(SB *b, Value v) {
    switch (v.t) {
    case V_NULL: sb_adds(b, "null"); break;
    case V_BOOL: sb_adds(b, v.as.b ? "true" : "false"); break;
    case V_INT: sb_adds(b, bi_to_str(v.as.i)); break;
    case V_FLOAT: sb_adds(b, nformat(v.as.f)); break;
    case V_STR: {
        char *q = quo(v.as.s->s);
        sb_adds(b, q);
        break;
    }
    case V_LIST:
        sb_addch(b, '[');
        for (int i = 0; i < v.as.l->n; i++) {
            if (i) sb_adds(b, ", ");
            inner_to(b, v.as.l->items[i]);
        }
        sb_addch(b, ']');
        break;
    case V_MAP:
        sb_addch(b, '{');
        for (int i = 0; i < v.as.m->n; i++) {
            if (i) sb_adds(b, ", ");
            char *q = quo(v.as.m->pairs[i].k);
            sb_adds(b, q);
            sb_adds(b, ": ");
            inner_to(b, v.as.m->pairs[i].v);
        }
        sb_addch(b, '}');
        break;
    default: sb_adds(b, "<function>"); break;
    }
}

Value value_inner(Value v) {
    SB b;
    sb_init(&b);
    inner_to(&b, v);
    return v_str_take(b.buf, b.len);
}

static void str_to(SB *b, Value v) {
    switch (v.t) {
    case V_NULL: sb_adds(b, "null"); break;
    case V_BOOL: sb_adds(b, v.as.b ? "true" : "false"); break;
    case V_INT: sb_adds(b, bi_to_str(v.as.i)); break;
    case V_FLOAT: sb_adds(b, nformat(v.as.f)); break;
    case V_STR: sb_add(b, v.as.s->s, v.as.s->len); break;
    case V_LIST:
    case V_MAP: {
        SB t;
        sb_init(&t);
        inner_to(&t, v);
        sb_add(b, t.buf, t.len);
        break;
    }
    default: sb_adds(b, "<function>"); break;
    }
}

Value value_str(Value v) {
    SB b;
    sb_init(&b);
    str_to(&b, v);
    return v_str_take(b.buf, b.len);
}

static void py_repr_to(SB *b, const char *s) {
    size_t len = strlen(s);
    int has_sq = strchr(s, '\'') != NULL;
    int has_dq = strchr(s, '"') != NULL;
    char q = (has_sq && !has_dq) ? '"' : '\'';
    sb_addch(b, q);
    for (size_t i = 0; i < len;) {
        int l = u8_seq_len((unsigned char)s[i]);
        uint32_t cp = u8_decode((const unsigned char *)s + i, l);
        if (cp == (uint32_t)(unsigned char)q) {
            sb_addch(b, '\\');
            sb_addch(b, q);
        } else if (cp == '\\') {
            sb_adds(b, "\\\\");
        } else if (cp == '\n') {
            sb_adds(b, "\\n");
        } else if (cp == '\r') {
            sb_adds(b, "\\r");
        } else if (cp == '\t') {
            sb_adds(b, "\\t");
        } else if (cp >= 0x20 && cp != 0x7f) {
            sb_add(b, s + i, (size_t)l);
        } else if (cp < 0x100) {
            sb_addf(b, "\\x%02x", cp);
        } else if (cp < 0x10000) {
            sb_addf(b, "\\u%04x", cp);
        } else {
            sb_addf(b, "\\U%08x", cp);
        }
        i += (size_t)l;
    }
    sb_addch(b, q);
}

char *py_repr(const char *s) {
    SB b;
    sb_init(&b);
    py_repr_to(&b, s);
    return sb_take(&b, NULL);
}

Value value_py_str(Value v) {
    SB b;
    sb_init(&b);
    switch (v.t) {
    case V_NULL: sb_adds(&b, "None"); break;
    case V_BOOL: sb_adds(&b, v.as.b ? "True" : "False"); break;
    case V_INT: sb_adds(&b, bi_to_str(v.as.i)); break;
    case V_FLOAT: sb_adds(&b, nformat_repr(v.as.f)); break;
    case V_STR: sb_add(&b, v.as.s->s, v.as.s->len); break;
    case V_LIST:
        sb_addch(&b, '[');
        for (int i = 0; i < v.as.l->n; i++) {
            if (i) sb_adds(&b, ", ");
            Value e = v.as.l->items[i];
            if (e.t == V_STR) py_repr_to(&b, e.as.s->s);
            else {
                Value s = value_py_str(e);
                sb_add(&b, s.as.s->s, s.as.s->len);
            }
        }
        sb_addch(&b, ']');
        break;
    case V_MAP:
        sb_addch(&b, '{');
        for (int i = 0; i < v.as.m->n; i++) {
            if (i) sb_adds(&b, ", ");
            py_repr_to(&b, v.as.m->pairs[i].k);
            sb_adds(&b, ": ");
            Value e = v.as.m->pairs[i].v;
            if (e.t == V_STR) py_repr_to(&b, e.as.s->s);
            else {
                Value s = value_py_str(e);
                sb_add(&b, s.as.s->s, s.as.s->len);
            }
        }
        sb_addch(&b, '}');
        break;
    default: sb_adds(&b, "<function>"); break;
    }
    return v_str_take(b.buf, b.len);
}

int value_truthy(Value v) {
    switch (v.t) {
    case V_NULL: return 0;
    case V_BOOL: return v.as.b;
    case V_INT: return !bi_is_zero(v.as.i);
    case V_FLOAT: return v.as.f != 0;
    case V_STR: return v.as.s->len > 0;
    case V_LIST: return v.as.l->n > 0;
    case V_MAP: return v.as.m->n > 0;
    default: return 1;
    }
}

const char *type_of(Value v) {
    switch (v.t) {
    case V_NULL: return "null";
    case V_BOOL: return "boolean";
    case V_INT:
    case V_FLOAT: return "number";
    case V_STR: return "string";
    case V_LIST: return "array";
    case V_MAP: return "object";
    default: return "function";
    }
}

const char *type_name(Value v) {
    switch (v.t) {
    case V_NULL: return "NoneType";
    case V_BOOL: return "bool";
    case V_INT: return "int";
    case V_FLOAT: return "float";
    case V_STR: return "str";
    case V_LIST: return "list";
    case V_MAP: return "dict";
    default: return "function";
    }
}

int value_is_number(Value v) { return v.t == V_INT || v.t == V_FLOAT; }
