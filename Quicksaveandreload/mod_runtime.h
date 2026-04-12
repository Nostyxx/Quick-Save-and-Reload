#pragma once

#include <Windows.h>

namespace quicksave {

bool Initialize(HMODULE self_module);
void Shutdown();

}  // namespace quicksave
