#pragma once

#include "runtime_health.h"

#include <array>
#include <cstddef>

namespace qsr::symbols {

enum class SymbolId {
    DirectLocalSave = 0,
    SaveServiceDriver,
    ServiceCommandBuild,
    InGameMenuLoadCore,
    BuildVisibleMap,
    LoadSelectedRefresh,
    LoadModalHandler,
    RenderSlotRow,
    SetControlText,
    Count
};

struct Definition {
    SymbolId id;
    const char* name;
    health::FeatureId feature;
    bool required;
    std::ptrdiff_t match_adjustment;
    std::array<const char*, 2> patterns;
};

const std::array<Definition, static_cast<std::size_t>(SymbolId::Count)>& Catalog();
const char* SymbolName(SymbolId id);

}  // namespace qsr::symbols
