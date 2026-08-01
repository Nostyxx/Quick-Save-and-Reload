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

struct ModuleImage {
    std::uint8_t* base = nullptr;
    IMAGE_NT_HEADERS* nt = nullptr;
};

std::array<Result, static_cast<std::size_t>(symbols::SymbolId::Count)> g_results{};

bool TryGetModuleImage(HMODULE module, ModuleImage& image) {
    if (module == nullptr) {
        return false;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) {
        return false;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    image.base = base;
    image.nt = nt;
    return true;
}

bool IsImageRangeValid(const ModuleImage& image, std::uint32_t rva, std::size_t size) {
    const std::size_t image_size = image.nt->OptionalHeader.SizeOfImage;
    return rva < image_size && size <= image_size - rva;
}

std::uintptr_t FindImportAddressTableSlot(
    HMODULE module,
    const char* expected_module,
    const char* expected_name) {
    ModuleImage image;
    if (!TryGetModuleImage(module, image)) {
        return 0;
    }

    const IMAGE_DATA_DIRECTORY& import_directory =
        image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (import_directory.VirtualAddress == 0
        || import_directory.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR)
        || !IsImageRangeValid(image, import_directory.VirtualAddress, import_directory.Size)) {
        return 0;
    }

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(
        image.base + import_directory.VirtualAddress);
    const std::size_t descriptor_count = import_directory.Size / sizeof(IMAGE_IMPORT_DESCRIPTOR);
    for (std::size_t descriptor_index = 0;
         descriptor_index < descriptor_count && descriptor[descriptor_index].Name != 0;
         ++descriptor_index) {
        const IMAGE_IMPORT_DESCRIPTOR& entry = descriptor[descriptor_index];
        if (!IsImageRangeValid(image, entry.Name, 1)
            || entry.OriginalFirstThunk == 0
            || entry.FirstThunk == 0
            || !IsImageRangeValid(image, entry.OriginalFirstThunk, sizeof(IMAGE_THUNK_DATA64))
            || !IsImageRangeValid(image, entry.FirstThunk, sizeof(IMAGE_THUNK_DATA64))) {
            continue;
        }

        const char* module_name = reinterpret_cast<const char*>(image.base + entry.Name);
        if (_stricmp(module_name, expected_module) != 0) {
            continue;
        }

        auto* name_thunks = reinterpret_cast<const IMAGE_THUNK_DATA64*>(
            image.base + entry.OriginalFirstThunk);
        auto* address_thunks = reinterpret_cast<IMAGE_THUNK_DATA64*>(
            image.base + entry.FirstThunk);
        const std::size_t max_name_thunks =
            (image.nt->OptionalHeader.SizeOfImage - entry.OriginalFirstThunk) / sizeof(IMAGE_THUNK_DATA64);
        const std::size_t max_address_thunks =
            (image.nt->OptionalHeader.SizeOfImage - entry.FirstThunk) / sizeof(IMAGE_THUNK_DATA64);
        const std::size_t max_thunks = (std::min)(max_name_thunks, max_address_thunks);

        for (std::size_t thunk_index = 0;
             thunk_index < max_thunks && name_thunks[thunk_index].u1.AddressOfData != 0;
             ++thunk_index) {
            const ULONGLONG name_value = name_thunks[thunk_index].u1.AddressOfData;
            if (IMAGE_SNAP_BY_ORDINAL64(name_value)) {
                continue;
            }

            const auto name_rva = static_cast<std::uint32_t>(name_value);
            if (!IsImageRangeValid(image, name_rva, sizeof(IMAGE_IMPORT_BY_NAME))) {
                continue;
            }

            const auto* import = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(image.base + name_rva);
            if (std::strcmp(reinterpret_cast<const char*>(import->Name), expected_name) == 0) {
                return reinterpret_cast<std::uintptr_t>(&address_thunks[thunk_index].u1.Function);
            }
        }
    }

    return 0;
}

bool ContainsBytes(
    const std::uint8_t* memory,
    std::size_t memory_size,
    const std::uint8_t* bytes,
    std::size_t byte_count) {
    if (memory == nullptr || bytes == nullptr || byte_count == 0 || memory_size < byte_count) {
        return false;
    }

    const std::size_t last = memory_size - byte_count;
    for (std::size_t offset = 0; offset <= last; ++offset) {
        if (std::memcmp(memory + offset, bytes, byte_count) == 0) {
            return true;
        }
    }
    return false;
}

bool IsSaveQueueWaitCall(
    const std::uint8_t* section,
    std::size_t section_size,
    std::size_t call_offset,
    std::uintptr_t iat_slot) {
    constexpr std::size_t kCallSize = 6;
    constexpr std::size_t kLookBehind = 24;
    constexpr std::size_t kLookAhead = 48;
    constexpr std::uint8_t kInfiniteWaitSetup[] = {
        0xBA, 0xFF, 0xFF, 0xFF, 0xFF,  // mov edx, INFINITE
        0x49, 0x8B, 0x0F               // mov rcx, [r15]
    };
    constexpr std::uint8_t kResultTest[] = {0x8B, 0xD8, 0x85, 0xC0};
    constexpr std::uint8_t kSemaphoreDecrement[] = {0xF0, 0x41, 0xFF, 0x4F, 0x10};
    constexpr std::uint8_t kWaitTimeoutCompare[] = {0x3D, 0x02, 0x01, 0x00, 0x00};

    if (call_offset < kLookBehind || call_offset + kCallSize + kLookAhead > section_size) {
        return false;
    }

    const auto* call = section + call_offset;
    if (call[0] != 0xFF || call[1] != 0x15) {
        return false;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, call + 2, sizeof(displacement));
    const auto return_address = reinterpret_cast<std::uintptr_t>(call + kCallSize);
    const auto referenced_slot = static_cast<std::uintptr_t>(
        static_cast<std::intptr_t>(return_address) + displacement);
    if (referenced_slot != iat_slot) {
        return false;
    }

    const auto* before = call - kLookBehind;
    const auto* after = call + kCallSize;
    return ContainsBytes(before, kLookBehind, kInfiniteWaitSetup, std::size(kInfiniteWaitSetup))
        && ContainsBytes(after, 12, kResultTest, std::size(kResultTest))
        && ContainsBytes(after, kLookAhead, kSemaphoreDecrement, std::size(kSemaphoreDecrement))
        && ContainsBytes(after, kLookAhead, kWaitTimeoutCompare, std::size(kWaitTimeoutCompare));
}

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

    ModuleImage image;
    if (!TryGetModuleImage(module, image)) {
        return stats;
    }

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(image.nt);
    for (unsigned short i = 0; i < image.nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const std::size_t size = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (size < pattern.bytes.size()) {
            continue;
        }

        const auto* memory = image.base + section->VirtualAddress;
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
        } else if (result.status == Status::Candidate) {
            log::Write("[resolver] %-20s status=Candidate pattern=%zu address=%p validation=pending\n",
                definition.name,
                result.matched_pattern,
                reinterpret_cast<void*>(result.address));
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

bool ValidatePreparedLocalSaveRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::PreparedLocalSave)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] PreparedLocalSave    status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateUiSaveRequestRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::UiSaveRequest)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] UiSaveRequest       status=%s validation=blocked reason=strict-current-signature-required\n",
            StatusName(result.status));
        return false;
    }

    result.status = Status::Validated;
    return true;
}

bool ValidateSaveServiceTickRuntime() {
    Result& result = g_results[static_cast<std::size_t>(symbols::SymbolId::SaveServiceTick)];
    if (result.status != Status::Candidate || result.matched_pattern != 1 || result.address == 0) {
        log::Write("[resolver] SaveServiceTick     status=%s validation=blocked reason=strict-current-signature-required\n",
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

    constexpr std::size_t kSearchSize = 0x430;
    constexpr std::size_t kRipInstructionSize = 7;
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(build_visible_map);
    __try {
        for (std::size_t offset = 0; offset + kRipInstructionSize <= kSearchSize; ++offset) {
            if (bytes[offset] != 0x48 || bytes[offset + 1] != 0x8B || bytes[offset + 2] != 0x05) {
                continue;
            }
            if (offset + 14 > kSearchSize
                || bytes[offset + 7] != 0x48
                || bytes[offset + 8] != 0x8B
                || bytes[offset + 9] != 0x88
                || bytes[offset + 10] != 0xB0
                || bytes[offset + 11] != 0x00
                || bytes[offset + 12] != 0x00
                || bytes[offset + 13] != 0x00) {
                continue;
            }

            std::int32_t displacement = 0;
            std::memcpy(&displacement, bytes + offset + 3, sizeof(displacement));
            const std::uintptr_t global_address = build_visible_map + offset + kRipInstructionSize + displacement;
            log::Write("[resolver] GameStateGlobal    status=Candidate address=%p source=BuildVisibleMap+0x%zX\n",
                reinterpret_cast<void*>(global_address),
                offset);
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

std::uintptr_t TryResolveSaveQueueWaitBoundary() {
    HMODULE game_module = GetModuleHandleW(nullptr);
    ModuleImage image;
    if (!TryGetModuleImage(game_module, image)) {
        log::Write("[resolver] SaveQueueWaitBoundary status=Unresolved reason=invalid-game-image\n");
        return 0;
    }

    std::uintptr_t iat_slot = FindImportAddressTableSlot(
        game_module,
        "KERNEL32.dll",
        "WaitForSingleObject");
    if (iat_slot == 0) {
        iat_slot = FindImportAddressTableSlot(
            game_module,
            "KERNEL32",
            "WaitForSingleObject");
    }
    if (iat_slot == 0) {
        log::Write("[resolver] SaveQueueWaitBoundary status=Unresolved reason=wait-import-not-found\n");
        return 0;
    }

    std::size_t hit_count = 0;
    std::uintptr_t first_return_address = 0;
    __try {
        IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(image.nt);
        for (unsigned short section_index = 0;
             section_index < image.nt->FileHeader.NumberOfSections;
             ++section_index, ++section) {
            if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
                continue;
            }

            const std::size_t section_size = static_cast<std::size_t>(section->Misc.VirtualSize);
            if (section_size < 80
                || !IsImageRangeValid(image, section->VirtualAddress, section_size)) {
                continue;
            }

            const auto* memory = image.base + section->VirtualAddress;
            for (std::size_t offset = 24; offset + 54 <= section_size; ++offset) {
                if (!IsSaveQueueWaitCall(memory, section_size, offset, iat_slot)) {
                    continue;
                }

                ++hit_count;
                if (first_return_address == 0) {
                    first_return_address = reinterpret_cast<std::uintptr_t>(memory + offset + 6);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        log::Write("[resolver] SaveQueueWaitBoundary status=Unresolved reason=exception code=0x%08lX\n",
            static_cast<unsigned long>(GetExceptionCode()));
        return 0;
    }

    if (hit_count != 1) {
        log::Write("[resolver] SaveQueueWaitBoundary status=%s hits=%zu validation=blocked\n",
            hit_count == 0 ? "Unresolved" : "Ambiguous",
            hit_count);
        return 0;
    }

    const auto game_base = reinterpret_cast<std::uintptr_t>(image.base);
    log::Write("[resolver] SaveQueueWaitBoundary status=Validated return=%p rva=0x%llX iat=%p source=semantic-import-call\n",
        reinterpret_cast<void*>(first_return_address),
        static_cast<unsigned long long>(first_return_address - game_base),
        reinterpret_cast<void*>(iat_slot));
    return first_return_address;
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
