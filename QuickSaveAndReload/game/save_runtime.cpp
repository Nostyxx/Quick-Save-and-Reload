#include "pch.h"

#include <MinHook.h>

#include "include/log.h"
#include "include/quick_slot_runtime.h"
#include "include/resolver.h"
#include "include/save_runtime.h"
#include "include/text_runtime.h"
#include "include/toast_runtime.h"

namespace qsr::save_runtime {
namespace {

constexpr bool kEnableSaveProbe = false;

using DirectLocalSaveFn = int* (__fastcall*)(std::int64_t, int*, int, std::uintptr_t);
using PreparedLocalSaveFn = int* (__fastcall*)(std::uintptr_t, int*, int*, const wchar_t*);
using UiSaveRequestFn = int* (__fastcall*)(std::uintptr_t, int*, std::uintptr_t*, unsigned int);

DirectLocalSaveFn g_original = nullptr;
PreparedLocalSaveFn g_prepared_original = nullptr;
UiSaveRequestFn g_ui_request_original = nullptr;
std::uintptr_t g_target = 0;
std::uintptr_t g_prepared_target = 0;
std::uintptr_t g_ui_request_target = 0;
std::atomic<std::uintptr_t> g_last_actor{0};
std::atomic<std::uintptr_t> g_last_context{0};
std::atomic<DWORD> g_last_thread_id{0};
std::atomic<int> g_last_slot{0};

struct DirectSaveCallScratch {
    int flags_word = 0;
    int flags_pad = 0;
    int out_result = 0;
};

int CaptureExceptionDetails(
    EXCEPTION_POINTERS* exception,
    DWORD* code,
    std::uintptr_t* address) {
    if (exception != nullptr && exception->ExceptionRecord != nullptr) {
        *code = exception->ExceptionRecord->ExceptionCode;
        *address = reinterpret_cast<std::uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int* __fastcall DirectLocalSaveHook(
    std::int64_t actor,
    int* out_result,
    int slot,
    std::uintptr_t context) {
    g_last_actor.store(static_cast<std::uintptr_t>(actor));
    g_last_context.store(context);
    g_last_thread_id.store(GetCurrentThreadId());
    g_last_slot.store(slot);
    if constexpr (kEnableSaveProbe) {
        std::uint32_t context_word = 0;
        __try {
            context_word = *reinterpret_cast<const std::uint32_t*>(context);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            context_word = 0xFFFFFFFFu;
        }
        log::Write("[save-probe] direct enter actor=%p out=%p slot=%d context=%p context_word=0x%08X tid=%lu\n",
            reinterpret_cast<void*>(actor),
            out_result,
            slot,
            reinterpret_cast<void*>(context),
            context_word,
            static_cast<unsigned long>(GetCurrentThreadId()));
    }
    int* result = g_original(actor, out_result, slot, context);
    if constexpr (kEnableSaveProbe) {
        log::Write("[save-probe] direct leave actor=%p slot=%d result=%p out=%d tid=%lu\n",
            reinterpret_cast<void*>(actor),
            slot,
            result,
            out_result != nullptr ? *out_result : -1,
            static_cast<unsigned long>(GetCurrentThreadId()));
    }
    return result;
}

int* __fastcall PreparedLocalSaveHook(
    std::uintptr_t actor,
    int* out_result,
    int* slot,
    const wchar_t* name) {
    if constexpr (kEnableSaveProbe) {
        log::Write("[save-probe] prepared enter actor=%p out=%p slot_ptr=%p slot=%d name=%p tid=%lu\n",
            reinterpret_cast<void*>(actor),
            out_result,
            slot,
            slot != nullptr ? *slot : -1,
            name,
            static_cast<unsigned long>(GetCurrentThreadId()));
    }
    int* result = g_prepared_original(actor, out_result, slot, name);
    if constexpr (kEnableSaveProbe) {
        log::Write("[save-probe] prepared leave actor=%p slot=%d result=%p out=%d tid=%lu\n",
            reinterpret_cast<void*>(actor),
            slot != nullptr ? *slot : -1,
            result,
            out_result != nullptr ? *out_result : -1,
            static_cast<unsigned long>(GetCurrentThreadId()));
    }
    return result;
}

int* __fastcall UiSaveRequestHook(
    std::uintptr_t self,
    int* out_result,
    std::uintptr_t* record,
    unsigned int command) {
    std::uintptr_t owner = 0;
    std::uintptr_t record_value = 0;
    __try {
        owner = *reinterpret_cast<const std::uintptr_t*>(self + 8);
        record_value = record != nullptr ? *record : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        owner = 0;
        record_value = 0;
    }
    log::Write("[save-probe] ui-request enter self=%p owner=%p out=%p record=%p record_value=%p command=%u tid=%lu\n",
        reinterpret_cast<void*>(self),
        reinterpret_cast<void*>(owner),
        out_result,
        record,
        reinterpret_cast<void*>(record_value),
        command,
        static_cast<unsigned long>(GetCurrentThreadId()));
    int* result = g_ui_request_original(self, out_result, record, command);
    log::Write("[save-probe] ui-request leave self=%p result=%p out=%d tid=%lu\n",
        reinterpret_cast<void*>(self),
        result,
        out_result != nullptr ? *out_result : -1,
        static_cast<unsigned long>(GetCurrentThreadId()));
    return result;
}

bool TryInstallPreparedHook() {
    g_prepared_target = resolver::ValidatedAddress(symbols::SymbolId::PreparedLocalSave);
    if (g_prepared_target == 0) {
        log::Write("[save-runtime] PreparedLocalSave unavailable; direct path remains enabled\n");
        return true;
    }

    const MH_STATUS create_status = MH_CreateHook(
        reinterpret_cast<LPVOID>(g_prepared_target),
        reinterpret_cast<LPVOID>(&PreparedLocalSaveHook),
        reinterpret_cast<LPVOID*>(&g_prepared_original));
    if (create_status != MH_OK) {
        log::Write("[save-runtime] PreparedLocalSave hook skipped status=%d target=%p\n",
            static_cast<int>(create_status),
            reinterpret_cast<void*>(g_prepared_target));
        g_prepared_target = 0;
        return true;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<LPVOID>(g_prepared_target));
    if (enable_status != MH_OK) {
        log::Write("[save-runtime] PreparedLocalSave enable skipped status=%d target=%p\n",
            static_cast<int>(enable_status),
            reinterpret_cast<void*>(g_prepared_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_prepared_target));
        g_prepared_original = nullptr;
        g_prepared_target = 0;
        return true;
    }

    log::Write("[save-runtime] PreparedLocalSave hook installed target=%p\n",
        reinterpret_cast<void*>(g_prepared_target));
    return true;
}

bool TryInstallUiRequestHook() {
    if (!kEnableSaveProbe) {
        return true;
    }
    g_ui_request_target = resolver::ValidatedAddress(symbols::SymbolId::UiSaveRequest);
    if (g_ui_request_target == 0) {
        log::Write("[save-probe] UiSaveRequest unavailable\n");
        return true;
    }

    const MH_STATUS create_status = MH_CreateHook(
        reinterpret_cast<LPVOID>(g_ui_request_target),
        reinterpret_cast<LPVOID>(&UiSaveRequestHook),
        reinterpret_cast<LPVOID*>(&g_ui_request_original));
    if (create_status != MH_OK) {
        log::Write("[save-probe] UiSaveRequest hook skipped status=%d target=%p\n",
            static_cast<int>(create_status),
            reinterpret_cast<void*>(g_ui_request_target));
        g_ui_request_target = 0;
        return true;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<LPVOID>(g_ui_request_target));
    if (enable_status != MH_OK) {
        log::Write("[save-probe] UiSaveRequest enable skipped status=%d target=%p\n",
            static_cast<int>(enable_status),
            reinterpret_cast<void*>(g_ui_request_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_ui_request_target));
        g_ui_request_original = nullptr;
        g_ui_request_target = 0;
        return true;
    }

    log::Write("[save-probe] UiSaveRequest hook installed target=%p\n",
        reinterpret_cast<void*>(g_ui_request_target));
    return true;
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

    return TryInstallPreparedHook() && TryInstallUiRequestHook();
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

    if (g_prepared_original != nullptr) {
        int prepared_out = -1;
        int prepared_slot = slot;
        int* prepared_result = nullptr;
        DWORD exception_code = 0;
        std::uintptr_t exception_address = 0;
        __try {
            prepared_result = g_prepared_original(actor, &prepared_out, &prepared_slot, L"Quick Save");
        } __except (CaptureExceptionDetails(GetExceptionInformation(), &exception_code, &exception_address)) {
            log::Write("[quick-save] source=%s path=prepared actor=%p slot=%d exception=0x%08lX address=%p tid=%lu\n",
                source != nullptr ? source : "<null>",
                reinterpret_cast<void*>(actor),
                slot,
                static_cast<unsigned long>(exception_code),
                reinterpret_cast<void*>(exception_address),
                static_cast<unsigned long>(GetCurrentThreadId()));
            toast::ShowFormatted(text::TextId::ToastQuickSaveFailedCode, static_cast<unsigned int>(exception_code));
            return false;
        }

        const bool prepared_succeeded = prepared_result != nullptr && prepared_out == 0;
        log::Write("[quick-save] source=%s path=prepared actor=%p slot=%d out=%d success=%d tid=%lu\n",
            source != nullptr ? source : "<null>",
            reinterpret_cast<void*>(actor),
            slot,
            prepared_out,
            prepared_succeeded ? 1 : 0,
            static_cast<unsigned long>(GetCurrentThreadId()));
        if (prepared_succeeded) {
            quick_slots::MarkQuickSaveSucceeded(slot);
            toast::Show(text::TextId::ToastQuickSaveSuccess);
            return true;
        }
    }

    DirectSaveCallScratch scratch{};
    int* result = nullptr;
    DWORD exception_code = 0;
    std::uintptr_t exception_address = 0;
    __try {
        result = g_original(
            static_cast<std::int64_t>(actor),
            &scratch.out_result,
            slot,
            reinterpret_cast<std::uintptr_t>(&scratch.flags_word));
    } __except (CaptureExceptionDetails(GetExceptionInformation(), &exception_code, &exception_address)) {
        log::Write("[quick-save] source=%s actor=%p slot=%d exception=0x%08lX address=%p tid=%lu\n",
            source != nullptr ? source : "<null>",
            reinterpret_cast<void*>(actor),
            slot,
            static_cast<unsigned long>(exception_code),
            reinterpret_cast<void*>(exception_address),
            static_cast<unsigned long>(GetCurrentThreadId()));
        toast::ShowFormatted(text::TextId::ToastQuickSaveFailedCode, static_cast<unsigned int>(exception_code));
        return false;
    }

    const bool succeeded = result != nullptr && scratch.out_result == 0;
    log::Write("[quick-save] source=%s actor=%p slot=%d out=%d success=%d tid=%lu\n",
        source != nullptr ? source : "<null>",
        reinterpret_cast<void*>(actor),
        slot,
        scratch.out_result,
        succeeded ? 1 : 0,
        static_cast<unsigned long>(GetCurrentThreadId()));
    if (succeeded) {
        quick_slots::MarkQuickSaveSucceeded(slot);
        toast::Show(text::TextId::ToastQuickSaveSuccess);
    } else {
        toast::ShowFormatted(text::TextId::ToastQuickSaveFailedCode, static_cast<unsigned int>(scratch.out_result));
    }
    return succeeded;
}

void Shutdown() {
    if (g_ui_request_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_ui_request_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_ui_request_target));
    }
    if (g_prepared_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_prepared_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_prepared_target));
    }
    if (g_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_target));
    }

    g_last_actor.store(0);
    g_last_context.store(0);
    g_last_thread_id.store(0);
    g_last_slot.store(0);
    g_original = nullptr;
    g_prepared_original = nullptr;
    g_ui_request_original = nullptr;
    g_target = 0;
    g_prepared_target = 0;
    g_ui_request_target = 0;
}

}  // namespace qsr::save_runtime
