#!/usr/bin/env python3
"""Fully regenerate docs/NODE_REFERENCE.md from src/editor/frame_loop.cpp
(VsNodeType enum + sVsNodeDefs + desc tooltips). Run from the repo root.
Replaces the whole doc so pin lists and counts can never drift."""
import re

SRC = "src/editor/frame_loop.cpp"
DOC = "docs/NODE_REFERENCE.md"
txt = open(SRC, encoding="utf-8", errors="replace").read()

m = re.search(r"enum class VsNodeType[^{]*\{(.*?)\bCOUNT\b", txt, re.S)
enum_body = re.sub(r"//[^\n]*", "", m.group(1))
enum_body = re.sub(r"/\*.*?\*/", "", enum_body, flags=re.S)
enum_names = [mm.group(1) for tok in enum_body.split(",")
              if (mm := re.match(r"\s*([A-Za-z_]\w*)", tok))]

m = re.search(r"sVsNodeDefs\[\]\s*=\s*\{(.*?)\n\};", txt, re.S)
defs_body = m.group(1)
entry_re = re.compile(
    r'\{\s*"([^"]+)"\s*,\s*(0x[0-9A-Fa-f]+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*\{([^}]*)\}\s*,\s*\{([^}]*)\}',
    re.S)
def_entries = []
for em in entry_re.finditer(defs_body):
    name, color, ei, eo, di, do, ins, outs = em.groups()
    def_entries.append(dict(
        name=name, color=color.lower(), ei=int(ei), eo=int(eo),
        di=int(di), do=int(do),
        ins=re.findall(r'"([^"]*)"', ins),
        outs=re.findall(r'"([^"]*)"', outs)))

desc = {}
for dm in re.finditer(r'VsNodeType::(\w+):\s*desc\s*=\s*"((?:[^"\\]|\\.)*)"\s*;', txt):
    s = dm.group(2)
    s = s.replace('\\n', ' ').replace('\\"', '"').replace('%%', '%').replace('\\t', ' ')
    desc.setdefault(dm.group(1), re.sub(r'\s+', ' ', s).strip())

idx_to_enum = {i: enum_names[i] for i in range(min(len(def_entries), len(enum_names)))}

def category(d):
    c = d["color"]
    if d["ei"] == 0 and d["eo"] >= 1 and c not in ("0xff885533",):
        return "Events"
    if c == "0xff885533":
        return "Gates / Conditions"
    if c in ("0xffaa55cc", "0xff9955bb", "0xffcc66aa"):
        return "Data / Math"
    if d["ei"] == 0 and d["eo"] == 0:
        return "Data / Math"
    return "Actions"

def pins_str(d):
    parts, ex = [], []
    if d["ei"]: ex.append("1 exec in")
    if d["eo"]: ex.append(f'{d["eo"]} exec out')
    if ex: parts.append(", ".join(ex))
    if d["ins"]: parts.append("inputs: " + ", ".join(f"`{x}`" for x in d["ins"]))
    if d["outs"]: parts.append("outputs: " + ", ".join(f"`{x}`" for x in d["outs"]))
    return "; ".join(parts) if parts else "no pins"

CATS = ["Events", "Actions", "Gates / Conditions", "Data / Math", "Other"]
groups = {c: [] for c in CATS}
seen = set()
for i, d in enumerate(def_entries):
    if d["name"] in seen:            # a few defs share display names across modes
        continue
    seen.add(d["name"])
    enm = idx_to_enum.get(i, "")
    text = desc.get(enm, "") or "_(see in-editor tooltip)_"
    cat = category(d)
    if cat not in groups: cat = "Other"
    groups[cat].append(f"### {d['name']}\n\n{text}\n\n*Pins:* {pins_str(d)}\n")

total = sum(len(v) for v in groups.values())
def anchor(c): return c.lower().replace(" / ", "--").replace(" ", "-")

out = []
out.append("# Affinity Node Reference\n")
out.append("Every Visual Script node available in the editor's **Nodes** tab, grouped by category, "
           "with what it does and its pins. Auto-generated from `src/editor/frame_loop.cpp` (the "
           "`VsNodeType` enum, `sVsNodeDefs` table, and the description tooltips) — regenerate with "
           "`python tools/regen_node_reference.py` whenever nodes or pins change.\n")
out.append(f"**{total} nodes total.**\n")
out.append("Pin colors in the editor: **green = events**, **blue = actions**, **orange = gates/conditions**, "
           "**purple = data/math**. Exec pins are the white triangles that wire the order of operations; "
           "data pins are the round colored dots that carry values.\n")
out.append("> Many nodes behave differently in **Mode 0** (top-down tile RPG) vs **Mode 4** (first-person 3D). "
           "Where a node only applies to one mode, the description says so. See `docs/NODE_CONVENTION.md` "
           "for how nodes map to emitted runtime code.\n")
out.append("## Categories\n")
for c in CATS:
    if groups[c]:
        out.append(f"- [{c}](#{anchor(c)}) — {len(groups[c])} nodes")
out.append("\n---\n")
for c in CATS:
    if not groups[c]:
        continue
    out.append(f"## {c}\n")
    out.append("\n".join(groups[c]))
    out.append("---\n")

open(DOC, "w", encoding="utf-8", newline="\n").write("\n".join(out))
print(f"wrote {DOC}: {total} nodes",
      {c: len(groups[c]) for c in CATS if groups[c]})
