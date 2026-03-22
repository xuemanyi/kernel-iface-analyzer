#!/usr/bin/env python3
import argparse
import glob
import json
import re
from collections import defaultdict
from pathlib import Path


def load_json(path: Path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def safe_name(path: str) -> str:
    return path.replace("/", "__").replace("\\", "__").replace(":", "_")


def inside_subsystem(path: str, subsystem: str) -> bool:
    return path == subsystem or path.startswith(subsystem + "/")


def guess_name(expr_text: str) -> str:
    if not expr_text:
        return ""
    s = expr_text.strip()
    pos = s.find("(")
    if pos != -1:
        s = s[:pos]
    s = s.strip().lstrip("*&(").rstrip(")")
    m = re.search(r"([A-Za-z_]\w*)$", s)
    return m.group(1) if m else ""


def iter_json_files(directory: Path):
    for path in sorted(glob.glob(str(directory / "*.json"))):
        yield Path(path)


def dedup_calls(calls):
    seen = set()
    out = []
    for c in calls:
        key = (
            c.get("function", ""),
            c.get("file", ""),
            c.get("line", 0),
            c.get("callee_kind", ""),
            c.get("called_at", {}).get("file", ""),
            c.get("called_at", {}).get("line", 0),
            c.get("called_at", {}).get("column", 0),
            c.get("unresolved_reason", ""),
        )
        if key in seen:
            continue
        seen.add(key)
        out.append(c)
    return out


def build_definition_indexes(functions_raw_dir: Path):
    defs_by_file = defaultdict(list)
    defs_by_name = defaultdict(list)

    for path in iter_json_files(functions_raw_dir):
        chunk = load_json(path)
        for d in chunk.get("definitions", []):
            rec = {
                "name": d["name"],
                "file": d["file"],
                "line": d["line"],
                "column": d["column"],
                "is_static": d.get("is_static", False),
                "return_type": d.get("return_type", ""),
                "parameters": d.get("parameters", []),
            }
            defs_by_file[d["file"]].append(rec)
            defs_by_name[d["name"]].append(rec)

    for file_path in defs_by_file:
        defs_by_file[file_path].sort(key=lambda x: (x["line"], x["column"], x["name"]))

    return defs_by_file, defs_by_name


def classify_target(target_file: str, subsystem: str) -> str:
    if target_file and inside_subsystem(target_file, subsystem):
        return "internal"
    return "external"


def match_target_files(defs_by_file, files, prefixes):
    all_files = sorted(defs_by_file.keys())
    matched = set()

    for f in files:
        if f in defs_by_file:
            matched.add(f)

    for prefix in prefixes:
        for f in all_files:
            if f.startswith(prefix):
                matched.add(f)

    return sorted(matched)


def could_contain_target_callers(path: Path, target_files):
    try:
        text = path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        return True

    for target_file in target_files:
        marker = f'"caller_file": "{target_file}"'
        if marker in text:
            return True
    return False


def normalize_macro_call(c, defs_by_name):
    callee_name = c.get("callee_name", "") or guess_name(
        c.get("callee_expr_text", "") or c.get("expr_text", "")
    )
    callee_file = c.get("callee_decl_file", "")
    callee_line = c.get("callee_decl_line", 0)

    if (not callee_file or not callee_line) and callee_name and len(defs_by_name.get(callee_name, [])) == 1:
        matched = defs_by_name[callee_name][0]
        callee_file = matched["file"]
        callee_line = matched["line"]

    return {
        "function": callee_name,
        "file": callee_file,
        "line": callee_line,
        "callee_kind": c.get("callee_kind", "macro_call"),
        "called_at": {
            "file": c.get("caller_file", ""),
            "line": c.get("line", 0),
            "column": c.get("column", 0),
        },
        "unresolved_reason": c.get("unresolved_reason", "macro_expansion"),
    }


def build_call_indexes(calls_raw_dir: Path, defs_by_name, target_files):
    resolved_by_caller = defaultdict(list)
    unresolved_by_caller = defaultdict(list)
    macro_by_caller = defaultdict(list)
    target_files_set = set(target_files)

    scanned_files = 0
    loaded_files = 0

    for path in iter_json_files(calls_raw_dir):
        scanned_files += 1

        if not could_contain_target_callers(path, target_files_set):
            continue

        loaded_files += 1
        chunk = load_json(path)

        for c in chunk.get("resolved_calls", []):
            caller_file = c.get("caller_file", "")
            caller_function = c.get("caller_function", "")
            if caller_file not in target_files_set or not caller_function:
                continue

            callee_name = c.get("callee_name", "") or guess_name(
                c.get("callee_expr_text", "") or c.get("expr_text", "")
            )
            callee_file = c.get("callee_decl_file", "")
            callee_line = c.get("callee_decl_line", 0)

            if (not callee_file or not callee_line) and callee_name and len(defs_by_name.get(callee_name, [])) == 1:
                matched = defs_by_name[callee_name][0]
                callee_file = matched["file"]
                callee_line = matched["line"]

            entry = {
                "function": callee_name,
                "file": callee_file,
                "line": callee_line,
                "callee_kind": c.get("callee_kind", "direct"),
                "called_at": {
                    "file": caller_file,
                    "line": c.get("line", 0),
                    "column": c.get("column", 0),
                },
            }
            resolved_by_caller[(caller_file, caller_function)].append(entry)

        for c in chunk.get("unresolved_calls", []):
            caller_file = c.get("caller_file", "")
            caller_function = c.get("caller_function", "")
            if caller_file not in target_files_set or not caller_function:
                continue

            callee_name = c.get("callee_name", "") or guess_name(
                c.get("callee_expr_text", "") or c.get("expr_text", "")
            )
            callee_file = c.get("callee_decl_file", "")
            callee_line = c.get("callee_decl_line", 0)

            if (not callee_file or not callee_line) and callee_name and len(defs_by_name.get(callee_name, [])) == 1:
                matched = defs_by_name[callee_name][0]
                callee_file = matched["file"]
                callee_line = matched["line"]

            entry = {
                "function": callee_name,
                "file": callee_file,
                "line": callee_line,
                "callee_kind": c.get("callee_kind", ""),
                "called_at": {
                    "file": caller_file,
                    "line": c.get("line", 0),
                    "column": c.get("column", 0),
                },
                "unresolved_reason": c.get("unresolved_reason", "unknown"),
            }
            unresolved_by_caller[(caller_file, caller_function)].append(entry)

        for c in chunk.get("macro_calls", []):
            caller_file = c.get("caller_file", "")
            caller_function = c.get("caller_function", "")
            if caller_file not in target_files_set or not caller_function:
                continue

            macro_by_caller[(caller_file, caller_function)].append(
                normalize_macro_call(c, defs_by_name)
            )

    print(f"scanned calls_raw files: {scanned_files}")
    print(f"loaded relevant calls_raw files: {loaded_files}")

    return resolved_by_caller, unresolved_by_caller, macro_by_caller


def build_result_for_file(target_file,
                          defs_by_file,
                          resolved_by_caller,
                          unresolved_by_caller,
                          macro_by_caller):
    file_defs = defs_by_file.get(target_file, [])
    if not file_defs:
        return None

    subsystem = target_file.split("/", 1)[0]

    result = {
        "file": target_file,
        "functions": [],
    }

    for fn in file_defs:
        key = (fn["file"], fn["name"])
        resolved_calls = dedup_calls(resolved_by_caller.get(key, []))
        unresolved_calls = dedup_calls(unresolved_by_caller.get(key, []))
        macro_calls = dedup_calls(macro_by_caller.get(key, []))

        internal_calls = []
        external_calls = []
        unresolved_out = []

        for c in resolved_calls:
            out = {
                "function": c.get("function", ""),
                "file": c.get("file", ""),
                "line": c.get("line", 0),
                "callee_kind": c.get("callee_kind", ""),
                "called_at": c.get("called_at", {}),
            }
            if classify_target(c.get("file", ""), subsystem) == "internal":
                internal_calls.append(out)
            else:
                external_calls.append(out)

        for c in unresolved_calls:
            unresolved_out.append({
                "function": c.get("function", ""),
                "file": c.get("file", ""),
                "line": c.get("line", 0),
                "callee_kind": c.get("callee_kind", ""),
                "called_at": c.get("called_at", {}),
                "unresolved_reason": c.get("unresolved_reason", ""),
            })

        for c in macro_calls:
            unresolved_out.append({
                "function": c.get("function", ""),
                "file": c.get("file", ""),
                "line": c.get("line", 0),
                "callee_kind": c.get("callee_kind", "macro_call"),
                "called_at": c.get("called_at", {}),
                "unresolved_reason": c.get("unresolved_reason", "macro_expansion"),
            })

        result["functions"].append({
            "name": fn["name"],
            "line": fn["line"],
            "internal_calls": internal_calls,
            "external_calls": external_calls,
            "unresolved_calls": dedup_calls(unresolved_out),
        })

    return result


def write_call_list(lines, key, calls, indent):
    lines.append(f'{" " * indent}"{key}": [')
    if calls:
        for idx, c in enumerate(calls):
            comma = "," if idx < len(calls) - 1 else ""
            lines.append(
                f'{" " * (indent + 2)}'
                + json.dumps(c, ensure_ascii=False, separators=(", ", ": "))
                + comma
            )
    lines.append(f'{" " * indent}]')


def write_result_json(result, out_path: Path):
    lines = []
    lines.append("{")
    lines.append(f'  "file": {json.dumps(result["file"], ensure_ascii=False)},')
    lines.append('  "functions": [')

    funcs = result.get("functions", [])
    for i, fn in enumerate(funcs):
        lines.append("    {")
        lines.append(f'      "name": {json.dumps(fn["name"], ensure_ascii=False)},')
        lines.append(f'      "line": {fn["line"]},')

        write_call_list(lines, "internal_calls", fn.get("internal_calls", []), 6)
        lines[-1] += ","

        write_call_list(lines, "external_calls", fn.get("external_calls", []), 6)
        lines[-1] += ","

        write_call_list(lines, "unresolved_calls", fn.get("unresolved_calls", []), 6)

        lines.append("    }" + ("," if i < len(funcs) - 1 else ""))

    lines.append("  ]")
    lines.append("}")
    out_path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--analysis-dir", required=True, help="analysis output dir containing functions_raw and calls_raw")
    ap.add_argument("--file", action="append", default=[], help="target source file, may be used multiple times")
    ap.add_argument("--prefix", action="append", default=[], help="target path prefix, e.g. init/")
    ap.add_argument("--output-dir", default=None, help="output dir, default: <analysis-dir>/raw_file_calls")
    args = ap.parse_args()

    if not args.file and not args.prefix:
        raise SystemExit("at least one --file or --prefix is required")

    analysis_dir = Path(args.analysis_dir).resolve()
    functions_raw_dir = analysis_dir / "functions_raw"
    calls_raw_dir = analysis_dir / "calls_raw"
    output_dir = Path(args.output_dir).resolve() if args.output_dir else (analysis_dir / "raw_file_calls")
    output_dir.mkdir(parents=True, exist_ok=True)

    defs_by_file, defs_by_name = build_definition_indexes(functions_raw_dir)

    target_files = match_target_files(defs_by_file, args.file, args.prefix)
    if not target_files:
        raise SystemExit("no matching source files found")

    print(f"matched target files: {len(target_files)}")
    for f in target_files:
        print(f"  {f}")

    resolved_by_caller, unresolved_by_caller, macro_by_caller = build_call_indexes(
        calls_raw_dir,
        defs_by_name,
        target_files,
    )

    written = []
    for target_file in target_files:
        result = build_result_for_file(
            target_file=target_file,
            defs_by_file=defs_by_file,
            resolved_by_caller=resolved_by_caller,
            unresolved_by_caller=unresolved_by_caller,
            macro_by_caller=macro_by_caller,
        )
        if result is None:
            continue

        out_name = f"{safe_name(target_file)}__calls_from_raw.json"
        out_path = output_dir / out_name
        write_result_json(result, out_path)
        written.append(str(out_path))

    print("generated files:")
    for path in written:
        print(path)


if __name__ == "__main__":
    main()