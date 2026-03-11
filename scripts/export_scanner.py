#!/usr/bin/env python3
import argparse
import json
import os
import re

EXPORT_RE = re.compile(r'^\s*EXPORT_SYMBOL\s*\(\s*([A-Za-z_]\w*)\s*\)\s*;', re.MULTILINE)
EXPORT_GPL_RE = re.compile(r'^\s*EXPORT_SYMBOL_GPL\s*\(\s*([A-Za-z_]\w*)\s*\)\s*;', re.MULTILINE)


def scan_file(path, source_root):
    with open(path, "r", encoding="utf-8", errors="ignore") as f:
        text = f.read()

    rel = os.path.relpath(path, source_root).replace("\\", "/")
    items = []

    for pattern, kind in ((EXPORT_RE, "EXPORT_SYMBOL"), (EXPORT_GPL_RE, "EXPORT_SYMBOL_GPL")):
        for m in pattern.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            items.append({
                "symbol": m.group(1),
                "kind": kind,
                "file": rel,
                "line": line
            })

    return items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--subsystem", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    base = os.path.join(args.source_root, args.subsystem)
    records = []

    for root, _, files in os.walk(base):
        for name in files:
            if not name.endswith(".c"):
                continue
            records.extend(scan_file(os.path.join(root, name), args.source_root))

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump({"exports": records}, f, indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()
