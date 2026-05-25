#pragma once

#include <Windows.h>

#include <string>

namespace qsr::config {

struct Settings {
    bool enable_mod = true;
    bool log_enabled = false;
    bool toast_notification = true;
    bool quick_load_confirmation = true;
    int quick_slot_id = 108;
    int quick_slot_count = 1;
    int quick_save_vk = VK_F5;
    int quick_load_vk = VK_F6;
    WORD quick_save_controller_mask = 0;
    WORD quick_load_controller_mask = 0;
    std::wstring language = L"en_US";
};

bool Load(HMODULE self_module, Settings& settings);

}  // namespace qsr::config
