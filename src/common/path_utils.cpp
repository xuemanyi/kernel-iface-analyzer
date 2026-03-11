#include "kia/common/path_utils.h"

#include <algorithm>
#include <filesystem>

namespace kia {

std::string to_posix_path(std::string s) {
    std::replace(s.begin(), s.end(), '\\', '/');
    return s;
}

std::string normalize_path(const std::string &path, const std::string &source_root) {
    namespace fs = std::filesystem;

    try {
        fs::path p = fs::weakly_canonical(fs::path(path));
        fs::path r = fs::weakly_canonical(fs::path(source_root));
        fs::path rel = fs::relative(p, r);
        return to_posix_path(rel.lexically_normal().string());
    } catch (...) {
        // 如果 relative 失败，尽量返回原始路径的 posix 形式，避免直接丢失信息
        return to_posix_path(path);
    }
}

bool inside_subsystem(const std::string &normalized_relative_path,
                      const std::string &subsystem) {
    if (normalized_relative_path == subsystem) {
        return true;
    }
    return normalized_relative_path.rfind(subsystem + "/", 0) == 0;
}

bool has_any_extension(const std::string &path,
                       const std::vector<std::string> &exts) {
    for (const auto &ext : exts) {
        if (path.size() >= ext.size() &&
            path.compare(path.size() - ext.size(), ext.size(), ext) == 0) {
            return true;
        }
    }
    return false;
}

} // namespace kia
