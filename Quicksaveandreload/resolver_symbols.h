#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace quicksave::resolver {

enum class SymbolId : std::uint32_t {
    DirectLocalSave = 0,
    SavePrecheck,
    WeatherTickAnchor,
    WeatherFrameHeartbeat,
    SaveServiceDriver,
    ServiceChildPoll,
    InGameMenuLoadCore,
    BuildVisibleMap,
    LoadListEventThunk,
    LoadSelectedRefresh,
    LoadModalHandler,
    GameServiceGlobal,
    GameStateGlobal,
    SaveManagerGlobal,
    RenderSlotRow,
    ResolveUiScript,
    SetControlText,
    AcquireClientActorScope,
    AcquireClientUserActorScope,
    ScopeSpecialRelease,
    Count
};

enum class FeatureGroup : std::uint32_t {
    CoreSave = 0,
    CoreLoad,
    LoadUi,
    Toast,
    Support,
    Count
};

struct NativeToastBridge {
    void* root_global = nullptr;
    std::uint32_t outer_offset = 0;
    std::ptrdiff_t manager_offset = 0;
    void* create_string = nullptr;
    void* push = nullptr;
    void* release_string = nullptr;
};

using LogSinkFn = void(*)(const char*);

bool Initialize(HMODULE module, LogSinkFn log_sink, bool log_enabled);
void Shutdown();
std::uintptr_t Address(SymbolId id);
bool IsFeatureEnabled(FeatureGroup group);
const NativeToastBridge& ToastBridge();

}  // namespace quicksave::resolver
