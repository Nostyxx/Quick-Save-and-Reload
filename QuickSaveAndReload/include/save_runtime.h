#pragma once

namespace qsr::save_runtime {

struct Options {
};

struct CapturedCall {
    std::uintptr_t actor = 0;
    std::uintptr_t context = 0;
    DWORD thread_id = 0;
    int slot = 0;
};

bool Install(const Options& options);
bool LatestCapture(CapturedCall& capture);
bool InvokeQuickSave(std::uintptr_t actor, int slot, const char* source);
void Shutdown();

}  // namespace qsr::save_runtime
