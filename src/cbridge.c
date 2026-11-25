#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"

#include <windows.h>
#include <stdlib.h>
#include <string.h>

static Value c_fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_err.set = 1;
    g_err.has_line = 0;
    g_err.is_exit = 0;
    vsnprintf(g_err.msg, sizeof(g_err.msg), fmt, ap);
    va_end(ap);
    return v_null();
}

static Value b_c_load(int argc, Value *argv) {
    if (argc != 1 || argv[0].t != V_STR) return c_fail("c_load() expects (path)");
    HMODULE h = LoadLibraryA(argv[0].as.s->s);
    if (!h) return c_fail("c_load: не удалось загрузить '%s' (код %lu)", argv[0].as.s->s,
                          (unsigned long)GetLastError());
    return v_int_i64((int64_t)(intptr_t)h);
}

static Value b_c_free(int argc, Value *argv) {
    if (argc != 1 || argv[0].t != V_INT) return c_fail("c_free() expects (handle)");
    int64_t h = 0;
    if (!bi_is_i64(argv[0].as.i, &h) || !h) return c_fail("c_free() expects (handle)");
    FreeLibrary((HMODULE)(intptr_t)h);
    return v_null();
}

static Value b_c_sym(int argc, Value *argv) {
    if (argc != 2 || argv[0].t != V_INT || argv[1].t != V_STR)
        return c_fail("c_sym() expects (handle, name)");
    int64_t h = 0;
    if (!bi_is_i64(argv[0].as.i, &h) || !h) return c_fail("c_sym() expects (handle, name)");
    FARPROC p = GetProcAddress((HMODULE)(intptr_t)h, argv[1].as.s->s);
    if (!p) return c_fail("c_sym: символ '%s' не найден (код %lu)", argv[1].as.s->s,
                          (unsigned long)GetLastError());
    return v_int_i64((int64_t)(intptr_t)p);
}

static int c_need(int argc, Value *argv, int n, const char *name) {
    if (argc < n) {
        c_fail("%s() expects (handle, name, signature, ...args)", name);
        return 0;
    }
    return 1;
}

static Value b_c_call(int argc, Value *argv) {
    if (!c_need(argc, argv, 3, "c_call")) return v_null();
    if (argv[0].t != V_INT || argv[1].t != V_STR || argv[2].t != V_STR)
        return c_fail("c_call() expects (handle, name, signature, ...args)");
    int64_t h = 0;
    if (!bi_is_i64(argv[0].as.i, &h) || !h) return c_fail("c_call() expects (handle, name, signature, ...args)");
    FARPROC fp = GetProcAddress((HMODULE)(intptr_t)h, argv[1].as.s->s);
    if (!fp) return c_fail("c_call: символ '%s' не найден (код %lu)", argv[1].as.s->s,
                           (unsigned long)GetLastError());
    const char *sig = argv[2].as.s->s;
    char ret = sig[0];
    if (!ret) return c_fail("c_call: пустая сигнатура");
    int np = (int)strlen(sig) - 1;
    if (np > 8) return c_fail("c_call: не больше 8 аргументов");
    if (argc - 3 != np) return c_fail("c_call: сигнатура ждёт %d аргумент(ов), получено %d", np, argc - 3);
    int all_dbl = np > 0, all_int = np > 0;
    for (int i = 0; i < np; i++) {
        if (sig[1 + i] == 'd') all_int = 0;
        else if (sig[1 + i] == 'i' || sig[1 + i] == 's') all_dbl = 0;
        else return c_fail("c_call: неизвестный тип '%c' (i, s, d)", sig[1 + i]);
    }
    int64_t ia[8];
    double da[8];
    char *sa[8];
    for (int i = 0; i < np; i++) {
        Value v = argv[3 + i];
        ia[i] = 0;
        da[i] = 0;
        sa[i] = NULL;
        if (sig[1 + i] == 'd') {
            if (v.t == V_INT) da[i] = bi_to_f64(v.as.i);
            else if (v.t == V_FLOAT) da[i] = v.as.f;
            else if (v.t == V_BOOL) da[i] = v.as.b;
            else return c_fail("c_call: аргумент %d должен быть числом", i + 1);
        } else if (sig[1 + i] == 's') {
            if (v.t != V_STR) return c_fail("c_call: аргумент %d должен быть строкой", i + 1);
            sa[i] = v.as.s->s;
            ia[i] = (int64_t)(intptr_t)sa[i];
        } else {
            if (v.t == V_INT) {
                if (!bi_is_i64(v.as.i, &ia[i])) return c_fail("c_call: аргумент %d слишком велик", i + 1);
            } else if (v.t == V_FLOAT) {
                ia[i] = (int64_t)v.as.f;
            } else if (v.t == V_BOOL) {
                ia[i] = v.as.b;
            } else {
                return c_fail("c_call: аргумент %d должен быть числом", i + 1);
            }
        }
    }
    if (np > 0 && !all_int && !all_dbl)
        return c_fail("c_call: смешанные int/double аргументы не поддерживаются, разделите вызов");
    if (ret == 'd') {
        double r = 0;
        switch (np) {
        case 0: r = ((double (*)(void))fp)(); break;
        case 1: r = ((double (*)(double))fp)(da[0]); break;
        case 2: r = ((double (*)(double, double))fp)(da[0], da[1]); break;
        case 3: r = ((double (*)(double, double, double))fp)(da[0], da[1], da[2]); break;
        case 4: r = ((double (*)(double, double, double, double))fp)(da[0], da[1], da[2], da[3]); break;
        default: return c_fail("c_call: для double поддерживается до 4 аргументов");
        }
        return v_float(r);
    }
    int64_t r = 0;
    switch (np) {
    case 0: r = ((int64_t (*)(void))fp)(); break;
    case 1: r = ((int64_t (*)(int64_t))fp)(ia[0]); break;
    case 2: r = ((int64_t (*)(int64_t, int64_t))fp)(ia[0], ia[1]); break;
    case 3: r = ((int64_t (*)(int64_t, int64_t, int64_t))fp)(ia[0], ia[1], ia[2]); break;
    case 4: r = ((int64_t (*)(int64_t, int64_t, int64_t, int64_t))fp)(ia[0], ia[1], ia[2], ia[3]); break;
    case 5: r = ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t))fp)(ia[0], ia[1], ia[2], ia[3], ia[4]); break;
    case 6: r = ((int64_t (*)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t))fp)(ia[0], ia[1], ia[2], ia[3], ia[4], ia[5]); break;
    default: return c_fail("c_call: поддерживается до 6 целых аргументов");
    }
    if (ret == 's') {
        if (!r) return v_null();
        return v_str((const char *)(intptr_t)r);
    }
    if (ret == 'v') return v_null();
    return v_int_i64(r);
}

void c_install(Env *env) {
    static const char *names[] = {"c_load", "c_call", "c_sym", "c_free", NULL};
    NativeFn fns[4];
    fns[0] = b_c_load;
    fns[1] = b_c_call;
    fns[2] = b_c_sym;
    fns[3] = b_c_free;
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
