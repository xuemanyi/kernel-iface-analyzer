#include "kia/common/json_utils.h"
#include "kia/common/path_utils.h"

#include <clang/Tooling/CompilationDatabase.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/JSON.h>

using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory KIACategory("kia tu indexer options");

static cl::opt<std::string> BuildDir(
    "build-dir", cl::desc("Build directory containing compile_commands.json"),
    cl::Required, cl::cat(KIACategory));

static cl::opt<std::string> SourceRoot(
    "source-root", cl::desc("Kernel source root"),
    cl::Required, cl::cat(KIACategory));

static cl::opt<std::string> Subsystem(
    "subsystem", cl::desc("Target subsystem, e.g. init"),
    cl::Required, cl::cat(KIACategory));

static cl::opt<std::string> Output(
    "output", cl::desc("Output tu_list.json"),
    cl::Required, cl::cat(KIACategory));

int main(int argc, const char **argv) {
    cl::HideUnrelatedOptions(KIACategory);
    cl::ParseCommandLineOptions(argc, argv);

    std::string error;
    auto db = CompilationDatabase::loadFromDirectory(BuildDir, error);
    if (!db) {
        errs() << "failed to load compilation database from " << BuildDir
               << ": " << error << "\n";
        return 1;
    }

    llvm::json::Array tus;
    size_t total = 0;
    size_t matched = 0;

    for (const auto &file : db->getAllFiles()) {
        ++total;
        std::string rel = kia::normalize_path(file, SourceRoot);
        if (!kia::inside_subsystem(rel, Subsystem) ||
            !kia::has_any_extension(rel, {".c"})) {
            continue;
        }

        ++matched;
        llvm::json::Object item;
        item["file"] = rel;
        item["absolute_file"] = file;
        tus.emplace_back(std::move(item));
    }

    llvm::json::Object out;
    out["subsystem"] = Subsystem;
    out["total_compile_db_files"] = static_cast<int64_t>(total);
    out["matched_translation_units"] = static_cast<int64_t>(matched);
    out["translation_units"] = std::move(tus);

    return kia::write_json_file(Output, llvm::json::Value(std::move(out))) ? 0 : 1;
}

