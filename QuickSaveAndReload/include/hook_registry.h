#pragma once

#include "config.h"

namespace qsr::hooks {

bool Prepare(const config::Settings& settings);
void Shutdown();

}  // namespace qsr::hooks

