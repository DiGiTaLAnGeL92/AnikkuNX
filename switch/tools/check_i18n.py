#!/usr/bin/env python3
"""Extract tr("...") keys from the C++ sources and check them against resources/lang/*.json."""
import os
import glob, json, re, sys, os

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SIMPLE = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "'": "'", "\\": "\\", "0": "\0", "?": "?", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}


def decode(lit):
    """Decode the body of a C string literal into bytes, then UTF-8 text (as the compiler would)."""
    out = bytearray()
    i = 0
    while i < len(lit):
        c = lit[i]
        if c != "\\":
            out += c.encode("utf-8"); i += 1; continue
        n = lit[i + 1]
        if n == "x":
            j = i + 2
            while j < len(lit) and lit[j] in "0123456789abcdefABCDEF":
                j += 1
            h = lit[i + 2:j]
            if len(h) > 2:
                raise ValueError("hex escape swallows extra digits: \\x" + h)
            out.append(int(h, 16)); i = j
        elif n in "01234567":
            j = i + 1
            while j < len(lit) and j < i + 4 and lit[j] in "01234567":
                j += 1
            out.append(int(lit[i + 1:j], 8)); i = j
        elif n in SIMPLE:
            out += SIMPLE[n].encode(); i += 2
        else:
            raise ValueError("unknown escape \\" + n)
    return out.decode("utf-8")


STR = r'"((?:[^"\\\n]|\\.)*)"'
TR = re.compile(r'\btr\(\s*((?:' + STR + r'\s*)+)')


def keys_in(text):
    for m in TR.finditer(text):
        parts = re.findall(STR, m.group(1))
        yield decode("".join(parts)), text.count("\n", 0, m.start()) + 1


files = []
for pat in ["src/activity/*.cpp", "src/view/*.cpp", "src/app/*.cpp", "src/net/*.cpp", "src/sources/common.cpp", "src/*.cpp"]:
    files += glob.glob(os.path.join(ROOT, pat))
keys = {}
for f in sorted(set(files)):
    if f.endswith("i18n.cpp"):
        continue
    for k, line in keys_in(open(f, encoding="utf-8").read()):
        keys.setdefault(k, f"{os.path.relpath(f, ROOT)}:{line}")

print(f"{len(keys)} distinct keys")
bad = 0
langs = sorted(glob.glob(os.path.join(ROOT, "resources/lang/*.json")))
for lf in langs:
    try:
        data = json.load(open(lf, encoding="utf-8"))
    except Exception as e:
        print("PARSE ERROR", lf, e); bad += 1; continue
    missing = [k for k in keys if k not in data]
    extra = [k for k in data if k not in keys]
    ph = [k for k in keys if k in data and k.count("{}") != data[k].count("{}")]
    empty = [k for k in keys if k in data and not data[k].strip()]
    name = os.path.basename(lf)
    for k in missing: print(f"{name}: MISSING {k!r} ({keys[k]})")
    for k in extra: print(f"{name}: EXTRA {k!r}")
    for k in ph: print(f"{name}: PLACEHOLDER MISMATCH {k!r} -> {data[k]!r}")
    for k in empty: print(f"{name}: EMPTY {k!r}")
    bad += len(missing) + len(extra) + len(ph) + len(empty)
    print(f"{name}: {len(data)} entries, {'OK' if not (missing or extra or ph or empty) else 'PROBLEMS'}")
if "--dump" in sys.argv:
    print(json.dumps(sorted(keys), ensure_ascii=False, indent=1))
sys.exit(1 if bad else 0)
