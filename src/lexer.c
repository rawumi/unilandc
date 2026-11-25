
#include "uniland.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t *rs;
    size_t n, i;
    int line, col;
    Token *toks;
    int nt, cap;
    uint32_t *br;
    int nbr, brcap;
} Lx;

static const char *keywords[] = {
    "let", "const", "func", "fn", "return", "if", "else", "for", "while",
    "break", "continue", "try", "catch", "finally", "throw", "true", "false",
    "null", "in", "python", NULL
};

static const char *multi_ops[] = {
    "==", "!=", ">=", "<=", "&&", "||", "++", "--", "+=", "-=", "*=", "/=",
    "%=", "=>", "..", NULL
};

static const char *single_ops = "+-*/%^=<>!&|";
static const char *punct = "(){}[],.;:";

static int is_letter(uint32_t c) {
    if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return 1;
}

static int is_digit(uint32_t c) { return c >= '0' && c <= '9'; }

static int in_set(const char *set, uint32_t c) {
    if (c == 0 || c > 127) return 0;
    return strchr(set, (int)c) != NULL;
}

static int kw_check(const char *w) {
    for (int i = 0; keywords[i]; i++)
        if (strcmp(keywords[i], w) == 0) return 1;
    return 0;
}

static void tok_add(Lx *l, TokType t, const char *v, int line, int col) {
    if (l->nt == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->toks = xrealloc(l->toks, sizeof(Token) * (size_t)l->cap);
    }
    l->toks[l->nt].t = t;
    l->toks[l->nt].v = xstrdup(v);
    l->toks[l->nt].line = line;
    l->toks[l->nt].col = col;
    l->nt++;
}

static void advance(Lx *l, int count) {
    for (int k = 0; k < count; k++) {
        if (l->i < l->n) {
            if (l->rs[l->i] == '\n') {
                l->line++;
                l->col = 1;
            } else {
                l->col++;
            }
            l->i++;
        }
    }
}

static int starts_with(Lx *l, const char *s) {
    size_t len = strlen(s);
    if (l->i + len > l->n) return 0;
    for (size_t k = 0; k < len; k++)
        if (l->rs[l->i + k] != (unsigned char)s[k]) return 0;
    return 1;
}

static Token *last_tok(Lx *l) {
    if (l->nt == 0) return NULL;
    return &l->toks[l->nt - 1];
}

static int cont_op(const char *v) {
    if (strcmp(v, "++") == 0 || strcmp(v, "--") == 0) return 0;
    for (int i = 0; multi_ops[i]; i++)
        if (strcmp(multi_ops[i], v) == 0) return 1;
    return strlen(v) == 1 && in_set(single_ops, (unsigned char)v[0]);
}

static int cont_punct(const char *v) {
    return strcmp(v, "(") == 0 || strcmp(v, "[") == 0 || strcmp(v, "{") == 0 ||
           strcmp(v, ",") == 0 || strcmp(v, ".") == 0 || strcmp(v, ":") == 0;
}

static void maybe_newline(Lx *l) {
    Token *last = last_tok(l);
    if (!last || last->t == T_NEWLINE) return;
    if (l->nbr > 0 && (l->br[l->nbr - 1] == '(' || l->br[l->nbr - 1] == '[')) return;
    if (last->t == T_OP && cont_op(last->v)) return;
    if (last->t == T_PUNCT && cont_punct(last->v)) return;
    tok_add(l, T_NEWLINE, "\n", l->line, l->col);
}

static char *runes_to_utf8(Lx *l, size_t from, size_t to) {
    SB b;
    sb_init(&b);
    for (size_t k = from; k < to; k++) {
        char *e = u8_encode(l->rs[k]);
        sb_adds(&b, e);
    }
    return sb_take(&b, NULL);
}

static void strip_trailing_newline(Lx *l) {
    while (l->nt > 0 && l->toks[l->nt - 1].t == T_NEWLINE) l->nt--;
}

static void read_number(Lx *l, int line, int col) {
    size_t start = l->i;
    int seen_dot = 0;
    while (l->i < l->n) {
        uint32_t ch = l->rs[l->i];
        if (is_digit(ch)) {
            advance(l, 1);
        } else if (ch == '.' && !seen_dot && !(l->i + 1 < l->n && l->rs[l->i + 1] == '.')) {
            seen_dot = 1;
            advance(l, 1);
        } else {
            break;
        }
    }
    char *v = runes_to_utf8(l, start, l->i);
    tok_add(l, T_NUMBER, v, line, col);
}

static char *capture_block(Lx *l, size_t brace_index) {
    while (l->i < brace_index) advance(l, 1);
    advance(l, 1);
    size_t start = l->i;
    int depth = 1;
    while (l->i < l->n && depth > 0) {
        uint32_t ch = l->rs[l->i];
        if (ch == '"' || ch == '\'') {
            if (l->i + 2 < l->n && l->rs[l->i + 1] == ch && l->rs[l->i + 2] == ch) {
                advance(l, 3);
                while (l->i + 2 < l->n && !(l->rs[l->i] == ch && l->rs[l->i + 1] == ch && l->rs[l->i + 2] == ch))
                    advance(l, 1);
                advance(l, 3);
            } else {
                advance(l, 1);
                while (l->i < l->n && l->rs[l->i] != ch) {
                    if (l->rs[l->i] == '\\') advance(l, 1);
                    advance(l, 1);
                }
                advance(l, 1);
            }
            continue;
        }
        if (ch == '#') {
            while (l->i < l->n && l->rs[l->i] != '\n') advance(l, 1);
            continue;
        }
        if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) break;
        }
        advance(l, 1);
    }
    char *code = runes_to_utf8(l, start, l->i);
    advance(l, 1);
    return code;
}

static void read_word(Lx *l, int line, int col) {
    size_t start = l->i;
    while (l->i < l->n && (is_letter(l->rs[l->i]) || is_digit(l->rs[l->i]) || l->rs[l->i] == '_'))
        advance(l, 1);
    char *word = runes_to_utf8(l, start, l->i);

    if (strcmp(word, "python") == 0 || strcmp(word, "go") == 0) {
        size_t j = l->i;
        while (j < l->n && (l->rs[j] == ' ' || l->rs[j] == '\t' || l->rs[j] == '\r' || l->rs[j] == '\n')) j++;
        if (l->i < l->n && l->rs[j] == '{') {
            char *code = capture_block(l, j);
            if (word[0] == 'g' && code[0] == '\n') {
                size_t cl = strlen(code);
                memmove(code, code + 1, cl);
            }
            tok_add(l, word[0] == 'p' ? T_PYBLOCK : T_GOBLOCK, code, line, col);
            return;
        }
    }

    if (kw_check(word)) tok_add(l, T_KEYWORD, word, line, col);
    else tok_add(l, T_IDENT, word, line, col);
}

static uint32_t unescape(uint32_t esc, int tpl) {
    if (esc == 'n') return '\n';
    if (esc == 't') return '\t';
    if (esc == 'r') return '\r';
    if (!tpl) {
        if (esc == '"') return '"';
        if (esc == '\\') return '\\';
        if (esc == '0') return 0;
        return esc;
    }
    if (esc == '`') return '`';
    if (esc == '\\') return '\\';
    if (esc == '{') return '{';
    if (esc == '}') return '}';
    return esc;
}

static int read_quoted(Lx *l, int line, int col, uint32_t quote, int tpl) {
    advance(l, 1);
    SB b;
    sb_init(&b);
    while (l->i < l->n && l->rs[l->i] != quote) {
        uint32_t ch = l->rs[l->i];
        if (ch == '\\') {
            advance(l, 1);
            if (l->i >= l->n) break;
            uint32_t esc = l->rs[l->i];
            uint32_t dec = unescape(esc, tpl);
            if (dec == 0) {
                sb_addch(&b, 0);
            } else {
                char *e = u8_encode(dec);
                sb_adds(&b, e);
            }
            advance(l, 1);
        } else {
            char *e = u8_encode(ch);
            sb_adds(&b, e);
            advance(l, 1);
        }
    }
    if (l->i >= l->n) {
        if (tpl) err_line(line, "Unterminated template string");
        else err_line(line, "Unterminated string");
        return 0;
    }
    advance(l, 1);
    tok_add(l, tpl ? T_TEMPLATE : T_STRING, b.buf, line, col);
    return 1;
}

Token *lex(const char *src, int *out_count) {
    err_clear();
    size_t blen = strlen(src);
    uint32_t *rs = xmalloc(sizeof(uint32_t) * (blen + 1));
    size_t n = 0;
    for (size_t k = 0; k < blen;) {
        unsigned char c = (unsigned char)src[k];
        int l = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
        uint32_t cp = 0;
        if (l == 1) cp = c;
        else if (l == 2) cp = ((uint32_t)(c & 0x1F) << 6) | (src[k + 1] & 0x3F);
        else if (l == 3) cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(src[k + 1] & 0x3F) << 6) | (src[k + 2] & 0x3F);
        else cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(src[k + 1] & 0x3F) << 12) |
                  ((uint32_t)(src[k + 2] & 0x3F) << 6) | (src[k + 3] & 0x3F);
        rs[n++] = cp;
        k += (size_t)l;
    }

    Lx l;
    memset(&l, 0, sizeof(l));
    l.rs = rs;
    l.n = n;
    l.line = 1;
    l.col = 1;

    while (l.i < l.n) {
        uint32_t c = l.rs[l.i];

        if (c == ' ' || c == '\t' || c == '\r') {
            advance(&l, 1);
            continue;
        }
        if (c == '\n') {
            maybe_newline(&l);
            advance(&l, 1);
            continue;
        }
        if (starts_with(&l, "//") || c == '#') {
            while (l.i < l.n && l.rs[l.i] != '\n') advance(&l, 1);
            continue;
        }
        if (starts_with(&l, "/*")) {
            while (l.i < l.n && !starts_with(&l, "*/")) advance(&l, 1);
            advance(&l, 2);
            continue;
        }

        int line = l.line, col = l.col;

        if (c == '"') {
            if (!read_quoted(&l, line, col, '"', 0)) return NULL;
            continue;
        }
        if (c == '`') {
            if (!read_quoted(&l, line, col, '`', 1)) return NULL;
            continue;
        }
        if (is_digit(c) || (c == '.' && l.i + 1 < l.n && is_digit(l.rs[l.i + 1]))) {
            read_number(&l, line, col);
            continue;
        }
        if (is_letter(c) || c == '_') {
            read_word(&l, line, col);
            continue;
        }

        if (l.i + 1 < l.n && c < 128) {
            char two[3] = {(char)c, (char)l.rs[l.i + 1], 0};
            int found = 0;
            for (int k = 0; multi_ops[k]; k++)
                if (strcmp(multi_ops[k], two) == 0) found = 1;
            if (found) {
                tok_add(&l, T_OP, two, line, col);
                advance(&l, 2);
                continue;
            }
        }
        if (in_set(single_ops, c)) {
            char one[2] = {(char)c, 0};
            tok_add(&l, T_OP, one, line, col);
            advance(&l, 1);
            continue;
        }
        if (in_set(punct, c)) {
            if (c == '(' || c == '[' || c == '{') {
                if (l.nbr == l.brcap) {
                    l.brcap = l.brcap ? l.brcap * 2 : 16;
                    l.br = xrealloc(l.br, sizeof(uint32_t) * (size_t)l.brcap);
                }
                l.br[l.nbr++] = c;
            } else if (c == ')' || c == ']' || c == '}') {
                if (l.nbr > 0) l.nbr--;
            }
            char one[2] = {(char)c, 0};
            tok_add(&l, T_PUNCT, one, line, col);
            advance(&l, 1);
            continue;
        }

        char *ch = u8_encode(c);
        char *rep = py_repr(ch);
        err_linef(line, "Unexpected character %s", rep);
        return NULL;
    }

    strip_trailing_newline(&l);
    tok_add(&l, T_EOF, "", l.line, l.col);
    *out_count = l.nt;
    return l.toks;
}

const char *tok_typename(TokType t) {
    switch (t) {
    case T_KEYWORD: return "KEYWORD";
    case T_IDENT: return "IDENTIFIER";
    case T_OP: return "OPERATOR";
    case T_NUMBER: return "NUMBER";
    case T_NEWLINE: return "NEWLINE";
    case T_PUNCT: return "PUNCTUATION";
    case T_TEMPLATE: return "TEMPLATE";
    case T_STRING: return "STRING";
    case T_PYBLOCK: return "PYBLOCK";
    case T_GOBLOCK: return "GOBLOCK";
    default: return "EOF";
    }
}

const char *tok_dump(const Token *t) {
    static char buf[4096];
    char *rep = py_repr(t->v);
    snprintf(buf, sizeof(buf), "Token(%s, %s, line=%d)", tok_typename(t->t), rep, t->line);
    return buf;
}
