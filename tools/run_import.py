import os
import subprocess
import sys

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "uniland.exe")
CASES = os.path.join(ROOT, "tests", "import")
ENV = dict(os.environ, UNILAND_NOGUI="1")


def run(script):
    proc = subprocess.run(
        [EXE, "run", os.path.join(CASES, script)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        env=ENV,
        cwd=ROOT,
    )
    return proc.returncode, proc.stdout, proc.stderr


def check(name, cond, detail):
    if cond:
        print("ok   %s" % name)
        return 0
    print("FAIL %s: %s" % (name, detail))
    return 1


def main():
    fails = 0
    with open(os.path.join(CASES, "main.out.txt"), encoding="utf-8") as f:
        expected = f.read()
    code, out, err = run("main.uni")
    fails += check("main.uni", code == 0 and out == expected, "code=%d out=%r err=%r" % (code, out, err))

    code, out, err = run("cycle_a.uni")
    fails += check(
        "cycle",
        code == 1 and "циклический импорт" in err and out == "",
        "code=%d out=%r err=%r" % (code, out, err),
    )

    code, out, err = run("missing.uni")
    fails += check(
        "missing",
        code == 1 and "файл не найден" in err and out == "",
        "code=%d out=%r err=%r" % (code, out, err),
    )

    if fails:
        print("%d failing" % fails)
        return 1
    print("all import tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
