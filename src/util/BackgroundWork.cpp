#include "BackgroundWork.h"

#include <I18n.h>

#include <atomic>

namespace BackgroundWork {
namespace {

struct Slot {
  std::atomic<uint8_t> depth{0};
  std::atomic<uint8_t> sequence{0};
};

Slot slots[static_cast<int>(Kind::Count)];
std::atomic<uint8_t> nextSequence{1};
std::atomic<uint32_t> stamp{0};

StrId labelId(const Kind kind) {
  switch (kind) {
    case Kind::FetchComments:
      return StrId::STR_BUSY_FETCH_COMMENTS;
    case Kind::Count:
      break;
  }
  return StrId::STR_BUSY_FETCH_COMMENTS;
}

}  // namespace

void begin(const Kind kind) {
  if (kind >= Kind::Count) return;
  Slot& slot = slots[static_cast<int>(kind)];
  if (slot.depth.fetch_add(1) == 0) {
    slot.sequence.store(nextSequence.fetch_add(1));
    stamp.fetch_add(1);
  }
}

void end(const Kind kind) {
  if (kind >= Kind::Count) return;
  Slot& slot = slots[static_cast<int>(kind)];
  uint8_t prev = slot.depth.load();
  while (prev > 0 && !slot.depth.compare_exchange_weak(prev, static_cast<uint8_t>(prev - 1))) {
  }
  if (prev == 1) stamp.fetch_add(1);
}

uint32_t generation() { return stamp.load(); }

const char* foregroundLabel() {
  const char* labels[kMax];
  if (copyLabels(labels, 1) == 0) return nullptr;
  return labels[0];
}

int copyLabels(const char** out, const int capacity) {
  if (!out || capacity <= 0) return 0;
  bool used[kMax] = {};
  int count = 0;
  while (count < capacity) {
    uint8_t bestSequence = 0;
    int best = -1;
    for (int i = 0; i < kMax; ++i) {
      if (used[i] || slots[i].depth.load() == 0) continue;
      const uint8_t sequence = slots[i].sequence.load();
      if (best < 0 || sequence > bestSequence) {
        best = i;
        bestSequence = sequence;
      }
    }
    if (best < 0) break;
    used[best] = true;
    out[count++] = I18N.get(labelId(static_cast<Kind>(best)));
  }
  return count;
}

}  // namespace BackgroundWork
