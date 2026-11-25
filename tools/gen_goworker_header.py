import io
import sys

src = io.open("src/goworker.go", encoding="utf-8").read()
lines = src.split("\n")
out = ["#ifndef GOWORKER_GO_H", "#define GOWORKER_GO_H", "static const char k_goworker_go[] ="]
for ln in lines:
    esc = ln.replace("\\", "\\\\").replace('"', '\\"').replace("\t", "\\t")
    out.append('    "' + esc + '\\n"')
out.append("    ;")
out.append("#endif")
io.open("src/goworker_go.h", "w", encoding="utf-8", newline="").write("\n".join(out) + "\n")
print("generated src/goworker_go.h")
