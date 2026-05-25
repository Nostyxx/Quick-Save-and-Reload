#pragma once

#include <Windows.h>

namespace qsr::log {

bool Open(HMODULE self_module, bool enabled);
void Write(const char* format, ...);
void Close();

}  // namespace qsr::log

