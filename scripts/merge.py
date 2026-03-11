#!/usr/bin/env python3
import argparse
import collections
import glob
import json
import os


def load_json(path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def stable_sig(name, return_type, parameters):
    return (
        name,
        return_type,
        tuple(p.get("type", "") for p in parameters),
    )


def dedup_definitions(defs):
    seen = set()
    out = []
    for d in defs:
        key = (d["file"], d["line"], d["column"], d["name"])
        if key in seen:
            continue
        seen.add(key)
        out.append(d)
    return out


def dedup_header_decls(decls):
    seen = set()
    out = []
    for d in decls:
        key = (
            d["decl_file"],
            d["decl_line"],
            d["decl_column"],
            d["name"],
            d["return_type"],
            tuple(p.get("type", "") for p in d.get("parameters", [])),
        )
        if key in seen:
            continue
        seen.add(key)
        out.append(d)
    return out


def collect_raw_jsons(directory):
    all_items = []
    for path in sorted(glob.glob(os.path.join(directory, "*.json"))):
        all_items.append(load_json(path))
    return all_items


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--functions-dir", required=True)
    ap.add_argument("--calls-dir", required=True)
    ap.add_argument("--exports", required=True)
    ap.add_argument("--subsystem", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    function_chunks = collect_raw_jsons(args.functions_dir)
    call_chunks = collect_raw_jsons(args.calls_dir)
    export_data = load_json(args.exports)

    definitions = []
    header_decls = []
    for chunk in function_chunks:
        definitions.extend(chunk.get("definitions", []))
        header_decls.extend(chunk.get("header_declarations", []))

    definitions = dedup_definitions(definitions)
    header_decls = dedup_header_decls(header_decls)

    resolved_calls = []
    unresolved_calls = []
    for chunk in call_chunks:
        resolved_calls.extend(chunk.get("resolved_calls", []))
        unresolved_calls.extend(chunk.get("unresolved_calls", []))

    exports_by_name = collections.defaultdict(list)
    for e in export_data.get("exports", []):
        exports_by_name[e["symbol"]].append(e)

    header_decl_index = collections.defaultdict(list)
    for d in header_decls:
        header_decl_index[d["name"]].append(d)

    defs_by_id = {}
    defs_by_name = collections.defaultdict(list)
    for d in definitions:
        d_id = f'{d["file"]}:{d["line"]}:{d["column"]}:{d["name"]}'
        d["id"] = d_id
        defs_by_id[d_id] = d
        defs_by_name[d["name"]].append(d)

    callers_by_def_id = collections.defaultdict(list)

    for c in resolved_calls:
        matched = None

        # 1) 精确位置匹配
        cfile = c.get("callee_decl_file", "")
        cline = c.get("callee_decl_line", 0)
        ccol = c.get("callee_decl_column", 0)
        cname = c.get("callee_name", "")

        if cfile and cname:
            for d in defs_by_name.get(cname, []):
                if d["file"] == cfile and d["line"] == cline and d["column"] == ccol:
                    matched = d
                    c["merge_fallback"] = "none"
                    break

        # 2) 单候选名字匹配
        if matched is None:
            cands = defs_by_name.get(cname, [])
            if len(cands) == 1:
                matched = cands[0]
                c["merge_fallback"] = "name_only"

        if matched is not None:
            callers_by_def_id[matched["id"]].append(c)

    merged = []

    for d in definitions:
        sig = stable_sig(d["name"], d["return_type"], d["parameters"])
        decls = []
        for hd in header_decl_index.get(d["name"], []):
            if stable_sig(hd["name"], hd["return_type"], hd["parameters"]) == sig:
                decls.append({
                    "file": hd["decl_file"],
                    "line": hd["decl_line"],
                    "column": hd["decl_column"],
                })

        exps = exports_by_name.get(d["name"], [])
        export_symbol = any(x["kind"] == "EXPORT_SYMBOL" for x in exps)
        export_symbol_gpl = any(x["kind"] == "EXPORT_SYMBOL_GPL" for x in exps)

        d["declared_in_header"] = bool(decls)
        d["header_declarations"] = decls
        d["export_symbol"] = export_symbol
        d["export_symbol_gpl"] = export_symbol_gpl
        d["exports"] = exps

        callers = callers_by_def_id.get(d["id"], [])
        merged.append({
            "function": d,
            "resolved_callers": callers,
        })

    out = {
        "subsystem": args.subsystem,
        "functions": merged,
        "unresolved_calls": unresolved_calls,
    }

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()
