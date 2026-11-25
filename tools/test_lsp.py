import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "uniland.exe"


def frame(obj):
    body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
    return b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body


requests = [
    {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {}},
    {"jsonrpc": "2.0", "method": "initialized", "params": {}},
    {"jsonrpc": "2.0", "method": "textDocument/didOpen",
     "params": {"textDocument": {"uri": "file:///t.uni", "text": "let x = \nprint(1)\n"}}},
    {"jsonrpc": "2.0", "id": 2, "method": "textDocument/formatting",
     "params": {"textDocument": {"uri": "file:///t.uni"}, "options": {}}},
    {"jsonrpc": "2.0", "method": "textDocument/didChange",
     "params": {"textDocument": {"uri": "file:///t.uni"},
                "contentChanges": [{"text": "func f() {\nreturn 1\n}\n"}]}},
    {"jsonrpc": "2.0", "id": 3, "method": "unknown/method", "params": {}},
    {"jsonrpc": "2.0", "id": 4, "method": "shutdown"},
    {"jsonrpc": "2.0", "method": "exit"},
]

payload = b"".join(frame(r) for r in requests)
p = subprocess.run([str(EXE), "lsp"], input=payload, capture_output=True)
text = p.stdout.decode("utf-8", "replace")


def split_messages(data):
    out = []
    while True:
        idx = data.find("\r\n\r\n")
        if idx < 0:
            break
        head = data[:idx]
        length = 0
        for line in head.split("\r\n"):
            if line.lower().startswith("content-length:"):
                length = int(line.split(":", 1)[1].strip())
        body = data[idx + 4: idx + 4 + length]
        data = data[idx + 4 + length:]
        out.append(json.loads(body))
    return out


msgs = split_messages(text)
print(f"exit={p.returncode} messages={len(msgs)}")
for m in msgs:
    print(json.dumps(m, ensure_ascii=False)[:160])

ok = True
if p.returncode != 0:
    ok = False
initialize = next((m for m in msgs if m.get("id") == 1), None)
if not initialize or "capabilities" not in initialize.get("result", {}):
    print("нет ответа на initialize")
    ok = False
diag = next((m for m in msgs if m.get("method") == "textDocument/publishDiagnostics"), None)
if not diag:
    print("нет диагностики")
    ok = False
fmt = next((m for m in msgs if m.get("id") == 2), None)
if not fmt or not isinstance(fmt.get("result"), list):
    print("нет textEdit на formatting")
    ok = False
err = next((m for m in msgs if m.get("id") == 3), None)
if not err or err.get("error", {}).get("code") != -32601:
    print("нет ошибки на неизвестный метод")
    ok = False
shutdown = next((m for m in msgs if m.get("id") == 4), None)
if not shutdown or "result" not in shutdown:
    print("нет ответа на shutdown")
    ok = False
sys.exit(0 if ok else 1)
