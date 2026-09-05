#!/usr/bin/env python3
# 把 tools/zf_decoder_helpers.inc + tools/zf_decoder_new.inc 拼入 kernel/krnl_vadd.cpp，
# 替换参考 decoder 函数体（从 'void decoder(' 起，到 'Decoder end' banner 前止）。
import io, sys

K = "kernel/krnl_vadd.cpp"
H = "tools/zf_decoder_helpers.inc"
B = "tools/zf_decoder_new.inc"

lines = open(K, encoding="utf-8").read().splitlines()

i0 = next(i for i, l in enumerate(lines) if l.strip().startswith("void decoder("))
e = next(i for i in range(i0, len(lines)) if "Decoder end" in lines[i])
# banner 起始 '/*' 是 e 之前最近的以 /* 开头行
iB = max(i for i in range(i0, e) if lines[i].lstrip().startswith("/*"))
assert i0 < iB, (i0, iB)

helpers = open(H, encoding="utf-8").read().rstrip("\n")
body = open(B, encoding="utf-8").read().rstrip("\n")

new = helpers + "\n\n" + body
out = "\n".join(lines[:i0]) + "\n" + new + "\n" + "\n".join(lines[iB:])
open(K, "w", encoding="utf-8").write(out)
print(f"replaced lines {i0}..{iB}: inserted {len(new.splitlines())} lines")
