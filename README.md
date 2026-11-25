# uniland-c

Порт интерпретатора UniLand 2.2.0 с Go на C11 (MSVC). Отдельный проект рядом с эталоном `../uniland-go`.

Автор языка UniLand — rawumi (ранее name-cho).

## Сборка

```
build.bat
```

Нужен MSVC (Build Tools 18). Результат — `uniland.exe` в корне проекта.

## Что умеет

- Лексер, парсер и дерево-интерпретатор: переменные, функции, замыкания, циклы, `try/catch/finally`, шаблонные строки, массивы, объекты.
- Длинная арифметика без внешних зависимостей (`bigint.c`), числа произвольной точности.
- Стандартная библиотека: строки, массивы, объекты, математика, JSON, файлы, время, `env`, `exit`, `argv`, `http_*`.
- CLI как у эталона: `run`, `eval`, `check`, `repl`, `fmt`, `lsp`, `-e`, `-i`, `-c`, `-d`, `-v`, `-h`.

## Мосты и интеграции

### Python (`src/pybridge.c`, `src/pyworker.py`)

Настоящий Python-процесс по протоколу эталона: JSON-строки через stdin/stdout, общий набор переменных на всё время работы.

- `python { ... }` и `pyexec(...)` исполняют код, `pyeval(...)` возвращает значение.
- `uni.x` в Python читает переменную UniLand, `uni.x = v` пишет её обратно, `uni.call("f", 21)` вызывает функцию UniLand.
- Вывод print из Python уходит в stdout интерпретатора, ошибки — в stderr.

Python ищется так: `UNILAND_PYTHON`, затем `python3.exe`, затем `python.exe` в PATH.

### C (`src/cbridge.c`)

```
let m = c_load("msvcrt.dll")
print(c_call(m, "strlen", "is", "hello"))
print(c_call(m, "pow", "ddd", 2, 10))
c_free(m)
```

- `c_load(path)` — адрес библиотеки, `c_sym(handle, name)`, `c_free(handle)`.
- `c_call(handle, name, sig, ...args)`: первый символ `sig` — тип результата, дальше типы аргументов: `i` — целое/указатель, `s` — строка, `d` — double. Результат: `i` — число, `d` — float, `s` — строка, `v` — null.
- Ограничения: аргументы в одном вызове либо все целые/строковые, либо все double (до 6 целых, до 4 double). Смешанные типы дают понятную ошибку.

### Go (`src/gobridge.c`, `src/goworker.go`)

`goeval`, `goexec` и блоки `go { ... }` исполняет постоянный воркер на yaegi — один процесс на весь запуск скрипта, поэтому переменные и объявления живут между вызовами.

- Предзагружены пакеты: `fmt`, `strings`, `strconv`, `math`, `sort`, `time`, `regexp`, `unicode`, `math/big`.
- `uni.SetInt/SetStr/SetFloat/SetBool/SetBig`, `uni.GetInt/GetStr/GetFloat/GetBool`, `uni.Call(name, args...)` работают через строковый RPC между Go и интерпретатором.
- Первый запуск собирает воркер: `go build` в `%TEMP%\uniland-goworker-<pid>`, модуль `yaegi v0.16.3` подтягивается из кеша Go или из сети. Дальше запуск идёт без компиляции.
- `go { ... }` оборачивается в функцию, поэтому объявления внутри блока локальны для блока; обмен данными — через `uni`.
- Воркер кешируется в `%TEMP%\uniland-goworker-c\worker.exe` и переиспользуется между запусками. Если `go` нет в PATH, `goeval`/`goexec`/`go { }` возвращают ошибку с причиной.

Windows-нюанс: `go { }` в исходнике должен открывать скобку на той же строке (`go {`), а тело — идти со следующей. Открывающая скобка на следующей строке уйдёт в тело как обрезанный `{`.

### HTTP (`src/http.c`)

`http_get`, `http_post`, `http_put`, `http_delete` на WinHTTP.

- Возвращают объект `{status, body, headers}`; `body` с нормализованными переводами строк, ключи `headers` в каноническом виде (`Content-Type`).
- `post`/`put` сериализуют второй аргумент в JSON и отправляют с `content-type: application/json`.
- Ошибки соединения приходят как исключение: `HTTP GET failed: код 12029`.

### Форматтер (`src/fmt.c`)

`uniland fmt [-w] [-l] FILE` — тот же алгоритм, что у эталона: четыре пробела на уровень, отступ закрывающих скобок, verbatim-строки и блоки `python`/`go` не трогаются. `-w` пишет файл, `-l` печатает имя, если файл изменился, и выходит с кодом 1.

### LSP (`src/lsp.c`)

`uniland lsp` — Language Server по stdio: `initialize`, `initialized`, `shutdown`, `exit`, `textDocument/didOpen`, `didChange`, `formatting` с диагностикой из парсера и правкой форматирования.

### GUI (`src/gui.c`)

Win32 с wide-API, поэтому кириллица в окнах и диалогах корректная.

- `gui_window`, `gui_label`, `gui_button` (с колбэком), `gui_input`, `gui_get`, `gui_set`, `gui_run`, `gui_close`, `gui_on_close`.
- `alert`, `confirm`, `prompt`, `pick_file`, `pick_save`, `pick_folder`.
- `gui_image`/`gui_image_button` грузят BMP/ICO; `gui_gif` и `gui_video` возвращают явную ошибку «не поддерживается».
- `UNILAND_NOGUI=1` включает поведение эталонной сборки с тегом `nogui` (все GUI-функции возвращают ту же ошибку) — так гоняются golden-тесты.

## Проверка

```
python tools\run_golden.py
python tools\run_tokens.py
python tools\check_fmt.py
python tools\test_lsp.py
```

- `run_golden.py` сверяет stdout/stderr/код возврата всех 16 кейсов `../uniland-go/compat/cases` с `compat/expected`.
- `run_tokens.py` сверяет дампы токенов через `run -d` с `compat/expected-tokens`.
- `check_fmt.py` проверяет идемпотентность форматтера и неизменность потока токенов на всех кейсах.
- `test_lsp.py` прогоняет initialize/didOpen/formatting/неизвестный метод/shutdown/exit.

## Чего нет

- Байткод-VM эталона: используется дерево-интерпретатор.
- GIF-анимация и видео в GUI; из изображений только BMP/ICO (без GDI+).
- Мост Go-кода в обратную сторону (`uni` внутри Go работает, но Go-функции не видны из UniLand как значения).

## Файлы

- `src/uniland.h` — типы и прототипы.
- `src/bigint.c` — длинная арифметика.
- `src/value.c` — значения, UTF-8, ошибки, форматирование.
- `src/lexer.c`, `src/parser.c` — токены и AST.
- `src/interp.c` — исполнение.
- `src/std.c` — стандартная библиотека и установка встроенных функций.
- `src/pybridge.c`, `src/cbridge.c`, `src/gobridge.c`, `src/http.c` — интеграции.
- `src/fmt.c`, `src/lsp.c`, `src/gui.c` — форматтер, LSP, GUI.
- `src/pyworker.py` + `src/pyworker_py.h`, `src/goworker.go` + `src/goworker_go.h` — воркеры и их встроенные копии (обновлять генераторами из `tools/`).
- `src/main.c` — CLI.
- `src_backup/` — копия исходников до удаления комментариев.
