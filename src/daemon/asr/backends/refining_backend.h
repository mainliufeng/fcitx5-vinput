#pragma once

#include <memory>
#include <string>

#include "daemon/asr/runtime/recognition_contract.h"

namespace vinput::daemon::asr {

// Composes a primary (typically streaming) backend with a second-pass offline
// backend.
//
// The primary keeps the interactive contract: its partial hypotheses are
// forwarded as they arrive, so the live preedit is unchanged. On finish the
// buffered utterance is re-decoded by `refine`, and its text is emitted as the
// last FinalText, which is the one the session manager commits.
//
// Refinement is best-effort: when `refine` rejects the audio or produces no
// text, only the primary result is emitted, so a failing second pass can never
// drop an utterance.
std::unique_ptr<AsrBackend> CreateRefiningBackend(std::unique_ptr<AsrBackend> primary,
                                                  std::unique_ptr<AsrBackend> refine,
                                                  std::string* error);

} // namespace vinput::daemon::asr
