#pragma once

#include <string>

namespace clang {
class SourceManager;
class SourceLocation;
class LangOptions;
class Stmt;
}

namespace kia {

std::string get_source_text(const clang::SourceManager &sm,
                            clang::SourceLocation begin,
                            clang::SourceLocation end,
                            const clang::LangOptions &lang_opts);

std::string get_stmt_text(const clang::Stmt *stmt,
                          const clang::SourceManager &sm,
                          const clang::LangOptions &lang_opts);

} // namespace kia
  //
