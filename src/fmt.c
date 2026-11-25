#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"

#include <stdlib.h>
#include <string.h>

static int count_lines(const char *s) {
    int n = 1;
    for (const char *p = s; *p; p++)
        if (*p == '\n') n++;
    return n;
}

static void depth_step(Token *toks, int from, int to, int *brace, int *paren) {
    for (int i = from; i < to; i++) {
        if (toks[i].t != T_PUNCT) continue;
        const char *v = toks[i].v;
        if (!strcmp(v, "{")) {
            (*brace)++;
        } else if (!strcmp(v, "}")) {
            if (*brace > 0) (*brace)--;
        } else if (!strcmp(v, "(") || !strcmp(v, "[")) {
            (*paren)++;
        } else if (!strcmp(v, ")") || !strcmp(v, "]")) {
            if (*paren > 0) (*paren)--;
        }
    }
}

char *unifmt_format(const char *src, char *err, size_t cap) {
    char *text = xstrdup(src);
    for (char *p = text; *p; p++) {
        if (*p == '\r') *p = '\n';
    }
    int ntok = 0;
    Token *toks = lex(text, &ntok);
    if (g_err.set) {
        snprintf(err, cap, "%s", g_err.msg);
        return NULL;
    }
    int nlines = count_lines(text);
    int *verbatim = xmalloc(sizeof(int) * (size_t)(nlines + 2));
    memset(verbatim, 0, sizeof(int) * (size_t)(nlines + 2));
    for (int i = 0; i < ntok; i++) {
        Token *t = &toks[i];
        if (t->t != T_PYBLOCK && t->t != T_GOBLOCK && t->t != T_TEMPLATE && t->t != T_STRING) continue;
        int span = 0;
        for (const char *p = t->v; *p; p++)
            if (*p == '\n') span++;
        for (int l = t->line + 1; l <= t->line + span && l <= nlines; l++) verbatim[l] = 1;
    }
    char **lines = xmalloc(sizeof(char *) * (size_t)(nlines + 1));
    int li = 0;
    lines[li++] = text;
    for (char *p = text; *p; p++) {
        if (*p == '\n') {
            *p = 0;
            lines[li++] = p + 1;
        }
    }
    SB out;
    sb_init(&out);
    int out_lines = 0;
    int last_blank = 0;
    int brace = 0, paren = 0;
    int tok_start = 0;
    int idx = 0;
    for (int line_no = 1; line_no <= nlines; line_no++) {
        int tok_end = tok_start;
        while (tok_end < ntok && toks[tok_end].line <= line_no) tok_end++;
        char *raw = lines[line_no - 1];
        size_t rl = strlen(raw);
        while (rl > 0 && (raw[rl - 1] == ' ' || raw[rl - 1] == '\t')) raw[--rl] = 0;
        if (verbatim[line_no]) {
            if (out_lines) sb_addch(&out, '\n');
            sb_adds(&out, raw);
            out_lines++;
            last_blank = raw[0] == 0;
            depth_step(toks, tok_start, tok_end, &brace, &paren);
            tok_start = tok_end;
            continue;
        }
        const char *body = raw;
        while (*body == ' ' || *body == '\t') body++;
        if (*body == 0) {
            if (out_lines && !last_blank) {
                sb_addch(&out, '\n');
                out_lines++;
                last_blank = 1;
            }
            depth_step(toks, tok_start, tok_end, &brace, &paren);
            tok_start = tok_end;
            continue;
        }
        int depth = brace + (paren > 0 ? 1 : 0);
        if (tok_start < tok_end) {
            Token *first = &toks[tok_start];
            if (first->t == T_PUNCT &&
                (!strcmp(first->v, "}") || !strcmp(first->v, ")") || !strcmp(first->v, "]"))) {
                if (depth > 0) depth--;
            }
        }
        if (out_lines) sb_addch(&out, '\n');
        for (int k = 0; k < depth; k++) sb_adds(&out, "    ");
        sb_adds(&out, body);
        out_lines++;
        last_blank = 0;
        depth_step(toks, tok_start, tok_end, &brace, &paren);
        tok_start = tok_end;
        idx++;
    }
    (void)idx;
    while (out.len > 0 && out.buf[out.len - 1] == '\n') out.buf[--out.len] = 0;
    char *res = xmalloc(out.len + 2);
    if (out.buf) memcpy(res, out.buf, out.len);
    res[out.len] = '\n';
    res[out.len + 1] = 0;
    return res;
}
