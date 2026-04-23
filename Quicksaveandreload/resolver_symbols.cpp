#include "pch.h"

#include "resolver_symbols.h"

#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace quicksave::resolver {
namespace {

enum class ResolveMode : std::uint8_t {
    StaticRva = 0,
    AobExecutable,
    AobAnySection
};

struct SymbolDef {
    SymbolId id;
    const char* name;
    FeatureGroup group;
    bool required;
    ResolveMode mode;
    std::uintptr_t known_rva;
    std::array<const char*, 3> patterns;
    bool rip_relative_target;
    std::uint8_t rip_relative_offset;
};

struct SymbolState {
    std::uintptr_t address = 0;
    std::uint8_t matched_pattern = 0;
    bool used_known_fallback = false;
    bool resolved = false;
};

HMODULE g_module = nullptr;
std::uintptr_t g_base = 0;
LogSinkFn g_log_sink = nullptr;
bool g_log_enabled = false;
std::array<SymbolState, static_cast<std::size_t>(SymbolId::Count)> g_symbols{};
std::array<bool, static_cast<std::size_t>(FeatureGroup::Count)> g_feature_enabled{};
NativeToastBridge g_toast_bridge{};

constexpr const char* kAobDirectLocalSaveStrict =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 44 89 44 24 18 55 41 54 41 55 41 56 41 57 "
    "48 89 E5 48 83 EC 60 4D 89 CD 45 89 C4 49 89 D6 49 89 CF 80 3D ?? ?? ?? ?? 00 75 ?? C7 02 00 00 00 00 E9 ?? ?? ?? ??";
constexpr const char* kAobDirectLocalSaveRelaxed =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 44 89 44 24 18 55 41 54 41 55 41 56 41 57 48 89 E5 48 83 EC 60 4D 89 CD 45 89 C4 49 89 D6 49 89 CF";

constexpr const char* kAobSavePrecheckStrict =
    "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 56 41 57 48 89 E5 48 83 EC 50 4C 89 CF 4C 89 C6 49 89 D6 31 DB "
    "48 8D 55 D8 49 8B 88 A0 00 00 00 E8 ?? ?? ?? ?? 90 44 0F B6 7D E8 48 8B 56 08 48 8D 4D 40";
constexpr const char* kAobSavePrecheckRelaxed =
    "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 56 41 57 48 89 E5 48 83 EC 50 4C 89 CF 4C 89 C6 49 89 D6 31 DB";

constexpr const char* kAobWeatherTickAnchorStrict =
    "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 D8 0E 00 00";
constexpr const char* kAobWeatherTickAnchorRelaxed =
    "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40";

constexpr const char* kAobSaveServiceDriverStrict =
    "48 89 4C 24 08 53 55 56 57 41 56 48 83 EC 60 48 89 D3 48 8B 02 48 8D 94 24 A0 00 00 00 "
    "48 89 D9 FF 90 D8 00 00 00 48 89 C6 48 8B 4B 68 48 8B 51 20 48 85 D2 48 8D 3D ?? ?? ?? ?? 74 04 48 8B 7A";
constexpr const char* kAobSaveServiceDriverRelaxed =
    "48 89 4C 24 08 53 55 56 57 41 56 48 83 EC 60 48 89 D3 48 8B 02 48 8D 94 24 A0 00 00 00 48 89 D9 FF 90 D8 00 00 00 48 89 C6 48 8B 4B 68 48 8B 51 20 48 85 D2";

constexpr const char* kAobServiceChildPollStrict =
    "48 89 5C 24 10 48 89 6C 24 20 56 57 41 56 48 83 EC 70 49 89 D6 48 89 CD 80 B9 95 00 00 00 00 75 ?? "
    "48 8B 79 78 8B B1 80 00 00 00 48 C1 E6 04 48 01 FE 48 39 F7 74 ?? 66 0F 1F 84 00 00 00 00 00";
constexpr const char* kAobServiceChildPollRelaxed =
    "48 89 5C 24 10 48 89 6C 24 20 56 57 41 56 48 83 EC 70 49 89 D6 48 89 CD 80 B9 95 00 00 00 00 75 ?? 48 8B 79 78 8B B1 80 00 00 00";

constexpr const char* kAobInGameMenuLoadCoreStrict =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 "
    "48 89 D3 48 89 CF 31 F6 89 74 24 20 40 38 B1 CA 0C 00 00 0F 84 ?? ?? ?? ?? 41 83 F9 02";
constexpr const char* kAobInGameMenuLoadCoreRelaxed =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D AC 24 50 FF FF FF 48 81 EC B0 01 00 00 48 89 D3 48 89 CF 31 F6";

constexpr const char* kAobBuildVisibleMapStrict =
    "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 78 33 F6 89 B1 A0 01 00 00 "
    "48 8D B9 98 01 00 00 48 89 7D A8 8D 56 0F 48 8B CF E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 4C 8B B8 B0 00 00 00";
constexpr const char* kAobBuildVisibleMapRelaxed =
    "48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8B EC 48 83 EC 78 33 F6 89 B1 A0 01 00 00 48 8D B9 98 01 00 00";

constexpr const char* kAobLoadListEventThunkStrict =
    "48 89 5C 24 08 57 48 81 EC 20 0B 00 00 48 8B D9 4C 39 81 38 01 00 00 0F 85 ?? ?? ?? ?? "
    "45 84 C9 0F 85 ?? ?? ?? ?? 48 8B 84 24 58 0B 00 00 F3 0F 10 40 04 0F 57 C9 0F 2F C8 76 ?? "
    "48 8B 99 28 01 00 00";
constexpr const char* kAobLoadListEventThunkRelaxed =
    "48 89 5C 24 08 57 48 81 EC 20 0B 00 00 48 8B D9 4C 39 81 38 01 00 00 0F 85 ?? ?? ?? ?? 45 84 C9 0F 85 ?? ?? ?? ??";

constexpr const char* kAobLoadSelectedRefreshStrict =
    "48 89 5C 24 20 55 56 57 41 56 41 57 48 81 EC 30 0B 00 00 48 8B D9 48 8B 81 28 01 00 00 8B 88 18 01 00 00 "
    "39 8B A0 01 00 00 0F 86 ?? ?? ?? ?? 48 8B 83 98 01 00 00 8B 0C 88 83 F9 FF 0F 84 ?? ?? ?? ??";
constexpr const char* kAobLoadSelectedRefreshRelaxed =
    "48 89 5C 24 20 55 56 57 41 56 41 57 48 81 EC 30 0B 00 00 48 8B D9 48 8B 81 28 01 00 00 8B 88 18 01 00 00 39 8B A0 01 00 00";

constexpr const char* kAobLoadModalHandlerStrict =
    "48 89 5C 24 18 55 56 57 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 49 8B F1 4C 8B F1 45 33 FF 48 8B 99 70 01 00 00 "
    "48 3B DA 0F 85 ?? ?? ?? ?? 45 84 C0 0F 84 ?? ?? ?? ??";
constexpr const char* kAobLoadModalHandlerRelaxed =
    "48 89 5C 24 18 55 56 57 41 56 41 57 48 8D 6C 24 80 48 81 EC 80 01 00 00 49 8B F1 4C 8B F1 45 33 FF 48 8B 99 70 01 00 00 48 3B DA";

constexpr const char* kAobGameServiceGlobalStrict =
    "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 49 50 E8 ?? ?? ?? ?? 84 C0 0F 94 C0 48 83 C4 28 C3";
constexpr const char* kAobGameServiceGlobalRelaxed =
    "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 49 50 E8 ?? ?? ?? ?? 84 C0";

constexpr const char* kAobGameStateGlobalStrict =
    "48 83 EC 28 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 80 38 00 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 51 68 48 8B 4A 60 48 8B D0 48 8B 09 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 A8 00 00 00 48 85 C9 74 ?? 48 8B 49 10 48 83 C4 28 E9 ?? ?? ?? ?? 48 83 C4 28 E9 ?? ?? ?? ?? 48 83 C4 28 C3";
constexpr const char* kAobGameStateGlobalRelaxed =
    "48 83 EC 28 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 80 38 00 74 ?? 48 8B 0D ?? ?? ?? ?? 48 8B 51 68 48 8B 4A 60 48 8B D0 48 8B 09 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 88 A8 00 00 00 48 85 C9 74 ?? 48 8B 49 10 48 83 C4 28 E9 ?? ?? ?? ?? 48 83 C4 28 E9 ?? ?? ?? ?? 48 83 C4 28 C3";

constexpr const char* kAobSaveManagerGlobalStrict =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F1 E8 ?? ?? ?? ?? 48 C7 86 A0 00 00 00 00 00 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 38";
constexpr const char* kAobSaveManagerGlobalRelaxed =
    "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8B F1 E8 ?? ?? ?? ?? 48 C7 86 A0 00 00 00 00 00 00 00 48 8B 05 ?? ?? ?? ?? 48 8B 38";

constexpr const char* kAobRenderSlotRowStrict =
    "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 D0 EE FF FF B8 30 12 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 45 0F B6 F0 48 8B FA 48 8B D9 45 84 C0 74 ?? 48 85 D2 75 ?? "
    "40 B6 01 EB ?? 40 32 F6";
constexpr const char* kAobRenderSlotRowRelaxed =
    "40 55 53 56 57 41 54 41 56 41 57 48 8D AC 24 D0 EE FF FF B8 30 12 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 45 0F B6 F0 48 8B FA 48 8B D9";

constexpr const char* kAobResolveUiScriptStrict =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 90 F6 FF FF 48 81 EC 70 0A 00 00 "
    "48 8B FA 4C 8B F1 48 8B 81 A8 00 00 00 48 85 C0 74 ?? 48 83 78 08 00 0F 85 ?? ?? ?? ??";
constexpr const char* kAobResolveUiScriptRelaxed =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D AC 24 90 F6 FF FF 48 81 EC 70 0A 00 00";

constexpr const char* kAobSetControlTextStrict =
    "40 55 53 56 57 41 56 48 8D AC 24 70 F0 FF FF B8 90 10 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B F1 "
    "48 8B 81 A8 00 00 00 48 85 C0 74 ?? 48 8B 48 08 48 85 C9 74 ?? E8 ?? ?? ?? ?? "
    "48 81 C4 90 10 00 00 41 5E 5F 5E 5B 5D C3";
constexpr const char* kAobSetControlTextRelaxed =
    "40 55 53 56 57 41 56 48 8D AC 24 70 F0 FF FF B8 90 10 00 00 E8 ?? ?? ?? ?? 48 2B E0 48 8B F1 "
    "48 8B 81 A8 00 00 00 48 85 C0 74 ?? 48 8B 48 08 48 85 C9";

constexpr const char* kAobAcquireClientActorScopeStrict =
    "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC 90 00 00 00 "
    "4C 8B F2 4C 8B E9 33 FF 8B F7 89 7D C7 48 8B 82 A0 00 00 00 48 85 C0 74 ?? 8B 50 60";
constexpr const char* kAobAcquireClientActorScopeRelaxed =
    "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC 90 00 00 00 "
    "4C 8B F2 4C 8B E9 33 FF 8B F7 89 7D C7 48 8B 82 A0 00 00 00 48 85 C0 74 ?? 8B 50 60";

constexpr const char* kAobAcquireClientUserActorScopeStrict =
    "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 50 48 8B DA 48 8B F9 C7 44 24 20 00 00 00 00 "
    "48 8B 51 28 48 85 D2 75 ?? 48 8B CB E8 ?? ?? ?? ?? 90 48 8D 05 ?? ?? ?? ?? 48 89 03 C7 44 24 20 01 00 00 00";
constexpr const char* kAobAcquireClientUserActorScopeRelaxed =
    "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 50 48 8B DA 48 8B F9 C7 44 24 20 00 00 00 00 48 8B 51 28 48 85 D2";

constexpr const char* kAobScopeSpecialReleaseStrict =
    "40 57 48 83 EC 70 48 8B F9 84 D2 0F 85 ?? ?? ?? ?? 48 89 5C 24 68 48 89 8C 24 90 00 00 00 E8 ?? ?? ?? ?? "
    "48 8B D8 C7 84 24 80 00 00 00 01 00 00 00";
constexpr const char* kAobScopeSpecialReleaseRelaxed =
    "40 57 48 83 EC 70 48 8B F9 84 D2 0F 85 ?? ?? ?? ?? 48 89 5C 24 68 48 89 8C 24 90 00 00 00";

constexpr std::array<SymbolDef, static_cast<std::size_t>(SymbolId::Count)> kSymbols{{
    {SymbolId::DirectLocalSave, "DirectLocalSave", FeatureGroup::CoreSave, true, ResolveMode::AobExecutable, 0x117011D0, {kAobDirectLocalSaveStrict, kAobDirectLocalSaveRelaxed, nullptr}},
    {SymbolId::SavePrecheck, "SavePrecheck", FeatureGroup::Support, false, ResolveMode::AobExecutable, 0x0D00D460, {kAobSavePrecheckStrict, kAobSavePrecheckRelaxed, nullptr}},
    {SymbolId::WeatherTickAnchor, "WeatherTickAnchor", FeatureGroup::Support, false, ResolveMode::AobAnySection, 0x035F9099, {kAobWeatherTickAnchorStrict, kAobWeatherTickAnchorRelaxed, nullptr}},
    {SymbolId::SaveServiceDriver, "SaveServiceDriver", FeatureGroup::CoreSave, true, ResolveMode::AobExecutable, 0x117AD330, {kAobSaveServiceDriverStrict, kAobSaveServiceDriverRelaxed, nullptr}},
    {SymbolId::ServiceChildPoll, "ServiceChildPoll", FeatureGroup::CoreSave, true, ResolveMode::AobExecutable, 0x116D7380, {kAobServiceChildPollStrict, kAobServiceChildPollRelaxed, nullptr}},
    {SymbolId::InGameMenuLoadCore, "InGameMenuLoadCore", FeatureGroup::CoreLoad, true, ResolveMode::AobExecutable, 0x08F8F870, {kAobInGameMenuLoadCoreStrict, kAobInGameMenuLoadCoreRelaxed, nullptr}},
    {SymbolId::BuildVisibleMap, "BuildVisibleMap", FeatureGroup::LoadUi, true, ResolveMode::AobExecutable, 0x00DBCB40, {kAobBuildVisibleMapStrict, kAobBuildVisibleMapRelaxed, nullptr}},
    {SymbolId::LoadListEventThunk, "LoadListEventThunk", FeatureGroup::LoadUi, false, ResolveMode::AobExecutable, 0x00DBC4A0, {kAobLoadListEventThunkStrict, kAobLoadListEventThunkRelaxed, nullptr}},
    {SymbolId::LoadSelectedRefresh, "LoadSelectedRefresh", FeatureGroup::LoadUi, true, ResolveMode::AobExecutable, 0x00DBD770, {kAobLoadSelectedRefreshStrict, kAobLoadSelectedRefreshRelaxed, nullptr}},
    {SymbolId::LoadModalHandler, "LoadModalHandler", FeatureGroup::LoadUi, true, ResolveMode::AobExecutable, 0x00DBDBA0, {kAobLoadModalHandlerStrict, kAobLoadModalHandlerRelaxed, nullptr}},
    {SymbolId::GameServiceGlobal, "GameServiceGlobal", FeatureGroup::CoreLoad, true, ResolveMode::AobAnySection, 0x05F0B1D0, {kAobGameServiceGlobalStrict, kAobGameServiceGlobalRelaxed, nullptr}, true, 4},
    {SymbolId::GameStateGlobal, "GameStateGlobal", FeatureGroup::LoadUi, true, ResolveMode::AobAnySection, 0x05F0B228, {kAobGameStateGlobalStrict, kAobGameStateGlobalRelaxed, nullptr}, true, 19},
    {SymbolId::SaveManagerGlobal, "SaveManagerGlobal", FeatureGroup::Support, false, ResolveMode::AobAnySection, 0x05F0B568, {kAobSaveManagerGlobalStrict, kAobSaveManagerGlobalRelaxed, nullptr}, true, 34},
    {SymbolId::RenderSlotRow, "RenderSlotRow", FeatureGroup::LoadUi, true, ResolveMode::AobExecutable, 0x00DBE2B0, {kAobRenderSlotRowStrict, kAobRenderSlotRowRelaxed, nullptr}},
    {SymbolId::ResolveUiScript, "ResolveUiScript", FeatureGroup::LoadUi, true, ResolveMode::AobExecutable, 0x035499B0, {kAobResolveUiScriptStrict, kAobResolveUiScriptRelaxed, nullptr}},
    {SymbolId::SetControlText, "SetControlText", FeatureGroup::LoadUi, true, ResolveMode::AobExecutable, 0x03545AB0, {kAobSetControlTextStrict, kAobSetControlTextRelaxed, nullptr}},
    {SymbolId::AcquireClientActorScope, "AcquireClientActorScope", FeatureGroup::Support, false, ResolveMode::AobExecutable, 0x006EDBD0, {kAobAcquireClientActorScopeStrict, kAobAcquireClientActorScopeRelaxed, nullptr}},
    {SymbolId::AcquireClientUserActorScope, "AcquireClientUserActorScope", FeatureGroup::CoreLoad, true, ResolveMode::AobExecutable, 0x006EDF90, {kAobAcquireClientUserActorScopeStrict, kAobAcquireClientUserActorScopeRelaxed, nullptr}},
    {SymbolId::ScopeSpecialRelease, "ScopeSpecialRelease", FeatureGroup::CoreLoad, true, ResolveMode::AobExecutable, 0x010C15A0, {kAobScopeSpecialReleaseStrict, kAobScopeSpecialReleaseRelaxed, nullptr}},
}};

constexpr const char* kToastBridgeStrict =
    "48 8B 05 ?? ?? ?? ?? 48 8B 48 ?? 48 8B 99 ?? ?? ?? ??";

void Logf(const char* fmt, ...) {
    if (!g_log_enabled || g_log_sink == nullptr || fmt == nullptr) {
        return;
    }

    char buffer[1024] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_log_sink(buffer);
}

std::pair<std::vector<std::uint8_t>, std::vector<std::uint8_t>> ParsePattern(const char* pattern) {
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint8_t> mask;
    if (pattern == nullptr) {
        return {bytes, mask};
    }

    const char* cursor = pattern;
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    while (*cursor != '\0') {
        while (*cursor == ' ') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (cursor[0] == '?' && cursor[1] == '?') {
            bytes.push_back(0);
            mask.push_back(0);
            cursor += 2;
            continue;
        }
        const int hi = hex(cursor[0]);
        const int lo = hex(cursor[1]);
        if (hi < 0 || lo < 0) {
            bytes.clear();
            mask.clear();
            return {bytes, mask};
        }
        bytes.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
        mask.push_back(0xFF);
        cursor += 2;
    }

    if (bytes.empty() || bytes.size() != mask.size()) {
        bytes.clear();
        mask.clear();
    }
    return {bytes, mask};
}

bool MatchAt(std::uintptr_t address, const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& mask) {
    if (address == 0 || bytes.empty() || bytes.size() != mask.size()) {
        return false;
    }
    const auto* mem = reinterpret_cast<const std::uint8_t*>(address);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        if (mask[i] != 0 && mem[i] != bytes[i]) {
            return false;
        }
    }
    return true;
}

struct ScanStats {
    std::uintptr_t first = 0;
    std::size_t hits = 0;
};

bool IsSectionEligible(const IMAGE_SECTION_HEADER& section, ResolveMode mode) {
    if (mode == ResolveMode::AobExecutable) {
        return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
    }
    if (mode == ResolveMode::AobAnySection) {
        return section.SizeOfRawData != 0 || section.Misc.VirtualSize != 0;
    }
    return false;
}

ScanStats ScanImage(const std::vector<std::uint8_t>& bytes, const std::vector<std::uint8_t>& mask, ResolveMode mode) {
    ScanStats stats{};
    if (g_base == 0 || bytes.empty() || bytes.size() != mask.size()) {
        return stats;
    }

    const auto* base = reinterpret_cast<const std::uint8_t*>(g_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return stats;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return stats;
    }

    const auto* section = IMAGE_FIRST_SECTION(nt);
    const std::size_t needle_size = bytes.size();
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!IsSectionEligible(*section, mode)) {
            continue;
        }
        const std::size_t section_size = static_cast<std::size_t>(section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize : section->SizeOfRawData);
        if (section_size < needle_size) {
            continue;
        }
        const auto* begin = base + section->VirtualAddress;
        const auto* end = begin + (section_size - needle_size + 1);
        for (const auto* ptr = begin; ptr < end; ++ptr) {
            bool matched = true;
            for (std::size_t j = 0; j < needle_size; ++j) {
                if (mask[j] != 0 && ptr[j] != bytes[j]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                if (stats.first == 0) {
                    stats.first = reinterpret_cast<std::uintptr_t>(ptr);
                }
                ++stats.hits;
                if (stats.hits > 1024) {
                    return stats;
                }
            }
        }
    }

    return stats;
}

std::vector<std::uintptr_t> ScanImageHits(
    const std::vector<std::uint8_t>& bytes,
    const std::vector<std::uint8_t>& mask,
    ResolveMode mode,
    std::size_t max_hits) {
    std::vector<std::uintptr_t> hits;
    if (g_base == 0 || bytes.empty() || bytes.size() != mask.size() || max_hits == 0) {
        return hits;
    }

    const auto* base = reinterpret_cast<const std::uint8_t*>(g_base);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return hits;
    }

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return hits;
    }

    const auto* section = IMAGE_FIRST_SECTION(nt);
    const std::size_t needle_size = bytes.size();
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!IsSectionEligible(*section, mode)) {
            continue;
        }
        const std::size_t section_size = static_cast<std::size_t>(section->Misc.VirtualSize != 0 ? section->Misc.VirtualSize : section->SizeOfRawData);
        if (section_size < needle_size) {
            continue;
        }
        const auto* begin = base + section->VirtualAddress;
        const auto* end = begin + (section_size - needle_size + 1);
        for (const auto* ptr = begin; ptr < end; ++ptr) {
            bool matched = true;
            for (std::size_t j = 0; j < needle_size; ++j) {
                if (mask[j] != 0 && ptr[j] != bytes[j]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                hits.push_back(reinterpret_cast<std::uintptr_t>(ptr));
                if (hits.size() >= max_hits) {
                    return hits;
                }
            }
        }
    }

    return hits;
}

std::uintptr_t ReadRipRelative(std::uintptr_t site) {
    if (site == 0) {
        return 0;
    }
    __try {
        const auto displacement = *reinterpret_cast<const std::int32_t*>(site + 3);
        return site + 7 + static_cast<std::intptr_t>(displacement);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uintptr_t ReadCallTarget(std::uintptr_t site) {
    if (site == 0) {
        return 0;
    }
    __try {
        if (*reinterpret_cast<const std::uint8_t*>(site) != 0xE8) {
            return 0;
        }
        const auto displacement = *reinterpret_cast<const std::int32_t*>(site + 1);
        return site + 5 + static_cast<std::intptr_t>(displacement);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadToastBridgeFields(std::uintptr_t site, std::uint32_t& outer_offset, std::uint32_t& manager_offset) {
    if (site == 0) {
        return false;
    }
    __try {
        outer_offset = static_cast<std::uint32_t>(*reinterpret_cast<const std::uint8_t*>(site + 10));
        manager_offset = *reinterpret_cast<const std::uint32_t*>(site + 14);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uintptr_t ResolveAddressFromMatch(const SymbolDef& def, std::uintptr_t match) {
    if (match == 0) {
        return 0;
    }
    if (!def.rip_relative_target) {
        return match;
    }
    return ReadRipRelative(match + def.rip_relative_offset);
}

std::uintptr_t ResolveSymbol(const SymbolDef& def) {
    auto& state = g_symbols[static_cast<std::size_t>(def.id)];
    state = {};

    if (def.mode == ResolveMode::StaticRva) {
        state.address = g_base + def.known_rva;
        state.resolved = true;
        Logf("[AOB] %-24s mode=static addr=%p rva=0x%llX\n",
             def.name,
             reinterpret_cast<void*>(state.address),
             static_cast<unsigned long long>(def.known_rva));
        return state.address;
    }

    for (std::size_t i = 0; i < def.patterns.size(); ++i) {
        if (def.patterns[i] == nullptr) {
            continue;
        }
        const auto [bytes, mask] = ParsePattern(def.patterns[i]);
        if (bytes.empty()) {
            continue;
        }
        const ScanStats stats = ScanImage(bytes, mask, def.mode);
        if (stats.hits == 1 && stats.first != 0) {
            state.address = ResolveAddressFromMatch(def, stats.first);
            state.matched_pattern = static_cast<std::uint8_t>(i + 1);
            state.resolved = state.address != 0;
            if (!state.resolved) {
                continue;
            }
            Logf("[AOB] %-24s mode=aob pattern=%u fallback=0 addr=%p\n",
                 def.name,
                 static_cast<unsigned>(state.matched_pattern),
                 reinterpret_cast<void*>(state.address));
            return state.address;
        }
        if (stats.hits > 1) {
            Logf("[AOB] %-24s mode=aob pattern=%u ambiguous hits=%zu\n",
                 def.name,
                 static_cast<unsigned>(i + 1),
                 stats.hits);
        }
    }

    if (def.patterns[0] != nullptr && def.known_rva != 0) {
        const auto [bytes, mask] = ParsePattern(def.patterns[0]);
        const std::uintptr_t fallback = g_base + def.known_rva;
        if (MatchAt(fallback, bytes, mask)) {
            state.address = ResolveAddressFromMatch(def, fallback);
            state.used_known_fallback = true;
            state.resolved = state.address != 0;
            if (!state.resolved) {
                Logf("[AOB] %-24s fallback-rip-read-failed rva=0x%llX\n",
                     def.name,
                     static_cast<unsigned long long>(def.known_rva));
                return 0;
            }
            Logf("[AOB] %-24s mode=aob pattern=0 fallback=1 addr=%p rva=0x%llX\n",
                 def.name,
                 reinterpret_cast<void*>(state.address),
                 static_cast<unsigned long long>(def.known_rva));
            return state.address;
        }
    }

    Logf("[AOB] %-24s unresolved required=%d\n", def.name, def.required ? 1 : 0);
    return 0;
}

bool ResolveToastBridge() {
    g_toast_bridge = {};
    const auto [bytes, mask] = ParsePattern(kToastBridgeStrict);
    if (bytes.empty()) {
        return false;
    }

    const auto hits = ScanImageHits(bytes, mask, ResolveMode::AobExecutable, 256);
    if (hits.empty()) {
        Logf("[AOB] %-24s unresolved required=0\n", "NativeToastBridge");
        return false;
    }

    std::uintptr_t selected_site = 0;
    NativeToastBridge selected{};
    std::size_t valid_candidates = 0;
    std::size_t distinct_candidates = 0;

    for (const std::uintptr_t site : hits) {
        std::uint32_t outer_offset = 0;
        std::uint32_t manager_offset = 0;
        if (!ReadToastBridgeFields(site, outer_offset, manager_offset)) {
            continue;
        }

        std::uintptr_t create_fn = 0;
        std::uintptr_t push_fn = 0;
        std::uintptr_t release_fn = 0;
        const auto* p = reinterpret_cast<const std::uint8_t*>(site);
        for (std::size_t k = 18; k + 8 <= 0x90; ++k) {
            if (!create_fn
                && p[k + 0] == 0x48 && p[k + 1] == 0x8B && p[k + 2] == 0xC8
                && p[k + 3] == 0xE8) {
                create_fn = ReadCallTarget(site + k + 3);
                continue;
            }
            if (!push_fn
                && p[k + 0] == 0x48 && p[k + 1] == 0x8B && p[k + 2] == 0xCB
                && p[k + 3] == 0xE8) {
                push_fn = ReadCallTarget(site + k + 3);
                continue;
            }
            if (!release_fn
                && p[k + 0] == 0x48 && p[k + 1] == 0x8B
                && p[k + 2] == 0x4C && p[k + 3] == 0x24) {
                std::size_t call_offset = k + 5;
                while (call_offset < 0x90 && p[call_offset] == 0x90) {
                    ++call_offset;
                }
                if (call_offset + 5 <= 0x90 && p[call_offset] == 0xE8) {
                    release_fn = ReadCallTarget(site + call_offset);
                }
            }
        }

        const std::uintptr_t root_global = ReadRipRelative(site);
        if (root_global == 0 || outer_offset == 0 || manager_offset == 0 || create_fn == 0 || push_fn == 0 || release_fn == 0) {
            continue;
        }

        NativeToastBridge candidate{};
        candidate.root_global = reinterpret_cast<void*>(root_global);
        candidate.outer_offset = outer_offset;
        candidate.manager_offset = static_cast<std::ptrdiff_t>(manager_offset);
        candidate.create_string = reinterpret_cast<void*>(create_fn);
        candidate.push = reinterpret_cast<void*>(push_fn);
        candidate.release_string = reinterpret_cast<void*>(release_fn);

        ++valid_candidates;
        if (selected_site == 0) {
            selected_site = site;
            selected = candidate;
            ++distinct_candidates;
            continue;
        }

        const bool same_candidate =
            selected.root_global == candidate.root_global &&
            selected.outer_offset == candidate.outer_offset &&
            selected.manager_offset == candidate.manager_offset &&
            selected.create_string == candidate.create_string &&
            selected.push == candidate.push &&
            selected.release_string == candidate.release_string;
        if (!same_candidate) {
            ++distinct_candidates;
            if (distinct_candidates > 1) {
                break;
            }
        }
    }

    if (valid_candidates == 0 || selected_site == 0) {
        Logf("[AOB] %-24s unresolved required=0\n", "NativeToastBridge");
        return false;
    }
    if (distinct_candidates > 1) {
        Logf("[AOB] %-24s mode=aob ambiguous-valid=%zu raw-hits=%zu\n", "NativeToastBridge", valid_candidates, hits.size());
        return false;
    }

    g_toast_bridge = selected;
    Logf("[AOB] %-24s mode=aob pattern=1 fallback=0 addr=%p\n",
         "NativeToastBridge",
         reinterpret_cast<void*>(selected_site));
    return true;
}

const SymbolDef& Definition(SymbolId id) {
    return kSymbols[static_cast<std::size_t>(id)];
}

}  // namespace

bool Initialize(HMODULE module, LogSinkFn log_sink, bool log_enabled) {
    Shutdown();

    g_module = module;
    g_base = reinterpret_cast<std::uintptr_t>(module != nullptr ? module : GetModuleHandleW(nullptr));
    g_log_sink = log_sink;
    g_log_enabled = log_enabled;
    g_feature_enabled.fill(true);

    for (const auto& def : kSymbols) {
        ResolveSymbol(def);
    }

    std::array<bool, static_cast<std::size_t>(FeatureGroup::Count)> required_ok{};
    required_ok.fill(true);
    std::array<bool, static_cast<std::size_t>(FeatureGroup::Count)> any_unresolved{};
    any_unresolved.fill(false);

    for (const auto& def : kSymbols) {
        const bool resolved = g_symbols[static_cast<std::size_t>(def.id)].resolved;
        if (!resolved) {
            any_unresolved[static_cast<std::size_t>(def.group)] = true;
            if (def.required) {
                required_ok[static_cast<std::size_t>(def.group)] = false;
            }
        }
    }

    g_feature_enabled[static_cast<std::size_t>(FeatureGroup::CoreSave)] =
        required_ok[static_cast<std::size_t>(FeatureGroup::CoreSave)];
    g_feature_enabled[static_cast<std::size_t>(FeatureGroup::CoreLoad)] =
        required_ok[static_cast<std::size_t>(FeatureGroup::CoreLoad)];
    g_feature_enabled[static_cast<std::size_t>(FeatureGroup::LoadUi)] =
        required_ok[static_cast<std::size_t>(FeatureGroup::LoadUi)];
    g_feature_enabled[static_cast<std::size_t>(FeatureGroup::Support)] = !any_unresolved[static_cast<std::size_t>(FeatureGroup::Support)];
    g_feature_enabled[static_cast<std::size_t>(FeatureGroup::Toast)] = ResolveToastBridge();

    if (!g_feature_enabled[static_cast<std::size_t>(FeatureGroup::LoadUi)]) {
        Logf("[AOB] feature=%s disabled unresolved symbols\n", "LoadUi");
    }
    if (!g_feature_enabled[static_cast<std::size_t>(FeatureGroup::Toast)]) {
        Logf("[AOB] feature=%s disabled unresolved symbols\n", "Toast");
    }

    const bool core_save_ready = g_feature_enabled[static_cast<std::size_t>(FeatureGroup::CoreSave)];
    const bool core_load_ready = g_feature_enabled[static_cast<std::size_t>(FeatureGroup::CoreLoad)];
    if (!core_save_ready || !core_load_ready) {
        Logf("[AOB] fatal core-save=%d core-load=%d\n", core_save_ready ? 1 : 0, core_load_ready ? 1 : 0);
        return false;
    }

    return true;
}

void Shutdown() {
    g_module = nullptr;
    g_base = 0;
    g_log_sink = nullptr;
    g_log_enabled = false;
    g_symbols = {};
    g_feature_enabled.fill(false);
    g_toast_bridge = {};
}

std::uintptr_t Address(SymbolId id) {
    return g_symbols[static_cast<std::size_t>(id)].address;
}

bool IsFeatureEnabled(FeatureGroup group) {
    return g_feature_enabled[static_cast<std::size_t>(group)];
}

const NativeToastBridge& ToastBridge() {
    return g_toast_bridge;
}

}  // namespace quicksave::resolver
