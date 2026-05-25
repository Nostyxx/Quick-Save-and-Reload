#include "pch.h"

#include "include/config.h"
#include "include/hook_registry.h"
#include "include/log.h"
#include "include/resolver.h"
#include "include/runtime.h"
#include "include/runtime_health.h"
#include "include/text_runtime.h"
#include "include/toast_runtime.h"

namespace qsr {
namespace {

std::atomic<bool> g_initialized{false};

}  // namespace

bool Initialize(HMODULE self_module) {
    bool expected = false;
    if (!g_initialized.compare_exchange_strong(expected, true)) {
        return true;
    }

    config::Settings settings;
    const bool config_loaded = config::Load(self_module, settings);
    if (!log::Open(self_module, settings.log_enabled)) {
        g_initialized.store(false);
        return false;
    }

    log::Write("===============================================\n");
    log::Write("  Quick Save and Reload\n");
    log::Write("===============================================\n\n");
    log::Write("[startup] base=%p configuration=%s enabled=%d log=%d toast=%d quick_load_confirm=%d quick_count=%d save_vk=%d load_vk=%d save_pad=0x%04X load_pad=0x%04X locale=%ls\n",
        reinterpret_cast<void*>(GetModuleHandleW(nullptr)),
        config_loaded ? "loaded-or-defaulted" : "path-error-defaults-used",
        settings.enable_mod ? 1 : 0,
        settings.log_enabled ? 1 : 0,
        settings.toast_notification ? 1 : 0,
        settings.quick_load_confirmation ? 1 : 0,
        settings.quick_slot_count,
        settings.quick_save_vk,
        settings.quick_load_vk,
        static_cast<unsigned>(settings.quick_save_controller_mask),
        static_cast<unsigned>(settings.quick_load_controller_mask),
        settings.language.c_str());

    text::Initialize(settings.language);
    toast::Initialize(settings.toast_notification);

    health::Reset();
    if (!settings.enable_mod) {
        log::Write("[startup] mod disabled in configuration; no scan or hooks requested\n");
        return true;
    }

    if (!resolver::ScanCandidateSignatures(GetModuleHandleW(nullptr))) {
        log::Write("[startup] candidate scan failed before hook evaluation\n");
        return false;
    }

    if (!hooks::Prepare(settings)) {
        health::LogSummary();
        log::Write("[startup] release hook validation or installation failed\n");
        return false;
    }
    log::Write("[startup] ready\n");
    return true;
}

void Shutdown() {
    if (!g_initialized.exchange(false)) {
        return;
    }

    hooks::Shutdown();
    toast::Shutdown();
    resolver::Reset();
    log::Write("[shutdown] complete\n");
    log::Close();
}

}  // namespace qsr
