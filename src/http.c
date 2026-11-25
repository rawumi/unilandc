#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"

#include <windows.h>
#include <winhttp.h>
#include <stdlib.h>
#include <string.h>

static Value h_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_err.set = 1;
    g_err.has_line = 0;
    g_err.is_exit = 0;
    vsnprintf(g_err.msg, sizeof(g_err.msg), fmt, ap);
    va_end(ap);
    return v_null();
}

static wchar_t *to_wide(const char *src) {
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = xmalloc(sizeof(wchar_t) * (size_t)n);
    MultiByteToWideChar(CP_UTF8, 0, src, -1, w, n);
    return w;
}

static char *from_wide_n(const wchar_t *src, int len) {
    int n = WideCharToMultiByte(CP_UTF8, 0, src, len, NULL, 0, NULL, NULL);
    if (n <= 0) return xstrdup("");
    char *s = xmalloc((size_t)n + 1);
    WideCharToMultiByte(CP_UTF8, 0, src, len, s, n, NULL, NULL);
    s[n] = 0;
    return s;
}

static char *normalize_newlines(const char *s) {
    size_t n = strlen(s);
    char *out = xmalloc(n + 1);
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            if (i + 1 < n && s[i + 1] == '\n') continue;
            out[k++] = '\n';
        } else {
            out[k++] = s[i];
        }
    }
    out[k] = 0;
    return out;
}

static const wchar_t *method_wide(const char *method) {
    if (!strcmp(method, "post")) return L"POST";
    if (!strcmp(method, "put")) return L"PUT";
    if (!strcmp(method, "delete")) return L"DELETE";
    return L"GET";
}

static char *canonical_header(const char *name) {
    char *out = xstrdup(name);
    int upper = 1;
    for (char *p = out; *p; p++) {
        if (*p == '-') {
            upper = 1;
            continue;
        }
        if (upper && *p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
        else if (!upper && *p >= 'A' && *p <= 'Z') *p = (char)(*p + 32);
        upper = 0;
    }
    return out;
}

static Value http_call(const char *method, int argc, Value *argv) {
    char upper[16];
    snprintf(upper, sizeof(upper), "%s", method);
    for (char *p = upper; *p; p++)
        if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if (argc < 1 || argv[0].t != V_STR) return h_fail("http_%s() needs a url string", method);
    const char *url = argv[0].as.s->s;
    wchar_t *wurl = to_wide(url);
    if (!wurl) return h_fail("HTTP %s failed: URL не в UTF-8", upper);
    wchar_t whost[256], wpath[4096];
    URL_COMPONENTS uc;
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = whost;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = wpath;
    uc.dwUrlPathLength = 4096;
    if (!WinHttpCrackUrl(wurl, 0, 0, &uc))
        return h_fail("HTTP %s failed: не разобрать URL '%s'", upper, url);
    wchar_t *whost_z = xmalloc(sizeof(wchar_t) * (size_t)(uc.dwHostNameLength + 1));
    memcpy(whost_z, uc.lpszHostName, sizeof(wchar_t) * uc.dwHostNameLength);
    whost_z[uc.dwHostNameLength] = 0;
    wchar_t *wpath_z = xmalloc(sizeof(wchar_t) * (size_t)(uc.dwUrlPathLength + 1));
    memcpy(wpath_z, uc.lpszUrlPath, sizeof(wchar_t) * uc.dwUrlPathLength);
    wpath_z[uc.dwUrlPathLength] = 0;
    char *body = NULL;
    int has_body = 0;
    if ((!strcmp(method, "post") || !strcmp(method, "put")) && argc > 1) {
        int jerr = 0;
        body = json_dumps(argv[1], -1, &jerr);
        if (jerr) return h_fail("HTTP %s failed: значение не сериализуется в JSON", upper);
        has_body = 1;
    }
    HINTERNET session = WinHttpOpen(L"uniland-c/2.2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return h_fail("HTTP %s failed: WinHttpOpen (%lu)", upper, (unsigned long)GetLastError());
    HINTERNET conn = WinHttpConnect(session, whost_z, uc.nPort, 0);
    if (!conn) {
        WinHttpCloseHandle(session);
        return h_fail("HTTP %s failed: WinHttpConnect (%lu)", upper, (unsigned long)GetLastError());
    }
    DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET req = WinHttpOpenRequest(conn, method_wide(method), wpath_z, NULL,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) {
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return h_fail("HTTP %s failed: WinHttpOpenRequest (%lu)", upper, (unsigned long)GetLastError());
    }
    BOOL ok;
    if (has_body) {
        wchar_t headers[] = L"content-type: application/json";
        ok = WinHttpSendRequest(req, headers, (DWORD)-1L, body, (DWORD)strlen(body),
                                (DWORD)strlen(body), 0);
    } else {
        ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }
    if (!ok || !WinHttpReceiveResponse(req, NULL)) {
        DWORD code = GetLastError();
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(conn);
        WinHttpCloseHandle(session);
        return h_fail("HTTP %s failed: код %lu", upper, (unsigned long)code);
    }
    DWORD status = 0, len = sizeof(status);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    DWORD hlen = 0;
    WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, NULL,
                        &hlen, WINHTTP_NO_HEADER_INDEX);
    char *raw_headers = NULL;
    if (hlen > 0) {
        wchar_t *wh = xmalloc(hlen + sizeof(wchar_t));
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                                wh, &hlen, WINHTTP_NO_HEADER_INDEX))
            raw_headers = from_wide_n(wh, -1);
    }
    SB body_buf;
    sb_init(&body_buf);
    for (;;) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
        char *chunk = xmalloc(avail + 1);
        DWORD got = 0;
        if (!WinHttpReadData(req, chunk, avail, &got)) break;
        sb_add(&body_buf, chunk, got);
    }
    Value out = v_map();
    map_set(out.as.m, "status", v_int_i64((int64_t)status));
    map_set(out.as.m, "body", v_str(normalize_newlines(body_buf.buf ? body_buf.buf : "")));
    Value hm = v_map();
    if (raw_headers) {
        char *save = NULL;
        char *line = strtok_s(raw_headers, "\r\n", &save);
        int first = 1;
        while (line) {
            if (first) {
                first = 0;
            } else {
                char *colon = strchr(line, ':');
                if (colon) {
                    *colon = 0;
                    char *name = canonical_header(line);
                    const char *val = colon + 1;
                    while (*val == ' ') val++;
                    if (map_has(hm.as.m, name)) {
                        Value old = map_get(hm.as.m, name);
                        SB joined;
                        sb_init(&joined);
                        sb_addf(&joined, "%s, %s", old.as.s->s, val);
                        map_set(hm.as.m, name, v_str(joined.buf));
                    } else {
                        map_set(hm.as.m, name, v_str(val));
                    }
                }
            }
            line = strtok_s(NULL, "\r\n", &save);
        }
    }
    map_set(out.as.m, "headers", hm);
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(session);
    return out;
}

static Value b_http_get(int argc, Value *argv) { return http_call("get", argc, argv); }
static Value b_http_post(int argc, Value *argv) { return http_call("post", argc, argv); }
static Value b_http_put(int argc, Value *argv) { return http_call("put", argc, argv); }
static Value b_http_delete(int argc, Value *argv) { return http_call("delete", argc, argv); }

void http_install(Env *env) {
    static const char *names[] = {"http_get", "http_post", "http_put", "http_delete", NULL};
    NativeFn fns[4];
    fns[0] = b_http_get;
    fns[1] = b_http_post;
    fns[2] = b_http_put;
    fns[3] = b_http_delete;
    for (int i = 0; names[i]; i++) {
        Func *f = xmalloc(sizeof(Func));
        f->name = names[i];
        f->nparams = 0;
        f->params = NULL;
        f->body = NULL;
        f->closure = NULL;
        f->native = fns[i];
        env_define_public(env, names[i], v_func(f), 0);
    }
}
