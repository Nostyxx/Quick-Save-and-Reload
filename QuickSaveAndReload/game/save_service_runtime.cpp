#include "pch.h"

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
using ServiceCommandBuildFn = std::uint32_t* (__fastcall*)(std::int64_t, std::uint32_t*, std::int64_t, std::int64_t);

SaveServiceDriverFn g_driver_original = nullptr;
ServiceCommandBuildFn g_service_command_build_original = nullptr;
std::uintptr_t g_driver_target = 0;
std::uintptr_t g_service_command_build_target = 0;
std::atomic<std::uintptr_t> g_last_service_object{0};
std::atomic<DWORD> g_last_driver_thread_id{0};
bool g_enable_quick_save_dispatch = false;

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

void TryDispatchQuickSave(const char* source) {
    if (!g_enable_quick_save_dispatch) {
        return;
    }

    const DWORD thread_id = GetCurrentThreadId();
    const DWORD driver_thread_id = g_last_driver_thread_id.load();
    const std::uintptr_t actor = g_last_service_object.load();
    if (actor == 0 || driver_thread_id == 0 || thread_id != driver_thread_id) {
        return;
    }

    if (!hotkeys::TakeQuickSaveRequest()) {
        return;
    }

    const int selected_slot = quick_slots::SelectSaveSlot();
    save_runtime::InvokeQuickSave(actor, selected_slot, source);
}

std::int64_t __fastcall SaveServiceDriverHook(std::int64_t self, std::int64_t service_object) {
    const DWORD thread_id = GetCurrentThreadId();
    g_last_service_object.store(static_cast<std::uintptr_t>(service_object));
    g_last_driver_thread_id.store(thread_id);

    return g_driver_original(self, service_object);
}

std::uint32_t* __fastcall ServiceCommandBuildHook(
    std::int64_t self,
    std::uint32_t* out_result,
    std::int64_t context,
    std::int64_t command) {
    std::uint32_t* result = g_service_command_build_original != nullptr
        ? g_service_command_build_original(self, out_result, context, command)
        : out_result;

    TryDispatchQuickSave("service-command-build");
    return result;
}

}  // namespace

bool Install(const Options& options) {
    g_driver_target = resolver::ValidatedAddress(symbols::SymbolId::SaveServiceDriver);
    g_service_command_build_target = options.enable_quick_save_dispatch
        ? resolver::ValidatedAddress(symbols::SymbolId::ServiceCommandBuild)
        : 0;
    if (g_driver_target == 0 || (options.enable_quick_save_dispatch && g_service_command_build_target == 0)) {
        log::Write("[save-service] install blocked; requested service targets are not validated\n");
        g_driver_target = 0;
        g_service_command_build_target = 0;
        return false;
    }

    g_last_service_object.store(0);
    g_last_driver_thread_id.store(0);
    g_enable_quick_save_dispatch = options.enable_quick_save_dispatch;

    if (!CreateHookRaw(
            "SaveServiceDriver",
            g_driver_target,
            reinterpret_cast<LPVOID>(&SaveServiceDriverHook),
            reinterpret_cast<LPVOID*>(&g_driver_original))) {
        g_driver_target = 0;
        g_service_command_build_target = 0;
        return false;
    }

    if (g_service_command_build_target != 0
        && !CreateHookRaw(
            "ServiceCommandBuild",
            g_service_command_build_target,
            reinterpret_cast<LPVOID>(&ServiceCommandBuildHook),
            reinterpret_cast<LPVOID*>(&g_service_command_build_original))) {
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_driver_target));
        g_driver_original = nullptr;
        g_driver_target = 0;
        g_service_command_build_target = 0;
        return false;
    }

    if (!EnableHookRaw("SaveServiceDriver", g_driver_target)
        || (g_service_command_build_target != 0
            && !EnableHookRaw("ServiceCommandBuild", g_service_command_build_target))) {
        Shutdown();
        return false;
    }

    return true;
}

bool LatestCapture(CapturedContext& capture) {
    capture.actor = g_last_service_object.load();
    capture.thread_id = g_last_driver_thread_id.load();
    return capture.actor != 0 && capture.thread_id != 0;
}

void Shutdown() {
    RemoveHookIfPresent(g_service_command_build_target);
    RemoveHookIfPresent(g_driver_target);
    g_driver_original = nullptr;
    g_service_command_build_original = nullptr;
    g_driver_target = 0;
    g_service_command_build_target = 0;
    g_last_service_object.store(0);
    g_last_driver_thread_id.store(0);
    g_enable_quick_save_dispatch = false;
}

}  // namespace qsr::save_service_runtime
