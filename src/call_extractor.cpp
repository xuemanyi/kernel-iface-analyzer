#include "kia/common/json_utils.h"
#include "kia/common/path_utils.h"
#include "kia/common/source_utils.h"

#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Tooling/CommonOptionsParser.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/CommandLine.h>

#include <algorithm>
#include <cctype>
#include <optional>
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

struct CalleeInfo {
    bool resolved = false;
    std::string callee_kind;
    std::string callee_name;
    std::string callee_decl_file;
    int64_t callee_decl_line = 0;
    int64_t callee_decl_column = 0;
    std::string unresolved_reason;
    std::string callee_expr_text;
    std::string callee_type;
};

struct FunctionRange {
    std::string file;
    std::string name;
    int64_t name_line = 0;
    int64_t name_column = 0;
    int64_t begin_line = 0;
    int64_t end_line = 0;
};

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

        if (StringRef(a).starts_with("-Wp,-MMD") ||
            StringRef(a).starts_with("-Wp,-MD") ||
            StringRef(a).starts_with("-Wp,-MF,") ||
            StringRef(a).starts_with("-Wp,-MT,") ||
            StringRef(a).starts_with("-Wp,-MQ,")) {
            continue;
        }

        out.push_back(a);
    }

    return out;
}

std::string getTypeText(QualType qt, const ASTContext &ctx) {
    PrintingPolicy policy(ctx.getLangOpts());
    policy.SuppressTagKeyword = false;
    policy.FullyQualifiedName = false;
    return qt.getAsString(policy);
}

std::string guessNameFromExprText(const std::string &text) {
    if (text.empty()) {
        return "";
    }

    std::string s = text;
    auto pos = s.find('(');
    if (pos != std::string::npos) {
        s = s.substr(0, pos);
    }

    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }

    while (!s.empty() &&
           (s.front() == '*' || s.front() == '&' || s.front() == '(' ||
            std::isspace(static_cast<unsigned char>(s.front())))) {
        s.erase(s.begin());
    }

    while (!s.empty() &&
           (s.back() == ')' || std::isspace(static_cast<unsigned char>(s.back())))) {
        s.pop_back();
    }

    size_t end = s.size();
    while (end > 0 &&
           (std::isalnum(static_cast<unsigned char>(s[end - 1])) ||
            s[end - 1] == '_')) {
        --end;
    }

    if (end < s.size()) {
        return s.substr(end);
    }

    return s;
}

SourceLocation getPreferredDeclLoc(const FunctionDecl *FD, const SourceManager &SM) {
    SourceLocation loc = FD->getLocation();
    if (loc.isInvalid()) {
        loc = FD->getNameInfo().getLoc();
    }
    if (loc.isInvalid()) {
        loc = FD->getBeginLoc();
    }
    if (loc.isInvalid()) {
        return loc;
    }
    return SM.getExpansionLoc(loc);
}

std::optional<CalleeInfo> infoFromFunctionDecl(const FunctionDecl *FD,
                                               const SourceManager &SM,
                                               const std::string &sourceRoot,
                                               const std::string &kind) {
    if (!FD) {
        return std::nullopt;
    }

    SourceLocation calleeLoc = getPreferredDeclLoc(FD, SM);
    std::string calleeAbsFile = SM.getFilename(calleeLoc).str();

    CalleeInfo info;
    info.resolved = true;
    info.callee_kind = kind;
    info.callee_name = FD->getNameAsString();
    info.callee_decl_file = calleeAbsFile.empty()
                                ? ""
                                : kia::normalize_path(calleeAbsFile, sourceRoot);
    info.callee_decl_line = static_cast<int64_t>(SM.getExpansionLineNumber(calleeLoc));
    info.callee_decl_column = static_cast<int64_t>(SM.getExpansionColumnNumber(calleeLoc));
    return info;
}

const FunctionDecl *tryResolveFunctionPointerInitializer(const VarDecl *VD) {
    if (!VD || !VD->hasInit()) {
        return nullptr;
    }

    const Expr *Init = VD->getInit();
    if (!Init) {
        return nullptr;
    }

    Init = Init->IgnoreParenImpCasts();

    if (const auto *DRE = dyn_cast<DeclRefExpr>(Init)) {
        return dyn_cast<FunctionDecl>(DRE->getDecl());
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(Init)) {
        if (UO->getOpcode() == UO_AddrOf) {
            const Expr *Sub = UO->getSubExpr()->IgnoreParenImpCasts();
            if (const auto *DRE = dyn_cast<DeclRefExpr>(Sub)) {
                return dyn_cast<FunctionDecl>(DRE->getDecl());
            }
        }
    }

    return nullptr;
}

CalleeInfo analyzeCalleeExpr(const Expr *calleeExpr,
                             ASTContext &Ctx,
                             const SourceManager &SM,
                             const std::string &sourceRoot) {
    CalleeInfo info;
    info.resolved = false;
    info.unresolved_reason = "unsupported_call";

    if (!calleeExpr) {
        info.callee_kind = "indirect_or_unknown";
        info.unresolved_reason = "missing_callee_expr";
        return info;
    }

    const Expr *E = calleeExpr->IgnoreParenImpCasts();
    QualType qt = E->getType();

    info.callee_expr_text = kia::get_stmt_text(E, SM, Ctx.getLangOpts());
    info.callee_type = getTypeText(qt, Ctx);

    if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
        if (const auto *FD = dyn_cast<FunctionDecl>(DRE->getDecl())) {
            auto resolved = infoFromFunctionDecl(FD, SM, sourceRoot, "declref_function");
            if (resolved) {
                resolved->callee_expr_text = info.callee_expr_text;
                resolved->callee_type = info.callee_type;
                return *resolved;
            }
        }

        if (const auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
            info.callee_kind = "function_pointer";
            info.callee_name = VD->getNameAsString();
            info.unresolved_reason = "function_pointer_variable";

            if (const FunctionDecl *Target = tryResolveFunctionPointerInitializer(VD)) {
                auto resolved = infoFromFunctionDecl(Target, SM, sourceRoot, "function_pointer");
                if (resolved) {
                    resolved->callee_expr_text = info.callee_expr_text;
                    resolved->callee_type = info.callee_type;
                    return *resolved;
                }
            }
            return info;
        }

        if (const auto *PVD = dyn_cast<ParmVarDecl>(DRE->getDecl())) {
            info.callee_kind = "function_pointer";
            info.callee_name = PVD->getNameAsString();
            info.unresolved_reason = "function_pointer_parameter";
            return info;
        }
    }

    if (const auto *ME = dyn_cast<MemberExpr>(E)) {
        info.callee_kind = "member_call";
        info.callee_name = ME->getMemberNameInfo().getAsString();
        info.unresolved_reason = "member_expr_call";

        if (const auto *FD = dyn_cast_or_null<FunctionDecl>(ME->getMemberDecl())) {
            auto resolved = infoFromFunctionDecl(FD, SM, sourceRoot, "member_call");
            if (resolved) {
                resolved->callee_expr_text = info.callee_expr_text;
                resolved->callee_type = info.callee_type;
                return *resolved;
            }
        }
        return info;
    }

    if (const auto *UO = dyn_cast<UnaryOperator>(E)) {
        if (UO->getOpcode() == UO_Deref || UO->getOpcode() == UO_AddrOf) {
            CalleeInfo sub = analyzeCalleeExpr(UO->getSubExpr(), Ctx, SM, sourceRoot);
            if (sub.callee_kind.empty()) {
                sub.callee_kind = "function_pointer";
            }
            if (sub.unresolved_reason == "unsupported_call") {
                sub.unresolved_reason = "unary_operator_wrapped_call";
            }
            return sub;
        }
    }

    if (!qt.isNull() && qt->isFunctionPointerType()) {
        info.callee_kind = "function_pointer";
        info.unresolved_reason = "function_pointer_type";
        info.callee_name = guessNameFromExprText(info.callee_expr_text);
        return info;
    }

    if (!qt.isNull() && qt->isFunctionType()) {
        info.callee_kind = "callable_expression";
        info.unresolved_reason = "function_type_expr";
        info.callee_name = guessNameFromExprText(info.callee_expr_text);
        return info;
    }

    info.callee_kind = "indirect_or_unknown";
    info.callee_name = guessNameFromExprText(info.callee_expr_text);
    return info;
}

bool shouldRecordMacroName(const std::string &name) {
    if (name.empty()) {
        return false;
    }

    static const std::vector<std::string> skip = {
        "__init", "__initdata", "__visible", "__always_inline",
        "__cold", "__noreturn", "__must_check", "__latent_entropy",
        "__ro_after_init", "__read_mostly", "__section", "__aligned",
        "__printf", "__scanf", "__malloc", "__packed", "__used",
        "__maybe_unused", "__weak", "__pure", "__rcu", "__iomem",
        "__force", "__must_hold", "__acquires", "__releases",
        "__sched", "__counted_by", "__no_sanitize_address",
        "__no_stack_protector", "asmlinkage"
    };

    return std::find(skip.begin(), skip.end(), name) == skip.end();
}

class CallVisitor : public RecursiveASTVisitor<CallVisitor> {
public:
    explicit CallVisitor(ASTContext &ctx,
                         const std::string &sourceRoot,
                         llvm::json::Array &resolvedCalls,
                         llvm::json::Array &unresolvedCalls,
                         std::vector<FunctionRange> &functionRanges)
        : Ctx(ctx),
          SourceRoot(sourceRoot),
          ResolvedCalls(resolvedCalls),
          UnresolvedCalls(unresolvedCalls),
          FunctionRanges(functionRanges) {}

    bool TraverseFunctionDecl(FunctionDecl *FD) {
        FunctionDecl *Prev = CurrentFunction;
        if (FD && FD->hasBody()) {
            CurrentFunction = FD;
        }
        RecursiveASTVisitor<CallVisitor>::TraverseFunctionDecl(FD);
        CurrentFunction = Prev;
        return true;
    }

    bool VisitFunctionDecl(FunctionDecl *FD) {
        if (!FD || !FD->hasBody()) {
            return true;
        }

        auto &SM = Ctx.getSourceManager();

        SourceLocation nameLoc = getPreferredDeclLoc(FD, SM);
        SourceLocation bodyBegin = SM.getExpansionLoc(FD->getBody()->getBeginLoc());
        SourceLocation bodyEnd = SM.getExpansionLoc(FD->getBody()->getEndLoc());

        if (nameLoc.isInvalid() || bodyBegin.isInvalid() || bodyEnd.isInvalid()) {
            return true;
        }

        std::string absFile = SM.getFilename(nameLoc).str();
        if (absFile.empty()) {
            return true;
        }

        FunctionRange fr;
        fr.file = kia::normalize_path(absFile, SourceRoot);
        fr.name = FD->getNameAsString();
        fr.name_line = static_cast<int64_t>(SM.getExpansionLineNumber(nameLoc));
        fr.name_column = static_cast<int64_t>(SM.getExpansionColumnNumber(nameLoc));
        fr.begin_line = static_cast<int64_t>(SM.getExpansionLineNumber(bodyBegin));
        fr.end_line = static_cast<int64_t>(SM.getExpansionLineNumber(bodyEnd));

        FunctionRanges.push_back(std::move(fr));
        return true;
    }

    bool VisitCallExpr(CallExpr *CE) {
        if (!CE) {
            return true;
        }

        auto &SM = Ctx.getSourceManager();
        SourceLocation callLoc = SM.getExpansionLoc(CE->getBeginLoc());
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
        base["line"] = static_cast<int64_t>(SM.getExpansionLineNumber(callLoc));
        base["column"] = static_cast<int64_t>(SM.getExpansionColumnNumber(callLoc));
        base["expr_text"] = exprText;

        if (CurrentFunction) {
            SourceLocation fnLoc = getPreferredDeclLoc(CurrentFunction, SM);
            base["caller_function"] = CurrentFunction->getNameAsString();
            base["caller_function_line"] =
                static_cast<int64_t>(SM.getExpansionLineNumber(fnLoc));
            base["caller_function_column"] =
                static_cast<int64_t>(SM.getExpansionColumnNumber(fnLoc));
        } else {
            base["caller_function"] = "";
            base["caller_function_line"] = static_cast<int64_t>(0);
            base["caller_function_column"] = static_cast<int64_t>(0);
        }

        if (const FunctionDecl *Callee = CE->getDirectCallee()) {
            llvm::json::Object obj = base;
            auto resolved = infoFromFunctionDecl(
                Callee, SM, SourceRoot,
                CE->getBeginLoc().isMacroID() ? "macro_wrapped" : "direct");

            obj["resolved"] = true;
            obj["callee_kind"] = resolved ? resolved->callee_kind : "direct";
            obj["callee_name"] = resolved ? resolved->callee_name : Callee->getNameAsString();
            obj["callee_decl_file"] = resolved ? resolved->callee_decl_file : "";
            obj["callee_decl_line"] = resolved ? resolved->callee_decl_line : 0;
            obj["callee_decl_column"] = resolved ? resolved->callee_decl_column : 0;

            const Expr *calleeExpr = CE->getCallee();
            if (calleeExpr) {
                const Expr *cleanExpr = calleeExpr->IgnoreParenImpCasts();
                obj["callee_expr_text"] =
                    kia::get_stmt_text(cleanExpr, SM, Ctx.getLangOpts());
                obj["callee_type"] = getTypeText(cleanExpr->getType(), Ctx);
            } else {
                obj["callee_expr_text"] = "";
                obj["callee_type"] = "";
            }

            ResolvedCalls.emplace_back(std::move(obj));
            return true;
        }

        const Expr *calleeExpr = CE->getCallee();
        CalleeInfo analyzed = analyzeCalleeExpr(calleeExpr, Ctx, SM, SourceRoot);

        if (CE->getBeginLoc().isMacroID()) {
            if (analyzed.callee_kind == "direct" ||
                analyzed.callee_kind == "declref_function") {
                analyzed.callee_kind = "macro_wrapped";
            } else if (analyzed.callee_kind.empty() ||
                       analyzed.callee_kind == "indirect_or_unknown") {
                analyzed.callee_kind = "macro_wrapped";
            }
            if (analyzed.unresolved_reason == "unsupported_call") {
                analyzed.unresolved_reason = "macro_wrapped_or_expansion";
            }
        }

        llvm::json::Object obj = base;
        obj["resolved"] = analyzed.resolved;
        obj["callee_kind"] = analyzed.callee_kind;
        obj["callee_name"] = analyzed.callee_name;
        obj["callee_decl_file"] = analyzed.callee_decl_file;
        obj["callee_decl_line"] = analyzed.callee_decl_line;
        obj["callee_decl_column"] = analyzed.callee_decl_column;
        obj["callee_expr_text"] = analyzed.callee_expr_text;
        obj["callee_type"] = analyzed.callee_type;

        if (analyzed.resolved) {
            ResolvedCalls.emplace_back(std::move(obj));
        } else {
            if (!CurrentFunction) {
                analyzed.unresolved_reason = "missing_caller_context";
                obj["caller_function"] = "";
                obj["caller_function_line"] = static_cast<int64_t>(0);
                obj["caller_function_column"] = static_cast<int64_t>(0);
            }
            obj["unresolved_reason"] = analyzed.unresolved_reason;
            UnresolvedCalls.emplace_back(std::move(obj));
        }

        return true;
    }

private:
    ASTContext &Ctx;
    std::string SourceRoot;
    FunctionDecl *CurrentFunction = nullptr;
    llvm::json::Array &ResolvedCalls;
    llvm::json::Array &UnresolvedCalls;
    std::vector<FunctionRange> &FunctionRanges;
};

class MacroTracker : public PPCallbacks {
public:
    MacroTracker(SourceManager &SM,
                 const LangOptions &LangOpts,
                 const std::string &SourceRoot,
                 const std::vector<FunctionRange> &FunctionRanges,
                 llvm::json::Array &MacroCalls)
        : SM(SM),
          LangOpts(LangOpts),
          SourceRoot(SourceRoot),
          FunctionRanges(FunctionRanges),
          MacroCalls(MacroCalls) {}

    void MacroExpands(const Token &MacroNameTok,
                      const MacroDefinition &,
                      SourceRange Range,
                      const MacroArgs *) override {
        SourceLocation loc = SM.getExpansionLoc(Range.getBegin());
        if (loc.isInvalid()) {
            return;
        }

        std::string absFile = SM.getFilename(loc).str();
        if (absFile.empty()) {
            return;
        }

        std::string file = kia::normalize_path(absFile, SourceRoot);
        if (file.empty()) {
            return;
        }

        std::string macroName =
            MacroNameTok.getIdentifierInfo()
                ? MacroNameTok.getIdentifierInfo()->getName().str()
                : "";

        if (!shouldRecordMacroName(macroName)) {
            return;
        }

        int64_t line = static_cast<int64_t>(SM.getExpansionLineNumber(loc));
        int64_t column = static_cast<int64_t>(SM.getExpansionColumnNumber(loc));

        const FunctionRange *owner = nullptr;
        for (const auto &fr : FunctionRanges) {
            if (fr.file == file && line >= fr.begin_line && line <= fr.end_line) {
                owner = &fr;
                break;
            }
        }

        if (!owner) {
            return;
        }

        llvm::json::Object obj;
        obj["caller_file"] = file;
        obj["caller_function"] = owner->name;
        obj["caller_function_line"] = owner->name_line;
        obj["caller_function_column"] = owner->name_column;
        obj["line"] = line;
        obj["column"] = column;
        obj["resolved"] = false;
        obj["callee_kind"] = "macro_call";
        obj["callee_name"] = macroName;
        obj["callee_decl_file"] = "";
        obj["callee_decl_line"] = static_cast<int64_t>(0);
        obj["callee_decl_column"] = static_cast<int64_t>(0);
        obj["expr_text"] = kia::get_source_text(SM, Range.getBegin(), Range.getEnd(), LangOpts);
        obj["callee_expr_text"] = obj["expr_text"];
        obj["callee_type"] = "";
        obj["unresolved_reason"] = "macro_expansion";

        MacroCalls.emplace_back(std::move(obj));
    }

private:
    SourceManager &SM;
    const LangOptions &LangOpts;
    const std::string &SourceRoot;
    const std::vector<FunctionRange> &FunctionRanges;
    llvm::json::Array &MacroCalls;
};

class CallConsumer : public ASTConsumer {
public:
    CallConsumer(ASTContext &ctx,
                 const std::string &sourceRoot,
                 llvm::json::Array &resolvedCalls,
                 llvm::json::Array &unresolvedCalls,
                 std::vector<FunctionRange> &functionRanges)
        : Visitor(ctx, sourceRoot, resolvedCalls, unresolvedCalls, functionRanges) {}

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
               llvm::json::Array &unresolvedCalls,
               llvm::json::Array &macroCalls)
        : SourceRoot(sourceRoot),
          ResolvedCalls(resolvedCalls),
          UnresolvedCalls(unresolvedCalls),
          MacroCalls(macroCalls) {}

    bool BeginSourceFileAction(CompilerInstance &CI) override {
        CI.getPreprocessor().addPPCallbacks(
            std::make_unique<MacroTracker>(
                CI.getSourceManager(),
                CI.getLangOpts(),
                SourceRoot,
                FunctionRanges,
                MacroCalls));
        return true;
    }

    std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI,
                                                   StringRef) override {
        return std::make_unique<CallConsumer>(
            CI.getASTContext(), SourceRoot, ResolvedCalls, UnresolvedCalls, FunctionRanges);
    }

private:
    std::string SourceRoot;
    llvm::json::Array &ResolvedCalls;
    llvm::json::Array &UnresolvedCalls;
    llvm::json::Array &MacroCalls;
    std::vector<FunctionRange> FunctionRanges;
};

class CallActionFactory : public FrontendActionFactory {
public:
    CallActionFactory(const std::string &sourceRoot,
                      llvm::json::Array &resolvedCalls,
                      llvm::json::Array &unresolvedCalls,
                      llvm::json::Array &macroCalls)
        : SourceRoot(sourceRoot),
          ResolvedCalls(resolvedCalls),
          UnresolvedCalls(unresolvedCalls),
          MacroCalls(macroCalls) {}

    std::unique_ptr<FrontendAction> create() override {
        return std::make_unique<CallAction>(
            SourceRoot, ResolvedCalls, UnresolvedCalls, MacroCalls);
    }

private:
    std::string SourceRoot;
    llvm::json::Array &ResolvedCalls;
    llvm::json::Array &UnresolvedCalls;
    llvm::json::Array &MacroCalls;
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
    llvm::json::Array macroCalls;

    CallActionFactory factory(SourceRoot, resolvedCalls, unresolvedCalls, macroCalls);
    int rc = Tool.run(&factory);
    if (rc != 0) {
        errs() << "call_extractor clang tool run failed: " << rc << "\n";
        return rc;
    }

    llvm::json::Object out;
    out["resolved_calls"] = std::move(resolvedCalls);
    out["unresolved_calls"] = std::move(unresolvedCalls);
    out["macro_calls"] = std::move(macroCalls);

    return kia::write_json_file(Output, llvm::json::Value(std::move(out))) ? 0 : 1;
}