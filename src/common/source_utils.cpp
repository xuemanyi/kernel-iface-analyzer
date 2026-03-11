#include "kia/common/source_utils.h"

#include <clang/AST/Stmt.h>
#include <clang/Lex/Lexer.h>

namespace kia {

std::string get_source_text(const clang::SourceManager &sm,
                            clang::SourceLocation begin,
                            clang::SourceLocation end,
                            const clang::LangOptions &lang_opts) {
    if (begin.isInvalid() || end.isInvalid()) {
        return "";
    }

    clang::CharSourceRange range =
        clang::CharSourceRange::getTokenRange(begin, end);

    auto text = clang::Lexer::getSourceText(range, sm, lang_opts);
    if (text.empty()) {
        return "";
    }
    return text.str();
}

std::string get_stmt_text(const clang::Stmt *stmt,
                          const clang::SourceManager &sm,
                          const clang::LangOptions &lang_opts) {
    if (!stmt) {
        return "";
    }
    return get_source_text(sm, stmt->getBeginLoc(), stmt->getEndLoc(), lang_opts);
}

} // namespace kia
