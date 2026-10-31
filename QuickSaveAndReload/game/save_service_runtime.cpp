#include "pch.h"

#include <intrin.h>
#include <MinHook.h>

#include "include/hotkey_service.h"
#include "include/log.h"
#include "include/quick_slot_runtime.h"
#include "include/resolver.h"
#include "include/save_runtime.h"
#include "include/save_service_runtime.h"

namespace qsr::save_service_runtime {
namespace {

using SaveServiceDriverFn = std::int64_t(__fastcall*)(std::int64_t, std::int64_t);
using WaitForSingleObjectFn = DWORD(WINAPI*)(HANDLE, DWORD);

SaveServiceDriverFn g_driver_original = nullptr;
WaitForSingleObjectFn g_wait_original = nullptr;
std::uintptr_t g_driver_target = 0;
std::uintptr_t g_wait_target = 0;
std::atomic<std::uintptr_t> g_last_service_object{0};
std::atomic<DWORD> g_last_driver_thread_id{0};
std::atomic<std::uint32_t> g_driver_capture_count{0};
std::atomic<std::uint32_t> g_scheduler_dispatch_count{0};
bool g_enable_quick_save_dispatch = false;

constexpr DWORD kPendingWaitMaximumMs = 25;

bool CreateHookRaw(const char* name, std::uintptr_t target, LPVOID detour, LPVOID* original) {
    const MH_STATUS status = MH_CreateHook(reinterpret_cast<LPVOID>(target), detour, original);
    if (status != MH_OK) {
        log::Write("[save-service] MH_CreateHook failed name=%s status=%d target=%p\n",
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
        log::Write("[save-service] MH_EnableHook failed name=%s status=%d target=%p\n",
            name,
            static_cast<int>(status),
            reinterpret_cast<void*>(target));
        return false;
    }
    return true;
}

void RemoveHookIfPresent(std::uintptr_t target) {
    if (target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
    }
}

bool TryDispatchQuickSave(const char* source) {
    if (!g_enable_quick_save_dispatch || !hotkeys::HasQuickSaveRequest()) {
        return false;
    }

    const std::uintptr_t actor = g_last_service_object.load();
    const DWORD capture_thread_id = g_last_driver_thread_id.load();
    const DWORD current_thread_id = GetCurrentThreadId();
    if (actor == 0 || capture_thread_id == 0 || current_thread_id != capture_thread_id) {
        log::Write("[quick-save] dispatch blocked source=%s actor=%p capture_tid=%lu current_tid=%lu\n",
            source != nullptr ? source : "<null>",
            reinterpret_cast<void*>(actor),
            static_cast<unsigned long>(capture_thread_id),
            static_cast<unsigned long>(current_thread_id));
        return false;
    }

    if (!hotkeys::TakeQuickSaveRequest()) {
        return false;
    }

    const int selected_slot = quick_slots::SelectSaveSlot();
    log::Write("[quick-save] dispatch source=%s path=prepared-local-save actor=%p slot=%d tid=%lu reason=quick-slot-rotation\n",
        source != nullptr ? source : "<null>",
        reinterpret_cast<void*>(actor),
        selected_slot,
        static_cast<unsigned long>(current_thread_id));
    return save_runtime::InvokeQuickSave(actor, selected_slot, source);
}

std::uintptr_t ResolveExportAddress(const wchar_t* module_name, const char* export_name) {
    HMODULE module = GetModuleHandleW(module_name);
    if (module == nullptr) {
        module = LoadLibraryW(module_name);
    }
    return module != nullptr
        ? reinterpret_cast<std::uintptr_t>(GetProcAddress(module, export_name))
        : 0;
}

std::int64_t __fastcall SaveServiceDriverHook(std::int64_t self, std::int64_t service_object) {
    const DWORD thread_id = GetCurrentThreadId();
    g_last_service_object.store(static_cast<std::uintptr_t>(service_object));
    g_last_driver_thread_id.store(thread_id);

    const std::uint32_t capture_count = g_driver_capture_count.fetch_add(1) + 1;
    log::Write("[save-service] driver captured count=%u self=%p service=%p tid=%lu\n",
        capture_count,
        reinterpret_cast<void*>(self),
        reinterpret_cast<void*>(service_object),
        static_cast<unsigned long>(thread_id));
    return g_driver_original(self, service_object);
}

DWORD WINAPI WaitForSingleObjectHook(HANDLE handle, DWORD milliseconds) {
    const DWORD current_thread_id = GetCurrentThreadId();
    const bool pending_on_save_thread = g_enable_quick_save_dispatch
        && hotkeys::HasQuickSaveRequest()
        && current_thread_id == g_last_driver_thread_id.load()
        && g_last_service_object.load() != 0;
    const DWORD effective_timeout = pending_on_save_thread && milliseconds > kPendingWaitMaximumMs
        ? kPendingWaitMaximumMs
        : milliseconds;
    const DWORD result = g_wait_original(handle, effective_timeout);

    if (pending_on_save_thread) {
        const std::uint32_t dispatch_count = g_scheduler_dispatch_count.fetch_add(1) + 1;
        log::Write("[save-service] save-thread scheduler count=%u return=%p wait_ms=%lu effective_ms=%lu result=0x%08lX tid=%lu\n",
            dispatch_count,
            _ReturnAddress(),
            static_cast<unsigned long>(milliseconds),
            static_cast<unsigned long>(effective_timeout),
            static_cast<unsigned long>(result),
            static_cast<unsigned long>(current_thread_id));
        TryDispatchQuickSave("save-thread-wait");
    }
    return result;
}

}  // namespace

bool Install(const Options& options) {
    g_driver_target = resolver::ValidatedAddress(symbols::SymbolId::SaveServiceDriver);
    g_wait_target = options.enable_quick_save_dispatch
        ? ResolveExportAddress(L"kernel32.dll", "WaitForSingleObject")
        : 0;
    if (g_driver_target == 0 || (options.enable_quick_save_dispatch && g_wait_target == 0)) {
        log::Write("[save-service] install blocked driver=%p wait=%p dispatch=%d\n",
            reinterpret_cast<void*>(g_driver_target),
            reinterpret_cast<void*>(g_wait_target),
            options.enable_quick_save_dispatch ? 1 : 0);
        g_driver_target = 0;
        g_wait_target = 0;
        return false;
    }

    g_last_service_object.store(0);
    g_last_driver_thread_id.store(0);
    g_driver_capture_count.store(0);
    g_scheduler_dispatch_count.store(0);
    g_enable_quick_save_dispatch = options.enable_quick_save_dispatch;

    if (!CreateHookRaw(
            "SaveServiceDriver",
            g_driver_target,
            reinterpret_cast<LPVOID>(&SaveServiceDriverHook),
            reinterpret_cast<LPVOID*>(&g_driver_original))) {
        Shutdown();
        return false;
    }

    if (g_wait_target != 0
        && !CreateHookRaw(
            "WaitForSingleObject",
            g_wait_target,
            reinterpret_cast<LPVOID>(&WaitForSingleObjectHook),
            reinterpret_cast<LPVOID*>(&g_wait_original))) {
        Shutdown();
        return false;
    }

    if ((g_wait_target != 0 && !EnableHookRaw("WaitForSingleObject", g_wait_target))
        || !EnableHookRaw("SaveServiceDriver", g_driver_target)) {
        Shutdown();
        return false;
    }

    log::Write("[save-service] installed driver=%p wait=%p dispatch=save-thread-wait enabled=%d\n",
        reinterpret_cast<void*>(g_driver_target),
        reinterpret_cast<void*>(g_wait_target),
        g_enable_quick_save_dispatch ? 1 : 0);
    return true;
}

bool DispatchPendingQuickSave(const char* source) {
    return TryDispatchQuickSave(source);
}

void Shutdown() {
    RemoveHookIfPresent(g_driver_target);
    RemoveHookIfPresent(g_wait_target);
    g_driver_original = nullptr;
    g_wait_original = nullptr;
    g_driver_target = 0;
    g_wait_target = 0;
    g_last_service_object.store(0);
    g_last_driver_thread_id.store(0);
    g_driver_capture_count.store(0);
    g_scheduler_dispatch_count.store(0);
    g_enable_quick_save_dispatch = false;
}

}  // namespace qsr::save_service_runtime
