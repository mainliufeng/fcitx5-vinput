#include "daemon/asr/backends/refining_backend.h"

#include <cstdint>
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
  RefiningSession(std::unique_ptr<RecognitionSession> primary,
                  std::unique_ptr<RecognitionSession> refine)
      : primary_(std::move(primary)), refine_(std::move(refine)) {}

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

    if (refine_ && !utterance_.empty()) {
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
    if (refine_) {
      refine_->Cancel();
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
    std::string refine_error;
    if (!refine_->PushAudio(utterance_, &refine_error) || !refine_->Finish(&refine_error)) {
      debug::Log("vinput: second pass skipped: %s\n", refine_error.c_str());
      return;
    }

    bool refined = false;
    for (auto& event : refine_->PollEvents()) {
      if (event.kind == RecognitionEventKind::FinalText && !event.text.empty()) {
        refined = true;
        events_.push_back(std::move(event));
      }
    }
    if (refined) {
      debug::Log("vinput: second pass replaced the streaming result samples=%zu\n",
                 utterance_.size());
    }
  }

  std::unique_ptr<RecognitionSession> primary_;
  std::unique_ptr<RecognitionSession> refine_;
  std::vector<int16_t> utterance_;
  std::vector<RecognitionEvent> events_;
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

    std::unique_ptr<RecognitionSession> refine_session;
    if (refine_) {
      std::string refine_error;
      refine_session = refine_->CreateSession(&refine_error);
      if (!refine_session) {
        // Degrade to the streaming-only behaviour rather than failing the
        // session: the user still gets text.
        debug::Log("vinput: second pass unavailable, continuing without it: %s\n",
                   refine_error.c_str());
      }
    }

    if (error) {
      error->clear();
    }
    return std::make_unique<RefiningSession>(std::move(primary_session), std::move(refine_session));
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
