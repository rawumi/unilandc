import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "uniland.exe")
TESTS = os.path.join(ROOT, "tests")
ENV = dict(os.environ, UNILAND_NOGUI="1")


def run(args):
    proc = subprocess.run(
        [EXE] + args,
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

    code, out, err = run(["run", os.path.join(TESTS, "trace.uni")])
    expected_stack = [
        "Стек вызовов:",
        "в c(), строка 6",
        "в b(), строка 10",
        "в a(), строка 13",
    ]
    fails += check(
        "stack",
        code == 1 and "Division by zero" in err and all(s in err for s in expected_stack),
        "code=%d err=%r" % (code, err),
    )

    code, out, err = run(["run", "-t", os.path.join(TESTS, "trace_ok.uni")])
    fails += check(
        "trace",
        code == 0
        and out == "3\n"
        and "[trace] -> f() строка 5" in err
        and "[trace] <- f()" in err
        and "[trace] строка 2" in err,
        "code=%d out=%r err=%r" % (code, out, err),
    )

    code, out, err = run(["run", os.path.join(TESTS, "trace_ok.uni")])
    fails += check("no trace by default", code == 0 and out == "3\n" and err == "", "out=%r err=%r" % (out, err))

    if fails:
        print("%d failing" % fails)
        return 1
    print("all trace tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
