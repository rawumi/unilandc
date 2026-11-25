import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT.parent / "uniland-go" / "compat" / "cases"
EXE = ROOT / "uniland.exe"

ENV = dict(os.environ)
ENV["UNILAND_NOGUI"] = "1"


def run(args, cwd=None):
    return subprocess.run(
        args,
        capture_output=True,
        cwd=cwd,
        stdin=subprocess.DEVNULL,
        env=ENV,
        timeout=60,
    )


def fmt_text(path):
    p = run([str(EXE), "fmt", str(path)])
    if p.returncode not in (0, 1):
        return None, p.stderr.decode("utf-8", "replace")
    return p.stdout.decode("utf-8", "replace").replace("\r\n", "\n"), ""


def token_dump(path):
    p = run([str(EXE), "run", "-d", Path(path).name], cwd=str(CASES))
    toks = []
    for ln in p.stdout.decode("utf-8", "replace").splitlines():
        s = ln[2:] if ln.startswith("  ") else ln
        if s.startswith("Token("):
            toks.append(s)
        elif toks:
            break
    return toks


bad = 0
tmpdir = Path(tempfile.mkdtemp(prefix="unifmt-"))
for case in sorted(CASES.glob("*.uni")):
    if case.name.startswith("_"):
        continue
    text, err = fmt_text(case)
    if text is None:
        print(f"FAIL fmt {case.name}: {err.strip()}")
        bad += 1
        continue
    tmp = tmpdir / case.name
    tmp.write_text(text, encoding="utf-8", newline="\n")
    text2, _ = fmt_text(tmp)
    if text2 != text:
        print(f"FAIL idempotent {case.name}")
        bad += 1
        continue
    try:
        before = token_dump(case)
        after = token_dump(tmp)
    except subprocess.TimeoutExpired:
        print(f"FAIL timeout {case.name}")
        bad += 1
        continue
    if before != after:
        print(f"FAIL tokens {case.name}: {len(before)} vs {len(after)}")
        bad += 1
        continue
    print(f"ok   {case.name}")
print(f"\n{bad} проблем")
sys.exit(1 if bad else 0)
