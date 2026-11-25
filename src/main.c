
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"

#include <windows.h>

#include <stdlib.h>
#include <string.h>

#define VERSION "2.2.0"

static const char *const k_version_line = "UniLand " VERSION;
static const char *const k_usage_line =
    "usage: uniland [-h] [-v] [-e CODE] [-i] [-c FILE] {run,eval,repl,check} ...";

static int usage_err(const char *msg) {
    fprintf(stderr, "%s\n", k_usage_line);
    fprintf(stderr, "uniland: error: %s\n", msg);
    return 2;
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

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 8192, len = 0;
    char *buf = xmalloc(cap);
    while (1) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            buf = xrealloc(buf, cap);
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) break;
    }
    fclose(f);
    buf[len] = 0;
    return buf;
}

static int report_error(void) {
    if (g_err.has_line)
        fprintf(stderr, "UniLand error: Error at line %d: %s\n", g_err.line, g_err.msg);
    else
        fprintf(stderr, "UniLand error: %s\n", g_err.msg);
    return 1;
}

static int run_source(const char *source, int debug, char **script_args, int n_script_args) {
    char *src = normalize_newlines(source);
    int ntok = 0;
    Token *tokens = lex(src, &ntok);
    if (g_err.set) return report_error();
    if (debug) {
        printf("Tokens:\n");
        for (int i = 0; i < ntok; i++) printf("  %s\n", tok_dump(&tokens[i]));
    }
    Node *prog = parse_program(tokens);
    if (g_err.set || !prog) return report_error();
    interp_init(stdout, stderr, stdin, script_args, n_script_args);
    Flow f = interp_interpret(prog);
    if (f == F_EXIT) return g_it.exit_code;
    if (f == F_ERROR) return report_error();
    return 0;
}

static int source_from_file(const char *path) {
    char *data = read_file(path);
    if (!data) {
        fprintf(stderr, "Error: file '%s' not found.\n", path);
        return 1;
    }
    return run_source(data, 0, NULL, 0);
}

static int cmd_check(const char *path) {
    char *data = read_file(path);
    if (!data) {
        fprintf(stderr, "Error: file '%s' not found.\n", path);
        return 1;
    }
    char *src = normalize_newlines(data);
    int ntok = 0;
    Token *tokens = lex(src, &ntok);
    if (g_err.set) return report_error();
    if (!parse_program(tokens) || g_err.set) return report_error();
    printf("OK: %s — синтаксис в порядке\n", path);
    return 0;
}

static int balanced(const char *src) {
    char stack[256];
    int sp = 0;
    char in_str = 0, prev = 0;
    for (const char *p = src; *p; p++) {
        char ch = *p;
        if (in_str) {
            if (ch == in_str && prev != '\\') in_str = 0;
        } else if (ch == '"' || ch == '`' || ch == '\'') {
            in_str = ch;
        } else if (ch == '(' || ch == '[' || ch == '{') {
            if (sp < (int)sizeof(stack)) stack[sp++] = ch;
        } else if (ch == ')' || ch == ']' || ch == '}') {
            char want = ch == ')' ? '(' : ch == ']' ? '[' : '{';
            if (sp > 0 && stack[sp - 1] == want) sp--;
        }
        prev = ch;
    }
    return sp == 0 && in_str == 0;
}

static int cmd_repl(void) {
    printf("UniLand %s — интерактивный режим.\n", VERSION);
    printf("Введи код и Enter. Выход: exit, quit или Ctrl+D.\n");
    interp_init(stdout, stderr, stdin, NULL, 0);
    SB buf;
    sb_init(&buf);
    char line[4096];
    for (;;) {
        printf("%s", buf.len ? "... " : "uni> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n");
            break;
        }
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r')) line[--ln] = 0;
        if (buf.len == 0) {
            const char *t = line;
            while (*t == ' ' || *t == '\t') t++;
            if (!strcmp(t, "exit") || !strcmp(t, "exit()") || !strcmp(t, "quit") ||
                !strcmp(t, "quit()"))
                return 0;
        }
        if (buf.len) sb_addch(&buf, '\n');
        sb_adds(&buf, line);
        const char *body = buf.buf;
        while (*body == ' ' || *body == '\t' || *body == '\n') body++;
        if (*body && !balanced(buf.buf)) continue;
        if (!*body) {
            buf.len = 0;
            buf.buf[0] = 0;
            continue;
        }
        char *src = normalize_newlines(buf.buf);
        buf.len = 0;
        buf.buf[0] = 0;
        int ntok = 0;
        Token *tokens = lex(src, &ntok);
        if (g_err.set) {
            report_error();
            err_clear();
            continue;
        }
        Node *prog = parse_program(tokens);
        if (g_err.set || !prog) {
            report_error();
            err_clear();
            continue;
        }
        char *echo = interp_eval_program(prog);
        if (g_err.set) {
            if (!g_err.is_exit) report_error();
            int code = g_err.is_exit ? g_err.exit_code : 0;
            if (g_err.is_exit) return code;
            err_clear();
            continue;
        }
        if (echo && *echo) printf("%s\n", echo);
    }
    return 0;
}

typedef struct {
    int version, help, interactive, run_debug;
    const char *check_file;
    const char *eval_code;
    const char *command;
    const char *run_file;
    const char *check_file2;
    const char *eval_code2;
    char **script_args;
    int n_script_args;
} Cli;

static int parse_args(int argc, char **argv, Cli *a) {
    memset(a, 0, sizeof(*a));
    a->script_args = xmalloc(sizeof(char *) * (size_t)(argc + 1));
    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        if (!strcmp(s, "-v") || !strcmp(s, "--version")) {
            a->version = 1;
        } else if (!strcmp(s, "-h") || !strcmp(s, "--help")) {
            a->help = 1;
        } else if (!strcmp(s, "-e") || !strcmp(s, "--eval")) {
            if (i + 1 >= argc) return usage_err("argument -e/--eval: expected one argument");
            a->eval_code = argv[++i];
        } else if (!strcmp(s, "-i") || !strcmp(s, "--interactive")) {
            a->interactive = 1;
        } else if (!strcmp(s, "-c") || !strcmp(s, "--check")) {
            if (i + 1 >= argc) return usage_err("argument -c/--check: expected one argument");
            a->check_file = argv[++i];
        } else if (!a->command) {
            a->command = s;
            if (!strcmp(s, "run")) {
                for (i++; i < argc; i++) {
                    const char *x = argv[i];
                    if (!a->run_file && (!strcmp(x, "-d") || !strcmp(x, "--debug"))) {
                        a->run_debug = 1;
                        continue;
                    }
                    if (!a->run_file && x[0] == '-') {
                        if (a->n_script_args > 0 || a->run_debug) {
                            a->script_args[a->n_script_args++] = (char *)x;
                            continue;
                        }
                        a->script_args[a->n_script_args++] = (char *)x;
                        for (int j = i + 1; j < argc; j++)
                            a->script_args[a->n_script_args++] = argv[j];
                        i = argc;
                        break;
                    }
                    if (!a->run_file) {
                        a->run_file = x;
                        continue;
                    }
                    a->script_args[a->n_script_args++] = (char *)x;
                }
            } else if (!strcmp(s, "eval")) {
                if (i + 1 < argc) a->eval_code2 = argv[i + 1];
                i++;
            } else if (!strcmp(s, "check")) {
                if (i + 1 < argc) a->check_file2 = argv[i + 1];
                i++;
            }
        } else {
            char msg[1024];
            snprintf(msg, sizeof(msg), "unrecognized arguments: %s", argv[i]);
            return usage_err(msg);
        }
    }
    return 0;
}

static void print_help(void) {
    printf("%s\n", k_usage_line);
    printf("\n");
    printf("UniLand — лёгкий дружелюбный язык сценариев\n");
    printf("\n");
    printf("positional arguments:\n");
    printf("  {run,eval,repl,check}\n");
    printf("    run                запустить .uni скрипт\n");
    printf("    eval               выполнить код одной строкой\n");
    printf("    repl               интерактивный режим (REPL)\n");
    printf("    check              проверить синтаксис без запуска\n");
    printf("options:\n");
    printf("  -h, --help           show this help message and exit\n");
    printf("  -v, --version        show program's version number and exit\n");
    printf("  -e CODE, --eval CODE\n");
    printf("                       выполнить код одной строкой и выйти\n");
    printf("  -i, --interactive    запустить интерактивный режим (REPL)\n");
    printf("  -c FILE, --check FILE\n");
    printf("                       проверить синтаксис файла без запуска\n");
    printf("\n");
    printf("Примеры:  uniland run script.uni   |   uniland -e 'print(2+2)'   |   uniland -i\n");
    printf("Форматтер (расширение uniland-go):  uniland fmt [-w] [-l] script.uni\n");
}

static int cmd_fmt(int argc, char **argv) {
    int write_back = 0, list = 0;
    const char *file = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-w") || !strcmp(a, "--write")) {
            write_back = 1;
        } else if (!strcmp(a, "-l") || !strcmp(a, "--list")) {
            list = 1;
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            printf("usage: uniland fmt [-w] [-l] FILE\n");
            printf("  без флагов — вывод в stdout; -w — записать; -l — только проверить\n");
            return 0;
        } else if (a[0] == '-' || file) {
            char msg[1024];
            snprintf(msg, sizeof(msg), "unrecognized arguments: %s", a);
            return usage_err(msg);
        } else {
            file = a;
        }
    }
    if (!file) return usage_err("the following arguments are required: file");
    char *data = read_file(file);
    if (!data) {
        fprintf(stderr, "Error: file '%s' not found.\n", file);
        return 1;
    }
    char err[2048];
    char *formatted = unifmt_format(data, err, sizeof(err));
    if (!formatted) {
        fprintf(stderr, "UniLand error: %s\n", err);
        return 1;
    }
    char *norm = normalize_newlines(data);
    int changed = strcmp(formatted, norm) != 0;
    if (list) {
        if (changed) {
            printf("%s\n", file);
            return 1;
        }
        return 0;
    }
    if (write_back) {
        if (!changed) return 0;
        FILE *f = fopen(file, "wb");
        if (!f) {
            fprintf(stderr, "UniLand error: не удалось записать %s\n", file);
            return 1;
        }
        fwrite(formatted, 1, strlen(formatted), f);
        fclose(f);
        return 0;
    }
    fputs(formatted, stdout);
    return 0;
}

int main(int argc, char **argv) {
    atexit(py_bridge_close);
    atexit(go_bridge_close);
    if (GetConsoleOutputCP() != 0) {
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);
    }
    if (argc > 1 && !strcmp(argv[1], "lsp")) {
        return lsp_serve();
    }
    if (argc > 1 && !strcmp(argv[1], "fmt")) {
        return cmd_fmt(argc - 2, argv + 2);
    }
    Cli a;
    int rc = parse_args(argc - 1, argv + 1, &a);
    if (rc) return rc;
    if (a.help) {
        print_help();
        return 0;
    }
    if (a.version) {
        printf("%s\n", k_version_line);
        return 0;
    }
    if (a.check_file) return cmd_check(a.check_file);
    if (a.eval_code) return run_source(a.eval_code, 0, NULL, 0);
    if (a.interactive) return cmd_repl();
    if (!a.command) return cmd_repl();
    if (!strcmp(a.command, "run")) {
        if (!a.run_file) return usage_err("the following arguments are required: file");
        char *data = read_file(a.run_file);
        if (!data) {
            fprintf(stderr, "Error: file '%s' not found.\n", a.run_file);
            return 1;
        }
        return run_source(data, a.run_debug, a.script_args, a.n_script_args);
    }
    if (!strcmp(a.command, "eval")) {
        if (!a.eval_code2) return usage_err("the following arguments are required: code");
        return run_source(a.eval_code2, 0, NULL, 0);
    }
    if (!strcmp(a.command, "check")) {
        if (!a.check_file2) return usage_err("the following arguments are required: file");
        return cmd_check(a.check_file2);
    }
    if (!strcmp(a.command, "repl")) return cmd_repl();
    return cmd_repl();
}
