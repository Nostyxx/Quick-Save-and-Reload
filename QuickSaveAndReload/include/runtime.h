#pragma once

#include <Windows.h>

namespace qsr {

bool Initialize(HMODULE self_module);
void Shutdown();

}  // namespace qsr

