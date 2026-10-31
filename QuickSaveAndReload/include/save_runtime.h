#pragma once

namespace qsr::save_runtime {

struct Options {
};

bool Install(const Options& options);
bool InvokeQuickSave(std::uintptr_t actor, int slot, const char* source);
void Shutdown();

}  // namespace qsr::save_runtime
