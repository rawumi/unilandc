import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT / "tests" / "compat" / "cases"
EXPECTED = ROOT / "tests" / "compat" / "expected"
EXE = ROOT / "uniland.exe"


def norm(text):
    return (text or "").replace("\r\n", "\n")


def run_case(case):
    exp_path = EXPECTED / (case.stem + ".json")
    if not exp_path.exists():
        return None
    exp = json.loads(exp_path.read_text(encoding="utf-8"))
    stdin_path = CASES / (case.stem + ".stdin")
    args_path = CASES / (case.stem + ".args")
    data = stdin_path.read_bytes() if stdin_path.exists() else (exp.get("stdin") or "").encode()
    args = list(exp.get("args") or [])
    if args_path.exists():
        args = [ln for ln in args_path.read_text(encoding="utf-8").splitlines() if ln]
    env = dict(os.environ)
    env["UNILAND_NOGUI"] = "1"
    p = subprocess.run(
        [str(EXE), "run", case.name] + args,
        input=data,
        capture_output=True,
        cwd=str(CASES),
        env=env,
        timeout=120,
    )
    out = norm(p.stdout.decode("utf-8", "replace"))
    err = norm(p.stderr.decode("utf-8", "replace"))
    want_out = norm(exp.get("stdout", ""))
    want_err = norm(exp.get("stderr", ""))
    ok = out == want_out and err == want_err and p.returncode == exp.get("exit", 0)
    return ok, out, want_out, err, want_err, p.returncode, exp.get("exit", 0)


bad = 0
for case in sorted(CASES.glob("*.uni")):
    res = run_case(case)
    if res is None:
        print(f"skip {case.name}")
        continue
    ok, out, want_out, err, want_err, rc, want_rc = res
    if ok:
        print(f"ok   {case.name}")
        continue
    bad += 1
    print(f"FAIL {case.name} exit={rc} want {want_rc}")
    if out != want_out:
        print("  stdout got:", repr(out[:400]))
        print("  stdout exp:", repr(want_out[:400]))
    if err != want_err:
        print("  stderr got:", repr(err[:400]))
        print("  stderr exp:", repr(want_err[:400]))
print(f"\n{bad} failing")
sys.exit(1 if bad else 0)
