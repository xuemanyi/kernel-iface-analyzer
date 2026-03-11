#include "kia/common/json_utils.h"
#include "kia/common/path_utils.h"
#include "kia/common/source_utils.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
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

static cl::OptionCategory KIACategory("kia call extractor options");

static cl::opt<std::string> SourceRoot(
    "source-root", cl::desc("Kernel source root"),
    cl::Required, cl::cat(KIACategory));

static cl::opt<std::string> Output(
    "output", cl::desc("Output JSON path"),
    cl::Required, cl::cat(KIACategory));

namespace {

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

class CallVisitor : public RecursiveASTVisitor<CallVisitor> {
public:
    explicit CallVisitor(ASTContext &ctx,
                         const std::string &sourceRoot,
                         llvm::json::Array &resolvedCalls,
                         llvm::json::Array &unresolvedCalls)
        : Ctx(ctx),
          SourceRoot(sourceRoot),
          ResolvedCalls(resolvedCalls),
          UnresolvedCalls(unresolvedCalls) {}

    bool TraverseFunctionDecl(FunctionDecl *FD) {
        FunctionDecl *Prev = CurrentFunction;
        if (FD && FD->hasBody()) {
            CurrentFunction = FD;
        }
        RecursiveASTVisitor<CallVisitor>::TraverseFunctionDecl(FD);
        CurrentFunction = Prev;
        return true;
    }

    bool VisitCallExpr(CallExpr *CE) {
        if (!CE) {
            return true;
        }

        auto &SM = Ctx.getSourceManager();
        SourceLocation callLoc = SM.getSpellingLoc(CE->getBeginLoc());
        if (callLoc.isInvalid()) {
            return true;
        }

        std::string callerAbsFile = SM.getFilename(callLoc).str();
        if (callerAbsFile.empty()) {
            return true;
        }

        std::string callerFile = kia::normalize_path(callerAbsFile, SourceRoot);
        std::string exprText = kia::get_stmt_text(CE, SM, Ctx.getLangOpts());

        llvm::json::Object base;
        base["caller_file"] = callerFile;
        base["line"] = static_cast<int64_t>(SM.getSpellingLineNumber(callLoc));
        base["column"] = static_cast<int64_t>(SM.getSpellingColumnNumber(callLoc));
        base["expr_text"] = exprText;

        if (CurrentFunction) {
            SourceLocation fnLoc = SM.getSpellingLoc(CurrentFunction->getBeginLoc());
            base["caller_function"] = CurrentFunction->getNameAsString();
            base["caller_function_line"] = static_cast<int64_t>(SM.getSpellingLineNumber(fnLoc));
            base["caller_function_column"] = static_cast<int64_t>(SM.getSpellingColumnNumber(fnLoc));
        } else {
            base["caller_function"] = "";
            base["caller_function_line"] = static_cast<int64_t>(0);
            base["caller_function_column"] = static_cast<int64_t>(0);
        }

        if (const FunctionDecl *Callee = CE->getDirectCallee()) {
            llvm::json::Object obj = base;
            SourceLocation calleeLoc = SM.getSpellingLoc(Callee->getBeginLoc());
            std::string calleeAbsFile = SM.getFilename(calleeLoc).str();

            obj["resolved"] = true;
            obj["callee_kind"] = "direct";
            obj["callee_name"] = Callee->getNameAsString();
            obj["callee_decl_file"] = calleeAbsFile.empty()
                                          ? ""
                                          : kia::normalize_path(calleeAbsFile, SourceRoot);
            obj["callee_decl_line"] = static_cast<int64_t>(SM.getSpellingLineNumber(calleeLoc));
            obj["callee_decl_column"] = static_cast<int64_t>(SM.getSpellingColumnNumber(calleeLoc));

            ResolvedCalls.emplace_back(std::move(obj));
            return true;
        }

        llvm::json::Object obj = base;
        obj["resolved"] = false;
        obj["callee_kind"] = "indirect_or_macro";
        obj["callee_name"] = "";

        std::string reason = "unsupported_call";
        const Expr *calleeExpr = CE->getCallee();
        if (calleeExpr) {
            calleeExpr = calleeExpr->IgnoreParenImpCasts();
            if (isa<MemberExpr>(calleeExpr)) {
                reason = "indirect_call";
            } else if (isa<DeclRefExpr>(calleeExpr) && CE->getBeginLoc().isMacroID()) {
                reason = "macro_wrapped_or_expansion";
            } else if (CE->getBeginLoc().isMacroID()) {
                reason = "macro_wrapped_or_expansion";
            }
        } else if (!CurrentFunction) {
            reason = "missing_caller_context";
        }

        if (!CurrentFunction) {
            reason = "missing_caller_context";
        }

        obj["unresolved_reason"] = reason;
        UnresolvedCalls.emplace_back(std::move(obj));
        return true;
    }

private:
    ASTContext &Ctx;
    std::string SourceRoot;
    FunctionDecl *CurrentFunction = nullptr;
    llvm::json::Array &ResolvedCalls;
    llvm::json::Array &UnresolvedCalls;
};

class CallConsumer : public ASTConsumer {
public:
    CallConsumer(ASTContext &ctx,
                 const std::string &sourceRoot,
                 llvm::json::Array &resolvedCalls,
                 llvm::json::Array &unresolvedCalls)
        : Visitor(ctx, sourceRoot, resolvedCalls, unresolvedCalls) {}

    void HandleTranslationUnit(ASTContext &ctx) override {
        Visitor.TraverseDecl(ctx.getTranslationUnitDecl());
    }

private:
    CallVisitor Visitor;
};

class CallAction : public ASTFrontendAction {
public:
    CallAction(const std::string &sourceRoot,
               llvm::json::Array &resolvedCalls,
               llvm::json::Array &unresolvedCalls)
        : SourceRoot(sourceRoot),
          ResolvedCalls(resolvedCalls),
          UnresolvedCalls(unresolvedCalls) {}

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef) override {
        return std::make_unique<CallConsumer>(
            CI.getASTContext(), SourceRoot, ResolvedCalls, UnresolvedCalls);
    }

private:
    std::string SourceRoot;
    llvm::json::Array &ResolvedCalls;
    llvm::json::Array &UnresolvedCalls;
};

class CallActionFactory : public FrontendActionFactory {
public:
    CallActionFactory(const std::string &sourceRoot,
                      llvm::json::Array &resolvedCalls,
                      llvm::json::Array &unresolvedCalls)
        : SourceRoot(sourceRoot),
          ResolvedCalls(resolvedCalls),
          UnresolvedCalls(unresolvedCalls) {}

    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<CallAction>(
            SourceRoot, ResolvedCalls, UnresolvedCalls);
    }

private:
    std::string SourceRoot;
    llvm::json::Array &ResolvedCalls;
    llvm::json::Array &UnresolvedCalls;
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

    llvm::json::Array resolvedCalls;
    llvm::json::Array unresolvedCalls;

    CallActionFactory factory(SourceRoot, resolvedCalls, unresolvedCalls);
    int rc = Tool.run(&factory);
    if (rc != 0) {
        errs() << "call_extractor clang tool run failed: " << rc << "\n";
        return rc;
    }

    llvm::json::Object out;
    out["resolved_calls"] = std::move(resolvedCalls);
    out["unresolved_calls"] = std::move(unresolvedCalls);

    return kia::write_json_file(Output, llvm::json::Value(std::move(out))) ? 0 : 1;
}
