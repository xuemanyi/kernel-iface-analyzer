#!/usr/bin/env python3
import argparse
import collections
import json


def inside_subsystem(path: str, subsystem: str) -> bool:
    return path == subsystem or path.startswith(subsystem + "/")


def classify_one(item, subsystem):
    f = item["function"]
    callers = item.get("resolved_callers", [])

    reasons = ["defined_in_target_subsystem"]
    internal_callers = []
    external_callers = []

    for c in callers:
        if inside_subsystem(c["caller_file"], subsystem):
            internal_callers.append(c)
        else:
            external_callers.append(c)

    if f["is_static"]:
        reasons.append("static_function")
        confidence = "high"
        if f.get("declared_in_header"):
            reasons.append("header_decl_present_but_static_definition")
            confidence = "medium"
        return "file_local_internal", reasons, confidence, internal_callers, external_callers

    if external_callers:
        reasons.append("called_by_files_outside_subsystem")
        if f.get("declared_in_header"):
            reasons.append("declared_in_header")
        if f.get("export_symbol"):
            reasons.append("exported_via_export_symbol")
        if f.get("export_symbol_gpl"):
            reasons.append("exported_via_export_symbol_gpl")

        confidence = "high"
        if any(c.get("merge_fallback") == "name_only" for c in external_callers):
            reasons.append("name_based_merge_fallback")
            confidence = "medium"

        return "external_interface", reasons, confidence, internal_callers, external_callers

    reasons.append("no_external_resolved_callers_found")
    if internal_callers:
        reasons.append("only_called_within_subsystem")
        confidence = "high"
        if any(c.get("merge_fallback") == "name_only" for c in internal_callers):
            reasons.append("name_based_merge_fallback")
            confidence = "medium"
    else:
        reasons.append("no_resolved_callers_found")
        confidence = "low"
        if f.get("declared_in_header"):
            reasons.append("declared_in_header_without_resolved_external_callers")
            confidence = "medium"
        if f.get("export_symbol") or f.get("export_symbol_gpl"):
            reasons.append("exported_without_resolved_external_callers")
            confidence = "medium"

    return "subsystem_local_internal", reasons, confidence, internal_callers, external_callers


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--merged", required=True)
    ap.add_argument("--subsystem", required=True)
    ap.add_argument("--internal-by-file", required=True)
    ap.add_argument("--external-interfaces", required=True)
    ap.add_argument("--unresolved-calls", required=True)
    ap.add_argument("--stats", required=True)
    args = ap.parse_args()

    data = json.load(open(args.merged, "r", encoding="utf-8"))
    merged_functions = data["functions"]
    unresolved_calls = data.get("unresolved_calls", [])

    internal_by_file = collections.defaultdict(list)
    external_interfaces = []
    unresolved_summary = collections.Counter()

    stats = collections.Counter()
    stats["function_count"] = len(merged_functions)

    for uc in unresolved_calls:
        unresolved_summary[uc.get("unresolved_reason", "unknown")] += 1

    for item in merged_functions:
        classification, reasons, confidence, internal_callers, external_callers = classify_one(
            item, args.subsystem
        )

        f = item["function"]
        enriched = {
            "function": f["name"],
            "defined_in": f["file"],
            "line": f["line"],
            "column": f["column"],
            "classification": classification,
            "return_type": f["return_type"],
            "parameters": f["parameters"],
            "declared_in_header": f.get("declared_in_header", False),
            "header_declarations": f.get("header_declarations", []),
            "export_symbol": f.get("export_symbol", False),
            "export_symbol_gpl": f.get("export_symbol_gpl", False),
            "reason": reasons,
            "confidence": confidence,
        }

        if classification == "external_interface":
            enriched["external_callers"] = [
                {
                    "file": c["caller_file"],
                    "caller_function": c.get("caller_function", ""),
                    "line": c["line"],
                    "column": c["column"],
                    "merge_fallback": c.get("merge_fallback", "none"),
                    "expr_text": c.get("expr_text", ""),
                }
                for c in external_callers
            ]
            external_interfaces.append(enriched)
            stats["external_interface_count"] += 1
        else:
            enriched["internal_callers"] = [
                {
                    "file": c["caller_file"],
                    "caller_function": c.get("caller_function", ""),
                    "line": c["line"],
                    "column": c["column"],
                    "merge_fallback": c.get("merge_fallback", "none"),
                    "expr_text": c.get("expr_text", ""),
                }
                for c in internal_callers
            ]
            internal_by_file[f["file"]].append(enriched)
            if classification == "file_local_internal":
                stats["file_local_internal_count"] += 1
            elif classification == "subsystem_local_internal":
                stats["subsystem_local_internal_count"] += 1

    unresolved_out = {
        "summary": {
            "total": len(unresolved_calls),
            **dict(unresolved_summary),
        },
        "unresolved_calls": unresolved_calls,
    }

    stats["unresolved_call_count"] = len(unresolved_calls)

    with open(args.internal_by_file, "w", encoding="utf-8") as f:
        json.dump(dict(internal_by_file), f, indent=2, ensure_ascii=False)

    with open(args.external_interfaces, "w", encoding="utf-8") as f:
        json.dump({"external_interfaces": external_interfaces}, f, indent=2, ensure_ascii=False)

    with open(args.unresolved_calls, "w", encoding="utf-8") as f:
        json.dump(unresolved_out, f, indent=2, ensure_ascii=False)

    with open(args.stats, "w", encoding="utf-8") as f:
        json.dump(dict(stats), f, indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()

