# UniLand с нуля: учебник по языку и по коду порта

Этот учебник рассчитан на две аудитории сразу: тем, кто хочет писать на UniLand, и тем, кто хочет читать и менять сам порт на C. Сначала язык, потом устройство реализации.

## Запуск

Собери `uniland.exe`:

```bat
build.bat
```

Основные команды:

```bat
uniland run script.uni
uniland eval "print(2 + 2)"
uniland -e "print(2 + 2)"
uniland repl
uniland check script.uni
uniland fmt -w script.uni
uniland lsp
uniland -v
```

`-c` проверяет синтаксис без запуска, `-d` печатает токены, `-i` идёт в REPL после запуска файла.

## Первая программа

```uni
let name = "мир"
print("привет, " + name)
```

`print` принимает любое число аргументов и печатает их через пробел.

## Переменные и лёгкий синтаксис

```uni
let x = 5
x = 6
```

Главное отличие от эталонного синтаксиса: если переменная уже объявлена, `let` можно не писать. Это то, что в README называется лёгким синтаксисом. Старые формы продолжают работать, поэтому старые программы не ломаются.

Значения:

```uni
let i = 42
let f = 3.5
let s = "текст"
let b = true
let nothing = null
let arr = [1, 2, 3]
let obj = {name: "Аня", age: 30}
```

Целые числа произвольной длины: `2^100` считается точно. Дробные — обычный double: `0.1 + 0.2` даёт `0.30000000000000004`.

## Строки и шаблоны

Обычные строки понимают `\n`, `\t`, `\r`, `\"`, `\\`. Неизвестная escape-последовательность сохраняется как есть, поэтому пути вида `"C:\temp\file.txt"` пишутся без удвоения, но лучше не полагаться на это и использовать прямые слэши.

Шаблонные строки — в обратных кавычках, выражения в фигурных скобках:

```uni
let x = 3
print(`x = {x}`)
```

## Арифметика и сравнения

`+ - * / % ^`, сравнения `== != < <= > >=`, логика `and or not`. Строки складываются через `+`. Остаток от деления сохраняет знак делимого, как в эталоне.

## Условия

```uni
if x > 10 {
    print("больше")
} else if x > 5 {
    print("средне")
} else {
    print("мало")
}
```

Скобки вокруг условия необязательны: `if x > 10 { ... }` — канонический вид, `if (x > 10) { ... }` тоже работает.

## Циклы

```uni
let i = 0
while i < 3 {
    print(i)
    i = i + 1
}

for v in [10, 20, 30] {
    print(v)
}

for j in 0..2 {
    print(j)
}
```

`a..b` — диапазон включительно. Для `for ... in` и для списков он работает одинаково.

## Массивы

```uni
let a = [1, 2, 3]
push(a, 4)
print(length(a))
print(a[0])
a[0] = 10
print(join(a, "-"))
print(map(a, fn(x) { return x * 2 }))
```

Основные функции: `push`, `pop`, `shift`, `unshift`, `slice`, `splice`, `reverse`, `sort`, `includes`, `indexOf`, `map`, `filter`, `reduce`, `find`, `each`, `sum`, `range`.

## Объекты

```uni
let user = {name: "Аня", tags: ["admin"]}
print(user.name)
user.age = 30
print(keys(user))
print(has(user, "age"))
```

Объект — это словарь строка → значение. `keys`, `values`, `entries`, `merge`, `clone`, `has`, `delete` работают по ключам.

## Функции

```uni
fn add(a, b) {
    return a + b
}

let mul = fn(a, b) { return a * b }

print(add(2, 3))
print(mul(2, 3))
```

Функции — значения: их можно класть в массив, передавать в `map`, возвращать из других функций. Замыкания захватывают окружение.

## Ошибки

```uni
try {
    throw "что-то пошло не так"
} catch (e) {
    print("поймали: " + e)
} finally {
    print("всё равно выполнится")
}
```

`finally` выполняется и при ошибке, и без неё; ошибка из `try` не теряется, если `catch` нет.

## Встроенные функции

Печать и типы: `print`, `output`, `input`, `typeof`, `to_string`, `to_number`, `to_boolean`, `is_*`.

Строки: `split`, `join`, `replace`, `substring`, `trim`, `uppercase`, `lowercase`, `startswith`, `endswith`, `repeat`, `char_code`, `from_char_code`.

Массивы: `length`, `push`, `pop`, `shift`, `unshift`, `slice`, `splice`, `reverse`, `sort`, `includes`, `indexOf`, `map`, `filter`, `reduce`, `find`, `each`, `sum`, `range`.

Математика: `abs`, `round`, `floor`, `ceil`, `sqrt`, `min`, `max`, `pow`, `random`.

Файлы и время: `read_file`, `write_file`, `append_file`, `file_exists`, `delete_file`, `list_dir`, `create_dir`, `get_time`, `get_date`, `format_time`, `sleep`.

JSON: `parse_json`, `stringify_json`.

Служебные: `env`, `exit`, `argv`, `log_info`, `log_debug`, `log_warn`, `log_error`, `log_fatal`.

Полный справочник с сигнатурами, краевыми случаями и примерами — `docs/stdlib.md`.

## Модули

Программу можно разбить на файлы и подключать их:

```uni
import "lib/text.uni"
import "lib/math.uni" as m

print(trim("  привет  "))
print(m.square(5))
```

`import "файл.uni"` добавляет все переменные и функции модуля в текущую область,
`import "файл.uni" as m` кладёт их в объект `m`.

Путь считается от файла, который импортирует, а не от папки запуска. Один и тот же
модуль выполняется один раз: повторный импорт берёт готовый результат. Циклический
импорт (`a` тянет `b`, `b` тянет `a`) — ошибка с понятным текстом.

## Ошибки и трассировка

Ошибка времени выполнения печатает строку и стек вызовов:

```
UniLand error: Error at line 2: Division by zero
Стек вызовов:
  в c(), строка 6
  в b(), строка 10
  в a(), строка 13
```

Флаг `-t` (`--trace`) показывает порядок выполнения: каждый узел и вход/выход из
функций.

```
uniland run -t script.uni
```

```
[trace] строка 5
[trace] -> f() строка 5
[trace] строка 2
[trace] <- f()
```

## Python внутри UniLand

Два способа. Первый — блок:

```uni
python {
import json
print(json.dumps({"a": 1}))
}
```

Второй — функции:

```uni
pyeval("1 + 2")
pyexec("import math")
py("math.sqrt(16)")
```

Блок `python { }` получает доступ к переменным UniLand через объект `uni`: `uni.x`, `uni.x = 5`, `uni.call("имя", аргументы)`. Воркер — отдельный процесс Python, поэтому состояние живёт между вызовами.

## Go внутри UniLand

Блок:

```uni
go {
fmt.Println("из Go")
}
```

Функции:

```uni
goeval("strings.ToUpper(\"abc\")")
goexec("x := 1")
```

И главное — обратный мост: функция Go как значение UniLand.

```uni
let up = gofunc("strings.ToUpper")
print(up("abc"))

let rep = gofunc("strings.Repeat")
print(rep("ab", 3))
```

`gofunc` разрешает символ через yaegi (`strings.ToUpper`, `fmt.Sprint`, своя функция, объявленная в `go { }`) и возвращает обычную функцию UniLand. Аргументы переводятся по типам параметров: строка, целое, дробное, bool. Возвращается текст результата, который UniLand приводит к числу, bool или строке.

Важно: воркер собирается из встроенного исходника и кешируется в `%TEMP%\uniland-goworker-c`. Кеш инвалидируется по хешу исходника, поэтому после правок `src/goworker.go` воркер пересобирается сам.

## C внутри UniLand

```uni
let libc = c_load("msvcrt.dll")
let strlen = c_sym(libc, "strlen", "is")
print(c_call(strlen, "hello"))
```

Сигнатуры: `i` — int, `s` — строка, `d` — double, `v` — void. Библиотека грузится через `LoadLibrary`, символ через `GetProcAddress`.

## HTTP

```uni
let r = http_get("https://example.com")
print(r.status)
print(r.body)
```

`http_get`, `http_post`, `http_put`, `http_delete` возвращают объект `{status, body, headers}`. Внутри WinHTTP, без внешних зависимостей.

## Окна

```uni
let w = gui_window("Моё окно", 640, 480)
let label = gui_label(w, "привет")
let input = gui_input(w, "имя")
let button = gui_button(w, "Нажать", fn() {
    print("нажали: " + gui_get(input))
})
gui_run()
```

Картинки: `gui_image` и `gui_image_button` понимают PNG, JPEG, GIF, BMP и ICO (GDI+). Анимированный GIF проигрывается сам: кадры переключаются таймером.

```uni
let g = gui_gif("anim.gif", 320, 240)
let pic = gui_image("photo.png", 320, 240)
```

Видео — через MCI: окно создаётся внутри GUI, файл открывается системным кодеком.

```uni
let v = gui_video("clip.avi", 640, 480)
```

Диалоги: `pick_file`, `pick_save`, `pick_folder`, `alert`, `confirm`, `prompt`. При `UNILAND_NOGUI=1` все функции GUI возвращают ошибку, как в эталонной сборке с тегом nogui.

## Форматтер и сервер редактора

```bat
uniland fmt script.uni
uniland fmt -w script.uni
uniland fmt -l .
uniland lsp
```

Форматтер не трогает блоки `python { }` и `go { }`, шаблонные строки и комментарии. `lsp` говорит на LSP по stdin/stdout: инициализация, диагностика, форматирование.

## Как устроен порт на C

Файлы:

- `src/lexer.c` — токены, строки, шаблоны, BOM.
- `src/parser.c` — AST, приоритеты, сахар (`if` без скобок, `a..b`).
- `src/interp.c` — обход дерева, окружения, вызовы, `try/catch`.
- `src/vm.c` — компилятор в байткод и стековый движок.
- `src/value.c` — значения, строки, списки, словари, JSON.
- `src/bigint.c` — длинная арифметика.
- `src/std.c` — встроенные функции.
- `src/pybridge.c` — Python-воркер.
- `src/gobridge.c` — Go-воркер и обратный мост.
- `src/cbridge.c` — вызов C-функций.
- `src/http.c` — WinHTTP.
- `src/gui.c` — окна, GDI+, MCI.
- `src/fmt.c`, `src/lsp.c` — форматтер и сервер.
- `src/main.c` — CLI.

Конвейер один: файл → лексер → парсер → AST → интерпретатор. Значение — тег плюс объединение; функции бывают двух видов: с телом (AST) и нативные (`native`), а нативные с контекстом (`native_ctx`) получают указатель на саму функцию, что и позволяет `gofunc` хранить имя символа в `data`.

## Дерево и байткод

По умолчанию AST обходится рекурсивно. С `UNILAND_VM=1` та же программа сначала
компилируется в инструкции, а потом исполняется стековым движком:

```
set UNILAND_VM=1
uniland run script.uni
```

Семантика общая: окружения, значения, вызовы и мосты одни и те же, поэтому
результат и текст ошибок совпадают. Стек вызовов и `-t` работают в обоих режимах.
Если конструкцию скомпилировать нельзя, VM тихо откатывается на дерево.

Скорость: цикл на 1 000 000 итераций — 0,34 с на дереве и 0,26 с на VM, `fib(24)` —
0,12 с и 0,16 с (минимум из серии, разброс до 20%). Такой скачок дал переход на
хеш-таблицу имён в окружении, арены для окружений и `BigInt`, отказ от нового
окружения в блоках без объявлений и аргументы вызова на стеке. VM вдобавок кэширует
найденные переменные по инструкции, поэтому быстрее на циклах, но платит за кадры
вызовов. Оставшееся узкое место — `BigInt` на каждую арифметику (см. `docs/ROADMAP.md`).


Добавить встроенную функцию просто: написать `static Value b_name(int argc, Value *argv)`, затем `def(env, "name", b_name)` в `std_install` (или в соответствующем `*_install`). Ошибки — через `err_msgf`, не через `exit`.

Мосты устроены одинаково: C держит канал к воркеру, отправляет JSON-запрос, читает ответ. Go-воркер при вызове `uni.*` из Go пишет строку `@@uni` в тот же канал и ждёт ответа, поэтому вызовы в обе стороны не блокируют друг друга.

## Шпаргалка

```uni
import "lib.uni" as lib
let x = 1
x = 2
if x > 1 { print("да") }
while x < 5 { x = x + 1 }
for i in 0..3 { print(i) }
let a = [1, 2, 3]
let o = {k: "v"}
fn f(a) { return a }
try { throw "e" } catch (e) { print(e) }
python { print("py") }
go { fmt.Println("go") }
let up = gofunc("strings.ToUpper")
let w = gui_window("окно", 400, 300)
gui_image("pic.png", 200, 150)
gui_video("clip.avi", 320, 240)
```

## Куда смотреть дальше

- `README.md` — что уже есть и чего пока нет.
- `tests/` — примеры на каждый мост и на сахар.
- `tools/` — сверка с эталоном, форматтер, LSP.
- `uniland-go` — эталонная реализация, по ней сверяется семантика.
