#pragma once

#include <Windows.h>

namespace qsr::hotkeys {

bool Start(int quick_save_vk, int quick_load_vk, WORD quick_save_controller_mask, WORD quick_load_controller_mask);
bool TakeQuickSaveRequest();
bool TakeQuickLoadRequest();
void Stop();

}  // namespace qsr::hotkeys
