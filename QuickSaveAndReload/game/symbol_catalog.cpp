#include "pch.h"

#include "include/symbol_catalog.h"

namespace qsr::symbols {
namespace {

constexpr const char* kDirectLocalSaveStrict =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 44 89 44 24 18 55 41 54 41 55 41 56 41 57 "
    "48 89 E5 48 83 EC 60 4D 89 CC 45 89 C5 49 89 D6 49 89 CF 80 3D ?? ?? ?? ?? 00 75 ?? "
    "C7 02 00 00 00 00 E9 ?? ?? ?? ??";
constexpr const char* kDirectLocalSaveRelaxed =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 44 89 44 24 18 55 41 54 41 55 41 56 41 57 "
    "48 89 E5 48 83 EC 60 4D 89 CC 45 89 C5 49 89 D6 49 89 CF";

constexpr const char* kSaveServiceDriverStrict =
    "FF 90 D8 00 00 00 48 89 C6 48 8B 4B 68 48 8B 51 20 48 85 D2 "
    "48 8D 3D ?? ?? ?? ?? 74 ?? 48 8B 7A 70";
constexpr const char* kSaveServiceDriverRelaxed =
    "FF 90 D8 00 00 00 48 89 C6 48 8B 4B 68 48 8B 51 20 48 85 D2";

constexpr const char* kServiceCommandBuildStrict =
    "48 89 5C 24 10 4C 89 4C 24 20 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 D9 48 81 EC C0 00 00 00 49 8B F1 4D 8B E8 4C 8B F2 4C 8B E1 "
    "48 8B 05 ?? ?? ?? ?? 4C 8B 38";
constexpr const char* kServiceCommandBuildRelaxed =
    "48 89 5C 24 10 4C 89 4C 24 20 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 D9 48 81 EC C0 00 00 00 49 8B F1 4D 8B E8 4C 8B F2 4C 8B E1";

constexpr const char* kInGameMenuLoadCoreStrict =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC D0 01 00 00 48 89 D3 48 89 CF 31 F6 89 74 24 20 "
    "40 38 B1 C2 0C 00 00 0F 84 ?? ?? ?? ?? 41 83 F9 02 76 ?? 41 8D 41 9C 83 F8 08";
constexpr const char* kInGameMenuLoadCoreRelaxed =
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 81 EC D0 01 00 00 48 89 D3 48 89 CF 31 F6 89 74 24 20 "
    "40 38 B1 C2 0C 00 00";

constexpr const char* kBuildVisibleMapStrict =
    "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 D0 48 81 EC 30 01 00 00 45 0F B6 F9 41 8B F0 4C 8B F1";
constexpr const char* kBuildVisibleMapRelaxed =
    "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 D0 48 81 EC 30 01 00 00";

constexpr const char* kLoadSelectedRefreshStrict =
    "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 E7 FF FF B8 50 19 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 48 8B D9 45 33 C9 44 89 4C 24 30 48 8B 81 60 01 00 00 "
    "44 8B B0 20 01 00 00";
constexpr const char* kLoadSelectedRefreshRelaxed =
    "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 E7 FF FF B8 50 19 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 48 8B D9";

constexpr const char* kLoadModalHandlerStrict =
    "40 53 48 83 EC 20 4D 89 CA 48 89 CB 48 39 91 58 01 00 00 74 ?? "
    "48 39 91 60 01 00 00 0F 85 ?? ?? ?? ?? 48 39 91 60 01 00 00 0F 94 D0";
constexpr const char* kLoadModalHandlerRelaxed =
    "40 53 48 83 EC 20 4D 89 CA 48 89 CB 48 39 91 58 01 00 00";

constexpr const char* kRenderSlotRowStrict =
    "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 EE FF FF B8 30 12 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 45 0F B6 F1 45 0F B6 F8 48 8B FA 48 8B D9";
constexpr const char* kRenderSlotRowRelaxed =
    "48 89 5C 24 20 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 D0 EE FF FF B8 30 12 00 00";

constexpr const char* kSetControlTextStrict =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D AC 24 80 F0 FF FF B8 80 10 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 48 8B F1 48 8B 81 A8 00 00 00 48 85 C0 74 ?? 48 8B 48 08 48 85 C9";
constexpr const char* kSetControlTextRelaxed =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 56 48 8D AC 24 80 F0 FF FF B8 80 10 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 48 8B F1";

constexpr std::array<Definition, static_cast<std::size_t>(SymbolId::Count)> kCatalog{{
    {SymbolId::DirectLocalSave, "DirectLocalSave", health::FeatureId::CoreSave, true, 0, {kDirectLocalSaveStrict, kDirectLocalSaveRelaxed}},
    {SymbolId::SaveServiceDriver, "SaveServiceDriver", health::FeatureId::CoreSave, true, -0x20, {kSaveServiceDriverStrict, kSaveServiceDriverRelaxed}},
    {SymbolId::ServiceCommandBuild, "ServiceCommandBuild", health::FeatureId::CoreSave, false, 0, {kServiceCommandBuildStrict, kServiceCommandBuildRelaxed}},
    {SymbolId::InGameMenuLoadCore, "InGameMenuLoadCore", health::FeatureId::CoreLoad, true, 0, {kInGameMenuLoadCoreStrict, kInGameMenuLoadCoreRelaxed}},
    {SymbolId::BuildVisibleMap, "BuildVisibleMap", health::FeatureId::LoadUi, false, 0, {kBuildVisibleMapStrict, kBuildVisibleMapRelaxed}},
    {SymbolId::LoadSelectedRefresh, "LoadSelectedRefresh", health::FeatureId::LoadUi, false, 0, {kLoadSelectedRefreshStrict, kLoadSelectedRefreshRelaxed}},
    {SymbolId::LoadModalHandler, "LoadModalHandler", health::FeatureId::LoadUi, false, 0, {kLoadModalHandlerStrict, kLoadModalHandlerRelaxed}},
    {SymbolId::RenderSlotRow, "RenderSlotRow", health::FeatureId::LoadUi, false, 0, {kRenderSlotRowStrict, kRenderSlotRowRelaxed}},
    {SymbolId::SetControlText, "SetControlText", health::FeatureId::LoadUi, false, 0, {kSetControlTextStrict, kSetControlTextRelaxed}},
}};

}  // namespace

const std::array<Definition, static_cast<std::size_t>(SymbolId::Count)>& Catalog() {
    return kCatalog;
}

const char* SymbolName(SymbolId id) {
    return kCatalog[static_cast<std::size_t>(id)].name;
}

}  // namespace qsr::symbols
