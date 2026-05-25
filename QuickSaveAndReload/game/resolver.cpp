#include "pch.h"

#include "include/log.h"
#include "include/resolver.h"

namespace qsr::resolver {
namespace {

struct Pattern {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;
};

struct ScanStats {
    std::size_t hits = 0;
    std::uintptr_t first = 0;
};

std::array<Result, static_cast<std::size_t>(symbols::SymbolId::Count)> g_results{};

int HexNibble(char value) {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    return -1;
}

Pattern ParsePattern(const char* text) {
    Pattern pattern;
    if (text == nullptr) {
        return pattern;
    }

    const char* cursor = text;
    while (*cursor != '\0') {
        while (*cursor == ' ') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        if (cursor[0] == '?' && cursor[1] == '?') {
            pattern.bytes.push_back(0);
            pattern.mask.push_back(0);
            cursor += 2;
            continue;
        }

        const int high = HexNibble(cursor[0]);
        const int low = HexNibble(cursor[1]);
        if (high < 0 || low < 0) {
            return {};
        }
        pattern.bytes.push_back(static_cast<std::uint8_t>((high << 4) | low));
        pattern.mask.push_back(0xFF);
        cursor += 2;
    }

    return pattern;
}

bool Matches(const std::uint8_t* memory, const Pattern& pattern) {
    for (std::size_t i = 0; i < pattern.bytes.size(); ++i) {
        if (pattern.mask[i] != 0 && memory[i] != pattern.bytes[i]) {
            return false;
        }
    }
    return true;
}

ScanStats ScanExecutableSections(HMODULE module, const Pattern& pattern) {
    ScanStats stats;
    if (module == nullptr || pattern.bytes.empty()) {
        return stats;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return stats;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return stats;
    }

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned short i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const std::size_t size = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (size < pattern.bytes.size()) {
            continue;
        }

        const auto* memory = base + section->VirtualAddress;
        const std::size_t last = size - pattern.bytes.size();
        for (std::size_t offset = 0; offset <= last; ++offset) {
            if (!Matches(memory + offset, pattern)) {
                continue;
            }
            ++stats.hits;
            if (stats.first == 0) {
                stats.first = reinterpret_cast<std::uintptr_t>(memory + offset);
            }
        }
    }

    return stats;
}

Result FindCandidate(HMODULE module, const symbols::Definition& definition) {
    Result result;
    std::size_t ambiguous_hits = 0;

    for (std::size_t i = 0; i < definition.patterns.size(); ++i) {
        const Pattern pattern = ParsePattern(definition.patterns[i]);
        const ScanStats stats = ScanExecutableSections(module, pattern);
        if (stats.hits == 1) {
            result.status = Status::Candidate;
            result.address = static_cast<std::uintptr_t>(
                static_cast<std::intptr_t>(stats.first) + definition.match_adjustment);
            result.hit_count = stats.hits;
            result.matched_pattern = i + 1;
            return result;
        }
        if (stats.hits > 1) {
            ambiguous_hits = std::max(ambiguous_hits, stats.hits);
        }
    }

    if (ambiguous_hits != 0) {
        result.status = Status::Ambiguous;
        result.hit_count = ambiguous_hits;
    }
    return result;
}

}  // namespace

bool ScanCandidateSignatures(HMODULE game_module) {
    Reset();
    if (game_module == nullptr) {
        log::Write("[resolver] executable module unavailable\n");
        return false;
    }

    for (const symbols::Definition& definition : symbols::Catalog()) {
        Result& result = g_results[static_cast<std::size_t>(definition.id)];
        result = FindCandidate(game_module, definition);
        if (result.status == Status::Ambiguous) {
            log::Write("[resolver] %-20s status=Ambiguous hits=%zu validation=blocked\n",
                definition.name,
                result.hit_count);
        } else {
            log::Write("[resolver] %-20s status=Unresolved validation=blocked\n", definition.name);
        }
    }
    return true;
}

const Result& Get(symbols::SymbolId id) {
    return g_results[static_cast<std::size_t>(id)];
}

bool HasAllRequiredCandidates(health::FeatureId feature) {
    bool has_required_symbol = false;
    for (const symbols::Definition& definition : symbols::Catalog()) {
        if (definition.feature != feature || !definition.required) {
            continue;
        }

        has_required_symbol = true;
        const Status status = Get(definition.id).status;
        if (status != Status::Candidate && status != Status::Validated) {
            return false;
        }
    }
    return has_required_symbol;
}

bool ValidateDirectLocalSaveRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::DirectLocalSave)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] DirectLocalSave      status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateSaveServiceRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::SaveServiceDriver)];
    if (result.status != Status::Candidate || result.address == 0) {
        log::Write("[resolver] SaveServiceDriver    status=%s validation=blocked reason=ida-verified-entry-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateServiceCommandBuildRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::ServiceCommandBuild)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] ServiceCommandBuild status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateInGameMenuLoadCoreRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::InGameMenuLoadCore)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] InGameMenuLoadCore status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateBuildVisibleMapRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::BuildVisibleMap)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] BuildVisibleMap    status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateLoadSelectedRefreshRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::LoadSelectedRefresh)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] LoadSelectedRefresh status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateLoadModalHandlerRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::LoadModalHandler)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] LoadModalHandler   status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateLoadUiRowRuntime() {
    Result& render_result = g_results[static_cast<std::size_t>(symbols::SymbolId::RenderSlotRow)];
    Result& text_result = g_results[static_cast<std::size_t>(symbols::SymbolId::SetControlText)];
    if (render_result.status != Status::Candidate || render_result.matched_pattern != 1 || render_result.address == 0) {
        log::Write("[resolver] RenderSlotRow      status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(render_result.status));
        return false;
    }
    if (text_result.status != Status::Candidate || text_result.matched_pattern != 1 || text_result.address == 0) {
        log::Write("[resolver] SetControlText     status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(text_result.status));
        return false;
    }

    render_result.status = Status::Validated;
    text_result.status = Status::Validated;
    return true;
}

std::uintptr_t ValidatedAddress(symbols::SymbolId id) {
    const Result& result = Get(id);
    return result.status == Status::Validated ? result.address : 0;
}

std::uintptr_t TryResolveGameStateGlobalFromBuildVisibleMap() {
    const std::uintptr_t build_visible_map = ValidatedAddress(symbols::SymbolId::BuildVisibleMap);
    if (build_visible_map == 0) {
        return 0;
    }

    constexpr std::size_t kSearchSize = 0x80;
    constexpr std::size_t kRipInstructionSize = 7;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(build_visible_map);
    __try {
        for (std::size_t offset = 0; offset + kRipInstructionSize <= kSearchSize; ++offset) {
            if (bytes[offset] != 0x48 || bytes[offset + 1] != 0x8B || bytes[offset + 2] != 0x05) {
                continue;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + offset + 3, sizeof(displacement));
            const std::uintptr_t global_address = build_visible_map + offset + kRipInstructionSize + displacement;
            return global_address;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("[resolver] GameStateGlobal    status=Unresolved reason=exception code=0x%08lX\n",
            static_cast<unsigned long>(GetExceptionCode()));
        return 0;
    }

    log::Write("[resolver] GameStateGlobal    status=Unresolved reason=no-rip-load-in-BuildVisibleMap\n");
    return 0;
}

const char* StatusName(Status status) {
    switch (status) {
    case Status::Unresolved:
        return "Unresolved";
    case Status::Ambiguous:
        return "Ambiguous";
    case Status::Candidate:
        return "Candidate";
    case Status::Validated:
        return "Validated";
    default:
        return "Unresolved";
    }
}

void Reset() {
    for (Result& result : g_results) {
        result = {};
    }
}

}  // namespace qsr::resolver
