import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT.parent / "uniland-go" / "compat" / "cases"
EXE = ROOT / "uniland.exe"
SKIP_TOKENS = {"t10_python.uni", "t11_golang.uni", "wordcount.uni", "greet_cli.uni"}


def fmt_text(path):
    p = subprocess.run([str(EXE), "fmt", str(path)], capture_output=True)
    if p.returncode != 0:
        return None, p.stderr.decode("utf-8", "replace")
    return p.stdout.decode("utf-8", "replace").replace("\r\n", "\n"), ""


def token_dump(path):
    p = subprocess.run([str(EXE), "run", "-d", Path(path).name], capture_output=True, cwd=str(CASES))
    raw = p.stdout.decode("utf-8", "replace").splitlines()
    toks = []
    for ln in raw:
        s = ln[2:] if ln.startswith("  ") else ln
        if s.startswith("Token("):
            toks.append(s)
        elif toks:
            break
    return toks


bad = 0
for case in sorted(CASES.glob("*.uni")):
    text, err = fmt_text(case)
    if text is None:
        print(f"FAIL fmt {case.name}: {err.strip()}")
        bad += 1
        continue
    tmp_dir = CASES
    fd = tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".uni", dir=tmp_dir,
                                     prefix="_fmt_", delete=False, newline="")
    fd.write(text)
    fd.close()
    tmp = Path(fd.name)
    try:
        text2, err2 = fmt_text(tmp)
        if text2 != text:
            print(f"FAIL idempotent {case.name}")
            bad += 1
            continue
        if case.name not in SKIP_TOKENS:
            before = token_dump(case)
            after = token_dump(tmp)
            if before != after:
                print(f"FAIL tokens {case.name}: {len(before)} vs {len(after)}")
                bad += 1
                continue
        print(f"ok   {case.name}")
    finally:
        tmp.unlink(missing_ok=True)
print(f"\n{bad} проблем")
sys.exit(1 if bad else 0)
