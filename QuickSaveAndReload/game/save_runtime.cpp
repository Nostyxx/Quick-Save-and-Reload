#include "pch.h"

#include <MinHook.h>

#include "include/log.h"
#include "include/resolver.h"
#include "include/save_runtime.h"
#include "include/text_runtime.h"
#include "include/toast_runtime.h"

namespace qsr::save_runtime {
namespace {

using DirectLocalSaveFn = int* (__fastcall*)(std::int64_t, int*, int, std::uintptr_t);

DirectLocalSaveFn g_original = nullptr;
std::uintptr_t g_target = 0;
std::atomic<std::uintptr_t> g_last_actor{0};
std::atomic<std::uintptr_t> g_last_context{0};
std::atomic<DWORD> g_last_thread_id{0};
std::atomic<int> g_last_slot{0};

struct DirectSaveCallScratch {
    int flags_word = 0;
    int flags_pad = 0;
    int out_result = 0;
};

int* __fastcall DirectLocalSaveHook(
    std::int64_t actor,
    int* out_result,
    int slot,
    std::uintptr_t context) {
    g_last_actor.store(static_cast<std::uintptr_t>(actor));
    g_last_context.store(context);
    g_last_thread_id.store(GetCurrentThreadId());
    g_last_slot.store(slot);
    return g_original(actor, out_result, slot, context);
}

}  // namespace

bool Install(const Options& options) {
    UNREFERENCED_PARAMETER(options);
    g_target = resolver::ValidatedAddress(symbols::SymbolId::DirectLocalSave);
    if (g_target == 0) {
        log::Write("[save-runtime] install blocked; DirectLocalSave is not validated\n");
        return false;
    }

    g_last_actor.store(0);
    g_last_context.store(0);
    g_last_thread_id.store(0);
    g_last_slot.store(0);

    const MH_STATUS create_status = MH_CreateHook(
        reinterpret_cast<LPVOID>(g_target),
        reinterpret_cast<LPVOID>(&DirectLocalSaveHook),
        reinterpret_cast<LPVOID*>(&g_original));
    if (create_status != MH_OK) {
        log::Write("[save-runtime] MH_CreateHook failed status=%d target=%p\n",
            static_cast<int>(create_status),
            reinterpret_cast<void*>(g_target));
        g_target = 0;
        return false;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<LPVOID>(g_target));
    if (enable_status != MH_OK) {
        log::Write("[save-runtime] MH_EnableHook failed status=%d target=%p\n",
            static_cast<int>(enable_status),
            reinterpret_cast<void*>(g_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_target));
        g_original = nullptr;
        g_target = 0;
        return false;
    }

    return true;
}

bool LatestCapture(CapturedCall& capture) {
    capture.actor = g_last_actor.load();
    capture.context = g_last_context.load();
    capture.thread_id = g_last_thread_id.load();
    capture.slot = g_last_slot.load();
    return capture.actor != 0 && capture.context != 0 && capture.thread_id != 0;
}

bool InvokeQuickSave(std::uintptr_t actor, int slot, const char* source) {
    if (g_original == nullptr || actor == 0 || slot < 0) {
        log::Write("[quick-save] source=%s unavailable actor=%p slot=%d original=%p\n",
            source != nullptr ? source : "<null>",
            reinterpret_cast<void*>(actor),
            slot,
            reinterpret_cast<void*>(g_original));
        toast::Show(actor == 0 ? text::TextId::ToastNoSaveActor : text::TextId::ToastSaveFunctionUnavailable);
        return false;
    }

    DirectSaveCallScratch scratch{};
    int* result = g_original(
        static_cast<std::int64_t>(actor),
        &scratch.out_result,
        slot,
        reinterpret_cast<std::uintptr_t>(&scratch.flags_word));
    const bool succeeded = result != nullptr && scratch.out_result == 0;
    log::Write("[quick-save] source=%s actor=%p slot=%d out=%d success=%d tid=%lu\n",
        source != nullptr ? source : "<null>",
        reinterpret_cast<void*>(actor),
        slot,
        scratch.out_result,
        succeeded ? 1 : 0,
        static_cast<unsigned long>(GetCurrentThreadId()));
    if (succeeded) {
        toast::Show(text::TextId::ToastQuickSaveSuccess);
    } else {
        toast::ShowFormatted(text::TextId::ToastQuickSaveFailedCode, static_cast<unsigned int>(scratch.out_result));
    }
    return succeeded;
}

void Shutdown() {
    if (g_target == 0) {
        return;
    }

    MH_DisableHook(reinterpret_cast<LPVOID>(g_target));
    MH_RemoveHook(reinterpret_cast<LPVOID>(g_target));
    g_last_actor.store(0);
    g_last_context.store(0);
    g_last_thread_id.store(0);
    g_last_slot.store(0);
    g_original = nullptr;
    g_target = 0;
}

}  // namespace qsr::save_runtime
