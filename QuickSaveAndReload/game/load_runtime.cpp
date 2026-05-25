#include "pch.h"

#include <MinHook.h>

#include "include/load_runtime.h"
#include "include/log.h"
#include "include/quick_slot_runtime.h"
#include "include/resolver.h"

namespace qsr::load_runtime {
namespace {

using InGameMenuLoadCoreFn = std::int32_t* (__fastcall*)(std::int64_t, std::int32_t*, std::int64_t*, unsigned int);
using BuildVisibleMapFn = std::int64_t(__fastcall*)(std::int64_t);
using LoadSelectedRefreshFn = std::int64_t(__fastcall*)(std::int64_t);
using LoadModalHandlerFn = void(__fastcall*)(std::uint64_t*, std::int64_t, char, unsigned int);

constexpr std::ptrdiff_t kRootOffsetListWidget = 0x150;
constexpr std::ptrdiff_t kRootOffsetModalSource = 0x198;
constexpr std::ptrdiff_t kRootOffsetActiveModal = 0x1B0;
constexpr std::ptrdiff_t kRootOffsetVisibleMap = 0x1C0;
constexpr std::ptrdiff_t kRootOffsetVisibleCount = 0x1C8;
constexpr std::ptrdiff_t kRootOffsetMode = 0x1E0;
constexpr std::ptrdiff_t kListWidgetSelectedRowOffset = 0x118;
constexpr std::uint32_t kMaxVisibleRows = 64;

InGameMenuLoadCoreFn g_in_game_menu_load_core_original = nullptr;
BuildVisibleMapFn g_build_visible_map_original = nullptr;
LoadSelectedRefreshFn g_load_selected_refresh_original = nullptr;
LoadModalHandlerFn g_load_modal_handler_original = nullptr;
std::uintptr_t g_in_game_menu_load_core_target = 0;
std::uintptr_t g_build_visible_map_target = 0;
std::uintptr_t g_load_selected_refresh_target = 0;
std::uintptr_t g_load_modal_handler_target = 0;
std::atomic<std::uintptr_t> g_last_core_self{0};
std::atomic<std::uint64_t> g_last_target_key{0};
std::atomic<std::uint32_t> g_last_slot{0};
std::atomic<std::uintptr_t> g_last_ui_root{0};
std::atomic<DWORD> g_last_thread_id{0};
std::atomic<bool> g_quick_load_confirm_modal_active{false};
std::atomic<std::uintptr_t> g_quick_load_confirm_root{0};
std::atomic<std::uintptr_t> g_quick_load_confirm_modal{0};
bool g_install_selected_refresh = true;

bool IsLikelyLoadUiRoot(std::uintptr_t root) {
    if (root == 0) {
        return false;
    }

    __try {
        const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(root);
        const std::uintptr_t list_widget = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetListWidget);
        const std::uintptr_t visible_map = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetVisibleMap);
        const std::uint32_t visible_count = *reinterpret_cast<const std::uint32_t*>(root + kRootOffsetVisibleCount);
        const std::uintptr_t active_modal = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetActiveModal);
        return vtable != 0
            && list_widget != 0
            && visible_map != 0
            && visible_count != 0
            && active_modal != root;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool BuildQuickLoadVisibleMap(std::uintptr_t root, int* rewritten_map, std::uint32_t visible_count) {
    if (rewritten_map == nullptr || visible_count == 0 || visible_count > kMaxVisibleRows) {
        return false;
    }

    const int selected_slot = quick_slots::SelectLoadSlot();
    const int quick_record_index = quick_slots::FindRecordIndexBySlot(selected_slot);
    if (quick_record_index < 0) {
        log::Write("[quick-load-confirm] unavailable reason=no-quick-record slot=%d\n", selected_slot);
        return false;
    }

    const auto* visible_map = reinterpret_cast<const int*>(*reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetVisibleMap));
    if (visible_map == nullptr) {
        return false;
    }

    std::uint32_t out_count = 0;
    rewritten_map[out_count++] = quick_record_index;

    bool skipped_quick = false;
    for (std::uint32_t row = 0; row < visible_count && out_count < visible_count; ++row) {
        const int record_index = visible_map[row];
        if (!skipped_quick && record_index == quick_record_index) {
            skipped_quick = true;
            continue;
        }
        rewritten_map[out_count++] = record_index;
    }

    while (out_count < visible_count) {
        rewritten_map[out_count++] = -1;
    }
    return true;
}

bool IsLoadStyleMode(std::uintptr_t root) {
    if (root == 0) {
        return false;
    }

    __try {
        return *reinterpret_cast<const std::uint8_t*>(root + kRootOffsetMode) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RewriteVisibleMapForReservedQuickSlot(std::uintptr_t root) {
    if (!IsLikelyLoadUiRoot(root)) {
        return false;
    }
    if (!IsLoadStyleMode(root)) {
        return false;
    }

    quick_slots::RecordInfo quick_records[quick_slots::kMaxQuickSlotCount] = {};
    const int quick_record_count = quick_slots::CollectExistingQuickSlotsNewestFirst(
        quick_records,
        static_cast<int>(std::size(quick_records)));
    if (quick_record_count <= 0) {
        return false;
    }

    __try {
        auto* visible_map = reinterpret_cast<int*>(*reinterpret_cast<std::uintptr_t*>(root + kRootOffsetVisibleMap));
        const std::uint32_t visible_count = *reinterpret_cast<const std::uint32_t*>(root + kRootOffsetVisibleCount);
        if (visible_map == nullptr || visible_count == 0 || visible_count > kMaxVisibleRows) {
            return false;
        }

        int rewritten_map[kMaxVisibleRows] = {};
        std::uint32_t out = 0;
        for (int i = 0; i < quick_record_count && out < visible_count; ++i) {
            rewritten_map[out++] = quick_records[i].record_index;
        }

        bool changed = false;
        for (std::uint32_t row = 0; row < visible_count && out < visible_count; ++row) {
            const int record_index = visible_map[row];
            bool is_quick_record = false;
            for (int i = 0; i < quick_record_count; ++i) {
                if (quick_records[i].record_index == record_index) {
                    is_quick_record = true;
                    break;
                }
            }
            if (is_quick_record) {
                changed = true;
                continue;
            }
            rewritten_map[out++] = record_index;
        }
        while (out < visible_count) {
            rewritten_map[out++] = -1;
        }

        for (std::uint32_t row = 0; row < visible_count; ++row) {
            if (visible_map[row] != rewritten_map[row]) {
                changed = true;
            }
            visible_map[row] = rewritten_map[row];
        }

        return changed;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("[load-ui] rewrite reserved quick slot failed root=%p code=0x%08lX\n",
            reinterpret_cast<void*>(root),
            static_cast<unsigned long>(GetExceptionCode()));
        return false;
    }
}

bool HideReservedQuickSlotFromSaveUi(std::uintptr_t root) {
    if (!IsLikelyLoadUiRoot(root) || IsLoadStyleMode(root)) {
        return false;
    }

    __try {
        auto* visible_map = reinterpret_cast<int*>(*reinterpret_cast<std::uintptr_t*>(root + kRootOffsetVisibleMap));
        auto* visible_count_ptr = reinterpret_cast<std::uint32_t*>(root + kRootOffsetVisibleCount);
        if (visible_map == nullptr || visible_count_ptr == nullptr || *visible_count_ptr == 0 || *visible_count_ptr > kMaxVisibleRows) {
            return false;
        }

        const std::uint32_t visible_before = *visible_count_ptr;
        const auto slots = quick_slots::Slots();
        std::uint32_t out = 0;
        int removed = 0;
        for (std::uint32_t row = 0; row < visible_before; ++row) {
            const int record_index = visible_map[row];
            bool is_reserved_manual_row = false;
            for (int i = 0; i < quick_slots::SlotCount(); ++i) {
                const int manual_index = slots[static_cast<std::size_t>(i)] - 100;
                if (manual_index >= 0 && record_index == manual_index) {
                    is_reserved_manual_row = true;
                    break;
                }
            }
            if (is_reserved_manual_row) {
                ++removed;
                continue;
            }
            visible_map[out++] = record_index;
        }

        if (removed == 0) {
            return false;
        }

        for (std::uint32_t row = out; row < visible_before; ++row) {
            visible_map[row] = -1;
        }
        *visible_count_ptr = out;

        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("[load-ui] hide reserved quick slot failed root=%p code=0x%08lX\n",
            reinterpret_cast<void*>(root),
            static_cast<unsigned long>(GetExceptionCode()));
        return false;
    }
}

std::int32_t* __fastcall InGameMenuLoadCoreHook(
    std::int64_t self,
    std::int32_t* out_result,
    std::int64_t* target_key,
    unsigned int slot) {
    const DWORD thread_id = GetCurrentThreadId();
    const std::uint64_t key = target_key != nullptr ? static_cast<std::uint64_t>(*target_key) : 0;
    g_last_core_self.store(static_cast<std::uintptr_t>(self));
    g_last_target_key.store(key);
    g_last_slot.store(slot);
    g_last_thread_id.store(thread_id);

    std::int32_t* result = g_in_game_menu_load_core_original(self, out_result, target_key, slot);
    const std::int32_t after = out_result != nullptr ? *out_result : -1;

    if (quick_slots::IsQuickSlot(static_cast<int>(slot)) && g_quick_load_confirm_modal_active.exchange(false)) {
        log::Write("[quick-load-confirm] accepted source=load-core root=%p modal=%p slot=%d out=%d success=%d\n",
            reinterpret_cast<void*>(g_quick_load_confirm_root.load()),
            reinterpret_cast<void*>(g_quick_load_confirm_modal.load()),
            static_cast<int>(slot),
            after,
            after == 0 ? 1 : 0);
        g_quick_load_confirm_root.store(0);
        g_quick_load_confirm_modal.store(0);
    }
    return result;
}

std::int64_t __fastcall BuildVisibleMapHook(std::int64_t root) {
    std::int64_t result = 0;
    if (g_build_visible_map_original != nullptr) {
        result = g_build_visible_map_original(root);
    }
    g_last_ui_root.store(static_cast<std::uintptr_t>(root));
    const std::uintptr_t root_u = static_cast<std::uintptr_t>(root);
    RewriteVisibleMapForReservedQuickSlot(root_u);
    HideReservedQuickSlotFromSaveUi(root_u);
    return result;
}

std::int64_t __fastcall LoadSelectedRefreshHook(std::int64_t root) {
    g_last_ui_root.store(static_cast<std::uintptr_t>(root));
    return g_load_selected_refresh_original(root);
}

void __fastcall LoadModalHandlerHook(std::uint64_t* self, std::int64_t source, char accepted, unsigned int arg4) {
    const std::uintptr_t root = reinterpret_cast<std::uintptr_t>(self);
    std::uintptr_t expected_source = 0;
    __try {
        expected_source = root != 0 ? *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetModalSource) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        expected_source = 0;
    }

    const bool is_quick_load_modal =
        g_quick_load_confirm_modal_active.load()
        && root != 0
        && root == g_quick_load_confirm_root.load()
        && expected_source != 0
        && static_cast<std::uintptr_t>(source) == expected_source;

    if (is_quick_load_modal) {
        log::Write("[quick-load-confirm] resolved accepted=%d root=%p modal=%p source=%p arg4=%u\n",
            accepted ? 1 : 0,
            reinterpret_cast<void*>(root),
            reinterpret_cast<void*>(g_quick_load_confirm_modal.load()),
            reinterpret_cast<void*>(source),
            arg4);
        if (!accepted) {
            g_quick_load_confirm_modal_active.store(false);
            g_quick_load_confirm_root.store(0);
            g_quick_load_confirm_modal.store(0);
        }
    }

    if (g_load_modal_handler_original != nullptr) {
        g_load_modal_handler_original(self, source, accepted, arg4);
    }
}

bool CreateHookRaw(const char* name, std::uintptr_t target, LPVOID detour, LPVOID* original) {
    const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, original);
    if (status != MH_OK) {
        log::Write("[load-runtime] MH_CreateHook failed name=%s status=%d target=%p\n",
            name,
            static_cast<int>(status),
            reinterpret_cast<void*>(target));
        return false;
    }
    return true;
}

bool EnableHookRaw(const char* name, std::uintptr_t target) {
    const MH_STATUS status = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (status != MH_OK) {
        log::Write("[load-runtime] MH_EnableHook failed name=%s status=%d target=%p\n",
            name,
            static_cast<int>(status),
            reinterpret_cast<void*>(target));
        return false;
    }
    return true;
}

}  // namespace

bool Install(const Options& options) {
    g_in_game_menu_load_core_target = resolver::ValidatedAddress(symbols::SymbolId::InGameMenuLoadCore);
    g_build_visible_map_target = resolver::ValidatedAddress(symbols::SymbolId::BuildVisibleMap);
    g_load_selected_refresh_target = options.install_selected_refresh
        ? resolver::ValidatedAddress(symbols::SymbolId::LoadSelectedRefresh)
        : 0;
    g_load_modal_handler_target = resolver::ValidatedAddress(symbols::SymbolId::LoadModalHandler);
    if (g_in_game_menu_load_core_target == 0 || (options.install_selected_refresh && g_load_selected_refresh_target == 0)) {
        log::Write("[load-runtime] install blocked; requested load target is not validated\n");
        g_in_game_menu_load_core_target = 0;
        g_build_visible_map_target = 0;
        g_load_selected_refresh_target = 0;
        g_load_modal_handler_target = 0;
        return false;
    }

    g_last_core_self.store(0);
    g_last_target_key.store(0);
    g_last_slot.store(0);
    g_last_ui_root.store(0);
    g_last_thread_id.store(0);
    g_quick_load_confirm_modal_active.store(false);
    g_quick_load_confirm_root.store(0);
    g_quick_load_confirm_modal.store(0);
    g_install_selected_refresh = options.install_selected_refresh;

    if (!CreateHookRaw(
            "InGameMenuLoadCore",
            g_in_game_menu_load_core_target,
            reinterpret_cast<LPVOID>(&InGameMenuLoadCoreHook),
            reinterpret_cast<LPVOID*>(&g_in_game_menu_load_core_original))) {
        g_in_game_menu_load_core_target = 0;
        g_build_visible_map_target = 0;
        g_load_selected_refresh_target = 0;
        return false;
    }
    if (g_build_visible_map_target != 0
        && !CreateHookRaw(
            "BuildVisibleMap",
            g_build_visible_map_target,
            reinterpret_cast<LPVOID>(&BuildVisibleMapHook),
            reinterpret_cast<LPVOID*>(&g_build_visible_map_original))) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_in_game_menu_load_core_target));
        g_in_game_menu_load_core_original = nullptr;
        g_in_game_menu_load_core_target = 0;
        g_build_visible_map_target = 0;
        g_load_selected_refresh_target = 0;
        g_load_modal_handler_target = 0;
        return false;
    }
    if (g_install_selected_refresh
        && !CreateHookRaw(
            "LoadSelectedRefresh",
            g_load_selected_refresh_target,
            reinterpret_cast<LPVOID>(&LoadSelectedRefreshHook),
            reinterpret_cast<LPVOID*>(&g_load_selected_refresh_original))) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_in_game_menu_load_core_target));
        if (g_build_visible_map_target != 0) {
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_build_visible_map_target));
        }
        g_in_game_menu_load_core_original = nullptr;
        g_build_visible_map_original = nullptr;
        g_in_game_menu_load_core_target = 0;
        g_build_visible_map_target = 0;
        g_load_selected_refresh_target = 0;
        g_load_modal_handler_target = 0;
        return false;
    }
    if (g_load_modal_handler_target != 0
        && !CreateHookRaw(
            "LoadModalHandler",
            g_load_modal_handler_target,
            reinterpret_cast<LPVOID>(&LoadModalHandlerHook),
            reinterpret_cast<LPVOID*>(&g_load_modal_handler_original))) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_in_game_menu_load_core_target));
        if (g_build_visible_map_target != 0) {
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_build_visible_map_target));
        }
        if (g_load_selected_refresh_target != 0) {
            MH_RemoveHook(reinterpret_cast<LPVOID>(g_load_selected_refresh_target));
        }
        g_in_game_menu_load_core_original = nullptr;
        g_build_visible_map_original = nullptr;
        g_load_selected_refresh_original = nullptr;
        g_load_modal_handler_original = nullptr;
        g_in_game_menu_load_core_target = 0;
        g_build_visible_map_target = 0;
        g_load_selected_refresh_target = 0;
        g_load_modal_handler_target = 0;
        return false;
    }

    if (!EnableHookRaw("InGameMenuLoadCore", g_in_game_menu_load_core_target)
        || (g_build_visible_map_target != 0 && !EnableHookRaw("BuildVisibleMap", g_build_visible_map_target))
        || (g_install_selected_refresh && !EnableHookRaw("LoadSelectedRefresh", g_load_selected_refresh_target))
        || (g_load_modal_handler_target != 0 && !EnableHookRaw("LoadModalHandler", g_load_modal_handler_target))) {
        Shutdown();
        return false;
    }

    return true;
}

bool LatestCapture(CapturedContext& capture) {
    capture.core_self = g_last_core_self.load();
    capture.target_key = g_last_target_key.load();
    capture.slot = g_last_slot.load();
    capture.ui_root = g_last_ui_root.load();
    capture.thread_id = g_last_thread_id.load();
    return capture.core_self != 0 || capture.ui_root != 0;
}

bool TryOpenQuickLoadConfirmationModal(const char* source) {
    if (g_load_selected_refresh_original == nullptr) {
        log::Write("[quick-load-confirm] unavailable reason=no-selected-refresh source=%s\n",
            source != nullptr ? source : "<null>");
        return false;
    }

    const std::uintptr_t root = g_last_ui_root.load();
    if (!IsLikelyLoadUiRoot(root)) {
        log::Write("[quick-load-confirm] unavailable reason=no-valid-cached-root root=%p source=%s\n",
            reinterpret_cast<void*>(root),
            source != nullptr ? source : "<null>");
        return false;
    }

    __try {
        const std::uintptr_t list_widget = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetListWidget);
        if (list_widget == 0) {
            log::Write("[quick-load-confirm] unavailable reason=no-list-widget root=%p source=%s\n",
                reinterpret_cast<void*>(root),
                source != nullptr ? source : "<null>");
            return false;
        }

        const std::uint32_t visible_count = *reinterpret_cast<const std::uint32_t*>(root + kRootOffsetVisibleCount);
        auto* visible_map = reinterpret_cast<int*>(*reinterpret_cast<std::uintptr_t*>(root + kRootOffsetVisibleMap));
        if (visible_count == 0 || visible_map == nullptr) {
            log::Write("[quick-load-confirm] unavailable reason=no-visible-map root=%p visible_count=%u source=%s\n",
                reinterpret_cast<void*>(root),
                visible_count,
                source != nullptr ? source : "<null>");
            return false;
        }
        if (visible_count > kMaxVisibleRows) {
            log::Write("[quick-load-confirm] unavailable reason=too-many-visible-rows root=%p visible_count=%u source=%s\n",
                reinterpret_cast<void*>(root),
                visible_count,
                source != nullptr ? source : "<null>");
            return false;
        }

        int original_map[kMaxVisibleRows] = {};
        int rewritten_map[kMaxVisibleRows] = {};
        for (std::uint32_t row = 0; row < visible_count; ++row) {
            original_map[row] = visible_map[row];
        }
        if (!BuildQuickLoadVisibleMap(root, rewritten_map, visible_count)) {
            return false;
        }

        const std::uint32_t previous_row = *reinterpret_cast<const std::uint32_t*>(list_widget + kListWidgetSelectedRowOffset);
        const std::uintptr_t previous_modal = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetActiveModal);
        for (std::uint32_t row = 0; row < visible_count; ++row) {
            visible_map[row] = rewritten_map[row];
        }
        *reinterpret_cast<std::uint32_t*>(list_widget + kListWidgetSelectedRowOffset) = 0;
        const std::int64_t refresh_result = g_load_selected_refresh_original(static_cast<std::int64_t>(root));
        const std::uintptr_t new_modal = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetActiveModal);
        *reinterpret_cast<std::uint32_t*>(list_widget + kListWidgetSelectedRowOffset) = previous_row;
        for (std::uint32_t row = 0; row < visible_count; ++row) {
            visible_map[row] = original_map[row];
        }

        const bool opened = new_modal != 0 && new_modal != previous_modal;
        if (opened) {
            g_quick_load_confirm_modal_active.store(true);
            g_quick_load_confirm_root.store(root);
            g_quick_load_confirm_modal.store(new_modal);
        }
        log::Write("[quick-load-confirm] source=%s root=%p quick_record=%d previous_row=%u refresh_result=%p previous_modal=%p new_modal=%p opened=%d\n",
            source != nullptr ? source : "<null>",
            reinterpret_cast<void*>(root),
            rewritten_map[0],
            previous_row,
            reinterpret_cast<void*>(refresh_result),
            reinterpret_cast<void*>(previous_modal),
            reinterpret_cast<void*>(new_modal),
            opened ? 1 : 0);
        return opened;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("[quick-load-confirm] unavailable reason=exception code=0x%08lX root=%p source=%s\n",
            static_cast<unsigned long>(GetExceptionCode()),
            reinterpret_cast<void*>(root),
            source != nullptr ? source : "<null>");
        return false;
    }
}

bool InvokeQuickLoadFromCachedContext(const char* source) {
    if (g_in_game_menu_load_core_original == nullptr) {
        log::Write("[quick-load] unavailable reason=no-load-core source=%s\n",
            source != nullptr ? source : "<null>");
        return false;
    }

    const std::uintptr_t self = g_last_core_self.load();
    std::uint64_t key = g_last_target_key.load();
    if (key == 0) {
        log::Write("[quick-load] cached target key missing; using sentinel source=%s\n",
            source != nullptr ? source : "<null>");
        key = 0x7FFFFFFFFFFFFFFFui64;
    }
    if (self == 0) {
        log::Write("[quick-load] unavailable reason=no-cached-core-context source=%s\n",
            source != nullptr ? source : "<null>");
        return false;
    }

    std::int32_t out_result = 0;
    auto target_key = static_cast<std::int64_t>(key);
    const int selected_slot = quick_slots::SelectLoadSlot();
    std::int32_t* result = nullptr;
    __try {
        result = g_in_game_menu_load_core_original(
            static_cast<std::int64_t>(self),
            &out_result,
            &target_key,
            static_cast<unsigned int>(selected_slot));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("[quick-load] source=%s exception=0x%08lX self=%p key=0x%llX slot=%d\n",
            source != nullptr ? source : "<null>",
            static_cast<unsigned long>(GetExceptionCode()),
            reinterpret_cast<void*>(self),
            static_cast<unsigned long long>(key),
            selected_slot);
        return false;
    }

    const bool success = result != nullptr && out_result == 0;
    log::Write("[quick-load] source=%s path=cached-in-game-core self=%p key=0x%llX slot=%d out=%d success=%d result=%p tid=%lu\n",
        source != nullptr ? source : "<null>",
        reinterpret_cast<void*>(self),
        static_cast<unsigned long long>(key),
        selected_slot,
        out_result,
        success ? 1 : 0,
        reinterpret_cast<void*>(result),
        static_cast<unsigned long>(GetCurrentThreadId()));
    return success;
}

void Shutdown() {
    if (g_load_modal_handler_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_load_modal_handler_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_load_modal_handler_target));
    }
    if (g_build_visible_map_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_build_visible_map_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_build_visible_map_target));
    }
    if (g_load_selected_refresh_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_load_selected_refresh_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_load_selected_refresh_target));
    }
    if (g_in_game_menu_load_core_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_in_game_menu_load_core_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_in_game_menu_load_core_target));
    }
    g_in_game_menu_load_core_original = nullptr;
    g_build_visible_map_original = nullptr;
    g_load_selected_refresh_original = nullptr;
    g_load_modal_handler_original = nullptr;
    g_in_game_menu_load_core_target = 0;
    g_build_visible_map_target = 0;
    g_load_selected_refresh_target = 0;
    g_load_modal_handler_target = 0;
    g_install_selected_refresh = true;
    g_last_core_self.store(0);
    g_last_target_key.store(0);
    g_last_slot.store(0);
    g_last_ui_root.store(0);
    g_last_thread_id.store(0);
    g_quick_load_confirm_modal_active.store(false);
    g_quick_load_confirm_root.store(0);
    g_quick_load_confirm_modal.store(0);
}

}  // namespace qsr::load_runtime
