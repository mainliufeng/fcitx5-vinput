#pragma once

#include <string>
#include <string_view>

namespace vinput::llm {

// Resolves a configured API key into the value that is actually sent.
//
// A key written as `$NAME` or `${NAME}` is looked up in the environment at call
// time, so a secret never has to be written into `~/.config/vinput/config.json`.
// Any other value is returned unchanged, which keeps plaintext keys working.
//
// Returns an empty string when an environment reference is malformed or the
// variable is not set; callers must treat that as "no credential available"
// rather than sending a request that is guaranteed to fail authentication.
std::string ResolveApiKey(std::string_view configured);

// True when the configured value is an environment reference rather than a
// literal key. Used to produce a clearer error when the variable is missing.
bool IsApiKeyFromEnvironment(std::string_view configured);

} // namespace vinput::llm
