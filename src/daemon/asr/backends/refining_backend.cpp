#include "daemon/asr/backends/refining_backend.h"

#include <cstdint>
#include <exception>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "common/utils/debug_log.h"

namespace vinput::daemon::asr {

namespace {

// A two-pass recognition session.
//
// Pass 1 (primary) runs while the user speaks and owns the partial hypotheses.
// Pass 2 (refine) runs once on finish over the buffered utterance and owns the
// committed text.
class RefiningSession : public RecognitionSession {
public:
  // `refine_backend` is non-owning: the backend outlives every session it
  // creates. It is null when the second pass is unavailable, which makes the
  // session degrade to the primary result.
  RefiningSession(std::unique_ptr<RecognitionSession> primary, AsrBackend* refine_backend)
      : primary_(std::move(primary)), refine_backend_(refine_backend) {}

  bool PushAudio(std::span<const int16_t> pcm, std::string* error) override {
    if (finished_) {
      if (error) {
        *error = "Recognition session already finished.";
      }
      return false;
    }
    if (!primary_) {
      if (error) {
        *error = "Refining session is missing its primary backend.";
      }
      return false;
    }

    if (!pcm.empty()) {
      utterance_.insert(utterance_.end(), pcm.begin(), pcm.end());
    }

    if (!primary_->PushAudio(pcm, error)) {
      return false;
    }
    DrainPrimary();

    if (error) {
      error->clear();
    }
    return true;
  }

  bool Finish(std::string* error) override {
    if (finished_) {
      if (error) {
        error->clear();
      }
      return true;
    }
    finished_ = true;

    // Pass 1 must be drained first: its FinalText is the fallback committed
    // when the second pass yields nothing.
    if (primary_) {
      std::string primary_error;
      if (!primary_->Finish(&primary_error) && !primary_error.empty()) {
        debug::Log("vinput: refining primary finish failed: %s\n", primary_error.c_str());
      }
      DrainPrimary();
    }

    if (refine_backend_ != nullptr && !utterance_.empty()) {
      RunSecondPass();
    }

    events_.push_back({RecognitionEventKind::Completed, {}, {}});
    if (error) {
      error->clear();
    }
    return true;
  }

  void Cancel() override {
    finished_ = true;
    events_.clear();
    utterance_.clear();
    if (primary_) {
      primary_->Cancel();
    }
    events_.push_back({RecognitionEventKind::Completed, {}, {}});
  }

  std::vector<RecognitionEvent> PollEvents() override {
    std::vector<RecognitionEvent> events;
    events.swap(events_);
    return events;
  }

private:
  void DrainPrimary() {
    if (!primary_) {
      return;
    }
    for (auto& event : primary_->PollEvents()) {
      switch (event.kind) {
      case RecognitionEventKind::FinalText:
        primary_final_ = event.text;
        events_.push_back(std::move(event));
        break;
      case RecognitionEventKind::Completed:
        break;
      default:
        events_.push_back(std::move(event));
        break;
      }
    }
  }

  // Runs the offline model over the whole utterance. A second pass is an
  // accuracy optimisation, never a correctness dependency: any failure keeps
  // the primary result and stays out of the user's way.
  void RunSecondPass() {
    // Measured limit: this offline X-ASR encoder fails on inputs past roughly
    // 40 s of audio (ONNX reshape error in the encoder's self-attention:
    // "Input shape:{1,1047,16}, requested shape:{-1,6093,4,4}"). 38.8 s passes,
    // 44.4 s throws. Long dictation is therefore decoded chunk by chunk rather
    // than skipped, so the second pass still applies to it.
    constexpr std::size_t kMaxChunkSamples = 16000 * 30;

    // The safety net for everything the length guard cannot predict: a decode
    // failure must never cost the user the whole utterance.
    try {
      std::vector<std::vector<int16_t>> chunks;
      if (utterance_.size() <= kMaxChunkSamples) {
        chunks.push_back(utterance_);
      } else {
        chunks = SplitForSecondPass(utterance_, kMaxChunkSamples);
        debug::Log("vinput: second pass split %zu samples into %zu chunks\n", utterance_.size(),
                   chunks.size());
      }

      std::string joined;
      std::size_t refined_chunks = 0;
      for (const auto& chunk : chunks) {
        std::string text = RunSecondPassChunk(chunk);
        if (text.empty()) {
          // A chunk that fails leaves a hole in the middle of the utterance, so
          // the whole second pass is dropped instead of committing half a
          // sentence.
          debug::Log("vinput: second pass chunk produced no text, keeping the streaming result\n");
          return;
        }
        joined += text;
        ++refined_chunks;
      }

      if (joined.empty()) {
        debug::Log("vinput: second pass produced no text, keeping the streaming result\n");
        return;
      }

      // Logged so the second pass can be judged against its latency cost.
      debug::Log("vinput: second pass replaced the streaming result samples=%zu chunks=%zu\n",
                 utterance_.size(), refined_chunks);
      debug::Log("vinput:   pass 1 (streaming): %s\n", primary_final_.c_str());
      debug::Log("vinput:   pass 2 (refined):   %s\n", joined.c_str());
      events_.push_back({RecognitionEventKind::FinalText, std::move(joined), {}});
    } catch (const std::exception& error) {
      debug::Log("vinput: second pass threw, keeping the streaming result: %s\n", error.what());
    } catch (...) {
      debug::Log("vinput: second pass threw an unknown exception, keeping the streaming result\n");
    }
  }

  // Cuts `pcm` into chunks of at most `max_samples`, placing each cut at the
  // quietest 200 ms window within 3 s of the nominal boundary so a cut lands in a
  // pause rather than inside a word.
  static std::vector<std::vector<int16_t>> SplitForSecondPass(const std::vector<int16_t>& pcm,
                                                              std::size_t max_samples) {
    constexpr std::size_t kWindow = 16000 / 5;
    constexpr std::size_t kSearch = 16000 * 3;
    std::vector<std::vector<int16_t>> chunks;
    std::size_t begin = 0;
    while (pcm.size() - begin > max_samples) {
      const std::size_t nominal = begin + max_samples;
      const std::size_t lo = nominal > kSearch ? nominal - kSearch : begin + kWindow;
      const std::size_t hi = std::min(pcm.size() - kWindow, nominal + kSearch);
      std::size_t cut = nominal;
      double quietest = std::numeric_limits<double>::max();
      for (std::size_t pos = lo; pos + kWindow <= hi; pos += kWindow / 2) {
        double energy = 0.0;
        for (std::size_t i = pos; i < pos + kWindow; ++i) {
          const auto sample = static_cast<double>(pcm[i]);
          energy += sample * sample;
        }
        if (energy < quietest) {
          quietest = energy;
          cut = pos;
        }
      }
      if (cut <= begin) {
        cut = nominal;
      }
      chunks.emplace_back(pcm.begin() + static_cast<std::ptrdiff_t>(begin),
                          pcm.begin() + static_cast<std::ptrdiff_t>(cut));
      begin = cut;
    }
    if (begin < pcm.size()) {
      chunks.emplace_back(pcm.begin() + static_cast<std::ptrdiff_t>(begin), pcm.end());
    }
    return chunks;
  }

  // A fresh session per chunk: the offline session is single-use (Finish() seals
  // it), while the recognizer underneath is reused.
  std::string RunSecondPassChunk(const std::vector<int16_t>& chunk) {
    std::string refine_error;
    auto session = refine_backend_->CreateSession(&refine_error);
    if (!session) {
      debug::Log("vinput: second pass session failed: %s\n", refine_error.c_str());
      return {};
    }
    if (!session->PushAudio(chunk, &refine_error) || !session->Finish(&refine_error)) {
      debug::Log("vinput: second pass chunk failed: %s\n", refine_error.c_str());
      return {};
    }
    std::string text;
    for (auto& event : session->PollEvents()) {
      if (event.kind == RecognitionEventKind::FinalText && !event.text.empty()) {
        text = std::move(event.text);
      }
    }
    return text;
  }

  std::unique_ptr<RecognitionSession> primary_;
  AsrBackend* refine_backend_ = nullptr;
  std::vector<int16_t> utterance_;
  std::vector<RecognitionEvent> events_;
  std::string primary_final_;
  bool finished_ = false;
};

class RefiningBackend : public AsrBackend {
public:
  RefiningBackend(std::unique_ptr<AsrBackend> primary, std::unique_ptr<AsrBackend> refine)
      : primary_(std::move(primary)), refine_(std::move(refine)) {
    descriptor_ = primary_->Describe();
    // The composite is driven by the primary: chunked delivery keeps the live
    // partial preview working, and the second pass is an internal detail.
    descriptor_.backend_id += "+refine";
    if (refine_) {
      descriptor_.capabilities.supports_hotwords =
          descriptor_.capabilities.supports_hotwords ||
          refine_->Describe().capabilities.supports_hotwords;
    }
  }

  BackendDescriptor Describe() const override { return descriptor_; }

  std::unique_ptr<RecognitionSession> CreateSession(std::string* error) override {
    auto primary_session = primary_->CreateSession(error);
    if (!primary_session) {
      return nullptr;
    }

    // Probe the second pass up front so an unavailable model degrades to the
    // streaming-only behaviour instead of failing the session.
    AsrBackend* refine_backend = nullptr;
    if (refine_) {
      std::string refine_error;
      if (refine_->CreateSession(&refine_error)) {
        refine_backend = refine_.get();
      } else {
        debug::Log("vinput: second pass unavailable, continuing without it: %s\n",
                   refine_error.c_str());
      }
    }

    if (error) {
      error->clear();
    }
    return std::make_unique<RefiningSession>(std::move(primary_session), refine_backend);
  }

private:
  std::unique_ptr<AsrBackend> primary_;
  std::unique_ptr<AsrBackend> refine_;
  BackendDescriptor descriptor_;
};

} // namespace

std::unique_ptr<AsrBackend> CreateRefiningBackend(std::unique_ptr<AsrBackend> primary,
                                                  std::unique_ptr<AsrBackend> refine,
                                                  std::string* error) {
  if (!primary) {
    if (error) {
      *error = "Refining backend requires a primary backend.";
    }
    return nullptr;
  }
  if (error) {
    error->clear();
  }
  return std::make_unique<RefiningBackend>(std::move(primary), std::move(refine));
}

} // namespace vinput::daemon::asr
