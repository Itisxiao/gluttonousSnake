#pragma once

#include <optional>
#include <string>

std::string escapeJson(const std::string& s);
std::optional<std::string> jsonString(const std::string& json, const std::string& key);
