#include "pch.h"

#include "include/symbol_catalog.h"

namespace qsr::symbols {
namespace {

constexpr const char* kDirectLocalSaveStrict =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 44 89 44 24 18 55 41 54 41 55 41 56 41 57 "
    "48 89 E5 48 83 EC 60 4D 89 CC 45 89 C5 49 89 D6 49 89 CF 80 3D ?? ?? ?? ?? 00 75 0B "
    "C7 02 00 00 00 00 E9 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B 78 10";
constexpr const char* kDirectLocalSaveRelaxed =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 44 89 44 24 18 55 41 54 41 55 41 56 41 57 "
    "48 89 E5 48 83 EC 60 4D 89 CC 45 89 C5 49 89 D6 49 89 CF";

constexpr const char* kPreparedLocalSaveStrict =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 "
    "48 8B EC 48 83 EC 60 4D 8B E1 4D 8B E8 4C 8B FA 4C 8B F1 41 8B 08 8D 41 9C 83 F8 08 "
    "76 74 8D 41 FD 85 C0 74 6D";
constexpr const char* kPreparedLocalSaveRelaxed =
    "41 8B 08 8D 41 9C 83 F8 08 76 ?? 8D 41 FD 85 C0 74 ?? "
    "C6 45 40 01 C6 45 D0 00 49 8B 06 49 8B CE FF 90 E8 00 00 00";

constexpr const char* kUiSaveRequestStrict =
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 20 "
    "45 89 CF 4D 89 C6 48 89 D7 48 89 CD 48 8B 71 08 48 8B 86 A0 00 00 00 4C 8B 50 68 "
    "49 8B 5A 10";
constexpr const char* kUiSaveRequestRelaxed =
    "45 89 CF 4D 89 C6 48 89 D7 48 89 CD 48 8B 71 08 48 8B 86 A0 00 00 00 4C 8B 50 68";

constexpr const char* kSaveServiceTickStrict =
    "48 89 5C 24 08 44 89 4C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 D9 48 81 EC C0 00 00 00 4C 8B FA 4C 8B 71 08";
constexpr const char* kSaveServiceTickRelaxed =
    "48 89 5C 24 08 44 89 4C 24 20 4C 89 44 24 18 55 56 57 41 54 41 55 41 56 41 57 "
    "48 8D 6C 24 D9 48 81 EC C0 00 00 00";

constexpr const char* kSaveServiceDriverStrict =
    "48 89 5C 24 20 48 89 4C 24 08 55 56 57 41 56 41 57 48 83 EC 50 "
    "48 89 D7 48 8B 02 48 8D 94 24 90 00 00 00 48 89 F9 FF 90 D0 00 00 00 "
    "48 89 C6 48 8B 4F 68 48 8B 51 20 48 85 D2";
constexpr const char* kSaveServiceDriverRelaxed =
    "48 89 5C 24 20 48 89 4C 24 08 55 56 57 41 56 41 57 48 83 EC 50 "
    "48 89 D7 48 8B 02 48 8D 94 24 90 00 00 00 48 89 F9";

constexpr const char* kInGameMenuLoadCoreStrict =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC B0 01 00 00 "
    "48 89 D7 48 89 CB 31 F6 40 38 B1 A0 0C 00 00 74 30 41 83 F9 02 76 63 "
    "41 8D 41 9C 83 F8 08 76 5A";
constexpr const char* kInGameMenuLoadCoreRelaxed =
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 81 EC B0 01 00 00 48 89 D7 48 89 CB 31 F6 "
    "40 38 B1 A0 0C 00 00";

constexpr const char* kBuildVisibleMapStrict =
    "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 "
    "48 8D 6C 24 D0 48 81 EC 30 01 00 00 45 0F B6 F9 41 8B F8 4C 8B F1 33 DB";
constexpr const char* kBuildVisibleMapRelaxed =
    "48 85 D2 0F 84 ?? ?? ?? ?? 48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 41 56 41 57 "
    "48 8D 6C 24 D0 48 81 EC 30 01 00 00";

constexpr const char* kLoadSelectedRefreshStrict =
    "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 D9 FF FF B8 50 27 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 48 8B D9 45 33 ED 44 89 6C 24 30 48 8B 81 60 01 00 00 "
    "44 8B B0 20 01 00 00";
constexpr const char* kLoadSelectedRefreshRelaxed =
    "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B0 D9 FF FF B8 50 27 00 00 "
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
    "48 85 D2 0F 84 ?? ?? ?? ?? 44 88 44 24 18 48 89 54 24 10 48 89 4C 24 08 "
    "55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 38 F0 FF FF B8 C8 10 00 00 "
    "E8 ?? ?? ?? ?? 48 2B E0 45 0F B6 E0 48 8B F2 48 8B D9";
constexpr const char* kSetControlTextRelaxed =
    "48 85 D2 0F 84 ?? ?? ?? ?? 44 88 44 24 18 48 89 54 24 10 48 89 4C 24 08 "
    "55 53 56 57 41 54 41 55 41 56 41 57";

constexpr std::array<Definition, static_cast<std::size_t>(SymbolId::Count)> kCatalog{{
    {SymbolId::DirectLocalSave, "DirectLocalSave", health::FeatureId::CoreSave, true, 0, {kDirectLocalSaveStrict, kDirectLocalSaveRelaxed}},
    {SymbolId::PreparedLocalSave, "PreparedLocalSave", health::FeatureId::CoreSave, false, 0, {kPreparedLocalSaveStrict, kPreparedLocalSaveRelaxed}},
    {SymbolId::UiSaveRequest, "UiSaveRequest", health::FeatureId::CoreSave, false, 0, {kUiSaveRequestStrict, kUiSaveRequestRelaxed}},
    {SymbolId::SaveServiceTick, "SaveServiceTick", health::FeatureId::CoreSave, false, 0, {kSaveServiceTickStrict, kSaveServiceTickRelaxed}},
    {SymbolId::SaveServiceDriver, "SaveServiceDriver", health::FeatureId::CoreSave, true, 0, {kSaveServiceDriverStrict, kSaveServiceDriverRelaxed}},
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
