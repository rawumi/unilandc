#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"
#include "goworker_go.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

static const char k_goworker_gosum[] =
    "github.com/traefik/yaegi v0.16.3 h1:s/8IcuvcVsOAiWCLemTcxUe0qtMtXl66osMt3YXFIeM=\n"
    "github.com/traefik/yaegi v0.16.3/go.mod h1:4eVhbPb3LnD2VigQjhYbEJ69vDRFdT2HQNrXx8eEwUY=\n"
    ;

static HANDLE g_hin, g_hout, g_hproc;
static int g_started, g_dead, g_next_id;
static char g_worker[MAX_PATH * 2];

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

static char *b64enc(const char *s, size_t n) {
    static const char *tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 4 * ((n + 2) / 3);
    char *r = xmalloc(out + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)s[i] << 16;
        if (i + 1 < n) v |= (unsigned char)s[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned char)s[i + 2];
        r[k++] = tbl[(v >> 18) & 63];
        r[k++] = tbl[(v >> 12) & 63];
        r[k++] = i + 1 < n ? tbl[(v >> 6) & 63] : '=';
        r[k++] = i + 2 < n ? tbl[v & 63] : '=';
    }
    r[k] = 0;
    return r;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static char *b64dec(const char *s) {
    size_t n = strlen(s);
    char *r = xmalloc(n / 4 * 3 + 4);
    size_t k = 0;
    for (size_t i = 0; i + 3 < n; i += 4) {
        int a = b64val(s[i]), b = b64val(s[i + 1]);
        int c = b64val(s[i + 2]), d = b64val(s[i + 3]);
        if (a < 0 || b < 0) break;
        r[k++] = (char)((a << 2) | (b >> 4));
        if (c >= 0) r[k++] = (char)(((b & 15) << 4) | (c >> 2));
        if (d >= 0) r[k++] = (char)(((c & 3) << 6) | d);
    }
    r[k] = 0;
    return r;
}

static Value coerce_arg(const char *s) {
    if (!strcmp(s, "true")) return v_bool(1);
    if (!strcmp(s, "false")) return v_bool(0);
    const char *p = s;
    if (*p == '+' || *p == '-') p++;
    int digits = *p != 0, dots = 0;
    for (const char *q = p; *q; q++) {
        if (*q == '.' && !dots) {
            dots = 1;
            continue;
        }
        if (*q < '0' || *q > '9') {
            digits = 0;
            break;
        }
    }
    if (digits) {
        if (dots) return v_float(strtod(s, NULL));
        return v_int(bi_from_str(s));
    }
    return v_str(s);
}

static int write_all(HANDLE h, const char *s) {
    DWORD n = 0;
    size_t len = strlen(s);
    return WriteFile(h, s, (DWORD)len, &n, NULL) && n == len;
}

static void gw_reply(HANDLE hin, const char *line) {
    write_all(hin, line);
}

static void gw_reply_err(HANDLE hin, const char *msg) {
    char *enc = b64enc(msg ? msg : "error", strlen(msg ? msg : "error"));
    char *line = xmalloc(strlen(enc) + 8);
    sprintf(line, "@@e\t%s\n", enc);
    gw_reply(hin, line);
}

static void gw_handle(HANDLE hin, char **f, int nf) {
    if (nf < 2) {
        gw_reply_err(hin, "uni: bad request");
        return;
    }
    const char *op = f[0];
    char *key = b64dec(f[1]);
    SB b;
    sb_init(&b);
    if (!strcmp(op, "get")) {
        Value v = interp_global_get(key);
        if (g_err.set) {
            char msg[2048];
            snprintf(msg, sizeof(msg), "%s", g_err.msg);
            err_clear();
            gw_reply_err(hin, msg);
            return;
        }
        const char *t = "n";
        const char *payload = "";
        if (v.t == V_INT) {
            t = "i";
            payload = bi_to_str(v.as.i);
        } else if (v.t == V_FLOAT) {
            t = "f";
            payload = nformat_repr(v.as.f);
        } else if (v.t == V_STR) {
            t = "s";
            payload = v.as.s->s;
        } else if (v.t == V_BOOL) {
            t = "b";
            payload = v.as.b ? "1" : "0";
        } else {
            Value s = value_py_str(v);
            t = "s";
            payload = s.as.s->s;
        }
        char *enc = b64enc(payload, strlen(payload));
        char *line = xmalloc(strlen(enc) + 16);
        sprintf(line, "@@r\t%s\t%s\n", t, enc);
        gw_reply(hin, line);
        return;
    }
    if (!strcmp(op, "set")) {
        const char *t = nf >= 3 ? f[2] : "";
        char *payload = nf >= 4 ? b64dec(f[3]) : xstrdup("");
        Value v;
        if (!strcmp(t, "i")) v = v_int(bi_from_str(payload));
        else if (!strcmp(t, "f")) v = v_float(strtod(payload, NULL));
        else if (!strcmp(t, "b")) v = v_bool(!strcmp(payload, "1"));
        else v = v_str(payload);
        interp_global_set(key, v);
        gw_reply(hin, "@@r\tn\t\n");
        return;
    }
    if (!strcmp(op, "call")) {
        int argc = nf - 2;
        Value *args = xmalloc(sizeof(Value) * (size_t)(argc > 0 ? argc : 1));
        for (int i = 0; i < argc; i++) args[i] = coerce_arg(b64dec(f[2 + i]));
        Value r = interp_call_named(key, argc, args);
        if (g_err.set) {
            char msg[2048];
            snprintf(msg, sizeof(msg), "%s", g_err.msg);
            err_clear();
            gw_reply_err(hin, msg);
            return;
        }
        Value s = value_py_str(r);
        char *enc = b64enc(s.as.s->s, s.as.s->len);
        char *line = xmalloc(strlen(enc) + 16);
        sprintf(line, "@@r\ts\t%s\n", enc);
        gw_reply(hin, line);
        return;
    }
    gw_reply_err(hin, "uni: неизвестная операция");
}

static int worker_build(char *err, size_t cap) {
    char dir[MAX_PATH], appdata[MAX_PATH], bin[MAX_PATH], go[MAX_PATH];
    DWORD n = GetTempPathA((DWORD)sizeof(dir), dir);
    if (n == 0 || n >= sizeof(dir)) {
        snprintf(err, cap, "не удалось получить временный каталог");
        return 0;
    }
    snprintf(dir + strlen(dir), sizeof(dir) - strlen(dir), "uniland-goworker-c");
    if (!CreateDirectoryA(dir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        snprintf(err, cap, "не удалось создать %s", dir);
        return 0;
    }
    char cached[MAX_PATH * 2];
    snprintf(cached, sizeof(cached), "%s\\worker.exe", dir);
    if (GetFileAttributesA(cached) != INVALID_FILE_ATTRIBUTES) {
        snprintf(g_worker, sizeof(g_worker), "%s", cached);
        return 1;
    }
    if (!SearchPathA(NULL, "go.exe", NULL, (DWORD)sizeof(go), go, NULL)) {
        snprintf(err, cap, "go не найден в PATH");
        return 0;
    }
    char mainfile[MAX_PATH * 2], modfile[MAX_PATH * 2], gosumfile[MAX_PATH * 2];
    char exe[MAX_PATH * 2];
    snprintf(mainfile, sizeof(mainfile), "%s\\main.go", dir);
    snprintf(modfile, sizeof(modfile), "%s\\go.mod", dir);
    snprintf(exe, sizeof(exe), "%s\\worker.exe", dir);
    FILE *f = fopen(mainfile, "wb");
    if (!f) {
        snprintf(err, cap, "не удалось записать %s", mainfile);
        return 0;
    }
    fwrite(k_goworker_go, 1, strlen(k_goworker_go), f);
    fclose(f);
    f = fopen(modfile, "wb");
    if (!f) {
        snprintf(err, cap, "не удалось записать %s", modfile);
        return 0;
    }
    fputs("module uniland-worker\n\ngo 1.21\n\nrequire github.com/traefik/yaegi v0.16.3\n", f);
    fclose(f);
    snprintf(gosumfile, sizeof(gosumfile), "%s\\go.sum", dir);
    f = fopen(gosumfile, "wb");
    if (f) {
        fputs(k_goworker_gosum, f);
        fclose(f);
    }
    n = GetEnvironmentVariableA("LOCALAPPDATA", appdata, sizeof(appdata));
    if (n == 0 || n >= sizeof(appdata)) snprintf(appdata, sizeof(appdata), "%s", dir);
    snprintf(bin, sizeof(bin), "%s\\uniland\\goworker", appdata);
    char mkdir_cmd[MAX_PATH * 4];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "\"%s\" build -o \"%s\" \"%s\"", go, exe, dir);
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE out_r = NULL, out_w = NULL;
    if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
        snprintf(err, cap, "не удалось создать канал");
        return 0;
    }
    SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = out_w;
    si.hStdError = out_w;
    if (!CreateProcessA(NULL, mkdir_cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, dir, &si, &pi)) {
        CloseHandle(out_r);
        CloseHandle(out_w);
        snprintf(err, cap, "не удалось запустить go build");
        return 0;
    }
    CloseHandle(out_w);
    CloseHandle(pi.hThread);
    char log[4096];
    DWORD got = 0;
    ReadFile(out_r, log, (DWORD)sizeof(log) - 1, &got, NULL);
    log[got] = 0;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(out_r);
    if (code != 0) {
        snprintf(err, cap, "go build воркера не удался: %s", log[0] ? log : "без вывода");
        return 0;
    }
    snprintf(g_worker, sizeof(g_worker), "%s", exe);
    return 1;
}

static int gw_start(char *err, size_t cap) {
    if (g_started) return 1;
    if (!g_worker[0] && !worker_build(err, cap)) return 0;
    SECURITY_ATTRIBUTES sa;
    memset(&sa, 0, sizeof(sa));
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE in_r = NULL, out_w = NULL;
    if (!CreatePipe(&in_r, &g_hin, &sa, 0) || !CreatePipe(&g_hout, &out_w, &sa, 0)) {
        snprintf(err, cap, "не удалось создать каналы воркера");
        return 0;
    }
    SetHandleInformation(g_hin, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(g_hout, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    char cmd[MAX_PATH * 3];
    snprintf(cmd, sizeof(cmd), "\"%s\"", g_worker);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        CloseHandle(in_r);
        CloseHandle(out_w);
        snprintf(err, cap, "не удалось запустить Go worker (код %lu)", (unsigned long)GetLastError());
        return 0;
    }
    CloseHandle(in_r);
    CloseHandle(out_w);
    CloseHandle(pi.hThread);
    g_hproc = pi.hProcess;
    g_started = 1;
    return 1;
}

static int gw_read_line(char *buf, size_t cap) {
    size_t k = 0;
    for (;;) {
        char c = 0;
        DWORD got = 0;
        if (g_dead) return 0;
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

int go_run(const char *op, const char *code, const char *scope, Value *out, char *err, size_t cap) {
    if (g_dead) {
        snprintf(err, cap, "Go worker остановлен");
        return 0;
    }
    if (!gw_start(err, cap)) return 0;
    int id = ++g_next_id;
    SB req;
    sb_init(&req);
    sb_addf(&req, "{\"id\":%d,\"op\":\"%s\",\"code\":", id, op);
    if (!strcmp(op, "exec")) {
        SB wrapped;
        sb_init(&wrapped);
        sb_addf(&wrapped, "func %s() {\n", scope);
        sb_adds(&wrapped, code);
        sb_adds(&wrapped, "\n}\n");
        js_str(&req, wrapped.buf);
    } else {
        js_str(&req, code);
    }
    sb_adds(&req, "}\n");
    if (!write_all(g_hin, req.buf)) {
        g_dead = 1;
        snprintf(err, cap, "Go worker: канал закрыт");
        return 0;
    }
    size_t lcap = 1 << 20;
    char *line = xmalloc(lcap);
    for (;;) {
        if (!gw_read_line(line, lcap)) {
            snprintf(err, cap, "Go worker закрылся");
            return 0;
        }
        if (!strncmp(line, "@@uni\t", 6)) {
            char *fields[16];
            int nf = 0;
            char *p = line + 6;
            fields[nf++] = p;
            for (char *q = p; *q && nf < 16; q++) {
                if (*q == '\t') {
                    *q = 0;
                    fields[nf++] = q + 1;
                }
            }
            gw_handle(g_hin, fields, nf);
            continue;
        }
        Value msg = json_parse_value(line);
        if (g_err.set) {
            err_clear();
            continue;
        }
        if (msg.t != V_MAP) continue;
        Value kind = map_get(msg.as.m, "kind");
        if (kind.t == V_STR && !strcmp(kind.as.s->s, "out")) {
            Value text = map_get(msg.as.m, "text");
            if (text.t == V_STR) {
                fputs(text.as.s->s, g_it.out);
                fflush(g_it.out);
            }
            continue;
        }
        Value idv = map_get(msg.as.m, "id");
        int64_t got = 0;
        if (idv.t != V_INT || !bi_is_i64(idv.as.i, &got) || got != id) continue;
        Value ok = map_get(msg.as.m, "ok");
        if (!(ok.t == V_BOOL && ok.as.b)) {
            Value e = map_get(msg.as.m, "err");
            snprintf(err, cap, "%s", e.t == V_STR ? e.as.s->s : "go error");
            return 0;
        }
        if (out) {
            Value hv = map_get(msg.as.m, "has");
            Value vv = map_get(msg.as.m, "value");
            if (hv.t == V_BOOL && hv.as.b && vv.t == V_STR) *out = coerce_arg(vv.as.s->s);
            else *out = v_null();
        }
        return 1;
    }
}

int go_eval(const char *expr, Value *out, char *err, size_t cap) {
    return go_run("eval", expr, NULL, out, err, cap);
}

int go_exec(const char *code, char *err, size_t cap) {
    static int seq;
    char scope[64];
    snprintf(scope, sizeof(scope), "_uniExec%d", ++seq);
    if (!go_run("exec", code, scope, NULL, err, cap)) return 0;
    char call[80];
    snprintf(call, sizeof(call), "%s()", scope);
    return go_run("eval", call, NULL, NULL, err, cap);
}

void go_bridge_close(void) {
    if (!g_started) return;
    write_all(g_hin, "{\"op\":\"shutdown\"}\n");
    CloseHandle(g_hin);
    if (g_hproc) {
        WaitForSingleObject(g_hproc, 2000);
        CloseHandle(g_hproc);
    }
    CloseHandle(g_hout);
    g_started = 0;
}
