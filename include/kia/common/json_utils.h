#pragma once

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <string>

namespace kia {

inline bool write_json_file(const std::string &path, const llvm::json::Value &v) {
    std::error_code ec;
    llvm::raw_fd_ostream os(path, ec);
    if (ec) {
        llvm::errs() << "failed to open json output: " << path
                     << " error: " << ec.message() << "\n";
        return false;
    }
    os << llvm::formatv("{0:2}", v);
    os << "\n";
    return true;
}

} // namespace kia
