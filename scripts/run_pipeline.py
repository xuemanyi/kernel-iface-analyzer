#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path


RUNTIME_TOPS = {
    "arch",
    "block",
    "certs",
    "crypto",
    "drivers",
    "fs",
    "init",
    "io_uring",
    "ipc",
    "kernel",
    "lib",
    "mm",
    "net",
    "security",
    "sound",
    "virt",
}


def load_compile_db(build_dir):
    path = Path(build_dir) / "compile_commands.json"
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def normalize_rel(source_root, path):
    return os.path.relpath(os.path.realpath(path), os.path.realpath(source_root)).replace("\\", "/")


def unique_tus(entries):
    seen = set()
    out = []
    for e in entries:
        f = os.path.realpath(e["file"])
        if f in seen:
            continue
        seen.add(f)
        out.append(f)
    return out


def safe_name(path):
    return path.replace("/", "__").replace("\\", "__").replace(":", "_")


def ensure_empty_dir(path):
    p = Path(path)
    if p.exists():
        shutil.rmtree(p)
    p.mkdir(parents=True, exist_ok=True)


def is_runtime_tu(source_root, path):
    rel = normalize_rel(source_root, path)

    if not rel.endswith(".c"):
        return False

    if rel.endswith(".lex.c") or rel.endswith(".tab.c"):
        return False

    top = rel.split("/", 1)[0]
    if top not in RUNTIME_TOPS:
        return False

    if rel.startswith("scripts/") or rel.startswith("tools/"):
        return False

    if "/scripts/" in rel or "/tools/" in rel:
        return False

    return True


def run(cmd, capture_output=False):
    print("+", " ".join(cmd))
    if capture_output:
        return subprocess.run(cmd, text=True, capture_output=True)
    subprocess.check_call(cmd)
    return None


def run_one_tu(cmd, failed_list, tu, stage):
    result = run(cmd, capture_output=True)
    if result.returncode == 0:
        return True

    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)

    failed_list.append({
        "stage": stage,
        "tu": tu,
        "command": cmd,
        "returncode": result.returncode,
        "stdout": result.stdout,
        "stderr": result.stderr,
    })
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-root", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--tool-build-dir", required=True)
    ap.add_argument("--subsystem", required=True)
    ap.add_argument("--out-dir", required=True)
    args = ap.parse_args()

    source_root = os.path.realpath(args.source_root)
    build_dir = os.path.realpath(args.build_dir)
    tool_build_dir = os.path.realpath(args.tool_build_dir)
    out_dir = os.path.realpath(args.out_dir)

    entries = load_compile_db(build_dir)

    all_tus = [x for x in unique_tus(entries) if is_runtime_tu(source_root, x)]

    subsystem_tus = []
    for tu in all_tus:
        rel = normalize_rel(source_root, tu)
        if rel == args.subsystem or rel.startswith(args.subsystem + "/"):
            subsystem_tus.append(tu)

    Path(out_dir).mkdir(parents=True, exist_ok=True)
    functions_raw = Path(out_dir) / "functions_raw"
    calls_raw = Path(out_dir) / "calls_raw"
    ensure_empty_dir(functions_raw)
    ensure_empty_dir(calls_raw)

    failed_tus = []

    run([
        str(Path(tool_build_dir) / "tu_indexer"),
        "--build-dir", build_dir,
        "--source-root", source_root,
        "--subsystem", args.subsystem,
        "--output", str(Path(out_dir) / "tu_list.json"),
    ])

    for tu in subsystem_tus:
        rel = normalize_rel(source_root, tu)
        out = functions_raw / f"{safe_name(rel)}.json"
        run_one_tu([
            str(Path(tool_build_dir) / "function_extractor"),
            "-p", build_dir,
            "--source-root", source_root,
            "--subsystem", args.subsystem,
            "--output", str(out),
            tu,
        ], failed_tus, rel, "function_extractor")

    for tu in all_tus:
        rel = normalize_rel(source_root, tu)
        out = calls_raw / f"{safe_name(rel)}.json"
        run_one_tu([
            str(Path(tool_build_dir) / "call_extractor"),
            "-p", build_dir,
            "--source-root", source_root,
            "--output", str(out),
            tu,
        ], failed_tus, rel, "call_extractor")

    run([
        sys.executable, str(Path(__file__).parent / "export_scanner.py"),
        "--source-root", source_root,
        "--subsystem", args.subsystem,
        "--output", str(Path(out_dir) / "exports.json"),
    ])

    run([
        sys.executable, str(Path(__file__).parent / "merge.py"),
        "--functions-dir", str(functions_raw),
        "--calls-dir", str(calls_raw),
        "--exports", str(Path(out_dir) / "exports.json"),
        "--subsystem", args.subsystem,
        "--output", str(Path(out_dir) / "merged_call_graph.json"),
    ])

    run([
        sys.executable, str(Path(__file__).parent / "classify.py"),
        "--merged", str(Path(out_dir) / "merged_call_graph.json"),
        "--subsystem", args.subsystem,
        "--internal-by-file", str(Path(out_dir) / "internal_by_file.json"),
        "--external-interfaces", str(Path(out_dir) / "external_interfaces.json"),
        "--unresolved-calls", str(Path(out_dir) / "unresolved_calls.json"),
        "--stats", str(Path(out_dir) / "stats.json"),
    ])

    with open(Path(out_dir) / "failed_tus.json", "w", encoding="utf-8") as f:
        json.dump({
            "failed_count": len(failed_tus),
            "failed_tus": failed_tus,
        }, f, indent=2, ensure_ascii=False)


if __name__ == "__main__":
    main()
