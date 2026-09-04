#include "pch.h"

#include "include/log.h"
#include "include/quick_slot_runtime.h"

namespace qsr::quick_slots {
namespace {

constexpr std::size_t kSaveRecordSize = 0x68;
constexpr std::ptrdiff_t kGameStateSaveCatalogOffset = 0xB8;

int g_first_slot = 108;
int g_slot_count = 1;
std::uintptr_t g_game_state_global = 0;
std::array<std::uint64_t, kMaxQuickSlotCount> g_fallback_order{};
std::uint64_t g_next_fallback_order = 1;

int ClampSlotCount(int value) {
    if (value < 1) {
        return 1;
    }
    if (value > kMaxQuickSlotCount) {
        return kMaxQuickSlotCount;
    }
    return value;
}

std::uint64_t GetSaveCatalog() {
    if (g_game_state_global == 0) {
        return 0;
    }

    __try {
        const auto* game_state_global = reinterpret_cast<const std::uint64_t*>(g_game_state_global);
        const std::uint64_t game_state = *game_state_global;
        return game_state != 0
            ? *reinterpret_cast<const std::uint64_t*>(game_state + kGameStateSaveCatalogOffset)
            : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool TryGetRecordByIndex(int index, RecordInfo& info) {
    info = {};
    const std::uint64_t catalog = GetSaveCatalog();
    if (catalog == 0 || index < 0) {
        return false;
    }

    __try {
        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(catalog + 0x48);
        if (static_cast<std::uint32_t>(index) >= count) {
            return false;
        }

        const auto* records = reinterpret_cast<const std::uint8_t*>(*reinterpret_cast<const std::uint64_t*>(catalog + 0x40));
        if (records == nullptr) {
            return false;
        }

        const auto* record = reinterpret_cast<const unsigned int*>(records + (static_cast<std::size_t>(index) * kSaveRecordSize));
        info.record_index = index;
        info.slot = static_cast<int>(record[0]);
        info.order_key = *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(record) + 0x8);
        info.record = record;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        info = {};
        return false;
    }
}

int SlotToFallbackIndex(int slot) {
    if (!IsQuickSlot(slot)) {
        return -1;
    }
    return g_first_slot - slot;
}

bool HasSaveCatalog() {
    return GetSaveCatalog() != 0;
}

}  // namespace

void Initialize(const Options& options) {
    g_first_slot = options.first_slot > 0 ? options.first_slot : 108;
    g_slot_count = ClampSlotCount(options.slot_count);
    g_game_state_global = options.game_state_global;
    g_fallback_order.fill(0);
    g_next_fallback_order = 1;
    log::Write("[quick-slots] configured first_slot=%d count=%d last_slot=%d game_state_global=%p\n",
        g_first_slot,
        g_slot_count,
        g_first_slot - g_slot_count + 1,
        reinterpret_cast<void*>(g_game_state_global));
}

int FirstSlot() {
    return g_first_slot;
}

int SlotCount() {
    return g_slot_count;
}

std::array<int, kMaxQuickSlotCount> Slots() {
    std::array<int, kMaxQuickSlotCount> slots{};
    for (int i = 0; i < g_slot_count; ++i) {
        slots[static_cast<std::size_t>(i)] = g_first_slot - i;
    }
    return slots;
}

bool IsQuickSlot(int slot) {
    return slot <= g_first_slot && slot > g_first_slot - g_slot_count;
}

int FindRecordIndexBySlot(int slot) {
    const std::uint64_t catalog = GetSaveCatalog();
    if (catalog == 0) {
        return -1;
    }

    __try {
        const std::uint32_t count = *reinterpret_cast<const std::uint32_t*>(catalog + 0x48);
        const auto* records = reinterpret_cast<const std::uint8_t*>(*reinterpret_cast<const std::uint64_t*>(catalog + 0x40));
        if (records == nullptr) {
            return -1;
        }

        for (std::uint32_t index = 0; index < count; ++index) {
            const auto* record = reinterpret_cast<const unsigned int*>(records + (static_cast<std::size_t>(index) * kSaveRecordSize));
            if (static_cast<int>(record[0]) == slot) {
                return static_cast<int>(index);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }

    return -1;
}

bool TryGetRecordBySlot(int slot, RecordInfo& info) {
    const int index = FindRecordIndexBySlot(slot);
    return TryGetRecordByIndex(index, info);
}

int CollectExistingQuickSlotsNewestFirst(RecordInfo* out_records, int capacity) {
    if (out_records == nullptr || capacity <= 0) {
        return 0;
    }

    int count = 0;
    const auto slots = Slots();
    for (int i = 0; i < g_slot_count && count < capacity; ++i) {
        RecordInfo info{};
        if (TryGetRecordBySlot(slots[static_cast<std::size_t>(i)], info)) {
            out_records[count++] = info;
        }
    }

    std::sort(out_records, out_records + count, [](const RecordInfo& lhs, const RecordInfo& rhs) {
        if (lhs.order_key != rhs.order_key) {
            return lhs.order_key > rhs.order_key;
        }
        return lhs.slot > rhs.slot;
    });
    return count;
}

int SelectSaveSlot() {
    const auto slots = Slots();
    if (!HasSaveCatalog()) {
        int oldest_slot = g_first_slot;
        std::uint64_t oldest_order = UINT64_MAX;
        for (int i = 0; i < g_slot_count; ++i) {
            const int slot = slots[static_cast<std::size_t>(i)];
            const std::uint64_t order = g_fallback_order[static_cast<std::size_t>(i)];
            if (order == 0) {
                log::Write("[quick-slots] selected save slot=%d reason=fallback-empty\n", slot);
                return slot;
            }
            if (order < oldest_order) {
                oldest_order = order;
                oldest_slot = slot;
            }
        }

        log::Write("[quick-slots] selected save slot=%d reason=fallback-oldest order=0x%llX\n",
            oldest_slot,
            static_cast<unsigned long long>(oldest_order));
        return oldest_slot;
    }

    RecordInfo oldest{};
    bool have_oldest = false;
    for (int i = 0; i < g_slot_count; ++i) {
        const int slot = slots[static_cast<std::size_t>(i)];
        RecordInfo info{};
        if (!TryGetRecordBySlot(slot, info)) {
            log::Write("[quick-slots] selected save slot=%d reason=empty\n", slot);
            return slot;
        }

        if (!have_oldest
            || info.order_key < oldest.order_key
            || (info.order_key == oldest.order_key && info.slot < oldest.slot)) {
            oldest = info;
            have_oldest = true;
        }
    }

    const int selected = have_oldest ? oldest.slot : g_first_slot;
    log::Write("[quick-slots] selected save slot=%d reason=oldest record=%d order=0x%llX\n",
        selected,
        oldest.record_index,
        static_cast<unsigned long long>(oldest.order_key));
    return selected;
}

int SelectLoadSlot() {
    RecordInfo newest{};
    bool have_newest = false;
    const auto slots = Slots();
    for (int i = 0; i < g_slot_count; ++i) {
        RecordInfo info{};
        if (!TryGetRecordBySlot(slots[static_cast<std::size_t>(i)], info)) {
            continue;
        }

        if (!have_newest
            || info.order_key > newest.order_key
            || (info.order_key == newest.order_key && info.slot > newest.slot)) {
            newest = info;
            have_newest = true;
        }
    }

    if (!have_newest) {
        int newest_slot = g_first_slot;
        std::uint64_t newest_order = 0;
        for (int i = 0; i < g_slot_count; ++i) {
            const std::uint64_t order = g_fallback_order[static_cast<std::size_t>(i)];
            if (order > newest_order) {
                newest_order = order;
                newest_slot = slots[static_cast<std::size_t>(i)];
            }
        }
        if (newest_order != 0) {
            log::Write("[quick-slots] selected load slot=%d reason=fallback-newest order=0x%llX\n",
                newest_slot,
                static_cast<unsigned long long>(newest_order));
            return newest_slot;
        }

        log::Write("[quick-slots] selected load slot=%d reason=no-existing-quick-save\n", g_first_slot);
        return g_first_slot;
    }

    log::Write("[quick-slots] selected load slot=%d reason=newest record=%d order=0x%llX\n",
        newest.slot,
        newest.record_index,
        static_cast<unsigned long long>(newest.order_key));
    return newest.slot;
}

void MarkQuickSaveSucceeded(int slot) {
    const int index = SlotToFallbackIndex(slot);
    if (index < 0 || index >= g_slot_count) {
        return;
    }

    g_fallback_order[static_cast<std::size_t>(index)] = g_next_fallback_order++;
    if (g_next_fallback_order == 0) {
        g_next_fallback_order = 1;
    }
    log::Write("[quick-slots] marked saved slot=%d fallback_order=0x%llX\n",
        slot,
        static_cast<unsigned long long>(g_fallback_order[static_cast<std::size_t>(index)]));
}

}  // namespace qsr::quick_slots
