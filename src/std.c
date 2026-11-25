
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <windows.h>
#include <sys/stat.h>

static Value fail(const char *msg) {
    err_msg(msg);
    return v_null();
}

static Value failf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_err.set = 1;
    g_err.has_line = 0;
    g_err.is_exit = 0;
    vsnprintf(g_err.msg, sizeof(g_err.msg), fmt, ap);
    va_end(ap);
    return v_null();
}

static int need(int argc, int n, const char *name) {
    if (argc != n) {
        err_msgf("%s() expects %d argument(s), got %d", name, n, argc);
        return 0;
    }
    return 1;
}

static int need_range(int argc, int lo, int hi, const char *name, const char *form) {
    if (argc < lo || argc > hi) {
        err_msgf("%s() expects %s", name, form);
        return 0;
    }
    return 1;
}

static List *arr_arg(int argc, Value *argv, const char *name, int idx) {
    if (idx < argc && argv[idx].t == V_LIST) return argv[idx].as.l;
    err_msgf("%s() argument must be an array", name);
    return NULL;
}

static Value str_attr(Value v, const char *attr) {
    return failf("'%s' object has no attribute '%s'", type_name(v), attr);
}

static const char *need_str_attr(Value v, const char *attr) {
    if (v.t == V_STR) return v.as.s->s;
    str_attr(v, attr);
    return NULL;
}

static int num_f64(Value v, double *out) {
    switch (v.t) {
    case V_BOOL: *out = v.as.b ? 1 : 0; return 1;
    case V_INT: *out = bi_to_f64(v.as.i); return 1;
    case V_FLOAT: *out = v.as.f; return 1;
    default: return 0;
    }
}

static int is_num(Value v) { return v.t == V_BOOL || v.t == V_INT || v.t == V_FLOAT; }

static BigInt *int_of(Value v);

static int is_zero_num(Value v) {
    switch (v.t) {
    case V_BOOL: return !v.as.b;
    case V_INT: return bi_is_zero(v.as.i);
    case V_FLOAT: return v.as.f == 0;
    default: return 0;
    }
}

static const char *not_nums(const char *op, Value a, Value b) {
    err_msgf("unsupported operand type(s) for %s: '%s' and '%s'", op, type_name(a), type_name(b));
    return NULL;
}

static BigInt *num_int(Value v) {
    switch (v.t) {
    case V_BOOL: return bi_from_i64(v.as.b);
    case V_INT: return v.as.i;
    default: return NULL;
    }
}

Value op_add(Value a, Value b) {
    BigInt *ai = num_int(a), *bi = num_int(b);
    if (ai && bi) return v_int(bi_add(ai, bi));
    double x, y;
    if (num_f64(a, &x) && num_f64(b, &y)) return v_float(x + y);
    not_nums("+", a, b);
    return v_null();
}

Value op_sub(Value a, Value b) {
    BigInt *ai = num_int(a), *bi = num_int(b);
    if (ai && bi) return v_int(bi_sub(ai, bi));
    double x, y;
    if (num_f64(a, &x) && num_f64(b, &y)) return v_float(x - y);
    not_nums("-", a, b);
    return v_null();
}

static int repeat_count(BigInt *n) {
    if (bi_sign(n) <= 0) return 0;
    int64_t c;
    if (bi_is_i64(n, &c)) {
        if (c > (int64_t)1 << 31) return (int)1 << 31;
        return (int)c;
    }
    return (int)1 << 31;
}

Value op_mul(Value a, Value b) {
    int a_seq = a.t == V_STR || a.t == V_LIST;
    int b_seq = b.t == V_STR || b.t == V_LIST;
    if (a_seq || b_seq) {
        Value seq = a_seq ? a : b;
        Value rhs = a_seq ? b : a;
        BigInt *rn = num_int(rhs);
        if (!rn) {
            not_nums("*", a, b);
            return v_null();
        }
        int c = repeat_count(rn);
        if (seq.t == V_STR) {
            size_t len = (size_t)c * seq.as.s->len;
            char *out = xmalloc(len ? len : 1);
            size_t pos = 0;
            for (int i = 0; i < c; i++) {
                memcpy(out + pos, seq.as.s->s, seq.as.s->len);
                pos += seq.as.s->len;
            }
            return v_str_take(out, len);
        }
        if ((int64_t)seq.as.l->n * (int64_t)c > ((int64_t)1 << 31)) {
            err_msg("repetition too large");
            return v_null();
        }
        Value l = v_list();
        for (int i = 0; i < c; i++)
            for (int j = 0; j < seq.as.l->n; j++) list_push(l.as.l, seq.as.l->items[j]);
        return l;
    }
    BigInt *ai = num_int(a), *bi = num_int(b);
    if (ai && bi) return v_int(bi_mul(ai, bi));
    double x, y;
    if (num_f64(a, &x) && num_f64(b, &y)) return v_float(x * y);
    not_nums("*", a, b);
    return v_null();
}

Value op_div(Value a, Value b) {
    double x, y;
    if (num_f64(a, &x) && num_f64(b, &y)) {
        double r = x / y;
        if (isinf(r)) {
            err_msg("integer division result too large for a float");
            return v_null();
        }
        return v_float(r);
    }
    not_nums("/", a, b);
    return v_null();
}

Value op_mod(Value a, Value b) {
    BigInt *ai = num_int(a), *bi = num_int(b);
    if (ai && bi) {
        if (bi_is_zero(bi)) {
            err_msg("integer division or modulo by zero");
            return v_null();
        }
        BigInt *r = bi_rem(ai, bi);
        if (bi_sign(r) != 0 && (bi_sign(r) < 0) != (bi_sign(bi) < 0)) r = bi_add(r, bi);
        return v_int(r);
    }
    double x, y;
    if (num_f64(a, &x) && num_f64(b, &y)) {
        if (y == 0) {
            err_msg("float modulo");
            return v_null();
        }
        double r = fmod(x, y);
        if (r != 0 && (r < 0) != (y < 0)) r += y;
        return v_float(r);
    }
    not_nums("%", a, b);
    return v_null();
}

Value op_pow(Value a, Value b) {
    BigInt *ai = num_int(a), *bi = num_int(b);
    if (ai && bi) {
        if (bi_is_zero(ai)) {
            if (bi_sign(bi) > 0) return v_int_i64(0);
            if (bi_sign(bi) == 0) return v_int_i64(1);
            err_msg("0.0 cannot be raised to a negative power");
            return v_null();
        }
        if (bi_sign(bi) < 0) {
            double x = bi_to_f64(ai), y = bi_to_f64(bi);
            return v_float(pow(x, y));
        }
        int64_t av = 0;
        if (bi_is_i64(ai, &av)) {
            if (av == 1) return v_int_i64(1);
            if (av == -1) return v_int_i64(bi_bit(bi, 0) ? -1 : 1);
        }
        uint64_t est = bi_bitlen(ai) * bi_bitlen(bi);
        if (est > (uint64_t)1 << 22) {
            err_msg("exponent too large");
            return v_null();
        }
        return v_int(bi_pow(ai, bi));
    }
    double x, y;
    if (num_f64(a, &x) && num_f64(b, &y)) {
        if (x == 0 && y < 0) {
            err_msg("0.0 cannot be raised to a negative power");
            return v_null();
        }
        if (x < 0 && y != trunc(y)) {
            err_msg("negative number cannot be raised to a fractional power");
            return v_null();
        }
        return v_float(pow(x, y));
    }
    not_nums("**", a, b);
    return v_null();
}

Value op_neg(Value v) {
    if (v.t == V_INT || v.t == V_BOOL) {
        BigInt *n = num_int(v);
        if (n) return v_int(bi_neg(n));
    }
    if (v.t == V_FLOAT) return v_float(-v.as.f);
    err_msgf("bad operand type for unary -: '%s'", type_name(v));
    return v_null();
}

static Value op_abs(Value v) {
    switch (v.t) {
    case V_BOOL: return v_int_i64(v.as.b);
    case V_INT: return v_int(bi_abs(v.as.i));
    case V_FLOAT: return v_float(fabs(v.as.f));
    default:
        err_msgf("bad operand type for abs(): '%s'", type_name(v));
        return v_null();
    }
}

int value_equal(Value a, Value b) {
    if (a.t == V_NULL && b.t == V_NULL) return 1;
    if (a.t == V_NULL || b.t == V_NULL) return 0;
    if (is_num(a) && is_num(b)) {
        BigInt *ai = num_int(a), *bi = num_int(b);
        if (ai && bi) return bi_cmp(ai, bi) == 0;
        double x, y;
        if (num_f64(a, &x) && num_f64(b, &y)) {
            if (isnan(x) || isnan(y)) return 0;
            return x == y;
        }
    }
    if (is_num(a) != is_num(b)) return 0;
    if (a.t == V_STR && b.t == V_STR) return strcmp(a.as.s->s, b.as.s->s) == 0;
    if (a.t == V_STR || b.t == V_STR) return 0;
    if (a.t == V_LIST && b.t == V_LIST) {
        if (a.as.l->n != b.as.l->n) return 0;
        for (int i = 0; i < a.as.l->n; i++)
            if (!value_equal(a.as.l->items[i], b.as.l->items[i])) return 0;
        return 1;
    }
    if (a.t == V_LIST || b.t == V_LIST) return 0;
    if (a.t == V_MAP && b.t == V_MAP) {
        if (a.as.m->n != b.as.m->n) return 0;
        for (int i = 0; i < a.as.m->n; i++) {
            Value bv = map_get(b.as.m, a.as.m->pairs[i].k);
            if (!map_has(b.as.m, a.as.m->pairs[i].k)) return 0;
            if (!value_equal(a.as.m->pairs[i].v, bv)) return 0;
        }
        return 1;
    }
    if (a.t == V_MAP || b.t == V_MAP) return 0;
    if (a.t == V_FUNC && b.t == V_FUNC) return a.as.fn == b.as.fn;
    return 0;
}

static int not_comparable(const char *op, Value a, Value b) {
    err_msgf("'%s' not supported between instances of '%s' and '%s'", op, type_name(a), type_name(b));
    return 0;
}

int value_less(Value a, Value b) {
    if (is_num(a) && is_num(b)) {
        BigInt *ai = num_int(a), *bi = num_int(b);
        if (ai && bi) return bi_cmp(ai, bi) < 0;
        double x, y;
        if (num_f64(a, &x) && num_f64(b, &y)) {
            if (isnan(x) || isnan(y)) return 0;
            return x < y;
        }
    }
    if (is_num(a) != is_num(b)) return not_comparable("<", a, b);
    if (a.t == V_STR && b.t == V_STR) return strcmp(a.as.s->s, b.as.s->s) < 0;
    if (a.t == V_LIST && b.t == V_LIST) {
        int n = a.as.l->n < b.as.l->n ? a.as.l->n : b.as.l->n;
        for (int i = 0; i < n; i++) {
            if (!value_equal(a.as.l->items[i], b.as.l->items[i]))
                return value_less(a.as.l->items[i], b.as.l->items[i]);
        }
        return a.as.l->n < b.as.l->n;
    }
    return not_comparable("<", a, b);
}

static BigInt *bi_from_double(double f) {
    if (fabs(f) < 9.2e18) return bi_from_i64((int64_t)f);
    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f", f);
    return bi_from_str(buf);
}

static BigInt *int_of(Value v) {
    switch (v.t) {
    case V_BOOL: return bi_from_i64(v.as.b);
    case V_INT: return v.as.i;
    case V_FLOAT: {
        double t = trunc(v.as.f);
        return bi_from_double(t);
    }
    case V_STR: {
        const char *s0 = v.as.s->s;
        while (*s0 && (*s0 == ' ' || *s0 == '\t' || *s0 == '\n' || *s0 == '\r')) s0++;
        const char *p = s0;
        if (*p == '+' || *p == '-') p++;
        if (*p == 0) break;
        for (; *p; p++)
            if (*p < '0' || *p > '9') break;
        if (*p != 0) break;
        return bi_from_str(s0);
    }
    default:
        break;
    }
    Value s = v.t == V_STR ? v : value_py_str(v);
    err_msgf("invalid literal for int() with base 10: '%s'", s.as.s->s);
    return NULL;
}

static int num_guard(int argc, Value *argv, const char *name, Value *out) {
    if (!need(argc, 1, name)) return 0;
    if (!is_num(argv[0])) {
        err_msgf("%s() argument must be a number", name);
        return 0;
    }
    *out = argv[0];
    return 1;
}

static void write_out(const char *s) { fwrite(s, 1, strlen(s), g_it.out); }

static Value b_print(int argc, Value *argv) {
    SB b;
    sb_init(&b);
    for (int i = 0; i < argc; i++) {
        Value s = value_str(argv[i]);
        if (i) sb_addch(&b, ' ');
        sb_add(&b, s.as.s->s, s.as.s->len);
    }
    sb_addch(&b, '\n');
    write_out(b.buf);
    return v_null();
}

static Value b_input(int argc, Value *argv) {
    if (argc > 0) {
        Value s = value_str(argv[0]);
        write_out(s.as.s->s);
    }
    if (!g_it.in) return v_str("");
    char buf[4096];
    if (!fgets(buf, sizeof(buf), g_it.in)) return v_str("");
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    return v_str_take(xstrndup(buf, len), len);
}

static Value b_logger(const char *prefix, int argc, Value *argv) {
    SB b;
    sb_init(&b);
    sb_adds(&b, prefix);
    sb_addch(&b, ' ');
    for (int i = 0; i < argc; i++) {
        Value s = value_str(argv[i]);
        if (i) sb_addch(&b, ' ');
        sb_add(&b, s.as.s->s, s.as.s->len);
    }
    sb_addch(&b, '\n');
    write_out(b.buf);
    return v_null();
}

static Value b_typeof(int argc, Value *argv) {
    if (!need(argc, 1, "typeof")) return v_null();
    return v_str(type_of(argv[0]));
}

static Value b_to_string(int argc, Value *argv) {
    if (!need(argc, 1, "to_string")) return v_null();
    return value_str(argv[0]);
}

static Value b_to_number(int argc, Value *argv) {
    if (!need(argc, 1, "to_number")) return v_null();
    Value v = argv[0];
    if (v.t == V_BOOL) return v_int_i64(v.as.b);
    Value ps = value_py_str(v);
    char *trimmed = u8_trim(ps.as.s->s, ps.as.s->len);
    if (trimmed[0] == '+' || trimmed[0] == '-' || (trimmed[0] >= '0' && trimmed[0] <= '9')) {
        int all_digits = 1, has_digit = 0;
        for (const char *p = trimmed + (trimmed[0] == '+' || trimmed[0] == '-' ? 1 : 0); *p; p++) {
            if (*p < '0' || *p > '9') {
                all_digits = 0;
                break;
            }
            has_digit = 1;
        }
        if (all_digits && has_digit) return v_int(bi_from_str(trimmed));
    }
    char *end = NULL;
    double f = strtod(trimmed, &end);
    if (end && *end == 0 && end != trimmed) return v_float(f);
    Value s = value_str(v);
    return failf("Cannot convert %s to number", s.as.s->s);
}

static Value b_to_boolean(int argc, Value *argv) {
    if (!need(argc, 1, "to_boolean")) return v_null();
    return v_bool(value_truthy(argv[0]));
}

static Value b_is_check(int argc, Value *argv, int kind) {
    if (!need(argc, 1, "<lambda>")) return v_null();
    int r = 0;
    switch (kind) {
    case 0: r = value_is_number(argv[0]); break;
    case 1: r = argv[0].t == V_STR; break;
    case 2: r = argv[0].t == V_LIST; break;
    case 3: r = argv[0].t == V_MAP; break;
    case 4: r = argv[0].t == V_BOOL; break;
    case 5: r = argv[0].t == V_NULL; break;
    case 6: r = argv[0].t == V_FUNC; break;
    }
    return v_bool(r);
}

static int64_t slice_bound(BigInt *vv, int64_t size) {
    int64_t x = 0;
    if (!bi_is_i64(vv, &x)) x = bi_sign(vv) > 0 ? INT64_MAX : INT64_MIN;
    if (x < 0) {
        if (x < -size) return 0;
        x += size;
    } else if (x > size) {
        x = size;
    }
    return x;
}

static void slice_bounds(BigInt *startI, BigInt *endI, int64_t size, int64_t *a, int64_t *b) {
    *a = slice_bound(startI, size);
    *b = slice_bound(endI, size);
    if (*b < *a) *b = *a;
}

static Value b_length(int argc, Value *argv) {
    if (!need(argc, 1, "length")) return v_null();
    switch (argv[0].t) {
    case V_STR: return v_int_i64((int64_t)u8_count(argv[0].as.s->s, argv[0].as.s->len));
    case V_LIST: return v_int_i64(argv[0].as.l->n);
    case V_MAP: return v_int_i64(argv[0].as.m->n);
    default:
        return fail("length() needs a string, array or object");
    }
}

static Value b_split(int argc, Value *argv) {
    if (!need_range(argc, 1, 2, "split", "(string) or (string, separator)")) return v_null();
    if (argv[0].t != V_STR) return fail("split() first argument must be a string");
    Value out = v_list();
    if (argc == 1) {
        const char *s = argv[0].as.s->s;
        const char *p = s;
        while (*p) {
            while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f')) p++;
            if (!*p) break;
            const char *q = p;
            while (*q && !(*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r' || *q == '\v' || *q == '\f')) q++;
            list_push(out.as.l, v_str_take(xstrndup(p, (size_t)(q - p)), (size_t)(q - p)));
            p = q;
        }
        return out;
    }
    if (argv[1].t != V_STR) return failf("must be str or None, not %s", type_name(argv[1]));
    const char *s = argv[0].as.s->s;
    const char *sep = argv[1].as.s->s;
    size_t slen = strlen(s), seplen = strlen(sep);
    if (seplen == 0) {
        size_t n = u8_count(s, slen);
        for (size_t i = 0; i < n; i++) {
            size_t clen = 0;
            const char *p = u8_at(s, slen, i, &clen);
            list_push(out.as.l, v_str_take(xstrndup(p, clen), clen));
        }
        return out;
    }
    const char *p = s;
    while (1) {
        const char *hit = strstr(p, sep);
        if (!hit) {
            list_push(out.as.l, v_str(p));
            break;
        }
        list_push(out.as.l, v_str_take(xstrndup(p, (size_t)(hit - p)), (size_t)(hit - p)));
        p = hit + seplen;
    }
    return out;
}

static Value b_join(int argc, Value *argv) {
    if (!need(argc, 2, "join")) return v_null();
    if (argv[0].t != V_LIST) return fail("join() first argument must be an array");
    if (argv[1].t != V_STR) return str_attr(argv[1], "join");
    SB b;
    sb_init(&b);
    for (int i = 0; i < argv[0].as.l->n; i++) {
        Value s = value_str(argv[0].as.l->items[i]);
        if (i) sb_adds(&b, argv[1].as.s->s);
        sb_add(&b, s.as.s->s, s.as.s->len);
    }
    return v_str_take(b.buf, b.len);
}

static Value b_replace(int argc, Value *argv) {
    if (!need(argc, 3, "replace")) return v_null();
    if (argv[0].t != V_STR) return str_attr(argv[0], "replace");
    if (argv[1].t != V_STR) return failf("replace() argument 1 must be str, not %s", type_name(argv[1]));
    if (argv[2].t != V_STR) return failf("replace() argument 2 must be str, not %s", type_name(argv[2]));
    const char *s = argv[0].as.s->s;
    const char *old = argv[1].as.s->s;
    const char *neu = argv[2].as.s->s;
    size_t oldlen = strlen(old), neulen = strlen(neu);
    SB b;
    sb_init(&b);
    if (oldlen == 0) {
        sb_adds(&b, s);
        return v_str_take(b.buf, b.len);
    }
    const char *p = s;
    while (1) {
        const char *hit = strstr(p, old);
        if (!hit) {
            sb_adds(&b, p);
            break;
        }
        sb_add(&b, p, (size_t)(hit - p));
        sb_add(&b, neu, neulen);
        p = hit + oldlen;
    }
    return v_str_take(b.buf, b.len);
}

static Value b_substring(int argc, Value *argv) {
    if (!need_range(argc, 2, 3, "substring", "(string, start[, end])")) return v_null();
    if (argv[0].t != V_STR) return failf("'%s' object is not subscriptable", type_name(argv[0]));
    BigInt *start = int_of(argv[1]);
    if (!start) return v_null();
    int64_t n = (int64_t)u8_count(argv[0].as.s->s, argv[0].as.s->len);
    BigInt *end = bi_from_i64(n);
    if (argc == 3) {
        end = int_of(argv[2]);
        if (!end) return v_null();
    }
    int64_t a, b;
    slice_bounds(start, end, n, &a, &b);
    if (a >= b) return v_str("");
    SB out;
    sb_init(&out);
    size_t i = 0;
    for (int64_t k = 0; k < b; k++) {
        size_t clen = 0;
        const char *p = u8_at(argv[0].as.s->s, argv[0].as.s->len, (size_t)k, &clen);
        if (k >= a) sb_add(&out, p, clen);
    }
    return v_str_take(out.buf, out.len);
}

static Value b_trim(int argc, Value *argv) {
    if (!need(argc, 1, "trim")) return v_null();
    if (argv[0].t != V_STR) return str_attr(argv[0], "trim");
    char *t = u8_trim(argv[0].as.s->s, argv[0].as.s->len);
    return v_str(t);
}

static Value b_upper(int argc, Value *argv) {
    if (!need(argc, 1, "uppercase")) return v_null();
    if (argv[0].t != V_STR) return str_attr(argv[0], "upper");
    return v_str(u8_upper(argv[0].as.s->s, argv[0].as.s->len));
}

static Value b_lower(int argc, Value *argv) {
    if (!need(argc, 1, "lowercase")) return v_null();
    if (argv[0].t != V_STR) return str_attr(argv[0], "lower");
    return v_str(u8_lower(argv[0].as.s->s, argv[0].as.s->len));
}

static Value b_startswith(int argc, Value *argv) {
    if (!need(argc, 2, "startswith")) return v_null();
    if (argv[0].t != V_STR) return str_attr(argv[0], "startswith");
    if (argv[1].t != V_STR) return failf("startswith first arg must be str or a tuple of str, not %s", type_name(argv[1]));
    return v_bool(strncmp(argv[0].as.s->s, argv[1].as.s->s, strlen(argv[1].as.s->s)) == 0);
}

static Value b_endswith(int argc, Value *argv) {
    if (!need(argc, 2, "endswith")) return v_null();
    if (argv[0].t != V_STR) return str_attr(argv[0], "endswith");
    if (argv[1].t != V_STR) return failf("endswith first arg must be str or a tuple of str, not %s", type_name(argv[1]));
    size_t sl = strlen(argv[0].as.s->s), pl = strlen(argv[1].as.s->s);
    return v_bool(pl <= sl && strcmp(argv[0].as.s->s + sl - pl, argv[1].as.s->s) == 0);
}

static Value b_repeat(int argc, Value *argv) {
    if (!need(argc, 2, "repeat")) return v_null();
    BigInt *n = int_of(argv[1]);
    if (!n) return v_null();
    return op_mul(argv[0], v_int(n));
}

static Value b_char_code(int argc, Value *argv) {
    if (!need(argc, 1, "char_code")) return v_null();
    if (argv[0].t != V_STR) return failf("ord() expected string of length 1, but %s found", type_name(argv[0]));
    size_t n = u8_count(argv[0].as.s->s, argv[0].as.s->len);
    if (n != 1) return failf("ord() expected a character, but string of length %d found", (int)n);
    const unsigned char *p = (const unsigned char *)argv[0].as.s->s;
    uint32_t cp = *p;
    if ((*p & 0xE0) == 0xC0) cp = ((uint32_t)(p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    else if ((*p & 0xF0) == 0xE0) cp = ((uint32_t)(p[0] & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    else if ((*p & 0xF8) == 0xF0) cp = ((uint32_t)(p[0] & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) | ((uint32_t)(p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return v_int_i64(cp);
}

static Value b_from_char_code(int argc, Value *argv) {
    if (!need(argc, 1, "from_char_code")) return v_null();
    BigInt *n = int_of(argv[0]);
    if (!n) return v_null();
    int64_t c;
    if (!bi_is_i64(n, &c) || c < 0 || c > 0x10FFFF) return fail("chr() arg not in range(0x110000)");
    if (c >= 0xD800 && c <= 0xDFFF) c = 0xFFFD;
    return v_str(u8_encode((uint32_t)c));
}

static Value b_push(int argc, Value *argv) {
    if (!need(argc, 2, "push")) return v_null();
    List *l = arr_arg(argc, argv, "push", 0);
    if (!l) return v_null();
    list_push(l, argv[1]);
    return v_int_i64(l->n);
}

static Value b_pop(int argc, Value *argv) {
    if (!need(argc, 1, "pop")) return v_null();
    List *l = arr_arg(argc, argv, "pop", 0);
    if (!l) return v_null();
    if (l->n == 0) return fail("pop() from empty array");
    Value last = l->items[l->n - 1];
    l->n--;
    return last;
}

static Value b_shift(int argc, Value *argv) {
    if (!need(argc, 1, "shift")) return v_null();
    List *l = arr_arg(argc, argv, "shift", 0);
    if (!l) return v_null();
    if (l->n == 0) return fail("shift() from empty array");
    Value first = l->items[0];
    memmove(&l->items[0], &l->items[1], sizeof(Value) * (size_t)(l->n - 1));
    l->n--;
    return first;
}

static Value b_unshift(int argc, Value *argv) {
    if (!need(argc, 2, "unshift")) return v_null();
    List *l = arr_arg(argc, argv, "unshift", 0);
    if (!l) return v_null();
    l->items = xrealloc(l->items, sizeof(Value) * (size_t)(l->n + 1));
    memmove(&l->items[1], &l->items[0], sizeof(Value) * (size_t)l->n);
    l->items[0] = argv[1];
    l->n++;
    return v_int_i64(l->n);
}

static Value b_slice(int argc, Value *argv) {
    if (!need_range(argc, 2, 3, "slice", "(array, start[, end])")) return v_null();
    List *l = arr_arg(argc, argv, "slice", 0);
    if (!l) return v_null();
    BigInt *start = int_of(argv[1]);
    if (!start) return v_null();
    int64_t n = l->n;
    BigInt *end = bi_from_i64(n);
    if (argc == 3) {
        end = int_of(argv[2]);
        if (!end) return v_null();
    }
    int64_t a, b;
    slice_bounds(start, end, n, &a, &b);
    Value out = v_list();
    for (int64_t i = a; i < b; i++) list_push(out.as.l, l->items[i]);
    return out;
}

static Value b_splice(int argc, Value *argv) {
    if (argc < 2) return fail("splice() expects (array, start[, count])");
    List *l = arr_arg(argc, argv, "splice", 0);
    if (!l) return v_null();
    BigInt *start = int_of(argv[1]);
    if (!start) return v_null();
    int64_t n = l->n;
    BigInt *count = bi_sub(bi_from_i64(n), start);
    if (argc >= 3) {
        count = int_of(argv[2]);
        if (!count) return v_null();
    }
    BigInt *end = bi_add(start, count);
    int64_t a, ae;
    slice_bounds(start, end, n, &a, &ae);
    Value removed = v_list();
    for (int64_t i = a; i < ae; i++) list_push(removed.as.l, l->items[i]);
    memmove(&l->items[a], &l->items[ae], sizeof(Value) * (size_t)(n - ae));
    l->n -= (int)(ae - a);
    return removed;
}

static Value b_reverse(int argc, Value *argv) {
    if (!need(argc, 1, "reverse")) return v_null();
    List *l = arr_arg(argc, argv, "reverse", 0);
    if (!l) return v_null();
    for (int i = 0, j = l->n - 1; i < j; i++, j--) {
        Value t = l->items[i];
        l->items[i] = l->items[j];
        l->items[j] = t;
    }
    return argv[0];
}

static int stable_sort_less(Value a, Value b) {
    int r = value_less(a, b);
    if (g_err.set) err_clear();
    return r;
}

static Value b_sort(int argc, Value *argv) {
    if (!need_range(argc, 1, 2, "sort", "(array[, keyfn])")) return v_null();
    List *l = arr_arg(argc, argv, "sort", 0);
    if (!l) return v_null();
    if (argc == 2) {
        Value *keys = xmalloc(sizeof(Value) * (size_t)(l->n ? l->n : 1));
        for (int i = 0; i < l->n; i++) {
            keys[i] = call_value(argv[1], 1, &l->items[i], -1);
            if (g_err.set) return v_null();
        }
        for (int i = 1; i < l->n; i++) {
            Value kk = keys[i];
            Value it = l->items[i];
            int j = i - 1;
            while (j >= 0 && stable_sort_less(kk, keys[j])) {
                keys[j + 1] = keys[j];
                l->items[j + 1] = l->items[j];
                j--;
            }
            keys[j + 1] = kk;
            l->items[j + 1] = it;
        }
        return argv[0];
    }
    for (int i = 1; i < l->n; i++) {
        Value it = l->items[i];
        int j = i - 1;
        while (j >= 0 && stable_sort_less(it, l->items[j])) {
            l->items[j + 1] = l->items[j];
            j--;
        }
        l->items[j + 1] = it;
    }
    if (stable_sort_less(argv[0], v_null())) {}
    return argv[0];}

static Value b_includes(int argc, Value *argv) {
    if (!need(argc, 2, "includes")) return v_null();
    if (argv[0].t == V_LIST) {
        for (int i = 0; i < argv[0].as.l->n; i++)
            if (value_equal(argv[0].as.l->items[i], argv[1])) return v_bool(1);
        return v_bool(0);
    }
    if (argv[0].t == V_STR) {
        if (argv[1].t != V_STR) return failf("in <string> requires string as left operand, not %s", type_name(argv[1]));
        return v_bool(strstr(argv[0].as.s->s, argv[1].as.s->s) != NULL);
    }
    return fail("includes() first argument must be an array or string");
}

static Value b_index_of(int argc, Value *argv) {
    if (!need(argc, 2, "indexOf")) return v_null();
    if (argv[0].t == V_LIST) {
        for (int i = 0; i < argv[0].as.l->n; i++)
            if (value_equal(argv[0].as.l->items[i], argv[1])) return v_int_i64(i);
        return v_int_i64(-1);
    }
    if (argv[0].t == V_STR) {
        if (argv[1].t != V_STR) return failf("must be str, not %s", type_name(argv[1]));
        const char *hit = strstr(argv[0].as.s->s, argv[1].as.s->s);
        return v_int_i64(hit ? (int64_t)(hit - argv[0].as.s->s) : -1);
    }
    return fail("indexOf() first argument must be an array or string");
}

static Value b_map(int argc, Value *argv) {
    if (!need(argc, 2, "map")) return v_null();
    List *l = arr_arg(argc, argv, "map", 0);
    if (!l) return v_null();
    Value out = v_list();
    for (int i = 0; i < l->n; i++) {
        Value r = call_value(argv[1], 1, &l->items[i], -1);
        if (g_err.set) return v_null();
        list_push(out.as.l, r);
    }
    return out;
}

static Value b_filter(int argc, Value *argv) {
    if (!need(argc, 2, "filter")) return v_null();
    List *l = arr_arg(argc, argv, "filter", 0);
    if (!l) return v_null();
    Value out = v_list();
    for (int i = 0; i < l->n; i++) {
        Value r = call_value(argv[1], 1, &l->items[i], -1);
        if (g_err.set) return v_null();
        if (value_truthy(r)) list_push(out.as.l, l->items[i]);
    }
    return out;
}

static Value b_reduce(int argc, Value *argv) {
    if (!need_range(argc, 2, 3, "reduce", "(array, fn[, initial])")) return v_null();
    List *l = arr_arg(argc, argv, "reduce", 0);
    if (!l) return v_null();
    Value acc = v_null();
    int start = 0;
    if (argc == 3) {
        acc = argv[2];
    } else {
        if (l->n == 0) return fail("reduce() of empty array with no initial value");
        acc = l->items[0];
        start = 1;
    }
    for (int i = start; i < l->n; i++) {
        Value args[2] = {acc, l->items[i]};
        acc = call_value(argv[1], 2, args, -1);
        if (g_err.set) return v_null();
    }
    return acc;
}

static Value b_find(int argc, Value *argv) {
    if (!need(argc, 2, "find")) return v_null();
    List *l = arr_arg(argc, argv, "find", 0);
    if (!l) return v_null();
    for (int i = 0; i < l->n; i++) {
        Value r = call_value(argv[1], 1, &l->items[i], -1);
        if (g_err.set) return v_null();
        if (value_truthy(r)) return l->items[i];
    }
    return v_null();
}

static Value b_each(int argc, Value *argv) {
    if (!need(argc, 2, "each")) return v_null();
    List *l = arr_arg(argc, argv, "each", 0);
    if (!l) return v_null();
    for (int i = 0; i < l->n; i++) {
        Value r = call_value(argv[1], 1, &l->items[i], -1);
        (void)r;
        if (g_err.set) return v_null();
    }
    return v_null();
}

static Value b_sum(int argc, Value *argv) {
    if (!need(argc, 1, "sum")) return v_null();
    List *l = arr_arg(argc, argv, "sum", 0);
    if (!l) return v_null();
    Value acc = v_int_i64(0);
    for (int i = 0; i < l->n; i++) {
        acc = op_add(acc, l->items[i]);
        if (g_err.set) return v_null();
    }
    return acc;
}

static Value b_range(int argc, Value *argv) {
    if (argc < 1 || argc > 3) return fail("range() expects 1-3 arguments");
    BigInt *parts[3] = {NULL, NULL, NULL};
    for (int i = 0; i < argc; i++) {
        parts[i] = int_of(argv[i]);
        if (!parts[i]) return v_null();
    }
    BigInt *start = argc == 1 ? bi_from_i64(0) : parts[0];
    BigInt *stop = argc == 1 ? parts[0] : parts[1];
    BigInt *step = argc == 3 ? parts[2] : bi_from_i64(1);
    if (bi_is_zero(step)) return fail("range() arg 3 must not be zero");
    Value out = v_list();
    int sign = bi_sign(step);
    BigInt *cur = bi_copy(start);
    for (;;) {
        int done = sign > 0 ? bi_cmp(cur, stop) >= 0 : bi_cmp(cur, stop) <= 0;
        if (done) break;
        if (out.as.l->n > (1 << 26)) return fail("range() result too large");
        list_push(out.as.l, v_int(bi_copy(cur)));
        cur = bi_add(cur, step);
    }
    return out;
}

static Value b_keys(int argc, Value *argv) {
    if (!need(argc, 1, "keys")) return v_null();
    if (argv[0].t != V_MAP) return fail("keys() argument must be an object");
    Value out = v_list();
    for (int i = 0; i < argv[0].as.m->n; i++) list_push(out.as.l, v_str(argv[0].as.m->pairs[i].k));
    return out;
}

static Value b_values(int argc, Value *argv) {
    if (!need(argc, 1, "values")) return v_null();
    if (argv[0].t != V_MAP) return fail("values() argument must be an object");
    Value out = v_list();
    for (int i = 0; i < argv[0].as.m->n; i++) list_push(out.as.l, argv[0].as.m->pairs[i].v);
    return out;
}

static Value b_entries(int argc, Value *argv) {
    if (!need(argc, 1, "entries")) return v_null();
    if (argv[0].t != V_MAP) return fail("entries() argument must be an object");
    Value out = v_list();
    for (int i = 0; i < argv[0].as.m->n; i++) {
        Value pair = v_list();
        list_push(pair.as.l, v_str(argv[0].as.m->pairs[i].k));
        list_push(pair.as.l, argv[0].as.m->pairs[i].v);
        list_push(out.as.l, pair);
    }
    return out;
}

static Value b_has(int argc, Value *argv) {
    if (!need(argc, 2, "has")) return v_null();
    if (argv[0].t != V_MAP) return fail("has() first argument must be an object");
    if (argv[1].t != V_STR) return v_bool(0);
    return v_bool(map_has(argv[0].as.m, argv[1].as.s->s));
}

static Value b_merge(int argc, Value *argv) {
    if (!need(argc, 2, "merge")) return v_null();
    if (argv[0].t != V_MAP || argv[1].t != V_MAP) return fail("merge() needs two objects");
    Value out = v_map();
    for (int i = 0; i < argv[0].as.m->n; i++) map_set(out.as.m, argv[0].as.m->pairs[i].k, argv[0].as.m->pairs[i].v);
    for (int i = 0; i < argv[1].as.m->n; i++) map_set(out.as.m, argv[1].as.m->pairs[i].k, argv[1].as.m->pairs[i].v);
    return out;
}

static Value b_clone(int argc, Value *argv) {
    if (!need(argc, 1, "clone")) return v_null();
    if (argv[0].t == V_MAP) {
        Value out = v_map();
        for (int i = 0; i < argv[0].as.m->n; i++) map_set(out.as.m, argv[0].as.m->pairs[i].k, argv[0].as.m->pairs[i].v);
        return out;
    }
    if (argv[0].t == V_LIST) {
        Value out = v_list();
        for (int i = 0; i < argv[0].as.l->n; i++) list_push(out.as.l, argv[0].as.l->items[i]);
        return out;
    }
    return argv[0];
}

static Value b_abs(int argc, Value *argv) {
    Value v;
    if (!num_guard(argc, argv, "abs", &v)) return v_null();
    return op_abs(v);
}

static double round_half_even(double x) { return nearbyint(x); }

static Value round_value(Value v, int *nd) {
    if (v.t == V_INT || v.t == V_BOOL) {
        BigInt *n = num_int(v);
        return v_int(n ? bi_copy(n) : bi_from_i64(0));
    }
    double f = v.as.f;
    if (nd == NULL) {
        if (isnan(f)) return v_float(f);
        return v_int(bi_from_double(round_half_even(f)));
    }
    if (isnan(f) || isinf(f)) return v_float(f);
    int d = *nd;
    if (d >= 0) {
        double scale = pow(10, d);
        return v_float(round_half_even(f * scale) / scale);
    }
    double scale = pow(10, -d);
    return v_int(bi_from_double(round_half_even(f / scale) * scale));
}

static Value b_round(int argc, Value *argv) {
    if (argc == 2) {
        BigInt *n = int_of(argv[1]);
        if (!n) return v_null();
        int64_t nd;
        int d = bi_is_i64(n, &nd) ? (int)nd : 0;
        return round_value(argv[0], &d);
    }
    Value v;
    if (!num_guard(argc, argv, "round", &v)) return v_null();
    return round_value(v, NULL);
}

static Value b_floor(int argc, Value *argv) {
    Value v;
    if (!num_guard(argc, argv, "floor", &v)) return v_null();
    if (v.t == V_INT) return v_int(bi_copy(v.as.i));
    if (v.t == V_BOOL) return v_int_i64(v.as.b);
    if (isnan(v.as.f)) return fail("cannot convert float NaN to integer");
    if (isinf(v.as.f)) return fail("cannot convert float infinity to integer");
    return v_int(bi_from_double(floor(v.as.f)));
}

static Value b_ceil(int argc, Value *argv) {
    Value v;
    if (!num_guard(argc, argv, "ceil", &v)) return v_null();
    if (v.t == V_INT) return v_int(bi_copy(v.as.i));
    if (v.t == V_BOOL) return v_int_i64(v.as.b);
    if (isnan(v.as.f)) return fail("cannot convert float NaN to integer");
    if (isinf(v.as.f)) return fail("cannot convert float infinity to integer");
    return v_int(bi_from_double(ceil(v.as.f)));
}

static Value b_sqrt(int argc, Value *argv) {
    Value v;
    if (!num_guard(argc, argv, "sqrt", &v)) return v_null();
    double f;
    if (!num_f64(v, &f)) return fail("sqrt() argument must be a number");
    if (f < 0) return fail("math domain error");
    return v_float(sqrt(f));
}

static Value b_minmax(int argc, Value *argv, int is_max) {
    if (argc == 0) return failf("%s expected at least 1 argument, got 0", is_max ? "max" : "min");
    Value *seq = argv;
    int n = argc;
    Value single;
    if (argc == 1 && argv[0].t == V_LIST) {
        single = argv[0];
        seq = single.as.l->items;
        n = single.as.l->n;
    }
    if (n == 0) return failf("%s() arg is an empty sequence", is_max ? "max" : "min");
    Value best = seq[0];
    for (int i = 1; i < n; i++) {
        int lt = is_max ? value_less(best, seq[i]) : value_less(seq[i], best);
        if (g_err.set) return v_null();
        if (lt) best = seq[i];
    }
    return best;
}

static Value b_pow(int argc, Value *argv) {
    if (!need(argc, 2, "pow")) return v_null();
    return op_pow(argv[0], argv[1]);
}

static Value b_random(int argc, Value *argv) {
    if (argc == 0) return v_float((double)rand() / (double)RAND_MAX);
    if (argc == 2) {
        BigInt *a = int_of(argv[0]);
        BigInt *b = int_of(argv[1]);
        if (!a || !b) return v_null();
        int64_t av, bv;
        if (!bi_is_i64(a, &av) || !bi_is_i64(b, &bv)) return fail("random_int() range too large");
        if (av > bv) {
            int64_t t = av;
            av = bv;
            bv = t;
        }
        return v_int_i64(av + rand() % (bv - av + 1));
    }
    return fail("random() expects 0 or 2 arguments");
}

typedef struct {
    const char *s;
    size_t n, pos;
    int line, col;
} JP;

static void j_step(JP *p) {
    if (p->pos < p->n && p->s[p->pos] == '\n') {
        p->line++;
        p->col = 1;
    } else {
        p->col++;
    }
    p->pos++;
}

static void j_skip_ws(JP *p) {
    while (p->pos < p->n && (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' || p->s[p->pos] == '\n' || p->s[p->pos] == '\r'))
        j_step(p);
}

static Value j_fail(JP *p, const char *what) {
    err_msgf("%s: line %d column %d (char %d)", what, p->line, p->col, (int)p->pos);
    return v_null();
}

static Value j_value(JP *p);

static int j_hex4(JP *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (p->pos >= p->n) return 0;
        char c = p->s[p->pos];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;
        v = v * 16 + (uint32_t)d;
        j_step(p);
    }
    *out = v;
    return 1;
}

static Value j_string(JP *p) {
    j_step(p);
    SB b;
    sb_init(&b);
    for (;;) {
        if (p->pos >= p->n) return j_fail(p, "Unterminated string starting at");
        char ch = p->s[p->pos];
        if (ch == '"') {
            j_step(p);
            return v_str_take(b.buf, b.len);
        }
        if (ch == '\\') {
            j_step(p);
            if (p->pos >= p->n) return j_fail(p, "Unterminated string starting at");
            char e = p->s[p->pos];
            switch (e) {
            case '"': case '\\': case '/': sb_addch(&b, e); j_step(p); break;
            case 'n': sb_addch(&b, '\n'); j_step(p); break;
            case 't': sb_addch(&b, '\t'); j_step(p); break;
            case 'r': sb_addch(&b, '\r'); j_step(p); break;
            case 'b': sb_addch(&b, '\b'); j_step(p); break;
            case 'f': sb_addch(&b, '\f'); j_step(p); break;
            case 'u': {
                j_step(p);
                uint32_t cp;
                if (!j_hex4(p, &cp)) return j_fail(p, "Invalid \\escape");
                if (cp >= 0xD800 && cp <= 0xDBFF && p->pos + 1 < p->n && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    size_t save = p->pos;
                    j_step(p);
                    j_step(p);
                    uint32_t lo;
                    if (j_hex4(p, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    } else {
                        p->pos = save;
                        cp = 0xFFFD;
                    }
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                sb_adds(&b, u8_encode(cp));
                break;
            }
            default:
                return j_fail(p, "Invalid \\escape");
            }
            continue;
        }
        if ((unsigned char)ch < 0x20) return j_fail(p, "Invalid control character at");
        size_t l = 1;
        sb_add(&b, p->s + p->pos, l);
        j_step(p);
    }
}

static Value j_object(JP *p) {
    j_step(p);
    Value m = v_map();
    j_skip_ws(p);
    if (p->pos < p->n && p->s[p->pos] == '}') {
        j_step(p);
        return m;
    }
    for (;;) {
        j_skip_ws(p);
        if (p->pos >= p->n || p->s[p->pos] != '"') return j_fail(p, "Expecting property name enclosed in double quotes");
        Value k = j_string(p);
        if (g_err.set) return v_null();
        j_skip_ws(p);
        if (p->pos >= p->n || p->s[p->pos] != ':') return j_fail(p, "Expecting ':' delimiter");
        j_step(p);
        j_skip_ws(p);
        Value v = j_value(p);
        if (g_err.set) return v_null();
        map_set(m.as.m, k.as.s->s, v);
        j_skip_ws(p);
        if (p->pos >= p->n) return j_fail(p, "Expecting ',' delimiter");
        if (p->s[p->pos] == ',') {
            j_step(p);
            j_skip_ws(p);
            if (p->pos < p->n && p->s[p->pos] == '}') return j_fail(p, "Expecting property name enclosed in double quotes");
            continue;
        }
        if (p->s[p->pos] == '}') {
            j_step(p);
            return m;
        }
        return j_fail(p, "Expecting ',' delimiter");
    }
}

static Value j_array(JP *p) {
    j_step(p);
    Value out = v_list();
    j_skip_ws(p);
    if (p->pos < p->n && p->s[p->pos] == ']') {
        j_step(p);
        return out;
    }
    for (;;) {
        j_skip_ws(p);
        Value v = j_value(p);
        if (g_err.set) return v_null();
        list_push(out.as.l, v);
        j_skip_ws(p);
        if (p->pos >= p->n) return j_fail(p, "Expecting ',' delimiter");
        if (p->s[p->pos] == ',') {
            j_step(p);
            continue;
        }
        if (p->s[p->pos] == ']') {
            j_step(p);
            return out;
        }
        return j_fail(p, "Expecting ',' delimiter");
    }
}

static Value j_number(JP *p) {
    size_t start = p->pos;
    if (p->pos < p->n && p->s[p->pos] == '-') j_step(p);
    while (p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') j_step(p);
    int is_float = 0;
    if (p->pos < p->n && p->s[p->pos] == '.') {
        is_float = 1;
        j_step(p);
        while (p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') j_step(p);
    }
    if (p->pos < p->n && (p->s[p->pos] == 'e' || p->s[p->pos] == 'E')) {
        is_float = 1;
        j_step(p);
        if (p->pos < p->n && (p->s[p->pos] == '+' || p->s[p->pos] == '-')) j_step(p);
        while (p->pos < p->n && p->s[p->pos] >= '0' && p->s[p->pos] <= '9') j_step(p);
    }
    size_t len = p->pos - start;
    char *buf = xstrndup(p->s + start, len);
    if (!buf[0] || (buf[0] == '-' && buf[1] == 0)) return j_fail(p, "Expecting value");
    if (is_float) {
        char *end = NULL;
        double f = strtod(buf, &end);
        if (end != buf + len) return j_fail(p, "Expecting value");
        return v_float(f);
    }
    return v_int(bi_from_str(buf));
}

static Value j_value(JP *p) {
    if (p->pos >= p->n) return j_fail(p, "Expecting value");
    char ch = p->s[p->pos];
    if (ch == '{') return j_object(p);
    if (ch == '[') return j_array(p);
    if (ch == '"') return j_string(p);
    if (ch == '-' || (ch >= '0' && ch <= '9')) return j_number(p);
    if (p->n - p->pos >= 4 && strncmp(p->s + p->pos, "true", 4) == 0) {
        for (int i = 0; i < 4; i++) j_step(p);
        return v_bool(1);
    }
    if (p->n - p->pos >= 5 && strncmp(p->s + p->pos, "false", 5) == 0) {
        for (int i = 0; i < 5; i++) j_step(p);
        return v_bool(0);
    }
    if (p->n - p->pos >= 4 && strncmp(p->s + p->pos, "null", 4) == 0) {
        for (int i = 0; i < 4; i++) j_step(p);
        return v_null();
    }
    return j_fail(p, "Expecting value");
}

static void json_escape(SB *b, const char *s) {
    sb_addch(b, '"');
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '\\': sb_adds(b, "\\\\"); break;
        case '"': sb_adds(b, "\\\""); break;
        case '\n': sb_adds(b, "\\n"); break;
        case '\r': sb_adds(b, "\\r"); break;
        case '\t': sb_adds(b, "\\t"); break;
        case '\b': sb_adds(b, "\\b"); break;
        case '\f': sb_adds(b, "\\f"); break;
        default:
            if (c < 0x20) sb_addf(b, "\\u%04x", c);
            else sb_addch(b, (char)c);
        }
    }
    sb_addch(b, '"');
}

static void jd(SB *b, Value v, int indent, int depth) {
    int one = indent < 0;
    switch (v.t) {
    case V_NULL: sb_adds(b, "null"); break;
    case V_BOOL: sb_adds(b, v.as.b ? "true" : "false"); break;
    case V_INT: sb_adds(b, bi_to_str(v.as.i)); break;
    case V_FLOAT:
        if (isnan(v.as.f)) sb_adds(b, "NaN");
        else if (isinf(v.as.f)) sb_adds(b, v.as.f > 0 ? "Infinity" : "-Infinity");
        else sb_adds(b, nformat_repr(v.as.f));
        break;
    case V_STR: json_escape(b, v.as.s->s); break;
    case V_LIST: {
        if (v.as.l->n == 0) {
            sb_adds(b, "[]");
            break;
        }
        sb_addch(b, '[');
        for (int i = 0; i < v.as.l->n; i++) {
            if (i) sb_addch(b, ',');
            if (!one) {
                sb_addch(b, '\n');
                for (int k = 0; k < indent * (depth + 1); k++) sb_addch(b, ' ');
            } else if (i) {
                sb_addch(b, ' ');
            }
            jd(b, v.as.l->items[i], indent, depth + 1);
        }
        if (!one) {
            sb_addch(b, '\n');
            for (int k = 0; k < indent * depth; k++) sb_addch(b, ' ');
        }
        sb_addch(b, ']');
        break;
    }
    case V_MAP: {
        if (v.as.m->n == 0) {
            sb_adds(b, "{}");
            break;
        }
        sb_addch(b, '{');
        for (int i = 0; i < v.as.m->n; i++) {
            if (i) sb_addch(b, ',');
            if (!one) {
                sb_addch(b, '\n');
                for (int k = 0; k < indent * (depth + 1); k++) sb_addch(b, ' ');
            } else if (i) {
                sb_addch(b, ' ');
            }
            json_escape(b, v.as.m->pairs[i].k);
            sb_adds(b, ": ");
            jd(b, v.as.m->pairs[i].v, indent, depth + 1);
        }
        if (!one) {
            sb_addch(b, '\n');
            for (int k = 0; k < indent * depth; k++) sb_addch(b, ' ');
        }
        sb_addch(b, '}');
        break;
    }
    default:
        err_msg("Object of type function is not JSON serializable");
    }
}

char *json_dumps(Value v, int indent, int *err) {
    SB b;
    sb_init(&b);
    if (err) *err = 0;
    err_clear();
    jd(&b, v, indent, 0);
    if (g_err.set) {
        if (err) *err = 1;
        return NULL;
    }
    return b.buf ? b.buf : xstrdup("");
}

Value json_parse_value(const char *src) {
    JP p;
    p.s = src;
    p.n = strlen(src);
    p.pos = 0;
    p.line = 1;
    p.col = 1;
    j_skip_ws(&p);
    Value v = j_value(&p);
    if (g_err.set) return v_null();
    j_skip_ws(&p);
    if (p.pos != p.n) {
        err_msg("JSON parse error: Extra data");
        return v_null();
    }
    return v;
}

static Value b_parse_json(int argc, Value *argv) {
    if (!need(argc, 1, "parse_json")) return v_null();
    if (argv[0].t != V_STR) return failf("the JSON object must be str, bytes or bytearray, not %s", type_name(argv[0]));
    JP p;
    p.s = argv[0].as.s->s;
    p.n = strlen(p.s);
    p.pos = 0;
    p.line = 1;
    p.col = 1;
    j_skip_ws(&p);
    Value v = j_value(&p);
    if (g_err.set) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "%s", g_err.msg);
        err_msgf("JSON parse error: %s", msg);
        return v_null();
    }
    j_skip_ws(&p);
    if (p.pos != p.n) {
        err_msgf("JSON parse error: Extra data: line %d column %d (char %d)", p.line, p.col, (int)p.pos);
        return v_null();
    }
    return v;
}

static Value b_stringify_json(int argc, Value *argv) {
    if (argc < 1 || argc > 2) return fail("stringify_json() expects (value[, indent])");
    int indent = -1;
    if (argc == 2) {
        BigInt *n = int_of(argv[1]);
        if (!n) {
            char msg[2048];
            snprintf(msg, sizeof(msg), "%s", g_err.msg);
            err_msgf("JSON stringify error: %s", msg);
            return v_null();
        }
        int64_t iv = 0;
        if (bi_is_i64(n, &iv) && iv > 0 && iv < 0x7fffffff) indent = (int)iv;
    }
    SB b;
    sb_init(&b);
    jd(&b, argv[0], indent, 0);
    if (g_err.set) {
        char msg[2048];
        snprintf(msg, sizeof(msg), "%s", g_err.msg);
        err_msgf("JSON stringify error: %s", msg);
        return v_null();
    }
    return v_str_take(b.buf, b.len);
}

static char *read_all(FILE *f, size_t *len) {
    SB b;
    sb_init(&b);
    char buf[8192];
    for (;;) {
        size_t r = fread(buf, 1, sizeof(buf), f);
        if (r == 0) break;
        sb_add(&b, buf, r);
    }
    *len = b.len;
    return b.buf;
}

static char *file_err_text(int err, const char *path) {
    int errno_ = 2;
    const char *reason = "No such file or directory";
    if (err == EACCES) {
        errno_ = 13;
        reason = "Permission denied";
    } else if (err == EEXIST) {
        errno_ = 17;
        reason = "File exists";
    }
    SB b;
    sb_init(&b);
    sb_addf(&b, "[Errno %d] %s: '%s'", errno_, reason, path);
    return sb_take(&b, NULL);
}

static char *normalize_nl(const char *s, size_t len) {
    SB b;
    sb_init(&b);
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\r' && i + 1 < len && s[i + 1] == '\n') i++;
        sb_addch(&b, s[i]);
    }
    return sb_take(&b, NULL);
}

static char *py_write_nl(const char *s, size_t *len) {
    SB b;
    sb_init(&b);
    for (size_t i = 0; i < *len; i++) {
        if (s[i] == '\n') sb_adds(&b, "\r\n");
        else sb_addch(&b, s[i]);
    }
    *len = b.len;
    return b.buf;
}

static Value b_read_file(int argc, Value *argv) {
    if (!need(argc, 1, "read_file")) return v_null();
    if (argv[0].t != V_STR) return failf("can only join str (not \"%s\") to str", type_name(argv[0]));
    const char *path = argv[0].as.s->s;
    FILE *f = fopen(path, "rb");
    if (!f) {
        char *t = file_err_text(errno, path);
        err_msgf("Failed to read file: %s", t);
        return v_null();
    }
    size_t len = 0;
    char *data = read_all(f, &len);
    fclose(f);
    char *norm = normalize_nl(data, len);
    return v_str(norm);
}

static Value b_write_file(int argc, Value *argv) {
    if (!need(argc, 2, "write_file")) return v_null();
    if (argv[0].t != V_STR) return failf("Failed to write file: str expected, not %s", type_name(argv[0]));
    Value s = value_str(argv[1]);
    size_t len = s.as.s->len;
    char *data = py_write_nl(s.as.s->s, &len);
    FILE *f = fopen(argv[0].as.s->s, "wb");
    if (!f) return failf("Failed to write file: %s", file_err_text(errno, argv[0].as.s->s));
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return fail("Failed to write file");
    }
    fclose(f);
    return v_null();
}

static Value b_append_file(int argc, Value *argv) {
    if (!need(argc, 2, "append_file")) return v_null();
    if (argv[0].t != V_STR) return failf("Failed to append to file: str expected, not %s", type_name(argv[0]));
    Value s = value_str(argv[1]);
    size_t len = s.as.s->len;
    char *data = py_write_nl(s.as.s->s, &len);
    FILE *f = fopen(argv[0].as.s->s, "ab");
    if (!f) return failf("Failed to append to file: %s", file_err_text(errno, argv[0].as.s->s));
    if (fwrite(data, 1, len, f) != len) {
        fclose(f);
        return fail("Failed to append to file");
    }
    fclose(f);
    return v_null();
}

static Value b_file_exists(int argc, Value *argv) {
    if (!need(argc, 1, "file_exists")) return v_null();
    if (argv[0].t != V_STR) return failf("os.stat: path should be string, bytes, os.PathLike or integer, not %s", type_name(argv[0]));
    struct stat st;
    if (stat(argv[0].as.s->s, &st) == 0) return v_bool(1);
    return v_bool(errno != ENOENT);
}

static Value b_delete_file(int argc, Value *argv) {
    if (!need(argc, 1, "delete_file")) return v_null();
    if (argv[0].t != V_STR) return failf("Failed to delete file: str expected, not %s", type_name(argv[0]));
    if (remove(argv[0].as.s->s) != 0) return failf("Failed to delete file: %s", file_err_text(errno, argv[0].as.s->s));
    return v_null();
}

static Value b_list_dir(int argc, Value *argv) {
    if (!need(argc, 1, "list_dir")) return v_null();
    if (argv[0].t != V_STR) return failf("Failed to list dir: str expected, not %s", type_name(argv[0]));
    char pattern[MAX_PATH];
    snprintf(pattern, sizeof(pattern), "%s\\*", argv[0].as.s->s);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    Value out = v_list();
    if (h == INVALID_HANDLE_VALUE) return failf("Failed to list dir: %s", file_err_text(errno, argv[0].as.s->s));
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        list_push(out.as.l, v_str(fd.cFileName));
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

static Value b_create_dir(int argc, Value *argv) {
    if (!need(argc, 1, "create_dir")) return v_null();
    if (argv[0].t != V_STR) return failf("Failed to create dir: str expected, not %s", type_name(argv[0]));
    if (CreateDirectoryA(argv[0].as.s->s, NULL)) return v_null();
    if (GetLastError() == ERROR_ALREADY_EXISTS) return v_null();
    return failf("Failed to create dir: %s", file_err_text(errno, argv[0].as.s->s));
}

static Value b_get_time(int argc, Value *argv) {
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) return v_float((double)time(NULL));
    return v_float((double)ts.tv_sec + (double)ts.tv_nsec / 1e9);
}

static Value b_get_date(int argc, Value *argv) {
    time_t t = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return v_str(buf);
}

static Value b_sleep(int argc, Value *argv) {
    if (!need(argc, 1, "sleep")) return v_null();
    double f;
    if (!num_f64(argv[0], &f)) return failf("a float is required, not %s", type_name(argv[0]));
    if (f < 0) return fail("sleep length must be non-negative");
    Sleep((DWORD)f);
    return v_null();
}

static Value b_env(int argc, Value *argv) {
    if (argc == 0) {
        Value out = v_map();
        LPCH envs = GetEnvironmentStringsA();
        if (envs) {
            for (LPCH e = envs; *e; e += strlen(e) + 1) {
                const char *eq = strchr(e, '=');
                if (eq) {
                    char *key = xstrndup(e, (size_t)(eq - e));
                    map_set(out.as.m, key, v_str(eq + 1));
                }
            }
            FreeEnvironmentStringsA(envs);
        }
        return out;
    }
    if (argv[0].t != V_STR) return failf("key must be str, not %s", type_name(argv[0]));
    const char *v = getenv(argv[0].as.s->s);
    if (v) return v_str(v);
    if (argc > 1) return argv[1];
    return v_null();
}

static Value b_exit(int argc, Value *argv) {
    int code = 0;
    if (argc > 0) {
        BigInt *n = int_of(argv[0]);
        if (!n) return v_null();
        int64_t v;
        if (!bi_is_i64(n, &v)) {
            code = bi_sign(n) > 0 ? INT32_MAX : INT32_MIN;
        } else {
            code = (int)v;
        }
    }
    err_exit(code);
    return v_null();
}

static Value b_args(int argc, Value *argv) {
    Value out = v_list();
    for (int i = 0; i < g_it.nargs; i++) list_push(out.as.l, v_str(g_it.args[i]));
    return out;
}

static Value b_py_bridge(int argc, Value *argv, const char *kind) {
    int is_eval = strcmp(kind, "pyeval") == 0 || strcmp(kind, "goeval") == 0;
    const char *label;
    if (is_eval) label = !strcmp(kind, "pyeval") ? "pyeval()" : "goeval()";
    else label = !strcmp(kind, "goexec") ? "goexec()" : "pyexec()/py() ";
    if (argc != 1 || argv[0].t != V_STR) return failf("%sexpects one string argument", label);
    char err[2048];
    int is_go = strcmp(kind, "goeval") == 0 || strcmp(kind, "goexec") == 0;
    if (is_eval) {
        Value out = v_null();
        int ok = is_go ? go_eval(argv[0].as.s->s, &out, err, sizeof(err))
                       : py_eval(argv[0].as.s->s, &out, err, sizeof(err));
        if (!ok) return failf("%s error: %s", is_go ? "Go" : "Python", err);
        return out;
    }
    int ok = is_go ? go_exec(argv[0].as.s->s, err, sizeof(err))
                   : py_exec(argv[0].as.s->s, err, sizeof(err));
    if (!ok) return failf("%s error: %s", is_go ? "Go" : "Python", err);
    return v_null();
}

#define LOGFN(NAME, PREFIX) static Value NAME(int argc, Value *argv) { return b_logger(PREFIX, argc, argv); }
LOGFN(b_log_info, "[INFO]")
LOGFN(b_log_debug, "[DEBUG]")
LOGFN(b_log_warn, "[WARN]")
LOGFN(b_log_error, "[ERROR]")
LOGFN(b_log_fatal, "[FATAL]")

#define ISFN(NAME, KIND) static Value NAME(int argc, Value *argv) { return b_is_check(argc, argv, KIND); }
ISFN(b_is_number, 0)
ISFN(b_is_string, 1)
ISFN(b_is_array, 2)
ISFN(b_is_object, 3)
ISFN(b_is_boolean, 4)
ISFN(b_is_null, 5)
ISFN(b_is_function, 6)

static Value b_min(int argc, Value *argv) { return b_minmax(argc, argv, 0); }
static Value b_max(int argc, Value *argv) { return b_minmax(argc, argv, 1); }

static Value b_pyeval(int argc, Value *argv) { return b_py_bridge(argc, argv, "pyeval"); }
static Value b_pyexec(int argc, Value *argv) { return b_py_bridge(argc, argv, "pyexec"); }
static Value b_py(int argc, Value *argv) { return b_py_bridge(argc, argv, "py"); }
static Value b_goeval(int argc, Value *argv) { return b_py_bridge(argc, argv, "goeval"); }
static Value b_goexec(int argc, Value *argv) { return b_py_bridge(argc, argv, "goexec"); }

static const char *const k_mon_abbr[12] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
static const char *const k_mon_full[12] = {"January", "February", "March", "April", "May", "June",
                                           "July", "August", "September", "October", "November", "December"};
static const char *const k_day_abbr[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const k_day_full[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                          "Thursday", "Friday", "Saturday"};

static void stftime(SB *b, const struct tm *tmv, int nsec, const char *fmt) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            sb_add(b, p, 1);
            continue;
        }
        p++;
        if (!*p) {
            sb_addch(b, '%');
            break;
        }
        switch (*p) {
        case 'Y': sb_addf(b, "%d", tmv->tm_year + 1900); break;
        case 'y': sb_addf(b, "%02d", (tmv->tm_year + 1900) % 100); break;
        case 'm': sb_addf(b, "%02d", tmv->tm_mon + 1); break;
        case 'd': sb_addf(b, "%02d", tmv->tm_mday); break;
        case 'H': sb_addf(b, "%02d", tmv->tm_hour); break;
        case 'M': sb_addf(b, "%02d", tmv->tm_min); break;
        case 'S': sb_addf(b, "%02d", tmv->tm_sec); break;
        case 'f': sb_addf(b, "%06d", nsec / 1000); break;
        case 'j': sb_addf(b, "%03d", tmv->tm_yday + 1); break;
        case 'b': sb_adds(b, k_mon_abbr[tmv->tm_mon]); break;
        case 'B': sb_adds(b, k_mon_full[tmv->tm_mon]); break;
        case 'a': sb_adds(b, k_day_abbr[tmv->tm_wday]); break;
        case 'A': sb_adds(b, k_day_full[tmv->tm_wday]); break;
        case 'p': sb_adds(b, tmv->tm_hour < 12 ? "AM" : "PM"); break;
        case 'I': {
            int h = tmv->tm_hour % 12;
            sb_addf(b, "%02d", h == 0 ? 12 : h);
            break;
        }
        case 'Z': sb_adds(b, tmv->tm_isdst > 0 ? "DST" : "STD"); break;
        case '%': sb_addch(b, '%'); break;
        default:
            sb_addch(b, '%');
            sb_addch(b, *p);
            break;
        }
    }
}

static Value b_format_time(int argc, Value *argv) {
    if (!need_range(argc, 1, 2, "format_time", "(timestamp[, format])")) return v_null();
    double ts;
    if (!num_f64(argv[0], &ts)) return failf("an integer is required, not %s", type_name(argv[0]));
    const char *fmt = "%Y-%m-%d %H:%M:%S";
    if (argc == 2) {
        if (argv[1].t != V_STR) return failf("strftime() argument must be str, not %s", type_name(argv[1]));
        fmt = argv[1].as.s->s;
    }
    int64_t sec = (int64_t)ts;
    int nsec = (int)((ts - (double)sec) * 1e9);
    time_t tt = (time_t)sec;
    struct tm tmv;
    localtime_s(&tmv, &tt);
    SB b;
    sb_init(&b);
    stftime(&b, &tmv, nsec, fmt);
    return v_str_take(b.buf, b.len);
}

static void def(Env *e, const char *name, NativeFn fn) {
    Func *f = xmalloc(sizeof(Func));
    f->name = name;
    f->nparams = 0;
    f->params = NULL;
    f->body = NULL;
    f->closure = NULL;
    f->native = fn;
    env_define_public(e, name, v_func(f), 0);
}

void std_install(Env *env) {
    def(env, "print", b_print);
    def(env, "output", b_print);
    def(env, "input", b_input);
    def(env, "log_info", b_log_info);
    def(env, "log_debug", b_log_debug);
    def(env, "log_warn", b_log_warn);
    def(env, "log_error", b_log_error);
    def(env, "log_fatal", b_log_fatal);

    def(env, "typeof", b_typeof);
    def(env, "to_string", b_to_string);
    def(env, "to_number", b_to_number);
    def(env, "to_boolean", b_to_boolean);
    def(env, "is_number", b_is_number);
    def(env, "is_string", b_is_string);
    def(env, "is_array", b_is_array);
    def(env, "is_object", b_is_object);
    def(env, "is_boolean", b_is_boolean);
    def(env, "is_null", b_is_null);
    def(env, "is_function", b_is_function);

    def(env, "length", b_length);
    def(env, "split", b_split);
    def(env, "join", b_join);
    def(env, "replace", b_replace);
    def(env, "substring", b_substring);
    def(env, "trim", b_trim);
    def(env, "uppercase", b_upper);
    def(env, "lowercase", b_lower);
    def(env, "startswith", b_startswith);
    def(env, "endswith", b_endswith);
    def(env, "repeat", b_repeat);
    def(env, "char_code", b_char_code);
    def(env, "from_char_code", b_from_char_code);

    def(env, "push", b_push);
    def(env, "pop", b_pop);
    def(env, "shift", b_shift);
    def(env, "unshift", b_unshift);
    def(env, "slice", b_slice);
    def(env, "splice", b_splice);
    def(env, "reverse", b_reverse);
    def(env, "sort", b_sort);
    def(env, "includes", b_includes);
    def(env, "indexOf", b_index_of);
    def(env, "map", b_map);
    def(env, "filter", b_filter);
    def(env, "reduce", b_reduce);
    def(env, "find", b_find);
    def(env, "each", b_each);
    def(env, "sum", b_sum);
    def(env, "range", b_range);

    def(env, "keys", b_keys);
    def(env, "values", b_values);
    def(env, "entries", b_entries);
    def(env, "has", b_has);
    def(env, "merge", b_merge);
    def(env, "clone", b_clone);

    def(env, "abs", b_abs);
    def(env, "round", b_round);
    def(env, "floor", b_floor);
    def(env, "ceil", b_ceil);
    def(env, "sqrt", b_sqrt);
    def(env, "min", b_min);
    def(env, "max", b_max);
    def(env, "pow", b_pow);
    def(env, "random", b_random);

    def(env, "parse_json", b_parse_json);
    def(env, "stringify_json", b_stringify_json);

    def(env, "read_file", b_read_file);
    def(env, "write_file", b_write_file);
    def(env, "append_file", b_append_file);
    def(env, "file_exists", b_file_exists);
    def(env, "delete_file", b_delete_file);
    def(env, "list_dir", b_list_dir);
    def(env, "create_dir", b_create_dir);

    def(env, "get_time", b_get_time);
    def(env, "get_date", b_get_date);
    def(env, "sleep", b_sleep);
    def(env, "format_time", b_format_time);

    def(env, "env", b_env);
    def(env, "exit", b_exit);
    def(env, "argv", b_args);

    def(env, "pyeval", b_pyeval);
    def(env, "pyexec", b_pyexec);
    def(env, "py", b_py);
    def(env, "goeval", b_goeval);
    def(env, "goexec", b_goexec);

    gui_install(env);
    c_install(env);
    http_install(env);
}