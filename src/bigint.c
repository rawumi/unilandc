
#include "uniland.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define BASE 1000000000u
#define BASE_DIGITS 9

struct BigInt {
    int sign;
    int n;
    uint32_t *d;
    uint32_t inl[3];
};

typedef struct BiBlock {
    struct BiBlock *next;
    size_t used;
    size_t cap;
    unsigned char data[1];
} BiBlock;

static BiBlock *g_bi_block;

static void *bi_arena(size_t size) {
    size = (size + 7u) & ~(size_t)7u;
    if (!g_bi_block || g_bi_block->used + size > g_bi_block->cap) {
        size_t cap = size > (1u << 20) ? size : (1u << 20);
        BiBlock *b = xmalloc(sizeof(BiBlock) + cap);
        b->next = g_bi_block;
        b->used = 0;
        b->cap = cap;
        g_bi_block = b;
    }
    void *p = g_bi_block->data + g_bi_block->used;
    g_bi_block->used += size;
    return p;
}

static BigInt *bi_new(int n) {
    BigInt *b = bi_arena(sizeof(BigInt));
    b->sign = 0;
    b->n = n;
    if (n <= 3) b->d = b->inl;
    else b->d = bi_arena(sizeof(uint32_t) * (size_t)n);
    for (int i = 0; i < n; i++) b->d[i] = 0;
    return b;
}

static void bi_trim(BigInt *b) {
    while (b->n > 0 && b->d[b->n - 1] == 0) b->n--;
    if (b->n == 0) b->sign = 0;
}

BigInt *bi_copy(const BigInt *n) {
    BigInt *r = bi_new(n->n);
    r->sign = n->sign;
    for (int i = 0; i < n->n; i++) r->d[i] = n->d[i];
    return r;
}

BigInt *bi_from_i64(int64_t v) {
    int neg = v < 0;
    uint64_t u = neg ? (uint64_t)(-(v + 1)) + 1u : (uint64_t)v;
    BigInt *r = bi_new(3);
    int k = 0;
    while (u > 0) {
        r->d[k++] = (uint32_t)(u % BASE);
        u /= BASE;
    }
    r->n = k;
    r->sign = k == 0 ? 0 : (neg ? -1 : 1);
    return r;
}

BigInt *bi_from_str(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0;
    if (*s == '+' || *s == '-') {
        neg = *s == '-';
        s++;
    }
    size_t len = strlen(s);
    BigInt *r = bi_new((int)(len / BASE_DIGITS) + 2);
    int limb = 0;
    size_t end = len;
    while (end > 0) {
        size_t start = end >= (size_t)BASE_DIGITS ? end - (size_t)BASE_DIGITS : 0;
        uint32_t cur = 0;
        for (size_t k = start; k < end; k++) cur = cur * 10 + (uint32_t)(s[k] - '0');
        r->d[limb++] = cur;
        end = start;
    }
    r->n = limb;
    r->sign = limb == 0 ? 0 : (neg ? -1 : 1);
    bi_trim(r);
    return r;
}

char *bi_to_str(const BigInt *n) {
    if (n->n == 0) return xstrdup("0");
    size_t cap = (size_t)n->n * BASE_DIGITS + 3;
    char *buf = xmalloc(cap);
    size_t pos = 0;
    if (n->sign < 0) buf[pos++] = '-';
    for (int i = n->n - 1; i >= 0; i--) {
        if (i == n->n - 1) {
            pos += (size_t)snprintf(buf + pos, cap - pos, "%u", n->d[i]);
        } else {
            pos += (size_t)snprintf(buf + pos, cap - pos, "%09u", n->d[i]);
        }
    }
    buf[pos] = 0;
    return buf;
}

int bi_sign(const BigInt *n) { return n->sign; }
int bi_is_zero(const BigInt *n) { return n->n == 0; }

static int cmp_mag(const BigInt *a, const BigInt *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) {
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    }
    return 0;
}

int bi_cmp(const BigInt *a, const BigInt *b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    if (a->sign == 0) return 0;
    int c = cmp_mag(a, b);
    return a->sign < 0 ? -c : c;
}

static BigInt *add_mag(const BigInt *a, const BigInt *b) {
    int n = (a->n > b->n ? a->n : b->n) + 1;
    BigInt *r = bi_new(n);
    uint32_t carry = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = carry;
        if (i < a->n) s += a->d[i];
        if (i < b->n) s += b->d[i];
        r->d[i] = (uint32_t)(s % BASE);
        carry = (uint32_t)(s / BASE);
    }
    r->n = n;
    bi_trim(r);
    return r;
}

static BigInt *sub_mag(const BigInt *a, const BigInt *b) {
    BigInt *r = bi_new(a->n);
    int64_t borrow = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t s = (int64_t)a->d[i] - borrow - (i < b->n ? (int64_t)b->d[i] : 0);
        if (s < 0) {
            s += BASE;
            borrow = 1;
        } else {
            borrow = 0;
        }
        r->d[i] = (uint32_t)s;
    }
    r->n = a->n;
    bi_trim(r);
    return r;
}

BigInt *bi_add(const BigInt *a, const BigInt *b) {
    if (a->sign == 0) return bi_copy(b);
    if (b->sign == 0) return bi_copy(a);
    BigInt *r;
    if (a->sign == b->sign) {
        r = add_mag(a, b);
        r->sign = a->sign;
        return r;
    }
    int c = cmp_mag(a, b);
    if (c == 0) return bi_from_i64(0);
    if (c > 0) {
        r = sub_mag(a, b);
        r->sign = a->sign;
    } else {
        r = sub_mag(b, a);
        r->sign = b->sign;
    }
    return r;
}

BigInt *bi_sub(const BigInt *a, const BigInt *b) {
    BigInt *nb = bi_neg(b);
    return bi_add(a, nb);
}

BigInt *bi_neg(const BigInt *a) {
    BigInt *r = bi_copy(a);
    r->sign = -r->sign;
    return r;
}

BigInt *bi_abs(const BigInt *a) {
    BigInt *r = bi_copy(a);
    r->sign = r->n == 0 ? 0 : 1;
    return r;
}

BigInt *bi_mul(const BigInt *a, const BigInt *b) {
    if (a->sign == 0 || b->sign == 0) return bi_from_i64(0);
    BigInt *r = bi_new(a->n + b->n + 1);
    for (int i = 0; i < a->n; i++) {
        uint64_t carry = 0;
        for (int j = 0; j < b->n; j++) {
            uint64_t cur = r->d[i + j] + (uint64_t)a->d[i] * b->d[j] + carry;
            r->d[i + j] = (uint32_t)(cur % BASE);
            carry = cur / BASE;
        }
        int k = i + b->n;
        while (carry) {
            uint64_t cur = r->d[k] + carry;
            r->d[k] = (uint32_t)(cur % BASE);
            carry = cur / BASE;
            k++;
        }
    }
    r->n = a->n + b->n + 1;
    r->sign = a->sign * b->sign;
    bi_trim(r);
    return r;
}

static BigInt *shift_limbs(const BigInt *b, int shift) {
    BigInt *r = bi_new(b->n + shift);
    for (int i = 0; i < b->n; i++) r->d[i + shift] = b->d[i];
    r->n = b->n + shift;
    r->sign = b->sign;
    return r;
}

static BigInt *div_mag(const BigInt *a, const BigInt *b, BigInt **rem_out) {
    BigInt *q = bi_new(a->n);
    q->n = a->n;
    BigInt *r = bi_copy(a);
    r->sign = r->n ? 1 : 0;
    if (cmp_mag(a, b) < 0) {
        *rem_out = bi_copy(a);
        (*rem_out)->sign = (*rem_out)->n ? 1 : 0;
        return bi_from_i64(0);
    }
    int shift = a->n - b->n;
    while (shift >= 0) {
        BigInt *bs = shift_limbs(b, shift);
        bs->sign = 1;
        uint32_t lo = 0, hi = BASE - 1, best = 0;
        while (lo <= hi) {
            uint32_t mid = (uint32_t)(((uint64_t)lo + hi) / 2);
            BigInt *m = bi_from_i64(mid);
            BigInt *prod = bi_mul(bs, m);
            int c = cmp_mag(prod, r);
            if (c <= 0) {
                best = mid;
                if (mid == BASE - 1) break;
                lo = mid + 1;
            } else {
                if (mid == 0) break;
                hi = mid - 1;
            }
        }
        if (best) {
            BigInt *m = bi_from_i64(best);
            BigInt *prod = bi_mul(bs, m);
            BigInt *nr = sub_mag(r, prod);
            r = nr;
            q->d[shift] = best;
        }
        shift--;
    }
    bi_trim(q);
    q->sign = q->n == 0 ? 0 : a->sign;
    r->sign = r->n == 0 ? 0 : a->sign;
    *rem_out = r;
    return q;
}

BigInt *bi_rem(const BigInt *a, const BigInt *b) {
    BigInt *rem = NULL;
    div_mag(a, b, &rem);
    return rem;
}

BigInt *bi_pow(const BigInt *a, const BigInt *b) {
    BigInt *one = bi_from_i64(1);
    BigInt *res = bi_from_i64(1);
    BigInt *base = bi_copy(a);
    BigInt *e = bi_copy(b);
    BigInt *two = bi_from_i64(2);
    while (!bi_is_zero(e)) {
        if (bi_bit(e, 0)) res = bi_mul(res, base);
        BigInt *rem = NULL;
        BigInt *half = div_mag(e, two, &rem);
        e = half;
        if (!bi_is_zero(e)) {
            BigInt *sq = bi_mul(base, base);
            base = sq;
        }
    }
    (void)one;
    return res;
}

int bi_is_i64(const BigInt *n, int64_t *out) {
    uint64_t v = 0;
    if (n->n > 3) return 0;
    for (int i = n->n - 1; i >= 0; i--) {
        if ((uint64_t)v * 10 > (uint64_t)0x7fffffffffffffffULL) return 0;
        v = v * BASE + n->d[i];
    }
    if (n->sign >= 0) {
        if (v > (uint64_t)INT64_MAX) return 0;
        *out = (int64_t)v;
    } else {
        if (v > (uint64_t)INT64_MAX + 1u) return 0;
        *out = v == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)v;
    }
    return 1;
}

int bi_bit(const BigInt *n, int i) {
    int limb = i / 30;
    if (limb >= n->n) return 0;

    if (i == 0) return (int)(n->d[0] & 1u);
    return 0;
}

uint64_t bi_bitlen(const BigInt *n) {
    if (n->n == 0) return 0;
    uint32_t top = n->d[n->n - 1];
    uint64_t bits = 0;
    while (top) {
        bits++;
        top >>= 1;
    }
    return bits + (uint64_t)(n->n - 1) * 30;
}

double bi_to_f64(const BigInt *n) {
    double r = 0;
    for (int i = n->n - 1; i >= 0; i--) r = r * (double)BASE + (double)n->d[i];
    return n->sign < 0 ? -r : r;
}
