#pragma once

#include "text_runtime.h"

namespace qsr::toast {

bool Initialize(bool enabled);
bool Ready();
void Show(const char* message);
void Show(text::TextId id);
void ShowFormatted(text::TextId id, unsigned int value);
void Shutdown();

}  // namespace qsr::toast
