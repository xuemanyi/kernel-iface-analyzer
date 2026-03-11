#pragma once

#include <string>
#include <vector>

namespace kia {

std::string normalize_path(const std::string &path, const std::string &source_root);
bool inside_subsystem(const std::string &normalized_relative_path,
                      const std::string &subsystem);
bool has_any_extension(const std::string &path,
                       const std::vector<std::string> &exts);
std::string to_posix_path(std::string s);

} // namespace kia
