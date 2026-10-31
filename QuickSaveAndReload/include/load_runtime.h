#pragma once

#include <Windows.h>
#include <cstdint>

namespace qsr::load_runtime {

struct Options {
    bool install_selected_refresh = true;
};

bool Install(const Options& options);
bool TryOpenQuickLoadConfirmationModal(const char* source);
bool InvokeQuickLoadFromCachedContext(const char* source);
void Shutdown();

}  // namespace qsr::load_runtime
