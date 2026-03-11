#include "kia/common/json_utils.h"
#include "kia/common/path_utils.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>

#include <string>
#include <vector>

using namespace clang;
using namespace clang::tooling;
using namespace llvm;

static cl::OptionCategory KIACategory("kia function extractor options");

static cl::opt<std::string> SourceRoot(
    "source-root", cl::desc("Kernel source root"),
    cl::Required, cl::cat(KIACategory));

static cl::opt<std::string> Subsystem(
    "subsystem", cl::desc("Target subsystem, e.g. init"),
    cl::Required, cl::cat(KIACategory));

static cl::opt<std::string> Output(
    "output", cl::desc("Output JSON path"),
    cl::Required, cl::cat(KIACategory));

namespace {

std::string getTypeText(QualType qt, const ASTContext &ctx) {
    PrintingPolicy policy(ctx.getLangOpts());
    policy.SuppressTagKeyword = false;
    policy.FullyQualifiedName = false;
    return qt.getAsString(policy);
}

std::vector<std::string> sanitizeArgs(const CommandLineArguments &args) {
    std::vector<std::string> out;
    out.reserve(args.size());

    auto skip_next_value = [&](size_t &i) {
        if (i + 1 < args.size()) {
            ++i;
        }
    };

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string &a = args[i];

        if (a == "-c") {
            continue;
        }

        if (a == "-o" || a == "-MF" || a == "-MT" || a == "-MQ" ||
            a == "-dependency-file" || a == "--dependency-file") {
            skip_next_value(i);
            continue;
        }

        if (a == "-MD" || a == "-MMD" || a == "-MP" || a == "-MG") {
            continue;
        }

        if (a == "-fcolor-diagnostics" || a == "-fdiagnostics-color") {
            continue;
        }

        if (StringRef(a).startswith("-Wp,-MMD") ||
            StringRef(a).startswith("-Wp,-MD") ||
            StringRef(a).startswith("-Wp,-MF,") ||
            StringRef(a).startswith("-Wp,-MT,") ||
            StringRef(a).startswith("-Wp,-MQ,")) {
            continue;
        }

        out.push_back(a);
    }

    return out;
}

class FunctionVisitor : public RecursiveASTVisitor<FunctionVisitor> {
public:
    explicit FunctionVisitor(ASTContext &ctx,
                             const std::string &sourceRoot,
                             const std::string &subsystem,
                             llvm::json::Array &definitions,
                             llvm::json::Array &headerDecls)
        : Ctx(ctx),
          SourceRoot(sourceRoot),
          Subsystem(subsystem),
          Definitions(definitions),
          HeaderDecls(headerDecls) {}

    bool VisitFunctionDecl(FunctionDecl *FD) {
        if (!FD) {
            return true;
        }

        auto &SM = Ctx.getSourceManager();
        auto spellingLoc = SM.getSpellingLoc(FD->getBeginLoc());
        if (spellingLoc.isInvalid()) {
            return true;
        }

        std::string absFile = SM.getFilename(spellingLoc).str();
        if (absFile.empty()) {
            return true;
        }

        std::string relFile = kia::normalize_path(absFile, SourceRoot);

        if (FD->isThisDeclarationADefinition() && FD->hasBody()) {
            if (!kia::inside_subsystem(relFile, Subsystem) ||
                !kia::has_any_extension(relFile, {".c"})) {
                return true;
            }

            llvm::json::Object obj;
            obj["name"] = FD->getNameAsString();
            obj["file"] = relFile;
            obj["line"] = static_cast<int64_t>(SM.getSpellingLineNumber(spellingLoc));
            obj["column"] = static_cast<int64_t>(SM.getSpellingColumnNumber(spellingLoc));
            obj["is_definition"] = true;
            obj["is_static"] = (FD->getStorageClass() == SC_Static);
            obj["storage_class"] = static_cast<int64_t>(FD->getStorageClass());
            obj["return_type"] = getTypeText(FD->getReturnType(), Ctx);
            obj["param_count"] = static_cast<int64_t>(FD->param_size());

            llvm::json::Array params;
            for (const ParmVarDecl *P : FD->parameters()) {
                llvm::json::Object po;
                po["name"] = P->getNameAsString();
                po["type"] = getTypeText(P->getType(), Ctx);
                params.emplace_back(std::move(po));
            }
            obj["parameters"] = std::move(params);

            obj["signature_text"] = FD->getNameInfo().getAsString();
            obj["from_macro_expansion"] = FD->getBeginLoc().isMacroID();
            obj["usr_hint"] = (relFile + ":" + FD->getNameAsString() + ":" +
                               std::to_string(SM.getSpellingLineNumber(spellingLoc)) + ":" +
                               std::to_string(SM.getSpellingColumnNumber(spellingLoc)));

            Definitions.emplace_back(std::move(obj));
            return true;
        }

        if (!FD->isThisDeclarationADefinition() &&
            kia::has_any_extension(relFile, {".h"})) {

            llvm::json::Object obj;
            obj["name"] = FD->getNameAsString();
            obj["decl_file"] = relFile;
            obj["decl_line"] = static_cast<int64_t>(SM.getSpellingLineNumber(spellingLoc));
            obj["decl_column"] = static_cast<int64_t>(SM.getSpellingColumnNumber(spellingLoc));
            obj["return_type"] = getTypeText(FD->getReturnType(), Ctx);
            obj["param_count"] = static_cast<int64_t>(FD->param_size());

            llvm::json::Array params;
            for (const ParmVarDecl *P : FD->parameters()) {
                llvm::json::Object po;
                po["name"] = P->getNameAsString();
                po["type"] = getTypeText(P->getType(), Ctx);
                params.emplace_back(std::move(po));
            }
            obj["parameters"] = std::move(params);
            HeaderDecls.emplace_back(std::move(obj));
        }

        return true;
    }

private:
    ASTContext &Ctx;
    std::string SourceRoot;
    std::string Subsystem;
    llvm::json::Array &Definitions;
    llvm::json::Array &HeaderDecls;
};

class FunctionConsumer : public ASTConsumer {
public:
    FunctionConsumer(ASTContext &ctx,
                     const std::string &sourceRoot,
                     const std::string &subsystem,
                     llvm::json::Array &definitions,
                     llvm::json::Array &headerDecls)
        : Visitor(ctx, sourceRoot, subsystem, definitions, headerDecls) {}

    void HandleTranslationUnit(ASTContext &ctx) override {
        Visitor.TraverseDecl(ctx.getTranslationUnitDecl());
    }

private:
    FunctionVisitor Visitor;
};

class FunctionAction : public ASTFrontendAction {
public:
    FunctionAction(const std::string &sourceRoot,
                   const std::string &subsystem,
                   llvm::json::Array &definitions,
                   llvm::json::Array &headerDecls)
        : SourceRoot(sourceRoot),
          Subsystem(subsystem),
          Definitions(definitions),
          HeaderDecls(headerDecls) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef) override {
        return std::make_unique<FunctionConsumer>(
            CI.getASTContext(), SourceRoot, Subsystem, Definitions, HeaderDecls);
    }

private:
    std::string SourceRoot;
    std::string Subsystem;
    llvm::json::Array &Definitions;
    llvm::json::Array &HeaderDecls;
};

class FunctionActionFactory : public FrontendActionFactory {
public:
    FunctionActionFactory(const std::string &sourceRoot,
                          const std::string &subsystem,
                          llvm::json::Array &definitions,
                          llvm::json::Array &headerDecls)
        : SourceRoot(sourceRoot),
          Subsystem(subsystem),
          Definitions(definitions),
          HeaderDecls(headerDecls) {}

    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<FunctionAction>(
            SourceRoot, Subsystem, Definitions, HeaderDecls);
    }

private:
    std::string SourceRoot;
    std::string Subsystem;
    llvm::json::Array &Definitions;
    llvm::json::Array &HeaderDecls;
};

} // namespace

int main(int argc, const char **argv) {
    auto ExpectedParser = CommonOptionsParser::create(argc, argv, KIACategory);
    if (!ExpectedParser) {
        errs() << ExpectedParser.takeError();
        return 1;
    }

    auto &Options = ExpectedParser.get();
    ClangTool Tool(Options.getCompilations(), Options.getSourcePathList());

    Tool.appendArgumentsAdjuster(
        [](const CommandLineArguments &args, StringRef) {
            return sanitizeArgs(args);
        });

    llvm::json::Array definitions;
    llvm::json::Array headerDecls;

    FunctionActionFactory factory(SourceRoot, Subsystem, definitions, headerDecls);
    int rc = Tool.run(&factory);
    if (rc != 0) {
        errs() << "function_extractor clang tool run failed: " << rc << "\n";
        return rc;
    }

    llvm::json::Object out;
    out["definitions"] = std::move(definitions);
    out["header_declarations"] = std::move(headerDecls);

    return kia::write_json_file(Output, llvm::json::Value(std::move(out))) ? 0 : 1;
}
