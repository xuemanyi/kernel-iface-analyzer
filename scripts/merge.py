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


def make_def_id(d):
    return f'{d["file"]}:{d["line"]}:{d["column"]}:{d["name"]}'


def make_fn_key(file_path, fn_name):
    return f"{file_path}::{fn_name}"


def inside_subsystem(path, subsystem):
    return path == subsystem or path.startswith(subsystem + "/")


def copy_call_record(c):
    return {
        "caller_file": c.get("caller_file", ""),
        "caller_function": c.get("caller_function", ""),
        "caller_function_line": c.get("caller_function_line", 0),
        "caller_function_column": c.get("caller_function_column", 0),
        "line": c.get("line", 0),
        "column": c.get("column", 0),
        "expr_text": c.get("expr_text", ""),
        "callee_kind": c.get("callee_kind", ""),
        "callee_name": c.get("callee_name", ""),
        "callee_decl_file": c.get("callee_decl_file", ""),
        "callee_decl_line": c.get("callee_decl_line", 0),
        "callee_decl_column": c.get("callee_decl_column", 0),
        "callee_expr_text": c.get("callee_expr_text", ""),
        "callee_type": c.get("callee_type", ""),
        "merge_fallback": c.get("merge_fallback", "none"),
        "resolved": c.get("resolved", False),
    }


def copy_unresolved_record(c):
    return {
        "caller_file": c.get("caller_file", ""),
        "caller_function": c.get("caller_function", ""),
        "caller_function_line": c.get("caller_function_line", 0),
        "caller_function_column": c.get("caller_function_column", 0),
        "line": c.get("line", 0),
        "column": c.get("column", 0),
        "expr_text": c.get("expr_text", ""),
        "callee_kind": c.get("callee_kind", ""),
        "callee_name": c.get("callee_name", ""),
        "callee_decl_file": c.get("callee_decl_file", ""),
        "callee_decl_line": c.get("callee_decl_line", 0),
        "callee_decl_column": c.get("callee_decl_column", 0),
        "callee_expr_text": c.get("callee_expr_text", ""),
        "callee_type": c.get("callee_type", ""),
        "unresolved_reason": c.get("unresolved_reason", "unknown"),
        "resolved": False,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--functions-dir", required=True)
    ap.add_argument("--calls-dir", required=True)
    ap.add_argument("--exports", required=True)
    ap.add_argument("--subsystem", required=True)
    ap.add_argument("--output", required=True)
    args = ap.parse_args()

    # 1) load definitions / decls
    definitions = []
    header_decls = []

    for path in iter_json_files(args.functions_dir):
        chunk = load_json(path)
        definitions.extend(chunk.get("definitions", []))
        header_decls.extend(chunk.get("header_declarations", []))

    definitions = dedup_definitions(definitions)
    header_decls = dedup_header_decls(header_decls)

    # 2) load exports
    export_data = load_json(args.exports)
    exports_by_name = collections.defaultdict(list)
    for e in export_data.get("exports", []):
        exports_by_name[e["symbol"]].append(e)

    # 3) build definition indexes
    defs_by_id = {}
    defs_by_name = collections.defaultdict(list)
    defs_by_fn_key = {}
    subsystem_def_ids = set()
    subsystem_fn_keys = set()

    for d in definitions:
        d_id = make_def_id(d)
        d["id"] = d_id
        defs_by_id[d_id] = d
        defs_by_name[d["name"]].append(d)
        fn_key = make_fn_key(d["file"], d["name"])
        defs_by_fn_key[fn_key] = d

        if inside_subsystem(d["file"], args.subsystem):
            subsystem_def_ids.add(d_id)
            subsystem_fn_keys.add(fn_key)

    # 4) build header decl index
    header_decl_index = collections.defaultdict(list)
    for d in header_decls:
        header_decl_index[d["name"]].append(d)

    # 5) process calls_raw
    callers_by_def_id = collections.defaultdict(list)
    callees_by_fn_key = collections.defaultdict(list)

    unresolved_callers_by_def_id = collections.defaultdict(list)
    unresolved_callees_by_fn_key = collections.defaultdict(list)
    kept_unresolved_calls = []

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

            # 1) exact location match
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

            # 2) single-candidate name match
            if matched is None:
                cands = defs_by_name.get(cname, [])
                if len(cands) == 1:
                    matched = cands[0]
                    c["merge_fallback"] = "name_only"

            caller_file = c.get("caller_file", "")
            caller_function = c.get("caller_function", "")
            caller_key = make_fn_key(caller_file, caller_function) if caller_file and caller_function else ""

            if matched is not None:
                matched_call_count += 1

                # keep incoming only if callee is in subsystem definitions
                if matched["id"] in subsystem_def_ids:
                    callers_by_def_id[matched["id"]].append(copy_call_record(c))

                # keep outgoing only if caller is a subsystem function
                if caller_key in subsystem_fn_keys:
                    out = copy_call_record(c)
                    out["resolved_target_id"] = matched["id"]
                    out["resolved_target_file"] = matched["file"]
                    out["resolved_target_line"] = matched["line"]
                    out["resolved_target_column"] = matched["column"]
                    out["resolved_target_name"] = matched["name"]
                    callees_by_fn_key[caller_key].append(out)
            else:
                unmatched_resolved_call_count += 1

                # unmatched resolved call: still useful as outgoing edge for subsystem caller
                if caller_key in subsystem_fn_keys:
                    out = copy_call_record(c)
                    out["merge_fallback"] = c.get("merge_fallback", "unmatched")
                    callees_by_fn_key[caller_key].append(out)

        for uc in chunk.get("unresolved_calls", []):
            unresolved_call_count += 1

            caller_file = uc.get("caller_file", "")
            caller_function = uc.get("caller_function", "")
            caller_key = make_fn_key(caller_file, caller_function) if caller_file and caller_function else ""

            uc_copy = copy_unresolved_record(uc)
            keep_top_level = False

            # keep outgoing unresolved only if caller is a subsystem function
            if caller_key in subsystem_fn_keys:
                unresolved_callees_by_fn_key[caller_key].append(uc_copy)
                keep_top_level = True

            # keep incoming unresolved only if callee_name uniquely maps to a subsystem definition
            cname = uc.get("callee_name", "")
            if cname:
                cands = defs_by_name.get(cname, [])
                if len(cands) == 1 and cands[0]["id"] in subsystem_def_ids:
                    unresolved_callers_by_def_id[cands[0]["id"]].append(uc_copy)
                    keep_top_level = True

            if keep_top_level:
                kept_unresolved_calls.append(uc_copy)

    # 6) build merged output
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

        def_id = d["id"]
        fn_key = make_fn_key(d["file"], d["name"])

        merged.append({
            "function": enriched_def,
            "resolved_callers": callers_by_def_id.get(def_id, []),
            "resolved_callees": callees_by_fn_key.get(fn_key, []),
            "unresolved_callers": unresolved_callers_by_def_id.get(def_id, []),
            "unresolved_callees": unresolved_callees_by_fn_key.get(fn_key, []),
        })

    out = {
        "subsystem": args.subsystem,
        "functions": merged,
        "unresolved_calls": kept_unresolved_calls,
        "merge_stats": {
            "definition_count": len(definitions),
            "header_declaration_count": len(header_decls),
            "resolved_call_count": resolved_call_count,
            "unresolved_call_count": unresolved_call_count,
            "matched_resolved_call_count": matched_call_count,
            "unmatched_resolved_call_count": unmatched_resolved_call_count,
            "kept_unresolved_call_count": len(kept_unresolved_calls),
        },
    }

    with open(args.output, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()