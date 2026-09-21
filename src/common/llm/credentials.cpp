#include "common/llm/credentials.h"

#include <cstdlib>

namespace vinput::llm {

namespace {

bool IsEnvName(std::string_view name) {
  if (name.empty()) {
    return false;
  }
  const auto is_alpha = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
  };
  if (!is_alpha(name.front())) {
    return false;
  }
  for (const char c : name) {
    if (!is_alpha(c) && !(c >= '0' && c <= '9')) {
      return false;
    }
  }
  return true;
}

// Extracts the variable name from `$NAME` / `${NAME}`. Returns an empty view for
// anything that is not an environment reference.
std::string_view EnvName(std::string_view configured) {
  if (configured.size() < 2 || configured.front() != '$') {
    return {};
  }
  std::string_view name = configured.substr(1);
  if (name.size() >= 2 && name.front() == '{' && name.back() == '}') {
    name = name.substr(1, name.size() - 2);
  }
  return IsEnvName(name) ? name : std::string_view{};
}

} // namespace

bool IsApiKeyFromEnvironment(std::string_view configured) {
  return !EnvName(configured).empty();
}

std::string ResolveApiKey(std::string_view configured) {
  const std::string_view name = EnvName(configured);
  if (name.empty()) {
    // Not an environment reference: keep the literal value.
    return std::string(configured);
  }
  const std::string owned(name);
  const char* value = std::getenv(owned.c_str());
  return value != nullptr ? std::string(value) : std::string();
}

} // namespace vinput::llm
