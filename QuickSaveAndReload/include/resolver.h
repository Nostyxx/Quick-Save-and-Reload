#pragma once

#include "runtime_health.h"
#include "symbol_catalog.h"

#include <Windows.h>
#include <cstddef>
#include <cstdint>

namespace qsr::resolver {

enum class Status {
    Unresolved = 0,
    Ambiguous,
    Candidate,
    Validated
};

struct Result {
    Status status = Status::Unresolved;
    std::uintptr_t address = 0;
    std::size_t hit_count = 0;
    std::size_t matched_pattern = 0;
};

bool ScanCandidateSignatures(HMODULE game_module);
const Result& Get(symbols::SymbolId id);
bool HasAllRequiredCandidates(health::FeatureId feature);
bool ValidateDirectLocalSaveRuntime();
bool ValidateSaveServiceRuntime();
bool ValidateServiceCommandBuildRuntime();
bool ValidateInGameMenuLoadCoreRuntime();
bool ValidateBuildVisibleMapRuntime();
bool ValidateLoadSelectedRefreshRuntime();
bool ValidateLoadModalHandlerRuntime();
bool ValidateLoadUiRowRuntime();
std::uintptr_t ValidatedAddress(symbols::SymbolId id);
std::uintptr_t TryResolveGameStateGlobalFromBuildVisibleMap();
const char* StatusName(Status status);
void Reset();

}  // namespace qsr::resolver
