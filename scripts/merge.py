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


def iter_json_files(directory):
    for path in sorted(glob.glob(os.path.join(directory, "*.json"))):
        yield path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--functions-dir", required=True)
    ap.add_argument("--calls-dir", required=True)
    ap.add_argument("--exports", required=True)
    ap.add_argument("--subsystem", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    # ----------------------------
    # 1) 读取 definitions / decls
    # ----------------------------
    definitions = []
    header_decls = []

    for path in iter_json_files(args.functions_dir):
        chunk = load_json(path)
        definitions.extend(chunk.get("definitions", []))
        header_decls.extend(chunk.get("header_declarations", []))

    definitions = dedup_definitions(definitions)
    header_decls = dedup_header_decls(header_decls)

    # ----------------------------
    # 2) 读取 exports
    # ----------------------------
    export_data = load_json(args.exports)
    exports_by_name = collections.defaultdict(list)
    for e in export_data.get("exports", []):
        exports_by_name[e["symbol"]].append(e)

    # ----------------------------
    # 3) 建 definition 索引
    # ----------------------------
    defs_by_id = {}
    defs_by_name = collections.defaultdict(list)

    for d in definitions:
        d_id = f'{d["file"]}:{d["line"]}:{d["column"]}:{d["name"]}'
        d["id"] = d_id
        defs_by_id[d_id] = d
        defs_by_name[d["name"]].append(d)

    # ----------------------------
    # 4) 建 header decl 索引
    # ----------------------------
    header_decl_index = collections.defaultdict(list)
    for d in header_decls:
        header_decl_index[d["name"]].append(d)

    # ----------------------------
    # 5) 流式处理 calls_raw
    # ----------------------------
    callers_by_def_id = collections.defaultdict(list)
    unresolved_calls = []

    resolved_call_count = 0
    unresolved_call_count = 0
    matched_call_count = 0
    unmatched_resolved_call_count = 0

    for path in iter_json_files(args.calls_dir):
        chunk = load_json(path)

        for c in chunk.get("resolved_calls", []):
            resolved_call_count += 1
            matched = None

            cfile = c.get("callee_decl_file", "")
            cline = c.get("callee_decl_line", 0)
            ccol = c.get("callee_decl_column", 0)
            cname = c.get("callee_name", "")

            # 1) 精确位置匹配
            if cfile and cname:
                for d in defs_by_name.get(cname, []):
                    if (
                        d["file"] == cfile
                        and d["line"] == cline
                        and d["column"] == ccol
                    ):
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
                matched_call_count += 1
            else:
                unmatched_resolved_call_count += 1

        for uc in chunk.get("unresolved_calls", []):
            unresolved_call_count += 1
            unresolved_calls.append(uc)

    # ----------------------------
    # 6) 生成 merged 输出
    # ----------------------------
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

        enriched_def = dict(d)
        enriched_def["declared_in_header"] = bool(decls)
        enriched_def["header_declarations"] = decls
        enriched_def["export_symbol"] = export_symbol
        enriched_def["export_symbol_gpl"] = export_symbol_gpl
        enriched_def["exports"] = exps

        callers = callers_by_def_id.get(d["id"], [])

        merged.append({
            "function": enriched_def,
            "resolved_callers": callers,
        })

    out = {
        "subsystem": args.subsystem,
        "functions": merged,
        "unresolved_calls": unresolved_calls,
        "merge_stats": {
            "definition_count": len(definitions),
            "header_declaration_count": len(header_decls),
            "resolved_call_count": resolved_call_count,
            "unresolved_call_count": unresolved_call_count,
            "matched_resolved_call_count": matched_call_count,
            "unmatched_resolved_call_count": unmatched_resolved_call_count,
        },
    }

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()

