#pragma once

#include <Windows.h>
#include <cstdint>

namespace qsr::load_runtime {

struct Options {
    bool install_selected_refresh = true;
};

struct CapturedContext {
    std::uintptr_t core_self = 0;
    std::uint64_t target_key = 0;
    std::uint32_t slot = 0;
    std::uintptr_t ui_root = 0;
    DWORD thread_id = 0;
};

bool Install(const Options& options);
bool LatestCapture(CapturedContext& capture);
bool TryOpenQuickLoadConfirmationModal(const char* source);
bool InvokeQuickLoadFromCachedContext(const char* source);
void Shutdown();

}  // namespace qsr::load_runtime
