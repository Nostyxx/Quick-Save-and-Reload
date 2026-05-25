#pragma once

namespace qsr::save_service_runtime {

struct Options {
    bool enable_quick_save_dispatch = false;
};

struct CapturedContext {
    std::uintptr_t actor = 0;
    DWORD thread_id = 0;
};

bool Install(const Options& options);
bool LatestCapture(CapturedContext& capture);
void Shutdown();

}  // namespace qsr::save_service_runtime
