#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *uri;
    char *text;
} Doc;

static Doc *g_docs;
static int g_ndocs, g_capdocs;

static void docs_set(const char *uri, const char *text) {
    for (int i = 0; i < g_ndocs; i++) {
        if (!strcmp(g_docs[i].uri, uri)) {
            g_docs[i].text = xstrdup(text);
            return;
        }
    }
    if (g_ndocs == g_capdocs) {
        g_capdocs = g_capdocs ? g_capdocs * 2 : 8;
        g_docs = xrealloc(g_docs, sizeof(Doc) * (size_t)g_capdocs);
    }
    g_docs[g_ndocs].uri = xstrdup(uri);
    g_docs[g_ndocs].text = xstrdup(text);
    g_ndocs++;
}

static const char *docs_get(const char *uri) {
    for (int i = 0; i < g_ndocs; i++)
        if (!strcmp(g_docs[i].uri, uri)) return g_docs[i].text;
    return NULL;
}

static void json_escape(SB *b, const char *s) {
    sb_addch(b, '"');
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': sb_adds(b, "\\\""); break;
        case '\\': sb_adds(b, "\\\\"); break;
        case '\n': sb_adds(b, "\\n"); break;
        case '\r': sb_adds(b, "\\r"); break;
        case '\t': sb_adds(b, "\\t"); break;
        default:
            if (*p < 0x20) sb_addf(b, "\\u%04x", (int)*p);
            else sb_addch(b, (char)*p);
            break;
        }
    }
    sb_addch(b, '"');
}

static void json_id(SB *b, Value id) {
    if (id.t == V_INT) {
        sb_adds(b, bi_to_str(id.as.i));
        return;
    }
    if (id.t == V_STR) {
        json_escape(b, id.as.s->s);
        return;
    }
    if (id.t == V_FLOAT) {
        char *s = nformat(id.as.f);
        sb_adds(b, s);
        return;
    }
    sb_adds(b, "null");
}

static void write_message(const char *body) {
    printf("Content-Length: %d\r\n\r\n%s", (int)strlen(body), body);
    fflush(stdout);
}

static int line_count(const char *text) {
    int n = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n') n++;
    return n;
}

static int line_length(const char *text, int line) {
    int cur = 0;
    const char *p = text;
    while (*p && cur < line) {
        if (*p == '\n') cur++;
        p++;
    }
    if (cur != line) return 0;
    int len = 0;
    while (*p && *p != '\n') {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80 || c >= 0xC0) len++;
        p++;
    }
    return len;
}

static int check_source(const char *src, char *msg, size_t cap, int *line) {
    err_clear();
    int ntok = 0;
    Token *toks = lex(src, &ntok);
    if (g_err.set) {
        snprintf(msg, cap, "%s", g_err.msg);
        *line = g_err.has_line ? g_err.line : 0;
        return 0;
    }
    parse_program(toks);
    if (g_err.set) {
        snprintf(msg, cap, "%s", g_err.msg);
        *line = g_err.has_line ? g_err.line : 0;
        return 0;
    }
    return 1;
}

static void publish_diagnostics(const char *uri, const char *text) {
    char msg[2048];
    int line = 0;
    int ok = check_source(text, msg, sizeof(msg), &line);
    SB b;
    sb_init(&b);
    sb_adds(&b, "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":");
    json_escape(&b, uri);
    sb_adds(&b, ",\"diagnostics\":[");
    if (!ok) {
        int row = line > 0 ? line - 1 : 0;
        sb_addf(&b, "{\"range\":{\"start\":{\"line\":%d,\"character\":0},", row);
        sb_addf(&b, "\"end\":{\"line\":%d,\"character\":%d}},", row, line_length(text, row));
        sb_adds(&b, "\"severity\":1,\"source\":\"uniland\",\"message\":");
        json_escape(&b, msg);
        sb_addch(&b, '}');
    }
    sb_adds(&b, "]}}");
    write_message(b.buf);
    err_clear();
}

static void reply_result(SB *b, Value id, const char *result_json) {
    sb_adds(b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_id(b, id);
    sb_adds(b, ",\"result\":");
    sb_adds(b, result_json);
    sb_addch(b, '}');
}

static void reply_error(Value id, const char *method) {
    SB b;
    sb_init(&b);
    sb_adds(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_id(&b, id);
    sb_adds(&b, ",\"error\":{\"code\":-32601,\"message\":\"method not found: ");
    sb_adds(&b, method);
    sb_adds(&b, "\"}}");
    write_message(b.buf);
}

static void handle_text_edit_request(Value id, const char *uri) {
    const char *text = docs_get(uri);
    if (!text) {
        SB b;
        sb_init(&b);
        reply_result(&b, id, "null");
        write_message(b.buf);
        return;
    }
    char err[2048];
    char *formatted = unifmt_format(text, err, sizeof(err));
    if (!formatted) {
        SB b;
        sb_init(&b);
        reply_result(&b, id, "null");
        write_message(b.buf);
        return;
    }
    SB b;
    sb_init(&b);
    sb_adds(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    json_id(&b, id);
    sb_adds(&b, ",\"result\":[{\"range\":{\"start\":{\"line\":0,\"character\":0},\"end\":{\"line\":");
    sb_addf(&b, "%d", line_count(text));
    sb_adds(&b, ",\"character\":0}},\"newText\":");
    json_escape(&b, formatted);
    sb_adds(&b, "}]}");
    write_message(b.buf);
}

static Value map_get_or_null(Map *m, const char *key) {
    if (!m) return v_null();
    return map_get(m, key);
}

static const char *str_at(Map *m, const char *key) {
    Value v = map_get_or_null(m, key);
    return v.t == V_STR ? v.as.s->s : NULL;
}

static int read_message(char **out) {
    int length = -1;
    char line[512];
    for (;;) {
        if (!fgets(line, sizeof(line), stdin)) return 0;
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
        if (!ln) break;
        if (!_strnicmp(line, "content-length:", 15)) {
            length = atoi(line + 15);
        }
    }
    if (length < 0) return 0;
    char *body = xmalloc((size_t)length + 1);
    size_t got = fread(body, 1, (size_t)length, stdin);
    body[got] = 0;
    if ((int)got != length) {
        return 0;
    }
    *out = body;
    return 1;
}

int lsp_serve(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    for (;;) {
        char *raw = NULL;
        if (!read_message(&raw)) return 0;
        Value msg = json_parse_value(raw);
        if (g_err.set) {
            err_clear();
            continue;
        }
        if (msg.t != V_MAP) continue;
        Map *m = msg.as.m;
        Value idv = map_get_or_null(m, "id");
        const char *method = str_at(m, "method");
        if (!method) continue;
        if (!strcmp(method, "exit")) return 0;
        if (!strcmp(method, "initialize")) {
            SB b;
            sb_init(&b);
            reply_result(&b, idv,
                         "{\"capabilities\":{\"textDocumentSync\":1,\"documentFormattingProvider\":true,"
                         "\"documentRangeFormattingProvider\":false},\"serverInfo\":{\"name\":"
                         "\"uniland-lsp\",\"version\":\"2.2.0\"}}");
            write_message(b.buf);
            continue;
        }
        if (!strcmp(method, "initialized")) continue;
        if (!strcmp(method, "shutdown")) {
            SB b;
            sb_init(&b);
            reply_result(&b, idv, "null");
            write_message(b.buf);
            continue;
        }
        Value params = map_get_or_null(m, "params");
        Map *pm = params.t == V_MAP ? params.as.m : NULL;
        if (!strcmp(method, "textDocument/didOpen") || !strcmp(method, "textDocument/didChange")) {
            Value td = map_get_or_null(pm, "textDocument");
            if (td.t != V_MAP) continue;
            const char *uri = str_at(td.as.m, "uri");
            if (!uri) continue;
            const char *text = str_at(td.as.m, "text");
            if (!text) {
                Value changes = map_get_or_null(pm, "contentChanges");
                if (changes.t == V_LIST && changes.as.l->n > 0) {
                    Value last = changes.as.l->items[changes.as.l->n - 1];
                    if (last.t == V_MAP) text = str_at(last.as.m, "text");
                }
            }
            if (!text) continue;
            docs_set(uri, text);
            publish_diagnostics(uri, text);
            continue;
        }
        if (!strcmp(method, "textDocument/formatting")) {
            Value td = map_get_or_null(pm, "textDocument");
            const char *uri = td.t == V_MAP ? str_at(td.as.m, "uri") : NULL;
            if (!uri || idv.t == V_NULL) {
                SB b;
                sb_init(&b);
                reply_result(&b, idv, "null");
                write_message(b.buf);
                continue;
            }
            handle_text_edit_request(idv, uri);
            continue;
        }
        if (idv.t != V_NULL) reply_error(idv, method);
    }
}
