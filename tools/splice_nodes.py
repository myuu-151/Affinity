#!/usr/bin/env python3
"""Splice generated node entries (tools/_new_entries.md, grouped by
@@CATEGORY:...@@) into docs/NODE_REFERENCE.md at the end of each matching
category section, and bump the counts. Idempotent-ish: skips a node whose
'### Name' header already exists."""
import re

DOC = "docs/NODE_REFERENCE.md"
doc = open(DOC, encoding="utf-8").read()
blocks = open("tools/_new_entries.md", encoding="utf-8").read()

# Parse generated blocks into {category: [entry, ...]}
cats = {}
cur = None
for chunk in re.split(r"(@@CATEGORY:[^@]+@@)", blocks):
    m = re.match(r"@@CATEGORY:(.+?)@@", chunk)
    if m:
        cur = m.group(1).strip()
        cats[cur] = []
    elif cur is not None and chunk.strip():
        for e in re.split(r"\n(?=### )", chunk.strip()):
            e = e.strip()
            if e.startswith("### "):
                cats[cur].append(e)

existing = set(re.findall(r"^### (.+)$", doc, re.M))
added = 0
for cat, entries in cats.items():
    new = [e for e in entries if e.split("\n", 1)[0][4:].strip() not in existing]
    if not new:
        continue
    # find the category section header "## <cat>"
    hm = re.search(rf"^## {re.escape(cat)}\b.*$", doc, re.M)
    if not hm:
        print("!! category section not found:", cat); continue
    # section ends at the next top-level "## " heading after it (or EOF)
    nxt = re.search(r"^## ", doc[hm.end():], re.M)
    end = hm.end() + nxt.start() if nxt else len(doc)
    insert = "\n\n" + "\n\n".join(new) + "\n"
    # trim trailing blank space before the boundary, then insert
    seg = doc[:end].rstrip()
    doc = seg + insert + "\n" + doc[end:].lstrip("\n")
    added += len(new)
    print(f"+{len(new)} -> {cat}")

# Bump counts in the header + category list + section headers.
def bump(pat, delta):
    global doc
    def r(m):
        return m.group(1) + str(int(m.group(2)) + delta) + m.group(3)
    doc = re.sub(pat, r, doc, count=1)

# per-category deltas
deltas = {"Actions": len(cats.get("Actions", [])),
          "Gates / Conditions": len(cats.get("Gates / Conditions", [])),
          "Data / Math": len(cats.get("Data / Math", []))}
# only count newly-added (not pre-existing) — recompute from 'added' split
# (use the entries we actually inserted)
# total
bump(r"(\*\*)(\d+)( nodes total\.\*\*)", added)
# category index lines: "- [Actions](#actions) — 131 nodes"
for cat, label in [("Actions", "Actions"), ("Gates / Conditions", "Gates / Conditions"), ("Data / Math", "Data / Math")]:
    d = deltas[cat]
    if d:
        bump(rf"(\[{re.escape(label)}\]\([^)]+\) — )(\d+)( nodes)", d)

open(DOC, "w", encoding="utf-8").write(doc)
print("total added:", added)
