import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CASES = ROOT.parent / "uniland-go" / "compat" / "cases"
EXPECTED = ROOT.parent / "uniland-go" / "compat" / "expected-tokens"
EXE = ROOT / "uniland.exe"

ENV = dict(os.environ)
ENV["UNILAND_NOGUI"] = "1"


def check(exp_path):
    case = CASES / (exp_path.stem + ".uni")
    if not case.exists():
        return None
    try:
        p = subprocess.run(
            [str(EXE), "run", "-d", case.name],
            capture_output=True,
            cwd=str(CASES),
            stdin=subprocess.DEVNULL,
            env=ENV,
            timeout=30,
        )
    except subprocess.TimeoutExpired:
        return case.name, "таймаут"
    raw = p.stdout.decode("utf-8", "replace").splitlines()
    tokens = []
    for ln in raw:
        s = ln[2:] if ln.startswith("  ") else ln
        if s.startswith("Token("):
            tokens.append(s)
        elif tokens:
            break
    got = "\n".join(tokens) + "\n"
    want = exp_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    if got == want:
        return case.name, None
    gl, wl = got.splitlines(), want.splitlines()
    for i in range(max(len(gl), len(wl))):
        g = gl[i] if i < len(gl) else "<нет>"
        w = wl[i] if i < len(wl) else "<нет>"
        if g != w:
            return case.name, f"строка {i + 1}: got {g!r} want {w!r}"
    return case.name, "разная длина"


files = sorted(EXPECTED.glob("*.txt"))
bad = 0
with ThreadPoolExecutor(max_workers=4) as pool:
    for res in pool.map(check, files):
        if res is None:
            continue
        name, err = res
        if err:
            bad += 1
            print(f"FAIL {name}: {err}")
        else:
            print(f"ok   {name}")
print(f"\n{bad} проблем")
sys.exit(1 if bad else 0)
