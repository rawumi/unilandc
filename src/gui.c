#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "uniland.h"

#include <windows.h>

#include <commdlg.h>
#include <shlobj.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define GUI_ERR "GUI недоступен в этой сборке: нет cgo или включён тег nogui"

typedef struct {
    HWND hwnd;
    int kind;
    int has_fn;
    Value fn;
} Widget;

static HWND g_win;
static Widget g_widgets[256];
static int g_nwidgets;
static int g_layout_y;
static Value g_on_close;
static int g_has_on_close;
static int g_class_ready;

static const wchar_t *const k_class = L"UniLandWindowW";
static const wchar_t *const k_prompt_class = L"UniLandPromptW";

static Value g_fail(const char *msg) {
    err_msg(msg);
    return v_null();
}

static int no_gui(void) {
    const char *v = getenv("UNILAND_NOGUI");
    return v && *v && *v != '0';
}

static wchar_t *u2w(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) n = 1;
    wchar_t *w = xmalloc(sizeof(wchar_t) * (size_t)n);
    if (MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) w[0] = 0;
    return w;
}

static char *w2u(const wchar_t *s) {
    if (!s) return xstrdup("");
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return xstrdup("");
    char *r = xmalloc((size_t)n);
    if (WideCharToMultiByte(CP_UTF8, 0, s, -1, r, n, NULL, NULL) <= 0) r[0] = 0;
    return r;
}

static int widget_index(HWND h) {
    for (int i = 0; i < g_nwidgets; i++)
        if (g_widgets[i].hwnd == h) return i;
    return -1;
}

static void call_cb(Value fn) {
    if (fn.t != V_FUNC) return;
    err_clear();
    call_value(fn, 0, NULL, -1);
    if (g_err.set) {
        if (!g_err.is_exit) fprintf(stderr, "UniLand error: %s\n", g_err.msg);
        err_clear();
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND: {
        int idx = widget_index((HWND)lp);
        if (idx >= 0 && HIWORD(wp) == BN_CLICKED && g_widgets[idx].has_fn)
            call_cb(g_widgets[idx].fn);
        return 0;
    }
    case WM_CLOSE:
        if (g_has_on_close) call_cb(g_on_close);
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

static void ensure_class(void) {
    if (g_class_ready) return;
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = k_class;
    RegisterClassW(&wc);
    g_class_ready = 1;
}

static int widget_put(HWND h, int kind) {
    if (g_nwidgets >= 256) return -1;
    g_widgets[g_nwidgets].hwnd = h;
    g_widgets[g_nwidgets].kind = kind;
    g_widgets[g_nwidgets].has_fn = 0;
    g_widgets[g_nwidgets].fn = v_null();
    return g_nwidgets++;
}

static HWND child(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, g_win,
                           (HMENU)(intptr_t)id, GetModuleHandleW(NULL), NULL);
}

static const char *arg_text(int argc, Value *argv, int i, const char *def) {
    if (i >= argc) return def;
    Value s = value_py_str(argv[i]);
    return s.as.s->s;
}

static int arg_int(int argc, Value *argv, int i, int def, int *ok) {
    *ok = 1;
    if (i >= argc) return def;
    if (argv[i].t == V_INT) {
        int64_t v = 0;
        if (bi_is_i64(argv[i].as.i, &v)) return (int)v;
    }
    if (argv[i].t == V_FLOAT) return (int)argv[i].as.f;
    if (argv[i].t == V_BOOL) return argv[i].as.b;
    *ok = 0;
    return def;
}

static HWND need_win(int argc, Value *argv, const char *name) {
    if (argc < 1) {
        err_msgf("%s() needs a window", name);
        return NULL;
    }
    if (!g_win) {
        err_msgf("%s() needs gui_window() first", name);
        return NULL;
    }
    return g_win;
}

static Value b_gui_window(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    ensure_class();
    int ok = 1;
    int w = arg_int(argc, argv, 1, 400, &ok);
    int h = arg_int(argc, argv, 2, 300, &ok);
    if (!ok) return g_fail("gui_window() expects numbers for width and height");
    if (g_win) return v_int_i64(1);
    wchar_t *title = u2w(arg_text(argc, argv, 0, "UniLand"));
    g_win = CreateWindowExW(0, k_class, title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, w,
                            h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_win) return g_fail("gui_window: не удалось создать окно");
    g_layout_y = 10;
    return v_int_i64(1);
}

static int client_width(void) {
    RECT rc;
    GetClientRect(g_win, &rc);
    return rc.right > 40 ? rc.right - 20 : 380;
}

static Value b_gui_label(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (!need_win(argc, argv, "gui_label")) return v_null();
    wchar_t *text = u2w(arg_text(argc, argv, 1, ""));
    HWND h = child(L"STATIC", text, SS_LEFT, 10, g_layout_y, client_width(), 20, 1000 + g_nwidgets);
    g_layout_y += 24;
    int id = widget_put(h, 1);
    return v_int_i64(id + 1);
}

static Value b_gui_button(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (!need_win(argc, argv, "gui_button")) return v_null();
    int idx = g_nwidgets;
    wchar_t *text = u2w(arg_text(argc, argv, 1, "Button"));
    HWND h = child(L"BUTTON", text, BS_PUSHBUTTON, 10, g_layout_y, client_width(), 26, 2000 + idx);
    g_layout_y += 30;
    widget_put(h, 3);
    if (argc > 2 && argv[2].t == V_FUNC) {
        g_widgets[idx].has_fn = 1;
        g_widgets[idx].fn = argv[2];
    }
    return v_int_i64(idx + 1);
}

static Value b_gui_input(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (!need_win(argc, argv, "gui_input")) return v_null();
    wchar_t *text = u2w(arg_text(argc, argv, 1, ""));
    HWND h = child(L"EDIT", text, WS_BORDER | ES_AUTOHSCROLL, 10, g_layout_y, client_width(), 24,
                   3000 + g_nwidgets);
    g_layout_y += 28;
    int id = widget_put(h, 2);
    return v_int_i64(id + 1);
}

static HWND widget_hwnd(Value v) {
    if (v.t != V_INT) return NULL;
    int64_t n = 0;
    if (!bi_is_i64(v.as.i, &n)) return NULL;
    if (n == 1) return g_win;
    int idx = (int)n - 1;
    if (idx < 0 || idx >= g_nwidgets) return NULL;
    return g_widgets[idx].hwnd;
}

static Value b_gui_get(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (argc < 1) return g_fail("gui_get() needs a widget");
    HWND h = widget_hwnd(argv[0]);
    if (!h) return g_fail("gui_get(): неизвестный виджет");
    int len = GetWindowTextLengthW(h);
    wchar_t *buf = xmalloc(sizeof(wchar_t) * (size_t)(len + 2));
    GetWindowTextW(h, buf, len + 2);
    char *text = w2u(buf);
    return v_str(text);
}

static Value b_gui_set(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (argc < 2) return g_fail("gui_set() needs a widget and a value");
    HWND h = widget_hwnd(argv[0]);
    if (!h) return g_fail("gui_set(): неизвестный виджет");
    Value s = value_py_str(argv[1]);
    wchar_t *text = u2w(s.as.s->s);
    SetWindowTextW(h, text);
    return v_null();
}

static Value b_gui_run(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (!g_win) return g_fail("gui_run(): нет окна, вызовите gui_window()");
    ShowWindow(g_win, SW_SHOW);
    UpdateWindow(g_win);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return v_null();
}

static Value b_gui_close(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (g_win) DestroyWindow(g_win);
    g_win = NULL;
    return v_null();
}

static Value b_gui_on_close(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    if (argc >= 2 && argv[1].t == V_FUNC) {
        g_has_on_close = 1;
        g_on_close = argv[1];
    }
    return v_null();
}

static Value make_image(int argc, Value *argv, int button) {
    if (!need_win(argc, argv, button ? "gui_image_button" : "gui_image")) return v_null();
    const char *path = arg_text(argc, argv, 1, "");
    int ok = 1;
    int w = arg_int(argc, argv, button ? 3 : 2, 0, &ok);
    int h = arg_int(argc, argv, button ? 4 : 3, 0, &ok);
    if (!ok) return g_fail("gui_image(): ширина и высота должны быть числами");
    wchar_t *wpath = u2w(path);
    HBITMAP bmp = (HBITMAP)LoadImageW(NULL, wpath, IMAGE_BITMAP, 0, 0,
                                      LR_LOADFROMFILE | LR_CREATEDIBSECTION);
    if (!bmp) return g_fail("gui_image: не удалось загрузить изображение (поддерживается BMP/ICO)");
    int cw = w > 0 ? w : 120;
    int ch = h > 0 ? h : 120;
    HWND st = child(L"STATIC", L"", SS_BITMAP | SS_CENTERIMAGE, 10, g_layout_y, cw, ch,
                    4000 + g_nwidgets);
    SendMessageW(st, STM_SETIMAGE, (WPARAM)IMAGE_BITMAP, (LPARAM)bmp);
    g_layout_y += ch + 6;
    int idx = widget_put(st, button ? 5 : 4);
    if (button && argc > 2 && argv[2].t == V_FUNC) {
        g_widgets[idx].has_fn = 1;
        g_widgets[idx].fn = argv[2];
    }
    return v_int_i64(idx + 1);
}

static Value b_gui_image(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    return make_image(argc, argv, 0);
}

static Value b_gui_image_button(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    return make_image(argc, argv, 1);
}

static Value b_gui_gif(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    return g_fail("gui_gif: анимация GIF не поддерживается, используйте gui_image (BMP)");
}

static Value b_gui_video(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    return g_fail("gui_video: воспроизведение видео не поддерживается");
}

static Value open_dialog(int save, const wchar_t *title) {
    wchar_t file[MAX_PATH * 4];
    file[0] = 0;
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_win;
    ofn.lpstrFilter = L"Все файлы\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST;
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (!ok) return v_null();
    return v_str(w2u(file));
}

static Value b_pick_file(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    return open_dialog(0, u2w(arg_text(argc, argv, 0, "Выбери файл")));
}

static Value b_pick_save(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    return open_dialog(1, u2w(arg_text(argc, argv, 0, "Сохранить как")));
}

static Value b_pick_folder(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    BROWSEINFOW bi;
    memset(&bi, 0, sizeof(bi));
    bi.hwndOwner = g_win;
    bi.lpszTitle = u2w(arg_text(argc, argv, 0, "Выбери папку"));
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return v_null();
    wchar_t path[MAX_PATH * 4];
    path[0] = 0;
    SHGetPathFromIDListW(pidl, path);
    CoTaskMemFree(pidl);
    if (!path[0]) return v_null();
    return v_str(w2u(path));
}

static Value b_alert(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    MessageBoxW(g_win, u2w(arg_text(argc, argv, 0, "")), L"UniLand", MB_OK | MB_ICONINFORMATION);
    return v_null();
}

static Value b_confirm(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    int r = MessageBoxW(g_win, u2w(arg_text(argc, argv, 0, "")), L"UniLand",
                        MB_YESNO | MB_ICONQUESTION);
    return v_bool(r == IDYES);
}

typedef struct {
    wchar_t text[1024];
    int done;
} PromptState;

static LRESULT CALLBACK prompt_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    PromptState *st = (PromptState *)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1) {
            GetDlgItemTextW(h, 10, st->text, 1024);
            st->done = 1;
            PostMessageW(h, WM_CLOSE, 0, 0);
        } else if (id == 2) {
            st->done = 0;
            PostMessageW(h, WM_CLOSE, 0, 0);
        }
        return 0;
    }
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

static Value b_prompt(int argc, Value *argv) {
    if (no_gui()) return g_fail(GUI_ERR);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = prompt_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = k_prompt_class;
    RegisterClassW(&wc);
    PromptState st;
    memset(&st, 0, sizeof(st));
    wchar_t *title = u2w(arg_text(argc, argv, 0, ""));
    wchar_t *initial = u2w(arg_text(argc, argv, 1, ""));
    wcscpy_s(st.text, 1024, initial);
    HWND w = CreateWindowExW(WS_EX_DLGMODALFRAME, k_prompt_class, title,
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 420,
                             160, g_win, NULL, GetModuleHandleW(NULL), NULL);
    if (!w) return g_fail("prompt: не удалось создать окно");
    SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)&st);
    CreateWindowExW(0, L"STATIC", title, WS_CHILD | WS_VISIBLE, 12, 12, 380, 20, w, NULL,
                    GetModuleHandleW(NULL), NULL);
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial,
                                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 12, 40, 380, 24, w, (HMENU)10,
                                GetModuleHandleW(NULL), NULL);
    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 220, 78, 80, 26, w,
                    (HMENU)1, GetModuleHandleW(NULL), NULL);
    CreateWindowExW(0, L"BUTTON", L"Отмена", WS_CHILD | WS_VISIBLE, 312, 78, 80, 26, w, (HMENU)2,
                    GetModuleHandleW(NULL), NULL);
    ShowWindow(w, SW_SHOW);
    SetFocus(edit);
    MSG msg;
    while (IsWindow(w) && GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(w);
    if (!st.done) return v_null();
    return v_str(w2u(st.text));
}

void gui_install(Env *env) {
    static const char *names[] = {
        "gui_window", "gui_label", "gui_button", "gui_input", "gui_get", "gui_set", "gui_run",
        "gui_close", "gui_on_close", "gui_image", "gui_image_button", "gui_gif", "gui_video",
        "pick_file", "pick_save", "pick_folder", "alert", "confirm", "prompt",
        "get_value", "set_text", "show_alert", "show_confirm", "show_prompt", NULL};
    NativeFn fns[24];
    fns[0] = b_gui_window;
    fns[1] = b_gui_label;
    fns[2] = b_gui_button;
    fns[3] = b_gui_input;
    fns[4] = b_gui_get;
    fns[5] = b_gui_set;
    fns[6] = b_gui_run;
    fns[7] = b_gui_close;
    fns[8] = b_gui_on_close;
    fns[9] = b_gui_image;
    fns[10] = b_gui_image_button;
    fns[11] = b_gui_gif;
    fns[12] = b_gui_video;
    fns[13] = b_pick_file;
    fns[14] = b_pick_save;
    fns[15] = b_pick_folder;
    fns[16] = b_alert;
    fns[17] = b_confirm;
    fns[18] = b_prompt;
    fns[19] = b_gui_get;
    fns[20] = b_gui_set;
    fns[21] = b_alert;
    fns[22] = b_confirm;
    fns[23] = b_prompt;
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
