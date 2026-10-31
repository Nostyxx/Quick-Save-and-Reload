#pragma once

namespace qsr::save_service_runtime {

struct Options {
    bool enable_quick_save_dispatch = false;
};

bool Install(const Options& options);
bool DispatchPendingQuickSave(const char* source);
void Shutdown();

}  // namespace qsr::save_service_runtime
