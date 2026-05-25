#pragma once

namespace qsr::load_ui_runtime {

struct Options {};

bool Install(const Options& options);
void Shutdown();

}  // namespace qsr::load_ui_runtime
