#pragma once

#include <string>
#include <string_view>

namespace vinput::daemon::asr {

// Recognizers return separate utterances without a leading space. Preserve
// English words and sentence boundaries when joining them; leave CJK spacing
// and any whitespace already emitted by the recognizer intact.
inline void AppendRecognizedText(std::string& joined, std::string_view text) {
  const auto is_ascii_word = [](unsigned char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
  };
  if (!joined.empty() && !text.empty() && is_ascii_word(text.front()) &&
      (is_ascii_word(joined.back()) ||
       std::string_view(".?!,;:").find(joined.back()) != std::string_view::npos)) {
    joined += ' ';
  }
  joined += text;
}

} // namespace vinput::daemon::asr
