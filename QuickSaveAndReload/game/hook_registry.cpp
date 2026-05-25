#include "pch.h"

#include <MinHook.h>

#include "include/hook_registry.h"
#include "include/hotkey_service.h"
#include "include/load_runtime.h"
#include "include/load_ui_runtime.h"
#include "include/log.h"
#include "include/quick_slot_runtime.h"
#include "include/resolver.h"
#include "include/runtime_health.h"
#include "include/save_runtime.h"
#include "include/save_service_runtime.h"
#include "include/toast_runtime.h"

namespace qsr::hooks {
namespace {

bool g_min_hook_initialized = false;
bool g_save_runtime_installed = false;
bool g_save_service_runtime_installed = false;
bool g_hotkey_service_started = false;
bool g_load_runtime_installed = false;
bool g_load_ui_runtime_installed = false;

void ShutdownInstalledRuntimes() {
    if (g_hotkey_service_started) {
        hotkeys::Stop();
        g_hotkey_service_started = false;
    }
    if (g_load_ui_runtime_installed) {
        load_ui_runtime::Shutdown();
        g_load_ui_runtime_installed = false;
    }
    if (g_load_runtime_installed) {
        load_runtime::Shutdown();
        g_load_runtime_installed = false;
    }
    if (g_save_service_runtime_installed) {
        save_service_runtime::Shutdown();
        g_save_service_runtime_installed = false;
    }
    if (g_save_runtime_installed) {
        save_runtime::Shutdown();
        g_save_runtime_installed = false;
    }
    if (g_min_hook_initialized) {
        MH_Uninitialize();
        g_min_hook_initialized = false;
    }
}

}  // namespace

bool Prepare(const config::Settings& settings) {
    const bool has_save_candidates = resolver::HasAllRequiredCandidates(health::FeatureId::CoreSave);
    const bool has_load_candidates = resolver::HasAllRequiredCandidates(health::FeatureId::CoreLoad);

    health::Set(health::FeatureId::CoreSave,
        has_save_candidates ? health::State::Candidate : health::State::Disabled,
        has_save_candidates ? "save candidates found; validating release hooks" : "required save candidates are missing or ambiguous");
    health::Set(health::FeatureId::CoreLoad,
        has_load_candidates ? health::State::Candidate : health::State::Disabled,
        has_load_candidates ? "load candidates found; validating release hooks" : "required load candidates are missing or ambiguous");
    health::Set(health::FeatureId::LoadUi, health::State::Disabled,
        "excluded until release load targets are validated");
    health::Set(health::FeatureId::Toast,
        toast::Ready() ? health::State::Candidate : health::State::Disabled,
        toast::Ready() ? "native toast bridge active" : "native toast bridge unavailable");
    health::Set(health::FeatureId::Input, health::State::Disabled,
        "excluded until release action paths are installed");

    log::Write("[hooks] mode=service-command-build-f5-dispatch vk=%d\n",
        settings.quick_save_vk);

    if (!resolver::ValidateDirectLocalSaveRuntime()
        || !resolver::ValidateSaveServiceRuntime()
        || !resolver::ValidateServiceCommandBuildRuntime()) {
        health::Set(health::FeatureId::CoreSave, health::State::Disabled,
            "quick-save dispatcher blocked because validated save targets were unavailable");
        return false;
    }

    const MH_STATUS init_status = MH_Initialize();
    if (init_status != MH_OK && init_status != MH_ERROR_ALREADY_INITIALIZED) {
        log::Write("[hooks] MH_Initialize failed status=%d\n", static_cast<int>(init_status));
        return false;
    }
    g_min_hook_initialized = true;

    g_save_runtime_installed = save_runtime::Install({});
    if (!g_save_runtime_installed) {
        ShutdownInstalledRuntimes();
        return false;
    }

    g_save_service_runtime_installed = save_service_runtime::Install({true});
    if (!g_save_service_runtime_installed) {
        ShutdownInstalledRuntimes();
        return false;
    }

    const bool load_core_ready = resolver::ValidateInGameMenuLoadCoreRuntime();
    const bool build_visible_map_ready = resolver::ValidateBuildVisibleMapRuntime();
    const bool load_ui_ready = resolver::ValidateLoadSelectedRefreshRuntime();
    const bool load_modal_ready = resolver::ValidateLoadModalHandlerRuntime();
    const bool load_ui_row_ready = resolver::ValidateLoadUiRowRuntime();

    quick_slots::Initialize({
        settings.quick_slot_id,
        settings.quick_slot_count,
        resolver::TryResolveGameStateGlobalFromBuildVisibleMap()
    });

    if (load_core_ready && load_ui_ready) {
        g_load_runtime_installed = load_runtime::Install({true});
        if (g_load_runtime_installed && load_ui_row_ready) {
            g_load_ui_runtime_installed = load_ui_runtime::Install({});
        }
    }

    if (g_load_runtime_installed) {
        health::Set(health::FeatureId::CoreLoad, health::State::Candidate,
            load_modal_ready
                ? "native load context and confirmation-modal accept/cancel tracking active"
                : "native load context and confirmation-modal opener active");
        health::Set(health::FeatureId::LoadUi, health::State::Candidate,
            build_visible_map_ready && g_load_ui_runtime_installed
                ? "quick-save load rows, reserved-slot hiding, and modal path active"
                : "load modal path active; row customization unavailable");
    } else {
        health::Set(health::FeatureId::CoreLoad, health::State::Disabled,
            "validated load targets were unavailable or hook installation failed");
        health::Set(health::FeatureId::LoadUi, health::State::Disabled,
            "load UI runtime unavailable");
    }

    g_hotkey_service_started = hotkeys::Start(
        settings.quick_save_vk,
        settings.quick_load_vk,
        settings.quick_save_controller_mask,
        settings.quick_load_controller_mask);
    if (!g_hotkey_service_started) {
        ShutdownInstalledRuntimes();
        return false;
    }

    health::Set(health::FeatureId::CoreSave, health::State::Candidate,
        "F5 native save dispatch active on service-command-build boundary");
    health::Set(health::FeatureId::Input, health::State::Candidate,
        "keyboard/controller quick-save and quick-load requests active");
    return true;
}

void Shutdown() {
    ShutdownInstalledRuntimes();
}

}  // namespace qsr::hooks
