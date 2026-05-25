#pragma once

#include <array>
#include <cstdint>

namespace qsr::quick_slots {

constexpr int kMaxQuickSlotCount = 8;

struct Options {
    int first_slot = 108;
    int slot_count = 1;
    std::uintptr_t game_state_global = 0;
};

struct RecordInfo {
    int record_index = -1;
    int slot = -1;
    std::uint64_t order_key = 0;
    const unsigned int* record = nullptr;
};

void Initialize(const Options& options);
int FirstSlot();
int SlotCount();
std::array<int, kMaxQuickSlotCount> Slots();
bool IsQuickSlot(int slot);
int SelectSaveSlot();
int SelectLoadSlot();
int FindRecordIndexBySlot(int slot);
bool TryGetRecordBySlot(int slot, RecordInfo& info);
int CollectExistingQuickSlotsNewestFirst(RecordInfo* out_records, int capacity);

}  // namespace qsr::quick_slots
