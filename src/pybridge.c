#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"
#include "pyworker_py.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

static HANDLE g_hin, g_hout, g_hproc;
static int g_started, g_dead, g_next_id;

static int64_t as_i64(Value v) {
    if (v.t == V_INT) {
        int64_t x = 0;
        if (bi_is_i64(v.as.i, &x)) return x;
        return 0;
    }
    if (v.t == V_FLOAT) return (int64_t)v.as.f;
    return 0;
}

static void js_str(SB *b, const char *s) {
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

static void enc_value(SB *b, Value v) {
    switch (v.t) {
    case V_NULL: sb_adds(b, "{\"T\":\"null\"}"); break;
    case V_BOOL: sb_adds(b, v.as.b ? "true" : "false"); break;
    case V_INT: {
        char *s = bi_to_str(v.as.i);
        sb_adds(b, "{\"T\":\"int\",\"v\":");
        js_str(b, s);
        sb_addch(b, '}');
        break;
    }
    case V_FLOAT: {
        char *s = nformat_repr(v.as.f);
        sb_adds(b, "{\"T\":\"float\",\"v\":");
        js_str(b, s);
        sb_addch(b, '}');
        break;
    }
    case V_STR:
        sb_adds(b, "{\"T\":\"str\",\"v\":");
        js_str(b, v.as.s->s);
        sb_addch(b, '}');
        break;
    case V_LIST:
        sb_adds(b, "{\"T\":\"list\",\"v\":[");
        for (int i = 0; i < v.as.l->n; i++) {
            if (i) sb_addch(b, ',');
            enc_value(b, v.as.l->items[i]);
        }
        sb_adds(b, "]}");
        break;
    case V_MAP:
        sb_adds(b, "{\"T\":\"obj\",\"k\":[");
        for (int i = 0; i < v.as.m->n; i++) {
            if (i) sb_addch(b, ',');
            js_str(b, v.as.m->pairs[i].k);
        }
        sb_adds(b, "],\"v\":{");
        for (int i = 0; i < v.as.m->n; i++) {
            if (i) sb_addch(b, ',');
            js_str(b, v.as.m->pairs[i].k);
            sb_addch(b, ':');
            enc_value(b, v.as.m->pairs[i].v);
        }
        sb_adds(b, "}}");
        break;
    default:
        sb_adds(b, "{\"T\":\"func\"}");
        break;
    }
}

static Value dec_value(Value w) {
    if (w.t == V_LIST) {
        Value out = v_list();
        for (int i = 0; i < w.as.l->n; i++) list_push(out.as.l, dec_value(w.as.l->items[i]));
        return out;
    }
    if (w.t != V_MAP) {
        if (w.t == V_FLOAT) {
            double f = w.as.f;
            int64_t t = (int64_t)f;
            if ((double)t == f) return v_int_i64(t);
        }
        return w;
    }
    Map *m = w.as.m;
    Value tv = map_get(m, "T");
    if (tv.t != V_STR) {
        Value out = v_map();
        for (int i = 0; i < m->n; i++) map_set(out.as.m, m->pairs[i].k, dec_value(m->pairs[i].v));
        return out;
    }
    const char *t = tv.as.s->s;
    Value vv = map_get(m, "v");
    if (!strcmp(t, "int")) return v_int(bi_from_str(vv.t == V_STR ? vv.as.s->s : "0"));
    if (!strcmp(t, "float")) return v_float(vv.t == V_STR ? strtod(vv.as.s->s, NULL) : 0);
    if (!strcmp(t, "str")) return v_str(vv.t == V_STR ? vv.as.s->s : "");
    if (!strcmp(t, "list")) return dec_value(vv);
    if (!strcmp(t, "obj")) {
        Value out = v_map();
        if (vv.t != V_MAP) return out;
        Value kn = map_get(m, "k");
        if (kn.t == V_LIST) {
            for (int i = 0; i < kn.as.l->n; i++) {
                Value k = kn.as.l->items[i];
                if (k.t != V_STR) continue;
                map_set(out.as.m, k.as.s->s, dec_value(map_get(vv.as.m, k.as.s->s)));
            }
        } else {
            for (int i = 0; i < vv.as.m->n; i++)
                map_set(out.as.m, vv.as.m->pairs[i].k, dec_value(vv.as.m->pairs[i].v));
        }
        return out;
    }
    return v_null();
}

static const char *find_python(void) {
    static char buf[MAX_PATH];
    const char *custom = getenv("UNILAND_PYTHON");
    if (custom && *custom) return custom;
    if (SearchPathA(NULL, "python3.exe", NULL, (DWORD)sizeof(buf), buf, NULL)) return buf;
    if (SearchPathA(NULL, "python.exe", NULL, (DWORD)sizeof(buf), buf, NULL)) return buf;
    return NULL;
}

static int write_worker_file(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t n = strlen(k_pyworker_py);
    size_t w = fwrite(k_pyworker_py, 1, n, f);
    fclose(f);
    return w == n;
}

static int pb_start(char *err, size_t cap) {
    if (g_started) return 1;
    const char *exe = find_python();
    if (!exe) {
        snprintf(err, cap, "python не найден (нужен python3 или python в PATH; можно задать UNILAND_PYTHON)");
        return 0;
    }
    char dir[MAX_PATH], py[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(dir), dir);
    if (n == 0 || n >= sizeof(dir)) {
        snprintf(err, cap, "не удалось получить временный каталог");
        return 0;
    }
    snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "uniland-pyworker-%lu",
             (unsigned long)GetCurrentProcessId());
    CreateDirectoryA(dir, NULL);
    snprintf(py, sizeof(py), "%s\\pyworker.py", dir);
    if (!write_worker_file(py)) {
        snprintf(err, cap, "не удалось записать pyworker.py");
        return 0;
    }
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE in_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &g_hin, &sa, 0) || !SetHandleInformation(g_hin, HANDLE_FLAG_INHERIT, 0)) {
        snprintf(err, cap, "не удалось создать канал stdin");
        return 0;
    }
    if (!CreatePipe(&g_hout, &out_w, &sa, 0) || !SetHandleInformation(g_hout, HANDLE_FLAG_INHERIT, 0)) {
        snprintf(err, cap, "не удалось создать канал stdout");
        return 0;
    }
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    char cmd[MAX_PATH * 4];
    snprintf(cmd, sizeof(cmd), "\"%s\" -u \"%s\"", exe, py);
    SetEnvironmentVariableA("PYTHONUTF8", "1");
    SetEnvironmentVariableA("PYTHONIOENCODING", "utf-8");
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        snprintf(err, cap, "не удалось запустить %s (код %lu)", exe, (unsigned long)GetLastError());
        CloseHandle(in_r);
        CloseHandle(out_w);
        return 0;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(pi.hThread);
    g_hproc = pi.hProcess;
    g_started = 1;
    return 1;
}

static void pb_write(const char *s) {
    DWORD n = 0;
    size_t len = strlen(s);
    if (!g_started || g_dead) return;
    if (!WriteFile(g_hin, s, (DWORD)len, &n, NULL) || n != len) g_dead = 1;
}

static int pb_read_line(char *buf, size_t cap) {
    size_t k = 0;
    for (;;) {
        char c = 0;
        DWORD got = 0;
        if (!g_started || g_dead) return 0;
        if (!ReadFile(g_hout, &c, 1, &got, NULL) || got == 0) {
            g_dead = 1;
            return 0;
        }
        if (c == '\n') break;
        if (c != '\r' && k + 1 < cap) buf[k++] = c;
    }
    buf[k] = 0;
    return 1;
}

static void reply_error(SB *b, const char *msg) {
    sb_adds(b, ",\"ok\":false,\"err\":");
    js_str(b, msg ? msg : "bridge error");
    sb_addch(b, '}');
    pb_write(b->buf);
    pb_write("\n");
}

static void take_err(char *dst, size_t cap) {
    snprintf(dst, cap, "%s", g_err.set ? g_err.msg : "bridge error");
    err_clear();
}

static void reply_request(Value msg) {
    Map *m = msg.as.m;
    Value idv = map_get(m, "id");
    Value opv = map_get(m, "op");
    Value kv = map_get(m, "k");
    const char *op = opv.t == V_STR ? opv.as.s->s : "";
    const char *key = kv.t == V_STR ? kv.as.s->s : "";
    SB b;
    sb_init(&b);
    sb_addf(&b, "{\"id\":%lld", (long long)as_i64(idv));
    char emsg[2048];
    if (!strcmp(op, "uni_get")) {
        Value v = interp_global_get(key);
        if (g_err.set) {
            take_err(emsg, sizeof(emsg));
            reply_error(&b, emsg);
            return;
        }
        sb_adds(&b, ",\"ok\":true,\"value\":");
        enc_value(&b, v);
        sb_addch(&b, '}');
    } else if (!strcmp(op, "uni_set")) {
        Value raw = map_get(m, "v");
        Value v = dec_value(raw);
        if (g_err.set) {
            take_err(emsg, sizeof(emsg));
            reply_error(&b, emsg);
            return;
        }
        interp_global_set(key, v);
        sb_adds(&b, ",\"ok\":true}");
    } else if (!strcmp(op, "uni_call")) {
        Value argv = map_get(m, "args");
        int argc = argv.t == V_LIST ? argv.as.l->n : 0;
        Value *args = xmalloc(sizeof(Value) * (size_t)(argc ? argc : 1));
        for (int i = 0; i < argc; i++) args[i] = dec_value(argv.as.l->items[i]);
        Value r = interp_call_named(key, argc, args);
        if (g_err.set) {
            take_err(emsg, sizeof(emsg));
            reply_error(&b, emsg);
            return;
        }
        sb_adds(&b, ",\"ok\":true,\"value\":");
        enc_value(&b, r);
        sb_addch(&b, '}');
    } else {
        reply_error(&b, "unknown request");
        return;
    }
    pb_write(b.buf);
    pb_write("\n");
}

static int call_worker(const char *op, const char *code, Value *out, char *err, size_t cap) {
    if (g_dead) {
        snprintf(err, cap, "Python worker остановлен");
        return 0;
    }
    if (!pb_start(err, cap)) return 0;
    int id = ++g_next_id;
    SB req;
    sb_init(&req);
    sb_addf(&req, "{\"id\":%d,\"op\":\"%s\"", id, op);
    if (code) {
        sb_adds(&req, ",\"code\":");
        js_str(&req, code);
    }
    sb_adds(&req, "}\n");
    pb_write(req.buf);
    if (g_dead) {
        snprintf(err, cap, "Python worker: канал закрыт");
        return 0;
    }
    size_t lcap = 1 << 20;
    char *line = xmalloc(lcap);
    for (;;) {
        if (!pb_read_line(line, lcap)) {
            snprintf(err, cap, "Python worker закрылся (проверьте, что python запускается)");
            return 0;
        }
        Value msg = json_parse_value(line);
        if (g_err.set) {
            err_clear();
            continue;
        }
        if (msg.t != V_MAP) continue;
        Value kind = map_get(msg.as.m, "kind");
        if (kind.t == V_STR) {
            Value text = map_get(msg.as.m, "text");
            const char *s = text.t == V_STR ? text.as.s->s : "";
            if (!strcmp(kind.as.s->s, "out")) {
                fputs(s, g_it.out);
                fflush(g_it.out);
                continue;
            }
            if (!strcmp(kind.as.s->s, "err")) {
                fputs(s, g_it.errw);
                fflush(g_it.errw);
                continue;
            }
            if (!strcmp(kind.as.s->s, "req")) {
                reply_request(msg);
                continue;
            }
        }
        if (as_i64(map_get(msg.as.m, "id")) != (int64_t)id) continue;
        Value ok = map_get(msg.as.m, "ok");
        if (!(ok.t == V_BOOL && ok.as.b)) {
            Value e = map_get(msg.as.m, "err");
            snprintf(err, cap, "%s", e.t == V_STR ? e.as.s->s : "python error");
            return 0;
        }
        if (out) *out = dec_value(map_get(msg.as.m, "value"));
        return 1;
    }
}

int py_eval(const char *code, Value *out, char *err, size_t cap) {
    return call_worker("eval", code, out, err, cap);
}

int py_exec(const char *code, char *err, size_t cap) {
    return call_worker("exec", code, NULL, err, cap);
}

void py_bridge_close(void) {
    if (!g_started) return;
    pb_write("{\"op\":\"shutdown\"}\n");
    CloseHandle(g_hin);
    if (g_hproc) {
        WaitForSingleObject(g_hproc, 2000);
        CloseHandle(g_hproc);
    }
    CloseHandle(g_hout);
    g_started = 0;
    g_dead = 0;
}

char *py_dedent(const char *s) {
    SB ex;
    sb_init(&ex);
    int col = 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\t') {
            int n = 8 - col % 8;
            for (int i = 0; i < n; i++) sb_addch(&ex, ' ');
            col += n;
            continue;
        }
        if (*p == '\n') {
            col = 0;
            sb_addch(&ex, '\n');
            continue;
        }
        col++;
        sb_addch(&ex, *p);
    }
    char *src = ex.buf ? ex.buf : xstrdup("");
    int nlines = 1;
    for (char *p = src; *p; p++)
        if (*p == '\n') nlines++;
    char **lines = xmalloc(sizeof(char *) * (size_t)nlines);
    int idx = 0;
    lines[idx++] = src;
    for (char *p = src; *p; p++) {
        if (*p == '\n') {
            *p = 0;
            if (idx < nlines) lines[idx++] = p + 1;
        }
    }
    nlines = idx;
    int margin = -1;
    for (int i = 1; i < nlines; i++) {
        int lead = 0;
        while (lines[i][lead] == ' ') lead++;
        if (lead > 0 && (margin == -1 || lead < margin)) margin = lead;
    }
    SB out;
    sb_init(&out);
    for (int i = 0; i < nlines; i++) {
        const char *l = lines[i];
        if (i > 0 && margin > 0) {
            int lead = 0;
            while (l[lead] == ' ') lead++;
            if (lead >= margin) l += margin;
        }
        if (i) sb_addch(&out, '\n');
        sb_adds(&out, l);
    }
    return out.buf ? out.buf : xstrdup("");
}
