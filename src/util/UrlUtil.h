#pragma once

#include <string>
#include <vector>

namespace autoupdater::util {

std::string joinUrl(const std::string& baseUrl, const std::string& relativePath);
bool urlStartsWithAny(const std::string& url, const std::vector<std::string>& allowed);
bool isFileUrl(const std::string& url);
std::string fileUrlToPath(const std::string& url);

} // namespace autoupdater::util

