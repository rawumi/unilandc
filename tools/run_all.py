import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, "uniland.exe")
TESTS = os.path.join(ROOT, "tests")
ENV = dict(os.environ, UNILAND_NOGUI="1")

SCRIPTS = [
    "run_golden.py",
    "run_tokens.py",
    "check_fmt.py",
    "test_lsp.py",
    "run_import.py",
    "run_trace.py",
]

VM_SCRIPTS = [
    "run_golden.py",
    "run_import.py",
    "run_trace.py",
]

EXAMPLES = [
    ("stdlib_demo.uni", "stdlib_demo.out.txt"),
    ("vm_smoke.uni", "vm_smoke.out.txt"),
]


def run_script(name, extra_env=None):
    env = dict(ENV)
    env.update(extra_env or {})
    return subprocess.run(
        [sys.executable, os.path.join(ROOT, "tools", name)],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def run_example(script, expected_file, extra_env=None):
    env = dict(ENV)
    env.update(extra_env or {})
    proc = subprocess.run(
        [EXE, "run", os.path.join(TESTS, script)],
        cwd=ROOT,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    with open(os.path.join(TESTS, expected_file), encoding="utf-8") as f:
        expected = f.read()
    return proc, expected


def report(label, proc):
    tail = [ln for ln in (proc.stdout or "").strip().splitlines() if ln.strip()]
    note = tail[-1] if tail else ""
    if proc.returncode == 0:
        print("ok   %-20s %s" % (label, note))
        return 0
    print("FAIL %-20s %s" % (label, note))
    if proc.stdout:
        print(proc.stdout)
    if proc.stderr:
        print(proc.stderr)
    return 1


def main():
    fails = 0

    for name in SCRIPTS:
        fails += report(name, run_script(name))

    for script, expected_file in EXAMPLES:
        proc, expected = run_example(script, expected_file)
        if proc.returncode == 0 and proc.stdout == expected:
            print("ok   %-20s" % script)
        else:
            fails += 1
            print("FAIL %-20s code=%d out=%r err=%r" % (script, proc.returncode, proc.stdout, proc.stderr))

    for name in VM_SCRIPTS:
        fails += report("vm: " + name, run_script(name, {"UNILAND_VM": "1"}))

    for script, expected_file in EXAMPLES:
        proc, expected = run_example(script, expected_file, {"UNILAND_VM": "1"})
        if proc.returncode == 0 and proc.stdout == expected:
            print("ok   %-20s" % ("vm: " + script))
        else:
            fails += 1
            print(
                "FAIL %-20s code=%d out=%r err=%r"
                % ("vm: " + script, proc.returncode, proc.stdout, proc.stderr)
            )

    if shutil.which("go"):
        proc, expected = run_example("gofunc.uni", "gofunc.out.txt")
        if proc.returncode == 0 and proc.stdout == expected:
            print("ok   %-20s" % "gofunc")
        else:
            fails += 1
            print("FAIL %-20s code=%d out=%r err=%r" % ("gofunc", proc.returncode, proc.stdout, proc.stderr))
    else:
        print("skip %-20s go not found" % "gofunc")

    if fails:
        print("%d failing" % fails)
        return 1
    print("all suites passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
