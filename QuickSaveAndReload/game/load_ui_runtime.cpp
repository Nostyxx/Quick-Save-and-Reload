#include "pch.h"

#include <MinHook.h>

#include "include/load_ui_runtime.h"
#include "include/log.h"
#include "include/quick_slot_runtime.h"
#include "include/resolver.h"
#include "include/text_runtime.h"

namespace qsr::load_ui_runtime {
namespace {

using RenderSlotRowFn = std::int64_t(__fastcall*)(std::uint64_t*, unsigned int*, char, char);
using SetControlTextFn = std::int64_t(__fastcall*)(std::uint64_t, const char*);

constexpr std::ptrdiff_t kRowOffsetTypeText = 0x360;
constexpr std::ptrdiff_t kRowOffsetIndexName = 0x368;

RenderSlotRowFn g_render_slot_row_original = nullptr;
SetControlTextFn g_set_control_text = nullptr;
std::uintptr_t g_render_slot_row_target = 0;

bool IsQuickRecord(const unsigned int* record) {
    if (record == nullptr) {
        return false;
    }

    __try {
        return quick_slots::IsQuickSlot(static_cast<int>(record[0]));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SetRowControlText(std::uint64_t row_script, std::ptrdiff_t offset, const char* text) {
    if (row_script == 0 || text == nullptr || g_set_control_text == nullptr) {
        return;
    }

    __try {
        const std::uint64_t control = *reinterpret_cast<const std::uint64_t*>(row_script + offset);
        if (control != 0) {
            g_set_control_text(control, text);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("[load-ui] set row text failed row_script=%p offset=0x%zX code=0x%08lX\n",
            reinterpret_cast<void*>(row_script),
            static_cast<std::size_t>(offset),
            static_cast<unsigned long>(GetExceptionCode()));
    }
}

std::int64_t __fastcall RenderSlotRowHook(std::uint64_t* row_script, unsigned int* record, char flag, char flag2) {
    if (g_render_slot_row_original == nullptr || !IsQuickRecord(record)) {
        return g_render_slot_row_original != nullptr
            ? g_render_slot_row_original(row_script, record, flag, flag2)
            : 0;
    }

    const std::int64_t result = g_render_slot_row_original(row_script, record, flag, flag2);
    const std::uint64_t row_script_u64 = reinterpret_cast<std::uint64_t>(row_script);
    SetRowControlText(row_script_u64, kRowOffsetTypeText, text::Get(text::TextId::UiRowLabel));
    SetRowControlText(row_script_u64, kRowOffsetIndexName, "");

    return result;
}

}  // namespace

bool Install(const Options& options) {
    UNREFERENCED_PARAMETER(options);
    g_render_slot_row_target = resolver::ValidatedAddress(symbols::SymbolId::RenderSlotRow);
    g_set_control_text = reinterpret_cast<SetControlTextFn>(
        resolver::ValidatedAddress(symbols::SymbolId::SetControlText));
    if (g_render_slot_row_target == 0 || g_set_control_text == nullptr) {
        log::Write("[load-ui] install blocked; row UI targets are not validated\n");
        g_render_slot_row_target = 0;
        g_set_control_text = nullptr;
        return false;
    }

    const MH_STATUS create_status = MH_CreateHook(
        reinterpret_cast<LPVOID>(g_render_slot_row_target),
        reinterpret_cast<LPVOID>(&RenderSlotRowHook),
        reinterpret_cast<LPVOID*>(&g_render_slot_row_original));
    if (create_status != MH_OK) {
        log::Write("[load-ui] MH_CreateHook failed name=RenderSlotRow status=%d target=%p\n",
            static_cast<int>(create_status),
            reinterpret_cast<void*>(g_render_slot_row_target));
        g_render_slot_row_target = 0;
        g_set_control_text = nullptr;
        return false;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<LPVOID>(g_render_slot_row_target));
    if (enable_status != MH_OK) {
        log::Write("[load-ui] MH_EnableHook failed name=RenderSlotRow status=%d target=%p\n",
            static_cast<int>(enable_status),
            reinterpret_cast<void*>(g_render_slot_row_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_render_slot_row_target));
        g_render_slot_row_original = nullptr;
        g_render_slot_row_target = 0;
        g_set_control_text = nullptr;
        return false;
    }

    return true;
}

void Shutdown() {
    if (g_render_slot_row_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_render_slot_row_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_render_slot_row_target));
    }
    g_render_slot_row_original = nullptr;
    g_set_control_text = nullptr;
    g_render_slot_row_target = 0;
}

}  // namespace qsr::load_ui_runtime
