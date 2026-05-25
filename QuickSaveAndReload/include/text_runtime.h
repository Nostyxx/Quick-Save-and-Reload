#pragma once

#include <string>

namespace qsr::text {

enum class TextId {
    UiRowLabel = 0,
    ToastQuickSaveSuccess,
    ToastQuickSaveFailed,
    ToastSaveFunctionUnavailable,
    ToastNoSaveActor,
    ToastQuickSaveFailedCode,
    ToastQuickLoadFailed,
    ToastLoadFunctionUnavailable,
    ToastNoQuickSaveFound,
    ToastGameStateUnavailable,
    Count
};

void Initialize(const std::wstring& locale);
const char* Get(TextId id);
std::string Format(TextId id, unsigned int value);

}  // namespace qsr::text
