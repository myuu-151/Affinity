#!/usr/bin/env python3
"""Extract node-reference markdown blocks for a given set of node display names
from src/editor/frame_loop.cpp (VsNodeType enum + sVsNodeDefs + desc tooltips).

Used to splice newly-added nodes into docs/NODE_REFERENCE.md without rewriting
the existing entries. Run from the repo root.
"""
import re, sys, json

SRC = "src/editor/frame_loop.cpp"
txt = open(SRC, encoding="utf-8", errors="replace").read()

# 1) VsNodeType enum order (the block ending in COUNT, before the def struct).
m = re.search(r"enum class VsNodeType[^{]*\{(.*?)\bCOUNT\b", txt, re.S)
if not m:
    m = re.search(r"VsNodeType\s*\{(.*?)\bCOUNT\b", txt, re.S)
enum_body = m.group(1)
# Strip // and /* */ comments, then split on commas (entries can share a line).
enum_body = re.sub(r"//[^\n]*", "", enum_body)
enum_body = re.sub(r"/\*.*?\*/", "", enum_body, flags=re.S)
enum_names = []
for tok in enum_body.split(","):
    mm = re.match(r"\s*([A-Za-z_]\w*)", tok)
    if mm:
        enum_names.append(mm.group(1))

# 2) sVsNodeDefs entries in order.
m = re.search(r"sVsNodeDefs\[\]\s*=\s*\{(.*?)\n\};", txt, re.S)
defs_body = m.group(1)
# each entry: { "Name", COLOR, execIn, execOut, dataIn, dataOut, {ins}, {outs}, {..} },
def_entries = []
entry_re = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}',
    re.S)
for em in entry_re.finditer(defs_body):
    name, color, ei, eo, di, do, ins, outs = em.groups()
    innames = re.findall(r'"([^"]*)"', ins)
    outnames = re.findall(r'"([^"]*)"', outs)
    def_entries.append(dict(name=name, color=color.lower(),
                            ei=int(ei), eo=int(eo), di=int(di), do=int(do),
                            ins=innames, outs=outnames))

# 3) descriptions: case VsNodeType::X: desc = "....";  (single line, escaped quotes ok)
desc = {}
for dm in re.finditer(r'VsNodeType::(\w+):\s*desc\s*=\s*"((?:[^"\\]|\\.)*)"\s*;', txt):
    s = dm.group(2)
    s = s.replace('\\n', ' ').replace('\\"', '"').replace('%%', '%').replace('\\t', ' ')
    s = re.sub(r'\s+', ' ', s).strip()
    desc.setdefault(dm.group(1), s)

# Map def index -> enum name (def order == enum order, skipping COUNT).
idx_to_enum = {i: enum_names[i] for i in range(min(len(def_entries), len(enum_names)))}

def category(d):
    c = d["color"]
    if d["ei"] == 0 and d["eo"] >= 1 and c not in ("0xff885533",):
        return "Events"
    if c == "0xff885533":      # orange
        return "Gates / Conditions"
    if c in ("0xffaa55cc", "0xff9955bb", "0xffcc66aa"):  # purple-ish (data)
        return "Data / Math"
    if d["ei"] == 0 and d["eo"] == 0:
        return "Data / Math"
    return "Actions"

def pins_str(d):
    parts = []
    ex = []
    if d["ei"]: ex.append("1 exec in")
    if d["eo"]: ex.append(f'{d["eo"]} exec out')
    if ex: parts.append(", ".join(ex))
    if d["ins"]: parts.append("inputs: " + ", ".join(f"`{x}`" for x in d["ins"]))
    if d["outs"]: parts.append("outputs: " + ", ".join(f"`{x}`" for x in d["outs"]))
    return "; ".join(parts) if parts else "no pins"

wanted = set(json.load(open(sys.argv[1]))) if len(sys.argv) > 1 else None
out = {}
for i, d in enumerate(def_entries):
    if wanted is not None and d["name"] not in wanted:
        continue
    enm = idx_to_enum.get(i, "")
    text = desc.get(enm, "")
    block = f"### {d['name']}\n\n{text or '_(see in-editor tooltip)_'}\n\n*Pins:* {pins_str(d)}\n"
    out.setdefault(category(d), []).append(block)

for cat in ["Events", "Actions", "Gates / Conditions", "Data / Math", "Other"]:
    if cat in out:
        print(f"@@CATEGORY:{cat}@@")
        print("\n".join(out[cat]))
