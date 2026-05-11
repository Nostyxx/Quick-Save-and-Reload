#include "pch.h"

#include "mod_runtime.h"
#include "resolver_symbols.h"

#include <MinHook.h>

#include <Xinput.h>
#include <hidsdi.h>
#include <hidusage.h>

#pragma comment(lib, "hid.lib")

#include <array>
#include <atomic>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

namespace quicksave {
namespace {

constexpr const char* kModName = "Quick Save and Reload";
constexpr const wchar_t* kIniFileName = L"QuickSaveAndReload.ini";
constexpr const wchar_t* kLogFileName = L"QuickSaveAndReload.log";
constexpr const char* kBuildSignature = "1_0_7_STABLE";

constexpr std::size_t kSaveRecordSize = 0x58;
constexpr std::ptrdiff_t kRootOffsetListWidget = 0x128;
constexpr std::ptrdiff_t kRootOffsetActiveModal = 0x188;
constexpr std::ptrdiff_t kRootOffsetVisibleMap = 0x198;
constexpr std::ptrdiff_t kRootOffsetVisibleCount = 0x1A0;
constexpr std::ptrdiff_t kRootOffsetMode = 0x1B8;
constexpr std::ptrdiff_t kUiSystemRootOffsetManager = 0xA8;
constexpr std::ptrdiff_t kUiManagerRootGameDataLoadListOffset = 0x758;
constexpr std::ptrdiff_t kUiManagerRootTitleListOffset = 0x788;
constexpr std::ptrdiff_t kUiManagerVectorDataOffset = 0x38;
constexpr std::ptrdiff_t kUiManagerVectorCountOffset = 0x40;
constexpr std::ptrdiff_t kModalOffsetSlotItem = 0x128;
constexpr std::ptrdiff_t kRowOffsetTypeText = 0x340;
constexpr std::ptrdiff_t kRowOffsetIndexName = 0x348;
constexpr DWORD kHotkeyPollIntervalMs = 25;
constexpr DWORD kHotkeyCooldownMs = 350;
constexpr DWORD kGameplayReadyDelayMs = 5000;
constexpr std::uintptr_t kRootTitleHandleOpenLoadViewRva = 0x00D5BBE0;
constexpr const wchar_t* kQuickSaveDispatchMessageName = L"Quick Save and Reload::QuickSave::66E1479A-160F-4E1B-B6D2-09465C58A11A";
constexpr const wchar_t* kQuickLoadDispatchMessageName = L"Quick Save and Reload::QuickLoad::5D28E3A8-52AA-49DE-84C5-54EB44827C0D";
constexpr const wchar_t* kQuickLoadCancelDispatchMessageName = L"Quick Save and Reload::QuickLoadCancel::A70E6A4B-3D2F-4D8E-89F1-5C2D3D709B31";
constexpr std::uintptr_t kQuickDispatchToken = 0x5153525F44495350ull;
struct Config {
    bool enable_mod = true;
    bool log_enabled = false;
    bool toast_notification_enabled = true;
    bool quick_load_confirmation_enabled = true;
    bool enable_reserved_load_row = true;
    bool hide_reserved_slot_in_save_ui = true;
    bool enable_experimental_load_ui = true;
    int quick_slot_id = 108;
    int quick_save_vk = VK_F5;
    int quick_load_vk = VK_F6;
    WORD quick_save_controller_mask = WORD(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_A);
    WORD quick_load_controller_mask = WORD(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_Y);
    std::wstring locale = L"en_US";
};

enum class TextId : std::size_t {
    UiRowLabel = 0,
    ToastQuickSaveSuccess,
    ToastQuickSaveFailed,
    ToastSaveFunctionUnavailable,
    ToastNoSaveActor,
    ToastSaveBlockedCode,
    ToastQuickSaveFailedCode,
    ToastQuickLoadFailed,
    ToastLoadFunctionUnavailable,
    ToastNoQuickSaveFound,
    ToastGameStateUnavailable,
    Count
};

struct LocalizedEntry {
    TextId id;
    const wchar_t* en_us;
    const wchar_t* ko_kr;
    const wchar_t* fr_fr;
};

enum class ControllerSource : std::uint8_t {
    None = 0,
    XInput,
    DualSenseRaw,
};

enum class QuickActionKind : std::int32_t {
    None = 0,
    Save = 1,
    Load = 2,
};

struct RuntimeState {
    HMODULE self_module = nullptr;
    FILE* log_file = nullptr;
    bool hooks_installed = false;
    wchar_t plugin_dir[MAX_PATH] = {};
    HANDLE input_thread = nullptr;
    HANDLE stop_event = nullptr;
    HWND game_window = nullptr;
    WNDPROC original_wndproc = nullptr;
    std::atomic<std::uintptr_t> raw_input_target_hwnd{ 0 };
    UINT quick_save_message = 0;
    UINT quick_load_message = 0;
    UINT quick_load_cancel_message = 0;
    std::atomic<std::uintptr_t> cached_save_request_actor{ 0 };
    std::atomic<std::uintptr_t> cached_direct_save_actor{ 0 };
    std::atomic<std::uintptr_t> cached_service_save_actor{ 0 };
    std::atomic<std::uintptr_t> cached_save_request_arg{ 0 };
    std::atomic<DWORD> cached_direct_save_thread_id{ 0 };
    std::atomic<DWORD> cached_service_save_thread_id{ 0 };
    std::atomic<std::uintptr_t> cached_load_worker_self{ 0 };
    std::atomic<std::uintptr_t> cached_load_io_context{ 0 };
    std::atomic<DWORD> cached_load_thread_id{ 0 };
    std::atomic<std::uintptr_t> cached_real_load_self{ 0 };
    std::uint32_t cached_real_load_packet_size = 0;
    std::uint32_t cached_real_load_blob_size = 0;
    alignas(16) std::array<std::uint8_t, 0x80> cached_real_load_packet{};
    alignas(16) std::array<std::uint8_t, 0x4000> cached_real_load_blob{};
    SRWLOCK cached_real_load_lock = SRWLOCK_INIT;
    std::atomic<std::uintptr_t> cached_login_character_self{ 0 };
    std::atomic<std::uintptr_t> cached_login_character_actor_wrapper{ 0 };
    std::atomic<DWORD> cached_login_character_thread_id{ 0 };
    std::atomic<std::uintptr_t> cached_in_game_load_core_self{ 0 };
    std::atomic<std::uint64_t> cached_in_game_load_target_key{ 0 };
    std::atomic<DWORD> cached_in_game_load_ui_thread_id{ 0 };
    std::atomic<std::uintptr_t> cached_load_ui_root{ 0 };
    std::atomic<std::uintptr_t> cached_load_ui_vftable{ 0 };
    std::uint32_t cached_login_character_packet_size = 0;
    std::uint32_t cached_login_character_blob_size = 0;
    alignas(16) std::array<std::uint8_t, 0x80> cached_login_character_packet{};
    alignas(16) std::array<std::uint8_t, 0x4000> cached_login_character_blob{};
    std::uint8_t cached_login_character_mode = 0;
    std::uint64_t cached_login_character_key = 0;
    std::uint32_t cached_login_character_slot = 0;
    std::uint32_t cached_login_character_payload_count = 0;
    alignas(16) std::array<std::uint16_t, 512> cached_login_character_payload{};
    SRWLOCK cached_login_character_lock = SRWLOCK_INIT;
    std::atomic<WORD> dualsense_buttons{ 0 };
    std::atomic<ULONGLONG> dualsense_last_input_ms{ 0 };
    std::atomic<bool> pending_quick_save{ false };
    std::atomic<bool> pending_quick_load{ false };
    std::atomic<bool> quick_save_active{ false };
    std::atomic<bool> quick_load_confirm_modal_active{ false };
    std::atomic<bool> quick_actions_enabled{ false };
    std::atomic<bool> gameplay_gate_armed{ true };
    std::atomic<ULONGLONG> gameplay_ready_since_ms{ 0 };
    std::atomic<ULONGLONG> gameplay_gate_last_tick_ms{ 0 };
    std::atomic<ULONGLONG> gameplay_gate_stable_ms{ 0 };
    std::atomic<std::int32_t> quick_action_active{ static_cast<std::int32_t>(QuickActionKind::None) };
    ControllerSource controller_source = ControllerSource::None;
    WORD previous_controller_buttons = 0;
    bool previous_quick_save_key_down = false;
    bool previous_quick_load_key_down = false;
    ULONGLONG last_quick_save_dispatch_ms = 0;
    ULONGLONG last_quick_load_dispatch_ms = 0;
    Config config{};
    std::array<std::string, static_cast<std::size_t>(TextId::Count)> text{};
};

RuntimeState g_state;

struct ClientActorScope {
    void* vftable = nullptr;
    void* object = nullptr;
    unsigned char valid = 0;
    unsigned char special_release = 0;
    unsigned char reserved[6] = {};
};

struct DirectSaveCallScratch {
    int flags_word = 0;
    int flags_pad = 0;
    int out_result = 0;
};

struct ResolvedWorldEnv {
    std::int64_t entity = 0;
    std::int64_t weather_state = 0;
    std::int64_t cloud_node = 0;
    std::int64_t wind_node = 0;
    std::int64_t particle_mgr = 0;
    bool valid = false;
};

using XInputGetStateDynFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

using LoadListEventFn =
    std::int64_t(__fastcall*)(std::int64_t, std::int64_t, char, std::int64_t, int, std::int64_t, std::int64_t, int, int, unsigned int);
using BuildVisibleMapFn = void(__fastcall*)(std::int64_t);
using LoadSelectedRefreshFn = std::int64_t(__fastcall*)(std::int64_t);
using LoadModalHandlerFn = void(__fastcall*)(std::uint64_t*, std::int64_t, char, unsigned int);

using RenderSlotRowFn = std::int64_t(__fastcall*)(std::uint64_t*, unsigned int*, char, char);
using ResolveUiScriptFn = std::uint64_t(__fastcall*)(std::uint64_t, std::int64_t, char);
using SetControlTextFn = std::int64_t(__fastcall*)(std::uint64_t, const void*);
using RootTitleHandleOpenLoadViewFn = void(__fastcall*)(std::int64_t);
using AcquireClientActorScopeFn = ClientActorScope* (__fastcall*)(std::int64_t, ClientActorScope*);
using AcquireClientUserActorScopeFn = ClientActorScope* (__fastcall*)(std::int64_t, ClientActorScope*);
using ScopeSpecialReleaseFn = char(__fastcall*)(void*);
using DirectLocalSaveFn = int* (__fastcall*)(std::int64_t, int*, int, unsigned char*);
using SavePrecheckFn = int* (__fastcall*)(std::int64_t, int*, std::int64_t, unsigned char*);
using NativeToastCreateStringFn = void* (__fastcall*)(const char*);
using NativeToastPushFn = void (__fastcall*)(void*, void**, unsigned int);
using NativeToastReleaseStringFn = void (__fastcall*)(void*);
using WeatherFrameHeartbeatFn = std::int64_t (__fastcall*)(std::uint64_t, float);
using SaveServiceDriverFn = std::int64_t(__fastcall*)(std::int64_t, std::uint64_t*);
using ServiceChildPollFn = std::int32_t* (__fastcall*)(std::int64_t, std::int32_t*);
using InGameMenuLoadCoreFn = std::int32_t* (__fastcall*)(std::int64_t, std::int32_t*, std::int64_t*, unsigned int);

BuildVisibleMapFn g_build_visible_map_original = nullptr;
LoadListEventFn g_load_list_event_original = nullptr;
LoadSelectedRefreshFn g_load_selected_refresh_original = nullptr;
LoadModalHandlerFn g_load_modal_handler_original = nullptr;

RenderSlotRowFn g_render_slot_row = nullptr;
RenderSlotRowFn g_render_slot_row_original = nullptr;
ResolveUiScriptFn g_resolve_ui_script = nullptr;
SetControlTextFn g_set_control_text = nullptr;
SetControlTextFn g_set_control_text_original = nullptr;
RootTitleHandleOpenLoadViewFn g_root_title_handle_open_load_view = nullptr;
XInputGetStateDynFn g_xinput_get_state = nullptr;
AcquireClientActorScopeFn g_acquire_client_actor_scope = nullptr;
AcquireClientUserActorScopeFn g_acquire_client_user_actor_scope = nullptr;
ScopeSpecialReleaseFn g_scope_special_release = nullptr;
DirectLocalSaveFn g_direct_local_save_original = nullptr;
SavePrecheckFn g_save_precheck = nullptr;
NativeToastCreateStringFn g_native_toast_create_string = nullptr;
NativeToastPushFn g_native_toast_push = nullptr;
NativeToastReleaseStringFn g_native_toast_release_string = nullptr;
void** g_native_toast_root_global = nullptr;
std::uint32_t g_native_toast_outer_offset = 0;
std::ptrdiff_t g_native_toast_manager_offset = 0;
WeatherFrameHeartbeatFn g_weather_frame_heartbeat_original = nullptr;
SaveServiceDriverFn g_save_service_driver_original = nullptr;
ServiceChildPollFn g_service_child_poll_original = nullptr;
InGameMenuLoadCoreFn g_in_game_menu_load_core_original = nullptr;
std::uint64_t* g_world_env_manager_global = nullptr;
bool g_xinput_resolve_attempted = false;
thread_local bool g_reserved_quick_row_text_override = false;
thread_local std::uintptr_t g_reserved_quick_row_script = 0;

constexpr std::size_t kRealLoadPacketCopySize = 0x60;
constexpr std::size_t kRealLoadBlobSlotOffset = 13;
using RealLoadPacketBuffer = std::array<std::uint8_t, kRealLoadPacketCopySize>;
using RealLoadBlobBuffer = std::array<std::uint8_t, 0x4000>;
constexpr std::size_t kLoginCharacterPacketCopySize = 0x60;
constexpr std::size_t kLoginCharacterBlobSlotOffset = 14;
using LoginCharacterPacketBuffer = std::array<std::uint8_t, kLoginCharacterPacketCopySize>;
using LoginCharacterBlobBuffer = std::array<std::uint8_t, 0x4000>;

void Log(const char* fmt, ...) {
    if (!g_state.config.log_enabled || g_state.log_file == nullptr) {
        return;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    std::fprintf(
        g_state.log_file,
        "[%02u:%02u:%02u.%03u] ",
        st.wHour,
        st.wMinute,
        st.wSecond,
        st.wMilliseconds);

    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_state.log_file, fmt, args);
    va_end(args);
    std::fflush(g_state.log_file);
}

std::wstring BuildPath(const wchar_t* file_name) {
    std::wstring path = g_state.plugin_dir;
    if (!path.empty() && path.back() != L'\\') {
        path.push_back(L'\\');
    }
    path.append(file_name);
    return path;
}

std::string NarrowWide(const wchar_t* text) {
    if (text == nullptr) {
        return {};
    }

    const int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) {
        return {};
    }

    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text,
        -1,
        result.data(),
        required,
        nullptr,
        nullptr);
    result.resize(static_cast<std::size_t>(required - 1));
    return result;
}

const wchar_t* ResolveLocalizedWide(TextId id, const std::wstring& locale) {
    static const LocalizedEntry kLocalizedEntries[] = {
        { TextId::UiRowLabel, L"Quick Save", L"빠른 저장", L"Sauvegarde rapide" },
        { TextId::ToastQuickSaveSuccess, L"QUICK SAVE SUCCESS", L"빠른 저장을 완료했습니다.", L"SUCCÈS DE LA SAUVEGARDE RAPIDE" },
        { TextId::ToastQuickSaveFailed, L"QUICK SAVE FAILED", L"빠른 저장을 실패했습니다.", L"ÉCHEC DE LA SAUVEGARDE RAPIDE" },
        { TextId::ToastSaveFunctionUnavailable, L"SAVE FUNCTION UNAVAILABLE", L"저장 기능을 사용할 수 없습니다.", L"SAUVEGARDE INDISPONIBLE" },
        { TextId::ToastNoSaveActor, L"NO SAVE ACTOR", L"유효한 저장 객체를 찾을 수 없습니다.", L"PAS D'ENTITÉ DE SAUVEGARDE" },
        { TextId::ToastSaveBlockedCode, L"SAVE BLOCKED (0x%08X)", L"저장이 차단되었습니다. (0x%08X)", L"SAUVEGARDE BLOQUÉE (0x%08X)" },
        { TextId::ToastQuickSaveFailedCode, L"QUICK SAVE FAILED (%u)", L"빠른 저장을 실패했습니다. (%u)", L"ÉCHEC DE LA SAUVEGARDE RAPIDE (%u)" },
        { TextId::ToastQuickLoadFailed, L"QUICK LOAD FAILED", L"빠른 불러오기에 실패했습니다.", L"ÉCHEC DU CHARGEMENT RAPIDE" },
        { TextId::ToastLoadFunctionUnavailable, L"LOAD FUNCTION UNAVAILABLE", L"불러오기 기능을 사용할 수 없습니다.", L"CHARGEMENT INDISPONIBLE" },
        { TextId::ToastNoQuickSaveFound, L"NO QUICK SAVE FOUND", L"빠른 저장 데이터를 찾을 수 없습니다.", L"SAUVEGARDE RAPIDE INTROUVABLE" },
        { TextId::ToastGameStateUnavailable, L"GAME STATE UNAVAILABLE", L"게임 상태를 확인할 수 없습니다.", L"CONTEXTE DU JEU INDISPONIBLE" },
    };

    const bool use_korean =
        _wcsicmp(locale.c_str(), L"ko_KR") == 0
        || _wcsicmp(locale.c_str(), L"ko") == 0
        || _wcsicmp(locale.c_str(), L"kr") == 0
        || _wcsicmp(locale.c_str(), L"korean") == 0;
    const bool use_french =
        _wcsicmp(locale.c_str(), L"fr_FR") == 0
        || _wcsicmp(locale.c_str(), L"fr") == 0
        || _wcsicmp(locale.c_str(), L"french") == 0
        || _wcsicmp(locale.c_str(), L"francais") == 0
        || _wcsicmp(locale.c_str(), L"français") == 0;

    for (const auto& entry : kLocalizedEntries) {
        if (entry.id == id) {
            if (use_korean) {
                return entry.ko_kr;
            }
            if (use_french) {
                return entry.fr_fr;
            }
            return entry.en_us;
        }
    }

    return L"";
}

void ApplyBuiltInLocaleText(const std::wstring& locale) {
    for (std::size_t i = 0; i < static_cast<std::size_t>(TextId::Count); ++i) {
        g_state.text[i] = NarrowWide(ResolveLocalizedWide(static_cast<TextId>(i), locale));
    }
}

void CacheRealLoadTemplate(std::int64_t self, const std::uint64_t* packet) {
    if (packet == nullptr) {
        return;
    }

    const auto* packet_bytes = reinterpret_cast<const std::uint8_t*>(packet);
    const auto blob_ptr = reinterpret_cast<const std::uint8_t*>(packet[3]);
    if (blob_ptr == nullptr) {
        return;
    }

    const std::uint16_t payload_size = *reinterpret_cast<const std::uint16_t*>(blob_ptr + 3);
    const std::uint32_t blob_size = static_cast<std::uint32_t>(payload_size) + 5U;
    if (blob_size > g_state.cached_real_load_blob.size()) {
        Log("[real-load-cache] skipping blob_size=%u exceeds cache\n", blob_size);
        return;
    }

    AcquireSRWLockExclusive(&g_state.cached_real_load_lock);
    std::memcpy(g_state.cached_real_load_packet.data(), packet_bytes, kRealLoadPacketCopySize);
    std::memcpy(g_state.cached_real_load_blob.data(), blob_ptr, blob_size);
    *reinterpret_cast<std::uint64_t*>(g_state.cached_real_load_packet.data() + 24) =
        reinterpret_cast<std::uint64_t>(g_state.cached_real_load_blob.data());
    g_state.cached_real_load_packet_size = static_cast<std::uint32_t>(kRealLoadPacketCopySize);
    g_state.cached_real_load_blob_size = blob_size;
    g_state.cached_real_load_self.store(static_cast<std::uintptr_t>(self));
    ReleaseSRWLockExclusive(&g_state.cached_real_load_lock);

    const int cached_slot = blob_size >= (kRealLoadBlobSlotOffset + sizeof(int))
        ? *reinterpret_cast<const int*>(g_state.cached_real_load_blob.data() + kRealLoadBlobSlotOffset)
        : -1;
    Log("[real-load-cache] self=%p blob_size=%u cached_slot=%d packet_arg1=%p\n",
        reinterpret_cast<void*>(self),
        blob_size,
        cached_slot,
        reinterpret_cast<void*>(packet[1]));
}

bool BuildPatchedRealLoadPacket(
    int slot,
    std::uintptr_t& out_self,
    RealLoadPacketBuffer& out_packet,
    RealLoadBlobBuffer& out_blob) {
    out_self = 0;

    AcquireSRWLockShared(&g_state.cached_real_load_lock);
    const std::uintptr_t cached_self = g_state.cached_real_load_self.load();
    const std::uint32_t packet_size = g_state.cached_real_load_packet_size;
    const std::uint32_t blob_size = g_state.cached_real_load_blob_size;
    const bool available =
        cached_self != 0
        && packet_size == kRealLoadPacketCopySize
        && blob_size >= (kRealLoadBlobSlotOffset + sizeof(int))
        && blob_size <= out_blob.size();
    if (available) {
        std::memcpy(out_packet.data(), g_state.cached_real_load_packet.data(), packet_size);
        std::memcpy(out_blob.data(), g_state.cached_real_load_blob.data(), blob_size);
        out_self = cached_self;
        *reinterpret_cast<std::uint64_t*>(out_packet.data() + 24) = reinterpret_cast<std::uint64_t>(out_blob.data());
        *reinterpret_cast<int*>(out_blob.data() + kRealLoadBlobSlotOffset) = slot;
    }
    ReleaseSRWLockShared(&g_state.cached_real_load_lock);

    return available;
}

void CacheLoginCharacterRequestTemplate(std::int64_t self, const std::uint64_t* packet) {
    if (packet == nullptr) {
        return;
    }

    const auto* packet_bytes = reinterpret_cast<const std::uint8_t*>(packet);
    const auto* blob_ptr = reinterpret_cast<const std::uint8_t*>(packet[3]);
    if (blob_ptr == nullptr) {
        return;
    }

    const std::uint16_t payload_size = *reinterpret_cast<const std::uint16_t*>(blob_ptr + 3);
    const std::uint32_t blob_size = static_cast<std::uint32_t>(payload_size) + 5U;
    if (blob_size > g_state.cached_login_character_blob.size()) {
        Log("[login-char-req-cache] skipping blob_size=%u exceeds cache\n", blob_size);
        return;
    }

    AcquireSRWLockExclusive(&g_state.cached_login_character_lock);
    std::memcpy(g_state.cached_login_character_packet.data(), packet_bytes, kLoginCharacterPacketCopySize);
    std::memcpy(g_state.cached_login_character_blob.data(), blob_ptr, blob_size);
    *reinterpret_cast<std::uint64_t*>(g_state.cached_login_character_packet.data() + 24) =
        reinterpret_cast<std::uint64_t>(g_state.cached_login_character_blob.data());
    g_state.cached_login_character_packet_size = static_cast<std::uint32_t>(kLoginCharacterPacketCopySize);
    g_state.cached_login_character_blob_size = blob_size;
    g_state.cached_login_character_self.store(static_cast<std::uintptr_t>(self));
    ReleaseSRWLockExclusive(&g_state.cached_login_character_lock);

    const int cached_slot = blob_size >= (kLoginCharacterBlobSlotOffset + sizeof(std::uint32_t))
        ? *reinterpret_cast<const int*>(g_state.cached_login_character_blob.data() + kLoginCharacterBlobSlotOffset)
        : -1;
    Log("[login-char-req-cache] self=%p blob_size=%u cached_slot=%d packet_arg1=%p packet_blob=%p\n",
        reinterpret_cast<void*>(self),
        blob_size,
        cached_slot,
        reinterpret_cast<void*>(packet[1]),
        blob_ptr);
}

bool BuildPatchedLoginCharacterPacket(
    int slot,
    std::uintptr_t& out_self,
    LoginCharacterPacketBuffer& out_packet,
    LoginCharacterBlobBuffer& out_blob) {
    out_self = 0;

    AcquireSRWLockShared(&g_state.cached_login_character_lock);
    const std::uintptr_t cached_self = g_state.cached_login_character_self.load();
    const std::uint32_t packet_size = g_state.cached_login_character_packet_size;
    const std::uint32_t blob_size = g_state.cached_login_character_blob_size;
    const bool available =
        cached_self != 0
        && packet_size == kLoginCharacterPacketCopySize
        && blob_size >= (kLoginCharacterBlobSlotOffset + sizeof(std::uint32_t))
        && blob_size <= out_blob.size();
    if (available) {
        std::memcpy(out_packet.data(), g_state.cached_login_character_packet.data(), packet_size);
        std::memcpy(out_blob.data(), g_state.cached_login_character_blob.data(), blob_size);
        out_self = cached_self;
        *reinterpret_cast<std::uint64_t*>(out_packet.data() + 24) =
            reinterpret_cast<std::uint64_t>(out_blob.data());
        *reinterpret_cast<int*>(out_blob.data() + kLoginCharacterBlobSlotOffset) = slot;
    }
    ReleaseSRWLockShared(&g_state.cached_login_character_lock);

    return available;
}

void CacheLoginCharacterTemplate(
    std::int64_t self,
    std::uint64_t** actor_wrapper,
    char mode,
    std::uint64_t key,
    unsigned int slot,
    std::int64_t payload_vec) {
    const auto* payload_data =
        payload_vec != 0 ? *reinterpret_cast<const std::uint16_t* const*>(payload_vec) : nullptr;
    const std::uint32_t payload_count =
        payload_vec != 0 ? *reinterpret_cast<const std::uint32_t*>(payload_vec + 8) : 0;
    const std::uint32_t copy_count =
        payload_count < static_cast<std::uint32_t>(g_state.cached_login_character_payload.size())
            ? payload_count
            : static_cast<std::uint32_t>(g_state.cached_login_character_payload.size());

    AcquireSRWLockExclusive(&g_state.cached_login_character_lock);
    g_state.cached_login_character_self.store(static_cast<std::uintptr_t>(self));
    g_state.cached_login_character_actor_wrapper.store(reinterpret_cast<std::uintptr_t>(actor_wrapper));
    g_state.cached_login_character_thread_id.store(GetCurrentThreadId());
    g_state.cached_login_character_mode = static_cast<std::uint8_t>(mode);
    g_state.cached_login_character_key = key;
    g_state.cached_login_character_slot = slot;
    g_state.cached_login_character_payload_count = payload_count;
    g_state.cached_login_character_payload.fill(0);
    if (payload_data != nullptr && copy_count != 0) {
        std::memcpy(
            g_state.cached_login_character_payload.data(),
            payload_data,
            static_cast<std::size_t>(copy_count) * sizeof(std::uint16_t));
    }
    ReleaseSRWLockExclusive(&g_state.cached_login_character_lock);

}

void ResolvePluginDir() {
    wchar_t dll_path[MAX_PATH] = {};
    if (GetModuleFileNameW(g_state.self_module, dll_path, MAX_PATH) == 0) {
        return;
    }

    wcsncpy_s(g_state.plugin_dir, MAX_PATH, dll_path, _TRUNCATE);
    wchar_t* slash = std::wcsrchr(g_state.plugin_dir, L'\\');
    if (slash != nullptr) {
        *slash = L'\0';
    }
}

int KeyNameToVK(const wchar_t* name, int fallback) {
    if (name == nullptr || !name[0]) {
        return 0;
    }

    if (_wcsicmp(name, L"NONE") == 0 || _wcsicmp(name, L"UNBOUND") == 0) return 0;
    if (_wcsicmp(name, L"F1") == 0) return VK_F1;
    if (_wcsicmp(name, L"F2") == 0) return VK_F2;
    if (_wcsicmp(name, L"F3") == 0) return VK_F3;
    if (_wcsicmp(name, L"F4") == 0) return VK_F4;
    if (_wcsicmp(name, L"F5") == 0) return VK_F5;
    if (_wcsicmp(name, L"F6") == 0) return VK_F6;
    if (_wcsicmp(name, L"F7") == 0) return VK_F7;
    if (_wcsicmp(name, L"F8") == 0) return VK_F8;
    if (_wcsicmp(name, L"F9") == 0) return VK_F9;
    if (_wcsicmp(name, L"F10") == 0) return VK_F10;
    if (_wcsicmp(name, L"F11") == 0) return VK_F11;
    if (_wcsicmp(name, L"F12") == 0) return VK_F12;
    if (_wcsicmp(name, L"INSERT") == 0) return VK_INSERT;
    if (_wcsicmp(name, L"DELETE") == 0) return VK_DELETE;
    if (_wcsicmp(name, L"HOME") == 0) return VK_HOME;
    if (_wcsicmp(name, L"END") == 0) return VK_END;
    if (_wcsicmp(name, L"PGUP") == 0) return VK_PRIOR;
    if (_wcsicmp(name, L"PGDN") == 0) return VK_NEXT;

    if (std::wcslen(name) == 1) {
        return std::towupper(name[0]);
    }

    return fallback;
}

WORD ControllerTokenToMask(const wchar_t* token) {
    if (token == nullptr || !token[0]) {
        return 0;
    }

    if (_wcsicmp(token, L"dpad_up") == 0 || _wcsicmp(token, L"up") == 0) return XINPUT_GAMEPAD_DPAD_UP;
    if (_wcsicmp(token, L"dpad_down") == 0 || _wcsicmp(token, L"down") == 0) return XINPUT_GAMEPAD_DPAD_DOWN;
    if (_wcsicmp(token, L"dpad_left") == 0 || _wcsicmp(token, L"left") == 0) return XINPUT_GAMEPAD_DPAD_LEFT;
    if (_wcsicmp(token, L"dpad_right") == 0 || _wcsicmp(token, L"right") == 0) return XINPUT_GAMEPAD_DPAD_RIGHT;
    if (_wcsicmp(token, L"a") == 0 || _wcsicmp(token, L"cross") == 0) return XINPUT_GAMEPAD_A;
    if (_wcsicmp(token, L"b") == 0 || _wcsicmp(token, L"circle") == 0) return XINPUT_GAMEPAD_B;
    if (_wcsicmp(token, L"x") == 0 || _wcsicmp(token, L"square") == 0) return XINPUT_GAMEPAD_X;
    if (_wcsicmp(token, L"y") == 0 || _wcsicmp(token, L"triangle") == 0) return XINPUT_GAMEPAD_Y;
    if (_wcsicmp(token, L"lb") == 0 || _wcsicmp(token, L"l1") == 0) return XINPUT_GAMEPAD_LEFT_SHOULDER;
    if (_wcsicmp(token, L"rb") == 0 || _wcsicmp(token, L"r1") == 0) return XINPUT_GAMEPAD_RIGHT_SHOULDER;
    if (_wcsicmp(token, L"start") == 0 || _wcsicmp(token, L"options") == 0) return XINPUT_GAMEPAD_START;
    if (_wcsicmp(token, L"back") == 0 || _wcsicmp(token, L"share") == 0 || _wcsicmp(token, L"select") == 0) {
        return XINPUT_GAMEPAD_BACK;
    }

    return 0;
}

WORD ParseControllerCombo(const wchar_t* text, WORD fallback) {
    if (text == nullptr || !text[0]) {
        return 0;
    }
    if (_wcsicmp(text, L"NONE") == 0 || _wcsicmp(text, L"UNBOUND") == 0) {
        return 0;
    }

    wchar_t copy[128] = {};
    wcsncpy_s(copy, text, _TRUNCATE);
    WORD mask = 0;
    wchar_t* context = nullptr;
    for (wchar_t* token = wcstok_s(copy, L"+|, ", &context); token != nullptr; token = wcstok_s(nullptr, L"+|, ", &context)) {
        mask = WORD(mask | ControllerTokenToMask(token));
    }
    return mask != 0 ? mask : fallback;
}

bool IsControllerComboPressed(WORD buttons, WORD combo_mask) {
    return combo_mask != 0 && (buttons & combo_mask) == combo_mask;
}

void WriteDefaultConfig(const std::wstring& ini_path) {
    WritePrivateProfileStringW(L"General", L"LogEnabled", L"0", ini_path.c_str());
    WritePrivateProfileStringW(L"General", L"ToastNotification", L"1", ini_path.c_str());
    WritePrivateProfileStringW(L"General", L"QuickLoadConfirmation", L"1", ini_path.c_str());
    WritePrivateProfileStringW(L"General", L"HotkeyQuickSave", L"F5", ini_path.c_str());
    WritePrivateProfileStringW(L"General", L"HotkeyQuickLoad", L"F6", ini_path.c_str());
    WritePrivateProfileStringW(
        L"General",
        L"_HotkeyOptions",
        L"F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z",
        ini_path.c_str());
    WritePrivateProfileStringW(L"Locale", L"Language", L"en_US", ini_path.c_str());
    WritePrivateProfileStringW(L"Locale", L"_LanguageOptions", L"en_US, ko_KR, fr_FR", ini_path.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"ControllerHotkeyQuickSave", L"lb+a", ini_path.c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"ControllerHotkeyQuickLoad", L"lb+y", ini_path.c_str());
    WritePrivateProfileStringW(
        L"Hotkeys",
        L"_ControllerHotkeyOptions",
        L"Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back",
        ini_path.c_str());
}

void EnsureConfigValue(const std::wstring& ini_path, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[64] = {};
    GetPrivateProfileStringW(section, key, L"__MISSING__", buffer, static_cast<DWORD>(std::size(buffer)), ini_path.c_str());
    if (wcscmp(buffer, L"__MISSING__") == 0) {
        WritePrivateProfileStringW(section, key, value, ini_path.c_str());
    }
}

void LoadConfig() {
    const std::wstring ini_path = BuildPath(kIniFileName);
    if (GetFileAttributesW(ini_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaultConfig(ini_path);
    }
    EnsureConfigValue(ini_path, L"General", L"LogEnabled", L"0");
    EnsureConfigValue(ini_path, L"General", L"ToastNotification", L"1");
    EnsureConfigValue(ini_path, L"General", L"QuickLoadConfirmation", L"1");
    EnsureConfigValue(ini_path, L"General", L"HotkeyQuickSave", L"F5");
    EnsureConfigValue(ini_path, L"General", L"HotkeyQuickLoad", L"F6");
    EnsureConfigValue(
        ini_path,
        L"General",
        L"_HotkeyOptions",
        L"F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z");
    EnsureConfigValue(ini_path, L"Locale", L"Language", L"en_US");
    EnsureConfigValue(ini_path, L"Locale", L"_LanguageOptions", L"en_US, ko_KR, fr_FR");
    WritePrivateProfileStringW(L"Locale", L"_LanguageOptions", L"en_US, ko_KR, fr_FR", ini_path.c_str());
    EnsureConfigValue(ini_path, L"Hotkeys", L"ControllerHotkeyQuickSave", L"lb+a");
    EnsureConfigValue(ini_path, L"Hotkeys", L"ControllerHotkeyQuickLoad", L"lb+y");
    EnsureConfigValue(
        ini_path,
        L"Hotkeys",
        L"_ControllerHotkeyOptions",
        L"Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back");

    g_state.config.enable_mod = GetPrivateProfileIntW(L"General", L"EnableMod", 1, ini_path.c_str()) != 0;
    g_state.config.log_enabled = GetPrivateProfileIntW(L"General", L"LogEnabled", 0, ini_path.c_str()) != 0;
    g_state.config.toast_notification_enabled =
        GetPrivateProfileIntW(L"General", L"ToastNotification", 1, ini_path.c_str()) != 0;
    g_state.config.quick_load_confirmation_enabled =
        GetPrivateProfileIntW(L"General", L"QuickLoadConfirmation", 1, ini_path.c_str()) != 0;
    wchar_t locale_text[64] = {};
    GetPrivateProfileStringW(L"Locale", L"Language", L"", locale_text, static_cast<DWORD>(std::size(locale_text)), ini_path.c_str());
    if (locale_text[0] == L'\0') {
        GetPrivateProfileStringW(L"General", L"Locale", L"en_US", locale_text, static_cast<DWORD>(std::size(locale_text)), ini_path.c_str());
    }
    g_state.config.locale = locale_text[0] != L'\0' ? locale_text : L"en_US";

    wchar_t hotkey_text[64] = {};
    GetPrivateProfileStringW(L"General", L"HotkeyQuickSave", L"F5", hotkey_text, static_cast<DWORD>(std::size(hotkey_text)), ini_path.c_str());
    g_state.config.quick_save_vk = KeyNameToVK(hotkey_text, VK_F5);
    GetPrivateProfileStringW(L"General", L"HotkeyQuickLoad", L"F6", hotkey_text, static_cast<DWORD>(std::size(hotkey_text)), ini_path.c_str());
    g_state.config.quick_load_vk = KeyNameToVK(hotkey_text, VK_F6);
    GetPrivateProfileStringW(
        L"Hotkeys",
        L"ControllerHotkeyQuickSave",
        L"lb+a",
        hotkey_text,
        static_cast<DWORD>(std::size(hotkey_text)),
        ini_path.c_str());
    g_state.config.quick_save_controller_mask =
        ParseControllerCombo(hotkey_text, WORD(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_A));
    GetPrivateProfileStringW(
        L"Hotkeys",
        L"ControllerHotkeyQuickLoad",
        L"lb+y",
        hotkey_text,
        static_cast<DWORD>(std::size(hotkey_text)),
        ini_path.c_str());
    g_state.config.quick_load_controller_mask =
        ParseControllerCombo(hotkey_text, WORD(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_B));

    g_state.config.enable_reserved_load_row = true;
    g_state.config.hide_reserved_slot_in_save_ui = true;
    g_state.config.enable_experimental_load_ui = true;
}

void OpenLog() {
    if (!g_state.config.log_enabled) {
        return;
    }

    const std::wstring log_path = BuildPath(kLogFileName);
    _wfopen_s(&g_state.log_file, log_path.c_str(), L"w");
    if (g_state.log_file == nullptr) {
        g_state.config.log_enabled = false;
    }
}

void CloseLog() {
    if (g_state.log_file != nullptr) {
        std::fclose(g_state.log_file);
        g_state.log_file = nullptr;
    }
}

std::uintptr_t ExeRva(std::uintptr_t rva) {
    auto* exe = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
    return exe == nullptr ? 0 : reinterpret_cast<std::uintptr_t>(exe + rva);
}

void* ResolveNativeToastManager() {
    if (g_native_toast_root_global == nullptr) {
        return nullptr;
    }

    void* root = *g_native_toast_root_global;
    if (root == nullptr || g_native_toast_outer_offset == 0 || g_native_toast_manager_offset == 0) {
        return nullptr;
    }

    __try {
        void* outer = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(root) + g_native_toast_outer_offset);
        if (outer == nullptr) {
            return nullptr;
        }
        return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(outer) + g_native_toast_manager_offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool NativeToastReady() {
    return g_native_toast_create_string != nullptr
        && g_native_toast_push != nullptr
        && g_native_toast_release_string != nullptr
        && g_native_toast_root_global != nullptr
        && g_native_toast_outer_offset != 0
        && g_native_toast_manager_offset != 0;
}

void ShowNativeToast(const char* msg) {
    if (msg == nullptr || msg[0] == '\0' || !NativeToastReady()) {
        return;
    }

    void* manager = ResolveNativeToastManager();
    if (manager == nullptr) {
        Log("[W] Native toast manager unavailable for message: %s\n", msg);
        return;
    }

    void* message_handle = g_native_toast_create_string(msg);
    if (message_handle == nullptr) {
        Log("[W] Failed to create native toast string: %s\n", msg);
        return;
    }

    g_native_toast_push(manager, &message_handle, 0);
    g_native_toast_release_string(message_handle);
    Log("[i] Native toast shown: %s\n", msg);
}

void ShowNativeToastFormatted(const char* fmt, ...) {
    if (!g_state.config.toast_notification_enabled || fmt == nullptr || fmt[0] == '\0') {
        return;
    }

    char buffer[128] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    buffer[sizeof(buffer) - 1] = '\0';
    if (buffer[0] == '\0') {
        return;
    }

    ShowNativeToast(buffer);
}

const char* LocalizedRowLabel() {
    const auto& text = g_state.text[static_cast<std::size_t>(TextId::UiRowLabel)];
    return text.empty() ? "Quick Save" : text.c_str();
}

const char* LocalizedTextValue(TextId id, const char* fallback) {
    const auto& text = g_state.text[static_cast<std::size_t>(id)];
    return text.empty() ? fallback : text.c_str();
}

template <typename T>
T ResolveCall(resolver::SymbolId id) {
    return reinterpret_cast<T>(resolver::Address(id));
}

template <typename T>
bool CreateHookAtResolved(resolver::SymbolId id, T detour, T* original, const char* label) {
    const std::uintptr_t target = resolver::Address(id);
    if (target == 0) {
        Log("[E] %s target resolve failed\n", label);
        return false;
    }

    const MH_STATUS create_status =
        MH_CreateHook(reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(original));
    if (create_status != MH_OK) {
        Log("[E] %s MH_CreateHook failed (%d) at %p\n", label, static_cast<int>(create_status), reinterpret_cast<void*>(target));
        return false;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (enable_status != MH_OK) {
        Log("[E] %s MH_EnableHook failed (%d) at %p\n", label, static_cast<int>(enable_status), reinterpret_cast<void*>(target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        return false;
    }

    Log("[+] %s hooked at %p\n", label, reinterpret_cast<void*>(target));
    return true;
}

template <typename T>
bool CreateHookAtAddress(std::uintptr_t target, T detour, T* original, const char* label) {
    if (target == 0) {
        Log("[E] %s target resolve failed\n", label);
        return false;
    }

    const MH_STATUS create_status =
        MH_CreateHook(reinterpret_cast<LPVOID>(target), reinterpret_cast<LPVOID>(detour), reinterpret_cast<LPVOID*>(original));
    if (create_status != MH_OK) {
        Log("[E] %s MH_CreateHook failed (%d) at %p\n", label, static_cast<int>(create_status), reinterpret_cast<void*>(target));
        return false;
    }

    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<LPVOID>(target));
    if (enable_status != MH_OK) {
        Log("[E] %s MH_EnableHook failed (%d) at %p\n", label, static_cast<int>(enable_status), reinterpret_cast<void*>(target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(target));
        return false;
    }

    Log("[+] %s hooked at %p\n", label, reinterpret_cast<void*>(target));
    return true;
}

void ResolverLogSink(const char* line) {
    if (line != nullptr && line[0] != '\0') {
        Log("%s", line);
    }
}

void SyncResolvedToastBridge() {
    const auto& bridge = resolver::ToastBridge();
    g_native_toast_root_global = reinterpret_cast<void**>(bridge.root_global);
    g_native_toast_outer_offset = bridge.outer_offset;
    g_native_toast_manager_offset = bridge.manager_offset;
    g_native_toast_create_string = reinterpret_cast<NativeToastCreateStringFn>(bridge.create_string);
    g_native_toast_push = reinterpret_cast<NativeToastPushFn>(bridge.push);
    g_native_toast_release_string = reinterpret_cast<NativeToastReleaseStringFn>(bridge.release_string);
}

std::uintptr_t ReadRipRelativeAt(std::uintptr_t site) {
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

bool ResolveDirectCalls() {
    g_render_slot_row = ResolveCall<RenderSlotRowFn>(resolver::SymbolId::RenderSlotRow);
    g_resolve_ui_script = ResolveCall<ResolveUiScriptFn>(resolver::SymbolId::ResolveUiScript);
    g_set_control_text = ResolveCall<SetControlTextFn>(resolver::SymbolId::SetControlText);
    g_root_title_handle_open_load_view = reinterpret_cast<RootTitleHandleOpenLoadViewFn>(
        reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr)) + kRootTitleHandleOpenLoadViewRva);
    g_acquire_client_actor_scope = ResolveCall<AcquireClientActorScopeFn>(resolver::SymbolId::AcquireClientActorScope);
    g_acquire_client_user_actor_scope = ResolveCall<AcquireClientUserActorScopeFn>(resolver::SymbolId::AcquireClientUserActorScope);
    g_scope_special_release = ResolveCall<ScopeSpecialReleaseFn>(resolver::SymbolId::ScopeSpecialRelease);
    g_save_precheck = ResolveCall<SavePrecheckFn>(resolver::SymbolId::SavePrecheck);
    g_world_env_manager_global = nullptr;
    const std::uintptr_t weather_tick_anchor = resolver::Address(resolver::SymbolId::WeatherTickAnchor);
    if (weather_tick_anchor != 0) {
        const std::uintptr_t env_site = weather_tick_anchor + 5;
        __try {
            if (*reinterpret_cast<const std::uint8_t*>(env_site) == 0x48) {
                g_world_env_manager_global = reinterpret_cast<std::uint64_t*>(ReadRipRelativeAt(env_site));
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_world_env_manager_global = nullptr;
        }
    }

    const bool ok = g_render_slot_row != nullptr && g_resolve_ui_script != nullptr && g_set_control_text != nullptr
        && g_root_title_handle_open_load_view != nullptr
        && g_acquire_client_user_actor_scope != nullptr
        && g_scope_special_release != nullptr;
    if (!ok) {
        Log("[E] Failed to resolve direct helpers render=%p resolve=%p set_text=%p open_load_view=%p acquire_actor_scope=%p acquire_user_scope=%p special_release=%p\n",
            reinterpret_cast<void*>(g_render_slot_row),
            reinterpret_cast<void*>(g_resolve_ui_script),
            reinterpret_cast<void*>(g_set_control_text),
            reinterpret_cast<void*>(g_root_title_handle_open_load_view),
            reinterpret_cast<void*>(g_acquire_client_actor_scope),
            reinterpret_cast<void*>(g_acquire_client_user_actor_scope),
            reinterpret_cast<void*>(g_scope_special_release));
    }
    if (g_save_precheck == nullptr) {
        Log("[W] Save precheck helper unavailable; quick-save invalid-state filtering disabled\n");
    }
    if (g_acquire_client_actor_scope == nullptr) {
        Log("[i] Client actor scope helper unavailable; optional legacy actor-scope path remains disabled\n");
    }
    if (g_world_env_manager_global == nullptr) {
        Log("[W] World env manager helper unavailable; transition gate will use native-ready only\n");
    }
    return ok;
}

bool ResolveDispatchMessages() {
    g_state.quick_save_message = RegisterWindowMessageW(kQuickSaveDispatchMessageName);
    g_state.quick_load_message = RegisterWindowMessageW(kQuickLoadDispatchMessageName);
    if (g_state.quick_save_message == 0 || g_state.quick_load_message == 0) {
        Log("[E] Failed to register dispatch messages save=0x%X load=0x%X gle=%lu\n",
            g_state.quick_save_message,
            g_state.quick_load_message,
            GetLastError());
        return false;
    }

    Log("[i] Dispatch QuickSave=0x%X QuickLoad=0x%X\n", g_state.quick_save_message, g_state.quick_load_message);
    return true;
}


bool AcquireLiveClientActorScope(ClientActorScope& out_scope);
bool AcquireLiveClientUserActorScope(ClientActorScope& out_scope);
void ReleaseClientActorScope(ClientActorScope& scope);
std::uintptr_t ResolveInGameLoadCoreFromService();
std::uintptr_t ResolveQuickSaveActor();
DWORD ResolveQuickSaveThreadId();
bool PassesNativeQuickActionReadyCheck(unsigned* out_code);
ResolvedWorldEnv ResolveWorldEnv();
void ArmGameplayActionGate(const char* reason);
void TickGameplayActionGateOnGameThread();
void CacheLoadUiRoot(std::int64_t root, const char* source);
void ClearCachedLoadUiRoot();
bool IsLikelyLoadUiRoot(std::int64_t root);
std::int64_t ResolveLiveQuickLoadUiRoot(const char* source);
bool TryOpenQuickLoadConfirmationModal(const char* source);
bool TryOpenQuickLoadConfirmationModalSafe(const char* source);
std::uint32_t GetVisibleCount(std::int64_t root);
int* GetVisibleMap(std::int64_t root);
std::uint32_t GetSelectedVisibleRow(std::int64_t root);
bool IsLoadStyleMode(std::int64_t root);
bool RewriteVisibleMapForReservedQuickSlot(std::int64_t root);
std::int64_t __fastcall LoadSelectedRefreshHook(std::int64_t root);
void* ResolveNativeToastManager();
bool NativeToastReady();
void ShowNativeToast(const char* msg);
const char* QuickActionKindToString(QuickActionKind kind);
bool TryBeginQuickAction(QuickActionKind kind, const char* source, const char* phase);
void EndQuickAction(QuickActionKind kind);
int FindRecordIndexBySlot(int slot_id);
void InvokeQuickSave(const char* source);
bool InvokeQuickLoadViaInGameCore(const char* source);
bool InvokeQuickLoadViaInGameCoreSafe(const char* source);
void SetRowControlText(std::uint64_t row_script, std::ptrdiff_t offset, const char* text);
bool ResolveDispatchMessages();

void LogSaveManagerState(const char* label, std::int64_t manager) {
    if (!g_state.config.log_enabled || label == nullptr || manager == 0) {
        return;
    }

    const std::uintptr_t iface = *reinterpret_cast<const std::uintptr_t*>(manager + 0x00);
    const auto busy = static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(manager + 0x58));
    const auto deadline = *reinterpret_cast<const std::uintptr_t*>(manager + 0x68);
    const auto last_slot = *reinterpret_cast<const std::int32_t*>(manager + 0x70);
    const auto seq = static_cast<unsigned>(*reinterpret_cast<const std::uint16_t*>(manager + 0x78));
    const auto pending_slot = *reinterpret_cast<const std::int32_t*>(manager + 0x7C);
    const auto path_store = *reinterpret_cast<const std::uintptr_t*>(manager + 0x80);
    Log("[%s] manager=%p iface=%p busy=%u deadline=%p last_slot=%d seq=%u pending_slot=%d path_store=%p\n",
        label,
        reinterpret_cast<void*>(manager),
        reinterpret_cast<void*>(iface),
        busy,
        reinterpret_cast<void*>(deadline),
        last_slot,
        seq,
        pending_slot,
        reinterpret_cast<void*>(path_store));
}

void LogSaveSourceState(const char* label, std::int64_t self) {
    if (!g_state.config.log_enabled || label == nullptr || self == 0) {
        return;
    }

    const auto root88 = *reinterpret_cast<const std::uintptr_t*>(self + 0x88);
    const auto entries = *reinterpret_cast<const std::uintptr_t*>(self + 0x78);
    const auto entry_count = *reinterpret_cast<const std::uint32_t*>(self + 0x80);
    const auto state = *reinterpret_cast<const std::uintptr_t*>(self + 0x88);
    const auto nested160 = *reinterpret_cast<const std::uintptr_t*>(self + 0xA0);
    const auto state_mode = state != 0 ? static_cast<int>(*reinterpret_cast<const std::uint8_t*>(state + 1)) : -1;
    Log("[%s] self=%p entries=%p entry_count=%u state=%p state_mode=%d nested160=%p\n",
        label,
        reinterpret_cast<void*>(self),
        reinterpret_cast<void*>(entries),
        entry_count,
        reinterpret_cast<void*>(root88),
        state_mode,
        reinterpret_cast<void*>(nested160));
}

void LogRealLoadControllerState(const char* label, std::int64_t self) {
    if (!g_state.config.log_enabled || label == nullptr || self == 0) {
        return;
    }

    const auto inner = *reinterpret_cast<const std::uintptr_t*>(self + 8);
    const auto active_object = inner != 0 ? *reinterpret_cast<const std::uintptr_t*>(inner + 496) : 0;
    const auto session_id = *reinterpret_cast<const std::uint64_t*>(self + 120);
    const auto session_seed = *reinterpret_cast<const std::int32_t*>(self + 132);
    const auto mode_byte = static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(self + 128));
    const auto attached_wide_ptr = *reinterpret_cast<const std::uintptr_t*>(self + 3144);
    const auto attached_narrow_ptr = *reinterpret_cast<const std::uintptr_t*>(self + 3368);
    const auto inline_wide = reinterpret_cast<const wchar_t*>(self + 136);
    const auto inline_narrow = reinterpret_cast<const char*>(self + 3160);
    const auto display_name = reinterpret_cast<const char*>(self + 392);
    Log("[%s] self=%p inner=%p active_object=%p session_id=%p session_seed=%d mode=%u attached_wide=%p attached_narrow=%p inline_wide=\"%s\" inline_narrow=\"%s\" display=\"%s\"\n",
        label,
        reinterpret_cast<void*>(self),
        reinterpret_cast<void*>(inner),
        reinterpret_cast<void*>(active_object),
        reinterpret_cast<void*>(session_id),
        session_seed,
        mode_byte,
        reinterpret_cast<void*>(attached_wide_ptr),
        reinterpret_cast<void*>(attached_narrow_ptr),
        NarrowWide(inline_wide).c_str(),
        inline_narrow != nullptr ? inline_narrow : "",
        display_name != nullptr ? display_name : "");
}

void LogRealLoadCandidateState(const char* label, std::int64_t candidate) {
    if (!g_state.config.log_enabled || label == nullptr || candidate == 0) {
        return;
    }

    const auto identity = *reinterpret_cast<const std::uint64_t*>(candidate + 0);
    const auto session_id = *reinterpret_cast<const std::uint64_t*>(candidate + 8);
    const auto session_seed = *reinterpret_cast<const std::int32_t*>(candidate + 16);
    const auto mode_byte = static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(candidate + 20));
    const auto wide_name = reinterpret_cast<const wchar_t*>(candidate + 22);
    const auto narrow_name = reinterpret_cast<const char*>(candidate + 3024);
    Log("[%s] candidate=%p identity=%p session_id=%p session_seed=%d mode=%u wide=\"%s\" narrow=\"%s\"\n",
        label,
        reinterpret_cast<void*>(candidate),
        reinterpret_cast<void*>(identity),
        reinterpret_cast<void*>(session_id),
        session_seed,
        mode_byte,
        NarrowWide(wide_name).c_str(),
        narrow_name != nullptr ? narrow_name : "");
}

void LogInGameLoadCoreState(const char* label, std::int64_t self) {
    if (!g_state.config.log_enabled || label == nullptr || self == 0) {
        return;
    }

    const auto slot = *reinterpret_cast<const std::int32_t*>(self + 3280);
    const auto pending_key = *reinterpret_cast<const std::uint64_t*>(self + 3224);
    const auto queued_key = *reinterpret_cast<const std::uint64_t*>(self + 3232);
    const auto has_user_actor = static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(self + 3273));
    const auto needs_refresh = static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(self + 3286));
    const auto scope_seen = static_cast<unsigned>(*reinterpret_cast<const std::uint8_t*>(self + 3287));
    const auto scope_source = *reinterpret_cast<const std::uintptr_t*>(self + 40);
    const auto event_sink = *reinterpret_cast<const std::uintptr_t*>(self + 88);
    Log("[%s] self=%p slot=%d pending_key=%p queued_key=%p has_user_actor=%u needs_refresh=%u scope_seen=%u scope_source=%p event_sink=%p\n",
        label,
        reinterpret_cast<void*>(self),
        slot,
        reinterpret_cast<void*>(pending_key),
        reinterpret_cast<void*>(queued_key),
        has_user_actor,
        needs_refresh,
        scope_seen,
        reinterpret_cast<void*>(scope_source),
        reinterpret_cast<void*>(event_sink));
}

bool ResolveXInputGetState() {
    if (g_xinput_get_state != nullptr) {
        return true;
    }
    if (g_xinput_resolve_attempted) {
        return false;
    }
    g_xinput_resolve_attempted = true;

    static constexpr const char* kModules[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (const char* module_name : kModules) {
        HMODULE module = GetModuleHandleA(module_name);
        if (module == nullptr) {
            module = LoadLibraryA(module_name);
        }
        if (module == nullptr) {
            continue;
        }

        auto* fn = reinterpret_cast<XInputGetStateDynFn>(GetProcAddress(module, "XInputGetState"));
        if (fn != nullptr) {
            g_xinput_get_state = fn;
            Log("[i] Resolved XInputGetState from %s\n", module_name);
            return true;
        }
    }

    Log("[W] Failed to resolve XInputGetState; XInput hotkeys unavailable\n");
    return false;
}

BOOL CALLBACK FindGameWindowProc(HWND hwnd, LPARAM lparam) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != GetCurrentProcessId()) {
        return TRUE;
    }
    if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
        return TRUE;
    }

    *reinterpret_cast<HWND*>(lparam) = hwnd;
    return FALSE;
}

HWND FindGameWindow() {
    HWND hwnd = nullptr;
    EnumWindows(&FindGameWindowProc, reinterpret_cast<LPARAM>(&hwnd));
    return hwnd;
}

bool IsGameWindowForeground() {
    return g_state.game_window != nullptr && GetForegroundWindow() == g_state.game_window;
}

bool IsXInputHidShim(HANDLE device) {
    if (device == nullptr) {
        return false;
    }

    UINT name_size = 0;
    if (GetRawInputDeviceInfoA(device, RIDI_DEVICENAME, nullptr, &name_size) != 0 || name_size == 0) {
        return false;
    }

    std::string name(name_size, '\0');
    if (GetRawInputDeviceInfoA(device, RIDI_DEVICENAME, name.data(), &name_size) == static_cast<UINT>(-1)) {
        return false;
    }

    return name.find("IG_") != std::string::npos;
}

bool IsDualSenseHidDevice(HANDLE device) {
    if (device == nullptr || IsXInputHidShim(device)) {
        return false;
    }

    RID_DEVICE_INFO info{};
    info.cbSize = sizeof(info);
    UINT size = sizeof(info);
    if (GetRawInputDeviceInfoA(device, RIDI_DEVICEINFO, &info, &size) == static_cast<UINT>(-1)) {
        return false;
    }
    if (info.dwType != RIM_TYPEHID || info.hid.dwVendorId != 0x054C) {
        return false;
    }

    return info.hid.dwProductId == 0x0CE6 || info.hid.dwProductId == 0x0DF2;
}

bool GetDualSensePreparsedData(HANDLE device, std::vector<BYTE>& storage, PHIDP_PREPARSED_DATA* out_data) {
    if (out_data != nullptr) {
        *out_data = nullptr;
    }

    UINT size = 0;
    if (GetRawInputDeviceInfoA(device, RIDI_PREPARSEDDATA, nullptr, &size) == static_cast<UINT>(-1) || size == 0) {
        return false;
    }

    storage.resize(size);
    if (GetRawInputDeviceInfoA(device, RIDI_PREPARSEDDATA, storage.data(), &size) == static_cast<UINT>(-1)) {
        return false;
    }

    if (out_data != nullptr) {
        *out_data = reinterpret_cast<PHIDP_PREPARSED_DATA>(storage.data());
    }
    return true;
}

WORD MapDualSenseReportToButtons(HANDLE device, const BYTE* report, ULONG report_len) {
    if (device == nullptr || report == nullptr || report_len == 0) {
        return 0;
    }

    std::vector<BYTE> preparsed_storage;
    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!GetDualSensePreparsedData(device, preparsed_storage, &preparsed) || preparsed == nullptr) {
        return 0;
    }

    WORD buttons = 0;
    USAGE usages[32] = {};
    ULONG usage_count = static_cast<ULONG>(std::size(usages));
    if (HidP_GetUsages(HidP_Input, HID_USAGE_PAGE_BUTTON, 0, usages, &usage_count,
            preparsed, reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), report_len) == HIDP_STATUS_SUCCESS) {
        for (ULONG i = 0; i < usage_count; ++i) {
            switch (usages[i]) {
            case 1: buttons |= XINPUT_GAMEPAD_X; break;
            case 2: buttons |= XINPUT_GAMEPAD_A; break;
            case 3: buttons |= XINPUT_GAMEPAD_B; break;
            case 4: buttons |= XINPUT_GAMEPAD_Y; break;
            case 5: buttons |= XINPUT_GAMEPAD_LEFT_SHOULDER; break;
            case 6: buttons |= XINPUT_GAMEPAD_RIGHT_SHOULDER; break;
            default: break;
            }
        }
    }

    ULONG hat_value = 8;
    if (HidP_GetUsageValue(HidP_Input, HID_USAGE_PAGE_GENERIC, 0, HID_USAGE_GENERIC_HATSWITCH,
            &hat_value, preparsed, reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)), report_len) == HIDP_STATUS_SUCCESS) {
        switch (hat_value) {
        case 0: buttons |= XINPUT_GAMEPAD_DPAD_UP; break;
        case 1: buttons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 2: buttons |= XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 3: buttons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_RIGHT; break;
        case 4: buttons |= XINPUT_GAMEPAD_DPAD_DOWN; break;
        case 5: buttons |= XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 6: buttons |= XINPUT_GAMEPAD_DPAD_LEFT; break;
        case 7: buttons |= XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_LEFT; break;
        default: break;
        }
    }

    return buttons;
}

void ProcessDualSenseRawInput(HRAWINPUT raw_input_handle) {
    if (raw_input_handle == nullptr) {
        return;
    }

    UINT raw_size = 0;
    if (GetRawInputData(raw_input_handle, RID_INPUT, nullptr, &raw_size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)
        || raw_size < sizeof(RAWINPUTHEADER)) {
        return;
    }

    std::vector<BYTE> storage(raw_size);
    RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(storage.data());
    if (GetRawInputData(raw_input_handle, RID_INPUT, raw, &raw_size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
        return;
    }
    if (raw->header.dwType != RIM_TYPEHID || !IsDualSenseHidDevice(raw->header.hDevice)) {
        return;
    }

    const ULONG report_size = raw->data.hid.dwSizeHid;
    const ULONG report_count = raw->data.hid.dwCount;
    BYTE* report_bytes = raw->data.hid.bRawData;
    if (report_size == 0 || report_count == 0 || report_bytes == nullptr) {
        return;
    }

    WORD buttons = 0;
    for (ULONG i = 0; i < report_count; ++i) {
        buttons |= MapDualSenseReportToButtons(raw->header.hDevice, report_bytes + (report_size * i), report_size);
    }

    g_state.dualsense_buttons.store(buttons);
    g_state.dualsense_last_input_ms.store(GetTickCount64());
}

void RegisterDualSenseRawInput(HWND hwnd) {
    if (hwnd == nullptr) {
        return;
    }

    const auto hwnd_value = reinterpret_cast<std::uintptr_t>(hwnd);
    if (g_state.raw_input_target_hwnd.load() == hwnd_value) {
        return;
    }

    RAWINPUTDEVICE devices[2] = {};
    devices[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[0].usUsage = HID_USAGE_GENERIC_GAMEPAD;
    devices[0].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    devices[0].hwndTarget = hwnd;
    devices[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[1].usUsage = HID_USAGE_GENERIC_JOYSTICK;
    devices[1].dwFlags = RIDEV_INPUTSINK | RIDEV_DEVNOTIFY;
    devices[1].hwndTarget = hwnd;

    if (RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        g_state.raw_input_target_hwnd.store(hwnd_value);
        Log("[+] Registered DualSense raw input on hwnd=%p\n", hwnd);
    } else {
        Log("[W] RegisterRawInputDevices failed gle=%lu\n", GetLastError());
    }
}

void QueueQuickCommand(UINT message, const char* source) {
    if (message == 0) {
        Log("[queue] source=%s message=0x%X unavailable no-dispatch-message\n", source, message);
        return;
    }
    if (g_state.game_window == nullptr) {
        Log("[queue] source=%s message=0x%X unavailable no-game-window\n", source, message);
        return;
    }

    if (!g_state.quick_actions_enabled.load()) {
        Log("[queue] source=%s message=0x%X unavailable transition-lock\n", source, message);
        return;
    }

    if (g_state.quick_load_confirm_modal_active.load()) {
        Log("[queue] source=%s message=0x%X unavailable busy active=load-confirm-modal\n", source, message);
        return;
    }

    const auto requested_kind = message == g_state.quick_save_message ? QuickActionKind::Save : QuickActionKind::Load;
    const auto active_kind = static_cast<QuickActionKind>(g_state.quick_action_active.load());
    if (active_kind != QuickActionKind::None) {
        Log("[queue] source=%s message=0x%X unavailable busy active=%s\n",
            source,
            message,
            QuickActionKindToString(active_kind));
        return;
    }

    if ((message == g_state.quick_save_message && g_state.pending_quick_load.load())
        || (message == g_state.quick_load_message && g_state.pending_quick_save.load())) {
        Log("[queue] source=%s message=0x%X unavailable pending-other requested=%s\n",
            source,
            message,
            QuickActionKindToString(requested_kind));
        return;
    }

    std::atomic<bool>* pending = message == g_state.quick_save_message ? &g_state.pending_quick_save : &g_state.pending_quick_load;
    bool expected = false;
    if (!pending->compare_exchange_strong(expected, true)) {
        return;
    }

    if (!PostMessageW(g_state.game_window, message, static_cast<WPARAM>(kQuickDispatchToken), 0)) {
        pending->store(false);
        Log("[queue] source=%s message=0x%X PostMessage failed gle=%lu\n", source, message, GetLastError());
        return;
    }

    Log("[queue] source=%s message=0x%X posted hwnd=%p\n", source, message, g_state.game_window);
}

LRESULT CALLBACK HookedGameWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_INPUT) {
        ProcessDualSenseRawInput(reinterpret_cast<HRAWINPUT>(lparam));
    } else if (msg == WM_INPUT_DEVICE_CHANGE && hwnd == g_state.game_window && wparam == GIDC_REMOVAL) {
        const HANDLE device = reinterpret_cast<HANDLE>(lparam);
        if (IsDualSenseHidDevice(device)) {
            g_state.dualsense_buttons.store(0);
            g_state.dualsense_last_input_ms.store(0);
            g_state.previous_controller_buttons = 0;
            if (g_state.controller_source == ControllerSource::DualSenseRaw) {
                g_state.controller_source = ControllerSource::None;
            }
        }
    }

    if (msg == g_state.quick_save_message && wparam == static_cast<WPARAM>(kQuickDispatchToken)) {
        const std::uintptr_t actor = ResolveQuickSaveActor();
        const DWORD save_tid = ResolveQuickSaveThreadId();
        if (save_tid == 0 || actor == 0) {
            g_state.pending_quick_save.store(false);
            Log("[quick-save] unavailable reason=no-save-context\n");
            ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastQuickSaveFailed, "QUICK SAVE FAILED"));
            return 0;
        }
        return 0;
    } else if (msg == g_state.quick_load_message && wparam == static_cast<WPARAM>(kQuickDispatchToken)) {
        const bool claimed = g_state.pending_quick_load.exchange(false);
        if (!claimed) {
            return 0;
        }

        Log("[quick-load] message received hwnd=%p\n", hwnd);

        if (TryOpenQuickLoadConfirmationModalSafe("wndproc-ui")) {
            return 0;
        }

        if (g_state.config.quick_load_confirmation_enabled) {
            Log("[quick-load] unavailable reason=confirmation-open-failed direct-fallback-blocked\n");
            ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastQuickLoadFailed, "QUICK LOAD FAILED"));
            return 0;
        }

        if (InvokeQuickLoadViaInGameCoreSafe("wndproc-ui")) {
            return 0;
        }

        Log("[quick-load] unavailable reason=in-game-core-failed\n");
        ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastQuickLoadFailed, "QUICK LOAD FAILED"));
        return 0;
    }

    if (g_state.original_wndproc != nullptr) {
        return CallWindowProcW(g_state.original_wndproc, hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void EnsureGameWindowHooked() {
    HWND hwnd = FindGameWindow();
    if (hwnd == nullptr || hwnd == g_state.game_window) {
        if (hwnd != nullptr && g_state.game_window == hwnd && g_state.original_wndproc == nullptr) {
            RegisterDualSenseRawInput(hwnd);
        }
        return;
    }

    if (g_state.game_window != nullptr && g_state.original_wndproc != nullptr) {
        SetWindowLongPtrW(g_state.game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_state.original_wndproc));
        g_state.original_wndproc = nullptr;
    }

    g_state.game_window = hwnd;
    g_state.original_wndproc =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedGameWndProc)));
    RegisterDualSenseRawInput(hwnd);
    Log("[+] Hooked game window hwnd=%p wndproc=%p\n", hwnd, reinterpret_cast<void*>(g_state.original_wndproc));
}

void ResetHotkeyEdgeState() {
    g_state.previous_quick_save_key_down = false;
    g_state.previous_quick_load_key_down = false;
    g_state.previous_controller_buttons = 0;
    g_state.controller_source = ControllerSource::None;
}

bool IsKeyboardHotkeyPressed(int vk, bool& previous_down) {
    if (vk == 0) {
        previous_down = false;
        return false;
    }

    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !previous_down;
    previous_down = down;
    return pressed;
}

bool PollControllerButtons(WORD& out_buttons) {
    out_buttons = 0;

    XINPUT_STATE xinput_state{};
    if (ResolveXInputGetState() && g_xinput_get_state != nullptr && g_xinput_get_state(0, &xinput_state) == ERROR_SUCCESS) {
        out_buttons = xinput_state.Gamepad.wButtons;
        if (g_state.controller_source != ControllerSource::XInput) {
            g_state.previous_controller_buttons = 0;
            g_state.controller_source = ControllerSource::XInput;
        }
        return true;
    }

    const ULONGLONG now = GetTickCount64();
    const ULONGLONG last_raw_input = g_state.dualsense_last_input_ms.load();
    const WORD dualsense_buttons = g_state.dualsense_buttons.load();
    if (last_raw_input != 0 && ((now - last_raw_input) <= 1500ULL || dualsense_buttons != 0)) {
        out_buttons = dualsense_buttons;
        if (g_state.controller_source != ControllerSource::DualSenseRaw) {
            g_state.previous_controller_buttons = 0;
            g_state.controller_source = ControllerSource::DualSenseRaw;
        }
        return true;
    }

    g_state.previous_controller_buttons = 0;
    g_state.controller_source = ControllerSource::None;
    return false;
}

bool IsControllerHotkeyPressed(WORD combo_mask, WORD buttons) {
    if (combo_mask == 0) {
        return false;
    }

    const bool pressed_now = IsControllerComboPressed(buttons, combo_mask);
    const bool pressed_prev = IsControllerComboPressed(g_state.previous_controller_buttons, combo_mask);
    return pressed_now && !pressed_prev;
}

void InvokeQuickSave(const char* source) {
    const ULONGLONG now = GetTickCount64();
    if (now < g_state.last_quick_save_dispatch_ms + kHotkeyCooldownMs) {
        return;
    }

    if (!TryBeginQuickAction(QuickActionKind::Save, source, "invoke")) {
        return;
    }

    if (g_direct_local_save_original == nullptr) {
        Log("[quick-save] source=%s unavailable reason=no-direct-save slot=%d\n", source, g_state.config.quick_slot_id);
        ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastSaveFunctionUnavailable, "SAVE FUNCTION UNAVAILABLE"));
        EndQuickAction(QuickActionKind::Save);
        return;
    }

    const std::uintptr_t actor = ResolveQuickSaveActor();
    if (actor == 0) {
        Log("[quick-save] source=%s unavailable reason=no-save-actor slot=%d\n", source, g_state.config.quick_slot_id);
        ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastNoSaveActor, "NO SAVE ACTOR"));
        EndQuickAction(QuickActionKind::Save);
        return;
    }

    int precheck_result = 0;
    if (g_save_precheck != nullptr) {
        DirectSaveCallScratch precheck_scratch{};
        auto* precheck_flags = reinterpret_cast<unsigned char*>(&precheck_scratch.flags_word);
        int* precheck_status =
            g_save_precheck(static_cast<std::int64_t>(actor), &precheck_result, static_cast<std::int64_t>(actor), precheck_flags);
        if (precheck_status == nullptr || precheck_result != 0) {
            Log("[quick-save] source=%s unavailable reason=native-precheck slot=%d code=0x%08X\n",
                source,
                g_state.config.quick_slot_id,
                static_cast<unsigned>(precheck_result));
            ShowNativeToastFormatted(LocalizedTextValue(TextId::ToastSaveBlockedCode, "SAVE BLOCKED (0x%08X)"), static_cast<unsigned>(precheck_result));
            EndQuickAction(QuickActionKind::Save);
            return;
        }
    }

    g_state.quick_save_active.store(true);
    DirectSaveCallScratch scratch{};
    auto* flags = reinterpret_cast<unsigned char*>(&scratch.flags_word);
    auto* out_result = &scratch.out_result;
    const int* result = g_direct_local_save_original(
        static_cast<std::int64_t>(actor),
        out_result,
        g_state.config.quick_slot_id,
        flags);
    g_state.quick_save_active.store(false);
    EndQuickAction(QuickActionKind::Save);

    g_state.last_quick_save_dispatch_ms = now;
    const bool save_succeeded = result != nullptr && *out_result == 0;
    Log("[quick-save] source=%s slot=%d out=%u success=%d\n",
        source,
        g_state.config.quick_slot_id,
        static_cast<unsigned>(*out_result),
        save_succeeded ? 1 : 0);
    if (save_succeeded && g_state.config.toast_notification_enabled) {
        ShowNativeToast(LocalizedTextValue(TextId::ToastQuickSaveSuccess, "QUICK SAVE SUCCESS"));
    } else if (!save_succeeded) {
        ShowNativeToastFormatted(LocalizedTextValue(TextId::ToastQuickSaveFailedCode, "QUICK SAVE FAILED (%u)"), static_cast<unsigned>(*out_result));
    }
}

bool InvokeQuickLoadViaInGameCore(const char* source) {
    if (source == nullptr) {
        return false;
    }

    if (g_in_game_menu_load_core_original == nullptr) {
        Log("[quick-load] source=%s unavailable reason=no-load-function\n", source);
        ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastLoadFunctionUnavailable, "LOAD FUNCTION UNAVAILABLE"));
        return false;
    }

    if (!TryBeginQuickAction(QuickActionKind::Load, source, "wndproc-ui")) {
        return false;
    }

    if (FindRecordIndexBySlot(g_state.config.quick_slot_id) < 0) {
        Log("[quick-load] source=%s unavailable reason=slot-missing slot=%d\n", source, g_state.config.quick_slot_id);
        ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastNoQuickSaveFound, "NO QUICK SAVE FOUND"));
        EndQuickAction(QuickActionKind::Load);
        return false;
    }

    ClientActorScope user_scope{};
    const bool has_user_scope = AcquireLiveClientUserActorScope(user_scope);
    const auto resolved_self = static_cast<std::int64_t>(ResolveInGameLoadCoreFromService());
    const auto cached_self = static_cast<std::int64_t>(g_state.cached_in_game_load_core_self.load());
    const auto self = resolved_self != 0 ? resolved_self : cached_self;
    std::uint64_t key = g_state.cached_in_game_load_target_key.load();
    if (key == 0) {
        key = 0x7FFFFFFFFFFFFFFFui64;
    }

    if (!has_user_scope || self == 0 || key == 0) {
        Log("[quick-load] source=%s unavailable reason=in-game-core-context\n", source);
        ShowNativeToastFormatted("%s", LocalizedTextValue(TextId::ToastGameStateUnavailable, "GAME STATE UNAVAILABLE"));
        ReleaseClientActorScope(user_scope);
        EndQuickAction(QuickActionKind::Load);
        return false;
    }

    std::int32_t out_result = 0;
    std::int64_t target_key = static_cast<std::int64_t>(key);
    ArmGameplayActionGate("quick-load-dispatch");
    std::int32_t* result = nullptr;
    __try {
        result = g_in_game_menu_load_core_original(
            self,
            &out_result,
            &target_key,
            static_cast<unsigned int>(g_state.config.quick_slot_id));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[quick-load] source=%s path=in-game-load-core exception=0x%08lX self=%p key=0x%llX slot=%d\n",
            source,
            static_cast<unsigned long>(GetExceptionCode()),
            reinterpret_cast<void*>(self),
            static_cast<unsigned long long>(key),
            g_state.config.quick_slot_id);
        ReleaseClientActorScope(user_scope);
        EndQuickAction(QuickActionKind::Load);
        return false;
    }
    Log("[quick-load] source=%s path=in-game-load-core slot=%d out=%d success=%d\n",
        source,
        g_state.config.quick_slot_id,
        out_result,
        (result != nullptr && out_result == 0) ? 1 : 0);
    ReleaseClientActorScope(user_scope);
    EndQuickAction(QuickActionKind::Load);
    return true;
}

bool InvokeQuickLoadViaInGameCoreSafe(const char* source) {
    __try {
        return InvokeQuickLoadViaInGameCore(source);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[quick-load] source=%s unavailable reason=exception code=0x%08lX\n",
            source != nullptr ? source : "<null>",
            static_cast<unsigned long>(GetExceptionCode()));
        EndQuickAction(QuickActionKind::Load);
        return false;
    }
}

void PollHotkeys() {
    if (!IsGameWindowForeground()) {
        ResetHotkeyEdgeState();
        return;
    }

    const bool quick_save_key = IsKeyboardHotkeyPressed(g_state.config.quick_save_vk, g_state.previous_quick_save_key_down);
    const bool quick_load_key = IsKeyboardHotkeyPressed(g_state.config.quick_load_vk, g_state.previous_quick_load_key_down);

    WORD controller_buttons = 0;
    const bool has_controller = PollControllerButtons(controller_buttons);
    const bool quick_save_controller =
        has_controller && IsControllerHotkeyPressed(g_state.config.quick_save_controller_mask, controller_buttons);
    const bool quick_load_controller =
        has_controller && IsControllerHotkeyPressed(g_state.config.quick_load_controller_mask, controller_buttons);
    if (has_controller) {
        g_state.previous_controller_buttons = controller_buttons;
    }

    if (quick_save_key || quick_save_controller) {
        QueueQuickCommand(g_state.quick_save_message, quick_save_key ? "keyboard" : "controller");
    }
    if (quick_load_key || quick_load_controller) {
        QueueQuickCommand(g_state.quick_load_message, quick_load_key ? "keyboard" : "controller");
    }
}

DWORD WINAPI HotkeyThreadProc(LPVOID) {
    Log("[+] Hotkey worker started\n");

    while (WaitForSingleObject(g_state.stop_event, kHotkeyPollIntervalMs) == WAIT_TIMEOUT) {
        EnsureGameWindowHooked();
        PollHotkeys();
    }

    Log("[i] Hotkey worker stopping\n");
    return 0;
}

std::uint64_t GetGameState() {
    auto* game_state = reinterpret_cast<std::uint64_t*>(resolver::Address(resolver::SymbolId::GameStateGlobal));
    return game_state != nullptr ? *game_state : 0;
}

bool AcquireLiveClientActorScope(ClientActorScope& out_scope) {
    std::memset(&out_scope, 0, sizeof(out_scope));

    if (g_acquire_client_actor_scope == nullptr) {
        return false;
    }

    __try {
        const std::uint64_t game_state = GetGameState();
        if (game_state == 0) {
            return false;
        }

        const std::uint64_t client_state = *reinterpret_cast<const std::uint64_t*>(game_state + 72);
        if (client_state == 0) {
            return false;
        }

        const std::uint64_t scope_source = *reinterpret_cast<const std::uint64_t*>(client_state + 40);
        if (scope_source == 0) {
            return false;
        }

        g_acquire_client_actor_scope(static_cast<std::int64_t>(scope_source), &out_scope);
        return out_scope.object != nullptr && out_scope.valid != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(&out_scope, 0, sizeof(out_scope));
        return false;
    }
}

bool AcquireLiveClientUserActorScope(ClientActorScope& out_scope) {
    std::memset(&out_scope, 0, sizeof(out_scope));

    if (g_acquire_client_user_actor_scope == nullptr) {
        return false;
    }

    __try {
        const auto* service_global = reinterpret_cast<const std::uint64_t*>(resolver::Address(resolver::SymbolId::GameServiceGlobal));
        const std::uint64_t service_root = service_global != nullptr ? *service_global : 0;
        if (service_root == 0) {
            return false;
        }

        const std::uint64_t scope_source = *reinterpret_cast<const std::uint64_t*>(service_root + 0x30);
        if (scope_source == 0) {
            return false;
        }

        g_acquire_client_user_actor_scope(static_cast<std::int64_t>(scope_source), &out_scope);
        return out_scope.object != nullptr && out_scope.valid != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::memset(&out_scope, 0, sizeof(out_scope));
        return false;
    }
}

void ReleaseClientActorScope(ClientActorScope& scope) {
    if (scope.object == nullptr || scope.valid == 0) {
        std::memset(&scope, 0, sizeof(scope));
        return;
    }

    char released = 0;
    if (scope.special_release != 0 && g_scope_special_release != nullptr) {
        released = g_scope_special_release(scope.object);
    } else {
        const auto vtable = *reinterpret_cast<std::uintptr_t*>(scope.object);
        if (vtable != 0) {
            auto* release_fn = reinterpret_cast<char(__fastcall*)(void*)>(*reinterpret_cast<std::uintptr_t*>(vtable + 0x10));
            if (release_fn != nullptr) {
                released = release_fn(scope.object);
            }
        }
    }

    if (released) {
        const auto vtable = *reinterpret_cast<std::uintptr_t*>(scope.object);
        if (vtable != 0) {
            auto* finalize_fn = reinterpret_cast<void(__fastcall*)(void*)>(*reinterpret_cast<std::uintptr_t*>(vtable + 0x18));
            if (finalize_fn != nullptr) {
                finalize_fn(scope.object);
            }
        }
    }

    std::memset(&scope, 0, sizeof(scope));
}

std::uintptr_t ResolveInGameLoadCoreFromService() {
    __try {
        const auto* service_global = reinterpret_cast<const std::uint64_t*>(resolver::Address(resolver::SymbolId::GameServiceGlobal));
        const std::uint64_t service_root = service_global != nullptr ? *service_global : 0;
        if (service_root == 0) {
            return 0;
        }

        return static_cast<std::uintptr_t>(*reinterpret_cast<const std::uint64_t*>(service_root + 0xD0));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uintptr_t ResolveQuickSaveActor() {
    const std::uintptr_t direct_actor = g_state.cached_direct_save_actor.load();
    if (direct_actor != 0) {
        return direct_actor;
    }
    return g_state.cached_service_save_actor.load();
}

DWORD ResolveQuickSaveThreadId() {
    const DWORD direct_tid = g_state.cached_direct_save_thread_id.load();
    if (direct_tid != 0) {
        return direct_tid;
    }
    return g_state.cached_service_save_thread_id.load();
}

bool PassesNativeQuickActionReadyCheck(unsigned* out_code) {
    if (out_code != nullptr) {
        *out_code = 0;
    }

    const std::uintptr_t actor = ResolveQuickSaveActor();
    if (actor == 0) {
        if (out_code != nullptr) {
            *out_code = 0xFFFFFFFFu;
        }
        return false;
    }
    return true;
}

ResolvedWorldEnv ResolveWorldEnv() {
    ResolvedWorldEnv env{};
    if (g_world_env_manager_global == nullptr) {
        return env;
    }

    __try {
        const std::uint64_t env_mgr = *g_world_env_manager_global;
        if (env_mgr == 0) {
            return env;
        }

        const auto* vtable = *reinterpret_cast<std::uintptr_t* const*>(env_mgr);
        if (vtable == nullptr) {
            return env;
        }

        auto get_entity = reinterpret_cast<std::int64_t(__fastcall*)(void*)>(vtable[0x40 / 8]);
        if (get_entity == nullptr) {
            return env;
        }

        env.entity = get_entity(reinterpret_cast<void*>(env_mgr));
        if (env.entity == 0) {
            return env;
        }

        env.weather_state = *reinterpret_cast<std::int64_t*>(env.entity + 0xED8);
        if (env.weather_state == 0) {
            return env;
        }

        const std::int64_t container = *reinterpret_cast<std::int64_t*>(env.weather_state + 0x50);
        if (container == 0) {
            return env;
        }

        env.cloud_node = *reinterpret_cast<std::int64_t*>(container + 0x18);
        env.wind_node = *reinterpret_cast<std::int64_t*>(container + 0x20);
        env.particle_mgr = *reinterpret_cast<std::int64_t*>(env.entity + 0xEE0);
        env.valid = true;
        return env;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ResolvedWorldEnv{};
    }
}

void ArmGameplayActionGate(const char* reason) {
    g_state.quick_actions_enabled.store(false);
    g_state.gameplay_gate_armed.store(true);
    g_state.gameplay_ready_since_ms.store(0);
    Log("[gate] armed reason=%s\n", reason != nullptr ? reason : "<null>");
}

void TickGameplayActionGateOnGameThread() {
    if (!g_state.gameplay_gate_armed.load()) {
        return;
    }

    unsigned native_ready_code = 0;
    if (!PassesNativeQuickActionReadyCheck(&native_ready_code)) {
        g_state.gameplay_ready_since_ms.store(0);
        return;
    }

    const ResolvedWorldEnv env = ResolveWorldEnv();
    const bool gameplay_root_ready = env.entity != 0 && env.weather_state != 0;
    if (!gameplay_root_ready) {
        g_state.gameplay_ready_since_ms.store(0);
        return;
    }

    const ULONGLONG now = GetTickCount64();
    ULONGLONG ready_since = g_state.gameplay_ready_since_ms.load();
    if (ready_since == 0) {
        g_state.gameplay_ready_since_ms.store(now);
        return;
    }

    if (now - ready_since < kGameplayReadyDelayMs) {
        return;
    }

    g_state.quick_actions_enabled.store(true);
    g_state.gameplay_gate_armed.store(false);
    Log("[gate] enabled after %llums stable-ready\n", static_cast<unsigned long long>(now - ready_since));
}

const char* QuickActionKindToString(QuickActionKind kind) {
    switch (kind) {
    case QuickActionKind::Save: return "save";
    case QuickActionKind::Load: return "load";
    default: return "none";
    }
}

bool TryBeginQuickAction(QuickActionKind kind, const char* source, const char* phase) {
    std::int32_t expected = static_cast<std::int32_t>(QuickActionKind::None);
    const std::int32_t desired = static_cast<std::int32_t>(kind);
    if (!g_state.quick_action_active.compare_exchange_strong(expected, desired)) {
        Log("[quick-%s] source=%s unavailable reason=busy active=%s phase=%s\n",
            QuickActionKindToString(kind),
            source != nullptr ? source : "<null>",
            QuickActionKindToString(static_cast<QuickActionKind>(expected)),
            phase != nullptr ? phase : "<null>");
        return false;
    }
    return true;
}

void EndQuickAction(QuickActionKind kind) {
    std::int32_t expected = static_cast<std::int32_t>(kind);
    g_state.quick_action_active.compare_exchange_strong(expected, static_cast<std::int32_t>(QuickActionKind::None));
}

void CacheLoadUiRoot(std::int64_t root, const char* source) {
    (void)source;
    if (root == 0 || !IsLoadStyleMode(root)) {
        return;
    }

    const std::uintptr_t root_u = static_cast<std::uintptr_t>(root);
    const std::uintptr_t vftable = *reinterpret_cast<const std::uintptr_t*>(root);
    g_state.cached_load_ui_root.store(root_u);
    g_state.cached_load_ui_vftable.store(vftable);
}

void ClearCachedLoadUiRoot() {
    g_state.cached_load_ui_root.store(0);
    g_state.cached_load_ui_vftable.store(0);
}

bool IsLikelyLoadUiRoot(std::int64_t root) {
    if (root == 0) {
        return false;
    }

    __try {
        const auto vftable = *reinterpret_cast<const std::uintptr_t*>(root);
        const auto expected_vftable = g_state.cached_load_ui_vftable.load();
        const auto list_widget = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetListWidget);
        const auto visible_map = GetVisibleMap(root);
        const auto visible_count = GetVisibleCount(root);
        const auto active_modal = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetActiveModal);
        const auto mode = *reinterpret_cast<const std::uint8_t*>(root + kRootOffsetMode);
        if (vftable == 0 || list_widget == 0 || visible_map == nullptr || visible_count == 0 || mode == 0) {
            return false;
        }
        if (expected_vftable != 0 && vftable != expected_vftable) {
            return false;
        }
        if (active_modal != 0 && active_modal == static_cast<std::uintptr_t>(root)) {
            return false;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::uint64_t GetUiSystemRoot() {
    const auto root_global = resolver::Address(resolver::SymbolId::GameStateGlobal);
    if (root_global == 0) {
        return 0;
    }

    __try {
        return *reinterpret_cast<const std::uint64_t*>(root_global);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::uint64_t GetUiSystemManager() {
    const std::uint64_t ui_root = GetUiSystemRoot();
    if (ui_root == 0) {
        return 0;
    }

    __try {
        return *reinterpret_cast<const std::uint64_t*>(ui_root + kUiSystemRootOffsetManager);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

std::int64_t FindFirstRegisteredUiObject(std::uint64_t manager, std::ptrdiff_t list_offset) {
    if (manager == 0) {
        return 0;
    }

    __try {
        const auto list = *reinterpret_cast<const std::uint64_t*>(manager + list_offset);
        if (list == 0) {
            return 0;
        }

        const auto count = *reinterpret_cast<const std::uint32_t*>(list + kUiManagerVectorCountOffset);
        const auto entries = *reinterpret_cast<const std::uint64_t*>(list + kUiManagerVectorDataOffset);
        if (count == 0 || entries == 0) {
            return 0;
        }

        for (std::uint32_t index = 0; index < count; ++index) {
            const auto entry = entries + static_cast<std::uint64_t>(index) * 24ULL;
            const auto object = *reinterpret_cast<const std::uint64_t*>(entry);
            if (object != 0) {
                return static_cast<std::int64_t>(object);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }

    return 0;
}

std::int64_t PrimeLiveQuickLoadUiRoot(std::int64_t root, const char* source) {
    if (root == 0) {
        return 0;
    }

    if (g_build_visible_map_original != nullptr) {
        __try {
            g_build_visible_map_original(root);
            CacheLoadUiRoot(root, source);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[quick-load-confirm] prime-root exception=0x%08lX root=%p source=%s\n",
                static_cast<unsigned long>(GetExceptionCode()),
                reinterpret_cast<void*>(root),
                source != nullptr ? source : "<null>");
            return 0;
        }
    }
    if (IsLikelyLoadUiRoot(root)) {
        CacheLoadUiRoot(root, source);
        return root;
    }
    return 0;
}

std::int64_t ResolveLiveQuickLoadUiRoot(const char* source) {
    const auto cached_root = static_cast<std::int64_t>(g_state.cached_load_ui_root.load());
    if (IsLikelyLoadUiRoot(cached_root)) {
        return cached_root;
    }
    if (cached_root != 0) {
        Log("[quick-load-confirm] ignored invalid cached root=%p\n", reinterpret_cast<void*>(cached_root));
        ClearCachedLoadUiRoot();
    }

    const std::uint64_t manager = GetUiSystemManager();
    if (manager == 0) {
        Log("[quick-load-confirm] unavailable reason=no-ui-manager\n");
        return 0;
    }

    auto root = FindFirstRegisteredUiObject(manager, kUiManagerRootGameDataLoadListOffset);
    if (IsLikelyLoadUiRoot(root)) {
        CacheLoadUiRoot(root, source);
        return root;
    }
    if (root != 0) {
        Log("[quick-load-confirm] load-root candidate requires prime root=%p\n", reinterpret_cast<void*>(root));
    }
    root = PrimeLiveQuickLoadUiRoot(root, source);
    if (IsLikelyLoadUiRoot(root)) {
        return root;
    }

    const auto root_title = FindFirstRegisteredUiObject(manager, kUiManagerRootTitleListOffset);
    if (root_title != 0 && g_root_title_handle_open_load_view != nullptr) {
        Log("[quick-load-confirm] attempting open-load-view root_title=%p\n", reinterpret_cast<void*>(root_title));
        __try {
            g_root_title_handle_open_load_view(root_title);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[quick-load-confirm] unavailable reason=open-load-view-except code=0x%08lX root_title=%p\n",
                static_cast<unsigned long>(GetExceptionCode()),
                reinterpret_cast<void*>(root_title));
            return 0;
        }

        root = FindFirstRegisteredUiObject(manager, kUiManagerRootGameDataLoadListOffset);
        if (root != 0) {
            Log("[quick-load-confirm] post-open load-root candidate=%p\n", reinterpret_cast<void*>(root));
        }
        root = PrimeLiveQuickLoadUiRoot(root, source);
        if (IsLikelyLoadUiRoot(root)) {
            CacheLoadUiRoot(root, source);
            return root;
        }
    }

    return 0;
}

bool TryOpenQuickLoadConfirmationModal(const char* source) {
    Log("[quick-load-confirm] begin source=%s\n", source != nullptr ? source : "<null>");

    if (!g_state.config.quick_load_confirmation_enabled || g_load_selected_refresh_original == nullptr) {
        return false;
    }

    if (g_state.quick_load_confirm_modal_active.load()) {
        Log("[quick-load-confirm] unavailable reason=modal-already-open\n");
        return true;
    }

    const auto root = ResolveLiveQuickLoadUiRoot(source);
    if (!IsLikelyLoadUiRoot(root)) {
        Log("[quick-load-confirm] unavailable reason=no-load-ui-root\n");
        return false;
    }

    const std::uint64_t list_widget = *reinterpret_cast<std::uint64_t*>(root + kRootOffsetListWidget);
    if (list_widget == 0) {
        Log("[quick-load-confirm] unavailable reason=no-list-widget\n");
        return false;
    }

    const auto previous_row = *reinterpret_cast<std::uint32_t*>(list_widget + 0x118);
    const auto previous_modal = *reinterpret_cast<std::uintptr_t*>(root + kRootOffsetActiveModal);
    RewriteVisibleMapForReservedQuickSlot(root);
    *reinterpret_cast<std::uint32_t*>(list_widget + 0x118) = 0;

    LoadSelectedRefreshHook(root);

    const auto new_modal = *reinterpret_cast<const std::uintptr_t*>(root + kRootOffsetActiveModal);
    *reinterpret_cast<std::uint32_t*>(list_widget + 0x118) = previous_row;

    if (new_modal != 0 && new_modal != previous_modal) {
        g_state.quick_load_confirm_modal_active.store(true);
        Log("[quick-load-confirm] opened slot=%d\n", g_state.config.quick_slot_id);
        return true;
    }

    Log("[quick-load-confirm] unavailable reason=open-failed\n");
    return false;
}

bool TryOpenQuickLoadConfirmationModalSafe(const char* source) {
    __try {
        return TryOpenQuickLoadConfirmationModal(source);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[quick-load-confirm] unavailable reason=exception code=0x%08lX\n",
            static_cast<unsigned long>(GetExceptionCode()));
        ClearCachedLoadUiRoot();
        g_state.quick_load_confirm_modal_active.store(false);
        return false;
    }
}

std::uint64_t GetSaveCatalog() {
    const std::uint64_t game_state = GetGameState();
    return game_state != 0 ? *reinterpret_cast<std::uint64_t*>(game_state + 0xB0) : 0;
}

unsigned int* GetRecordByIndex(int index) {
    const std::uint64_t catalog = GetSaveCatalog();
    if (catalog == 0 || index < 0) {
        return nullptr;
    }

    const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(catalog + 0x48);
    if (static_cast<std::uint32_t>(index) >= count) {
        return nullptr;
    }

    auto* records = reinterpret_cast<std::uint8_t*>(*reinterpret_cast<std::uint64_t*>(catalog + 0x40));
    if (records == nullptr) {
        return nullptr;
    }

    return reinterpret_cast<unsigned int*>(records + (static_cast<std::size_t>(index) * kSaveRecordSize));
}

int FindRecordIndexBySlot(int slot_id) {
    const std::uint64_t catalog = GetSaveCatalog();
    if (catalog == 0) {
        return -1;
    }

    const std::uint32_t count = *reinterpret_cast<std::uint32_t*>(catalog + 0x48);
    auto* records = reinterpret_cast<std::uint8_t*>(*reinterpret_cast<std::uint64_t*>(catalog + 0x40));
    if (records == nullptr) {
        return -1;
    }

    for (std::uint32_t index = 0; index < count; ++index) {
        auto* record = reinterpret_cast<unsigned int*>(records + (static_cast<std::size_t>(index) * kSaveRecordSize));
        if (static_cast<int>(record[0]) == slot_id) {
            return static_cast<int>(index);
        }
    }

    return -1;
}

unsigned int* GetRecordBySlot(int slot_id) {
    const int index = FindRecordIndexBySlot(slot_id);
    return index >= 0 ? GetRecordByIndex(index) : nullptr;
}

std::uint32_t GetVisibleCount(std::int64_t root) {
    return root != 0 ? *reinterpret_cast<std::uint32_t*>(root + kRootOffsetVisibleCount) : 0;
}

int* GetVisibleMap(std::int64_t root) {
    return root != 0 ? reinterpret_cast<int*>(*reinterpret_cast<std::uint64_t*>(root + kRootOffsetVisibleMap)) : nullptr;
}

std::uint32_t GetSelectedVisibleRow(std::int64_t root) {
    if (root == 0) {
        return static_cast<std::uint32_t>(-1);
    }

    const std::uint64_t list_widget = *reinterpret_cast<std::uint64_t*>(root + kRootOffsetListWidget);
    return list_widget != 0 ? *reinterpret_cast<std::uint32_t*>(list_widget + 0x118) : static_cast<std::uint32_t>(-1);
}

std::uint32_t ResolveLoadEventRow(std::int64_t root, char event_type, int a5, unsigned int a10) {
    const std::uint32_t visible_count = GetVisibleCount(root);

    if (event_type == 0) {
        if (a5 >= 0 && static_cast<std::uint32_t>(a5) < visible_count) {
            return static_cast<std::uint32_t>(a5);
        }
        if (a10 < visible_count) {
            return a10;
        }
    }

    const std::uint32_t selected_row = GetSelectedVisibleRow(root);
    if (selected_row != static_cast<std::uint32_t>(-1) && selected_row < visible_count) {
        return selected_row;
    }

    if (a5 >= 0 && static_cast<std::uint32_t>(a5) < visible_count) {
        return static_cast<std::uint32_t>(a5);
    }

    if (a10 < visible_count) {
        return a10;
    }

    return static_cast<std::uint32_t>(-1);
}

int GetVirtualRecordIndexForRow(std::int64_t root, std::uint32_t visible_row) {
    const std::uint32_t visible_count = GetVisibleCount(root);
    int* visible_map = GetVisibleMap(root);
    if (visible_count == 0 || visible_map == nullptr || visible_row >= visible_count) {
        return -1;
    }

    if (visible_row == 0) {
        return FindRecordIndexBySlot(g_state.config.quick_slot_id);
    }

    return visible_map[visible_row - 1];
}

int GetActualRecordIndexForRow(std::int64_t root, std::uint32_t visible_row) {
    const std::uint32_t visible_count = GetVisibleCount(root);
    int* visible_map = GetVisibleMap(root);
    if (visible_count == 0 || visible_map == nullptr || visible_row >= visible_count) {
        return -1;
    }

    return visible_map[visible_row];
}

unsigned int* GetVirtualRecordForRow(std::int64_t root, std::uint32_t visible_row) {
    const int record_index = GetVirtualRecordIndexForRow(root, visible_row);
    return record_index >= 0 ? GetRecordByIndex(record_index) : nullptr;
}

bool BuildVirtualLoadMap(std::int64_t root, std::vector<int>& shadow_map) {
    if (!g_state.config.enable_reserved_load_row) {
        return false;
    }

    const std::uint32_t visible_count = GetVisibleCount(root);
    int* visible_map = GetVisibleMap(root);
    if (visible_count == 0 || visible_map == nullptr) {
        return false;
    }

    shadow_map.resize(visible_count);
    for (std::uint32_t row = 0; row < visible_count; ++row) {
        shadow_map[row] = GetVirtualRecordIndexForRow(root, row);
    }

    return true;
}

bool ShouldInstallLoadUiHooks() {
    return g_state.config.quick_load_confirmation_enabled
        || g_state.config.hide_reserved_slot_in_save_ui
        || (g_state.config.enable_reserved_load_row && g_state.config.enable_experimental_load_ui);
}

bool IsLoadStyleMode(std::int64_t root) {
    return root != 0 && *reinterpret_cast<std::uint8_t*>(root + kRootOffsetMode) != 0;
}

bool RewriteVisibleMapForReservedQuickSlot(std::int64_t root) {
    if (!g_state.config.enable_reserved_load_row || !g_state.config.enable_experimental_load_ui || !IsLoadStyleMode(root)) {
        return false;
    }

    const std::uint32_t visible_count = GetVisibleCount(root);
    int* visible_map = GetVisibleMap(root);
    if (visible_count == 0 || visible_map == nullptr) {
        return false;
    }

    const int quick_record_index = FindRecordIndexBySlot(g_state.config.quick_slot_id);
    std::vector<int> rewritten;
    rewritten.reserve(visible_count);
    rewritten.push_back(quick_record_index);

    bool skipped_quick = false;
    for (std::uint32_t row = 0; row < visible_count && rewritten.size() < visible_count; ++row) {
        const int record_index = visible_map[row];
        if (quick_record_index >= 0 && !skipped_quick && record_index == quick_record_index) {
            skipped_quick = true;
            continue;
        }

        rewritten.push_back(record_index);
    }

    while (rewritten.size() < visible_count) {
        rewritten.push_back(-1);
    }

    for (std::uint32_t row = 0; row < visible_count; ++row) {
        visible_map[row] = rewritten[row];
    }

    return true;
}

bool HideReservedQuickSlotFromSaveUi(std::int64_t root) {
    if (!g_state.config.hide_reserved_slot_in_save_ui || root == 0 || IsLoadStyleMode(root)) {
        return false;
    }

    const int reserved_manual_index = g_state.config.quick_slot_id - 100;
    if (reserved_manual_index < 0) {
        return false;
    }

    std::uint32_t visible_count = GetVisibleCount(root);
    int* visible_map = GetVisibleMap(root);
    if (visible_count == 0 || visible_map == nullptr || static_cast<std::uint32_t>(reserved_manual_index) >= visible_count) {
        return false;
    }

    for (std::uint32_t row = static_cast<std::uint32_t>(reserved_manual_index); row + 1 < visible_count; ++row) {
        visible_map[row] = visible_map[row + 1];
    }
    visible_map[visible_count - 1] = -1;
    --visible_count;
    *reinterpret_cast<std::uint32_t*>(root + kRootOffsetVisibleCount) = visible_count;

    return true;
}

void LogVisibleMapSnapshot(const char* label, std::int64_t root) {
    if (!g_state.config.log_enabled || root == 0) {
        return;
    }

    const std::uint32_t visible_count = GetVisibleCount(root);
    int* visible_map = GetVisibleMap(root);
    const std::uint32_t selected_row = GetSelectedVisibleRow(root);
    Log("[%s] root=%p visible_count=%u selected_row=%u visible_map=%p\n",
        label,
        reinterpret_cast<void*>(root),
        visible_count,
        selected_row,
        visible_map);

    if (visible_map == nullptr) {
        return;
    }

    for (std::uint32_t row = 0; row < visible_count; ++row) {
        const int record_index = visible_map[row];
        unsigned int* record = nullptr;
        int slot_id = -1;
        if (record_index >= 0) {
            record = GetRecordByIndex(record_index);
            if (record != nullptr) {
                slot_id = static_cast<int>(record[0]);
            }
        }

        Log("[%s]   row=%u record_index=%d slot=%d record=%p\n",
            label,
            row,
            record_index,
            slot_id,
            record);
    }
}

void LogRecordSummary(const char* label, unsigned int* record) {
    if (!g_state.config.log_enabled) {
        return;
    }

    if (record == nullptr) {
        Log("[%s] record=<null>\n", label);
        return;
    }

    Log("[%s] record=%p slot=%u field1=%u field2=%u text_ptr=%p\n",
        label,
        record,
        record[0],
        record[1],
        record[2],
        reinterpret_cast<void*>(reinterpret_cast<std::uint64_t*>(record)[2]));
}

class ScopedVisibleMapOverride {
public:
    ScopedVisibleMapOverride(std::int64_t root, std::vector<int>& shadow_map)
        : slot_(nullptr), original_(0) {
        if (root == 0 || shadow_map.empty()) {
            return;
        }

        slot_ = reinterpret_cast<std::uint64_t*>(root + kRootOffsetVisibleMap);
        original_ = *slot_;
        *slot_ = reinterpret_cast<std::uint64_t>(shadow_map.data());
    }

    ~ScopedVisibleMapOverride() {
        if (slot_ != nullptr) {
            *slot_ = original_;
        }
    }

    ScopedVisibleMapOverride(const ScopedVisibleMapOverride&) = delete;
    ScopedVisibleMapOverride& operator=(const ScopedVisibleMapOverride&) = delete;

private:
    std::uint64_t* slot_;
    std::uint64_t original_;
};

std::uint64_t ResolveRowScript(std::int64_t row_key, std::int64_t view_ctx) {
    if (view_ctx == 0) {
        return 0;
    }

    const std::uint64_t host = *reinterpret_cast<std::uint64_t*>(view_ctx + 0xA8);
    if (host != 0 && *reinterpret_cast<std::uint64_t*>(host + 0x8) != 0) {
        return 0;
    }

    std::uint64_t row_script = *reinterpret_cast<std::uint64_t*>(view_ctx + 0x140);
    if (row_script != 0) {
        return row_script;
    }

    if (host == 0 || g_resolve_ui_script == nullptr) {
        return 0;
    }

    const std::uint64_t resolver = *reinterpret_cast<std::uint64_t*>(host + 0x50);
    if (resolver == 0) {
        return 0;
    }

    return g_resolve_ui_script(resolver, row_key, 1);
}

class ScopedReservedQuickRowTextOverride {
public:
    explicit ScopedReservedQuickRowTextOverride(std::uint64_t row_script)
        : previous_enabled_(g_reserved_quick_row_text_override),
          previous_script_(g_reserved_quick_row_script) {
        g_reserved_quick_row_text_override = row_script != 0;
        g_reserved_quick_row_script = row_script;
    }

    ~ScopedReservedQuickRowTextOverride() {
        g_reserved_quick_row_text_override = previous_enabled_;
        g_reserved_quick_row_script = previous_script_;
    }

    ScopedReservedQuickRowTextOverride(const ScopedReservedQuickRowTextOverride&) = delete;
    ScopedReservedQuickRowTextOverride& operator=(const ScopedReservedQuickRowTextOverride&) = delete;

private:
    bool previous_enabled_;
    std::uintptr_t previous_script_;
};

std::int64_t __fastcall SetControlTextHook(std::uint64_t control, const void* text) {
    if (g_reserved_quick_row_text_override && g_reserved_quick_row_script != 0) {
        const std::uint64_t type_control = *reinterpret_cast<std::uint64_t*>(g_reserved_quick_row_script + kRowOffsetTypeText);
        if (control != 0 && control == type_control) {
            text = LocalizedRowLabel();
        }
    }

    return g_set_control_text_original != nullptr ? g_set_control_text_original(control, text) : 0;
}


std::int64_t __fastcall RenderSlotRowHook(std::uint64_t* row_script, unsigned int* record, char flag, char flag2) {
    if (g_render_slot_row_original == nullptr) {
        return 0;
    }

    if (row_script != nullptr && record != nullptr && static_cast<int>(record[0]) == g_state.config.quick_slot_id) {
        alignas(16) std::uint8_t shadow_record_bytes[kSaveRecordSize] = {};
        std::memcpy(shadow_record_bytes, record, kSaveRecordSize);
        auto* shadow_record = reinterpret_cast<unsigned int*>(shadow_record_bytes);
        shadow_record[0] = 0;

        const std::int64_t result = g_render_slot_row_original(row_script, shadow_record, flag, flag2);
        const std::uint64_t row_script_u64 = reinterpret_cast<std::uint64_t>(row_script);
        SetRowControlText(row_script_u64, kRowOffsetTypeText, LocalizedRowLabel());
        SetRowControlText(row_script_u64, kRowOffsetIndexName, "");
        return result;
    }

    return g_render_slot_row_original(row_script, record, flag, flag2);
}


void SetRowControlText(std::uint64_t row_script, std::ptrdiff_t offset, const char* text) {
    if (row_script == 0 || text == nullptr || g_set_control_text == nullptr) {
        return;
    }

    const std::uint64_t control = *reinterpret_cast<std::uint64_t*>(row_script + offset);
    if (control != 0) {
        g_set_control_text(control, text);
    }
}

void RenderReservedQuickRowScript(std::uint64_t row_script, const char* context) {
    if (row_script == 0 || g_render_slot_row == nullptr) {
        return;
    }

    unsigned int* quick_record = GetRecordBySlot(g_state.config.quick_slot_id);
    if (quick_record != nullptr) {
        alignas(16) std::uint8_t shadow_record_bytes[kSaveRecordSize] = {};
        std::memcpy(shadow_record_bytes, quick_record, kSaveRecordSize);
        auto* shadow_record = reinterpret_cast<unsigned int*>(shadow_record_bytes);
        shadow_record[0] = 0;

        const ScopedReservedQuickRowTextOverride override_scope(row_script);
        g_render_slot_row(reinterpret_cast<std::uint64_t*>(row_script), shadow_record, 0, 0);
        SetRowControlText(row_script, kRowOffsetTypeText, LocalizedRowLabel());
        SetRowControlText(row_script, kRowOffsetIndexName, "");
    } else {
        const ScopedReservedQuickRowTextOverride override_scope(row_script);
        g_render_slot_row(reinterpret_cast<std::uint64_t*>(row_script), nullptr, 1, 0);
        SetRowControlText(row_script, kRowOffsetTypeText, LocalizedRowLabel());
        SetRowControlText(row_script, kRowOffsetIndexName, LocalizedRowLabel());
    }
}

void RenderReservedQuickRow(std::int64_t row_key, std::int64_t view_ctx) {
    const std::uint64_t row_script = ResolveRowScript(row_key, view_ctx);
    RenderReservedQuickRowScript(row_script, "load-row");
}

std::int64_t __fastcall SaveServiceDriverHook(std::int64_t self, std::uint64_t* actor) {
    const DWORD tid = GetCurrentThreadId();
    const std::uintptr_t actor_u = reinterpret_cast<std::uintptr_t>(actor);
    if (actor_u != 0) {
        g_state.cached_service_save_actor.store(actor_u);
        g_state.cached_service_save_thread_id.store(tid);
    }

    return g_save_service_driver_original(self, actor);
}

std::int64_t __fastcall WeatherFrameHeartbeatHook(std::uint64_t self, float dt) {
    const std::int64_t result = g_weather_frame_heartbeat_original != nullptr
        ? g_weather_frame_heartbeat_original(self, dt)
        : 0;
    TickGameplayActionGateOnGameThread();
    return result;
}

std::int32_t* __fastcall ServiceChildPollHook(std::int64_t self, std::int32_t* out_result) {
    const DWORD tid = GetCurrentThreadId();
    const DWORD primed_tid = ResolveQuickSaveThreadId();
    const bool pending_quick_save = g_state.pending_quick_save.load();
    const std::uint32_t child_count = self != 0 ? *reinterpret_cast<const std::uint32_t*>(self + 128) : 0;
    const bool child_poll_active = self != 0 ? (*reinterpret_cast<const std::uint8_t*>(self + 148) != 0) : false;
    if (self != 0 && child_count == 46 && child_poll_active) {
        g_state.cached_service_save_actor.store(static_cast<std::uintptr_t>(self));
        g_state.cached_service_save_thread_id.store(tid);
    }

    const std::uintptr_t save_actor = ResolveQuickSaveActor();
    if (pending_quick_save && primed_tid != 0 && tid == primed_tid && save_actor != 0) {
        const bool should_dispatch = g_state.pending_quick_save.exchange(false);
        if (should_dispatch) {
            InvokeQuickSave("service-child-poll");
        }
    }

    return g_service_child_poll_original(self, out_result);
}

int* __fastcall DirectLocalSaveHook(std::int64_t actor, int* out_result, int slot, unsigned char* flags) {
    g_state.cached_direct_save_actor.store(static_cast<std::uintptr_t>(actor));
    g_state.cached_direct_save_thread_id.store(GetCurrentThreadId());
    return g_direct_local_save_original(actor, out_result, slot, flags);
}

std::int32_t* __fastcall InGameMenuLoadCoreHook(
    std::int64_t self,
    std::int32_t* out_result,
    std::int64_t* target_key,
    unsigned int slot) {
    ArmGameplayActionGate("in-game-load-core");
    const auto key_value = target_key != nullptr ? static_cast<std::uint64_t>(*target_key) : 0;
    if (self != 0) {
        g_state.cached_in_game_load_core_self.store(static_cast<std::uintptr_t>(self));
        g_state.cached_in_game_load_target_key.store(key_value);
        g_state.cached_in_game_load_ui_thread_id.store(GetCurrentThreadId());
    }
    return g_in_game_menu_load_core_original(self, out_result, target_key, slot);
}

void __fastcall BuildVisibleMapHook(std::int64_t root) {
    g_build_visible_map_original(root);

    CacheLoadUiRoot(root, "build-visible-map");

    bool changed = false;
    if (RewriteVisibleMapForReservedQuickSlot(root)) {
        changed = true;
    }
    if (HideReservedQuickSlotFromSaveUi(root)) {
        changed = true;
    }
    if (changed) {
        LogVisibleMapSnapshot("build-visible-map", root);
    }
}

std::int64_t __fastcall LoadListEventHook(
    std::int64_t a1,
    std::int64_t a2,
    char a3,
    std::int64_t a4,
    int a5,
    std::int64_t a6,
    std::int64_t a7,
    int a8,
    int a9,
    unsigned int a10) {
    const std::uint32_t resolved_row = ResolveLoadEventRow(a1, a3, a5, a10);

    const std::int64_t result = g_load_list_event_original(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10);

    if (g_state.config.enable_reserved_load_row && g_state.config.enable_experimental_load_ui && IsLoadStyleMode(a1) && !a3
        && resolved_row == 0) {
        RenderReservedQuickRow(a2, a4);
    }

    return result;
}

std::int64_t __fastcall LoadSelectedRefreshHook(std::int64_t root) {
    CacheLoadUiRoot(root, "load-selected-refresh");

    const std::int64_t result = g_load_selected_refresh_original(root);

    if (g_state.config.enable_reserved_load_row && g_state.config.enable_experimental_load_ui && IsLoadStyleMode(root)
        && GetSelectedVisibleRow(root) == 0) {
        __try {
            const std::uint64_t modal = *reinterpret_cast<std::uint64_t*>(root + kRootOffsetActiveModal);
            if (modal != 0) {
                const std::uint64_t row_script = *reinterpret_cast<std::uint64_t*>(modal + kModalOffsetSlotItem);
                RenderReservedQuickRowScript(row_script, "load-modal-row");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[load-modal-row] skipped exception=0x%08lX root=%p\n",
                static_cast<unsigned long>(GetExceptionCode()),
                reinterpret_cast<void*>(root));
        }
    }

    return result;
}

void __fastcall LoadModalHandlerHook(std::uint64_t* self, std::int64_t source, char accepted, unsigned int arg4) {
    CacheLoadUiRoot(reinterpret_cast<std::int64_t>(self), "load-modal");
    const auto self_u64 = reinterpret_cast<std::uintptr_t>(self);
    const auto source_u64 = static_cast<std::uintptr_t>(source);
    const auto field49 = self != nullptr ? *reinterpret_cast<std::uintptr_t*>(self_u64 + 49 * sizeof(std::uint64_t)) : 0;
    if (field49 != 0 && source_u64 == field49) {
        const bool was_active = g_state.quick_load_confirm_modal_active.exchange(false);
        Log("[quick-load-confirm] resolved accepted=%d active=%d\n", static_cast<int>(accepted), was_active ? 1 : 0);
        if (accepted) {
            ArmGameplayActionGate("quick-load-confirm-accepted");
        }
    }

    g_load_modal_handler_original(self, source, accepted, arg4);
}

void LogResolvedAddresses() {}

bool InstallHooks() {
    if (!resolver::Initialize(GetModuleHandleW(nullptr), &ResolverLogSink, g_state.config.log_enabled)) {
        Log("[E] Resolver initialization failed\n");
        return false;
    }

    SyncResolvedToastBridge();

    const MH_STATUS init_status = MH_Initialize();
    if (init_status != MH_OK) {
        Log("[E] MH_Initialize failed (%d)\n", static_cast<int>(init_status));
        return false;
    }

    bool ok = ResolveDirectCalls();
    ok &= CreateHookAtResolved(resolver::SymbolId::DirectLocalSave, &DirectLocalSaveHook, &g_direct_local_save_original, "DirectLocalSave");
    ok &= CreateHookAtResolved(resolver::SymbolId::WeatherFrameHeartbeat, &WeatherFrameHeartbeatHook, &g_weather_frame_heartbeat_original, "WeatherFrameHeartbeat");
    ok &= CreateHookAtResolved(resolver::SymbolId::SaveServiceDriver, &SaveServiceDriverHook, &g_save_service_driver_original, "SaveServiceDriver");
    ok &= CreateHookAtResolved(resolver::SymbolId::ServiceChildPoll, &ServiceChildPollHook, &g_service_child_poll_original, "ServiceChildPoll");
    ok &= CreateHookAtResolved(resolver::SymbolId::InGameMenuLoadCore, &InGameMenuLoadCoreHook, &g_in_game_menu_load_core_original, "InGameMenuLoadCore");

    if (resolver::IsFeatureEnabled(resolver::FeatureGroup::LoadUi) && ShouldInstallLoadUiHooks()) {
        ok &= CreateHookAtResolved(resolver::SymbolId::RenderSlotRow, &RenderSlotRowHook, &g_render_slot_row_original, "RenderSlotRow");
        ok &= CreateHookAtResolved(resolver::SymbolId::SetControlText, &SetControlTextHook, &g_set_control_text_original, "SetControlText");
        ok &= CreateHookAtResolved(resolver::SymbolId::BuildVisibleMap, &BuildVisibleMapHook, &g_build_visible_map_original, "BuildVisibleMap");
        ok &= CreateHookAtResolved(
            resolver::SymbolId::LoadSelectedRefresh,
            &LoadSelectedRefreshHook,
            &g_load_selected_refresh_original,
            "LoadSelectedRefresh");
        ok &= CreateHookAtResolved(
            resolver::SymbolId::LoadModalHandler,
            &LoadModalHandlerHook,
            &g_load_modal_handler_original,
            "LoadModalHandler");
    } else {
        g_state.config.enable_reserved_load_row = false;
        g_state.config.hide_reserved_slot_in_save_ui = false;
        g_state.config.quick_load_confirmation_enabled = false;
    }

    if (!ok) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        resolver::Shutdown();
        return false;
    }

    g_state.hooks_installed = true;
    Log("[+] Runtime hooks installed\n");
    return true;
}

}  // namespace

bool Initialize(HMODULE self_module) {
    g_state.self_module = nullptr;
    g_state.log_file = nullptr;
    g_state.hooks_installed = false;
    g_state.plugin_dir[0] = L'\0';
    g_state.input_thread = nullptr;
    g_state.stop_event = nullptr;
    g_state.game_window = nullptr;
    g_state.original_wndproc = nullptr;
    g_state.quick_save_message = 0;
    g_state.quick_load_message = 0;
    g_state.cached_save_request_actor.store(0);
    g_state.cached_direct_save_actor.store(0);
    g_state.cached_service_save_actor.store(0);
    g_state.cached_save_request_arg.store(0);
    g_state.cached_direct_save_thread_id.store(0);
    g_state.cached_service_save_thread_id.store(0);
    g_state.cached_load_worker_self.store(0);
    g_state.cached_load_io_context.store(0);
    g_state.cached_load_thread_id.store(0);
    g_state.cached_real_load_self.store(0);
    g_state.cached_real_load_packet_size = 0;
    g_state.cached_real_load_blob_size = 0;
    g_state.cached_real_load_packet.fill(0);
    g_state.cached_real_load_blob.fill(0);
    g_state.cached_login_character_self.store(0);
    g_state.cached_login_character_actor_wrapper.store(0);
    g_state.cached_login_character_thread_id.store(0);
    g_state.cached_login_character_packet_size = 0;
    g_state.cached_login_character_blob_size = 0;
    g_state.cached_login_character_packet.fill(0);
    g_state.cached_login_character_blob.fill(0);
    g_state.cached_login_character_mode = 0;
    g_state.cached_login_character_key = 0;
    g_state.cached_login_character_slot = 0;
    g_state.cached_login_character_payload_count = 0;
    g_state.cached_login_character_payload.fill(0);
    g_state.cached_in_game_load_core_self.store(0);
    g_state.cached_in_game_load_target_key.store(0);
    g_state.cached_in_game_load_ui_thread_id.store(0);
    ClearCachedLoadUiRoot();
    g_state.dualsense_buttons.store(0);
    g_state.dualsense_last_input_ms.store(0);
    g_state.pending_quick_save.store(false);
    g_state.pending_quick_load.store(false);
    g_state.quick_save_active.store(false);
    g_state.quick_load_confirm_modal_active.store(false);
    g_state.quick_actions_enabled.store(false);
    g_state.gameplay_gate_armed.store(true);
    g_state.gameplay_ready_since_ms.store(0);
    g_state.controller_source = ControllerSource::None;
    g_state.previous_controller_buttons = 0;
    g_state.previous_quick_save_key_down = false;
    g_state.previous_quick_load_key_down = false;
    g_state.last_quick_save_dispatch_ms = 0;
    g_state.last_quick_load_dispatch_ms = 0;
    g_state.config = {};
    g_state.self_module = self_module;
    g_xinput_get_state = nullptr;
    g_xinput_resolve_attempted = false;

    ResolvePluginDir();
    LoadConfig();
    ApplyBuiltInLocaleText(g_state.config.locale);
    OpenLog();

    Log("===============================================\n");
    Log("  %s\n", kModName);
    Log("===============================================\n\n");
    Log("[i] Base: %p\n", reinterpret_cast<void*>(GetModuleHandleW(nullptr)));
    Log("[i] BuildSig:  %s\n", kBuildSignature);
    Log("[i] Config EnableMod=%d LogEnabled=%d QuickSlotId=%d SaveVk=%d LoadVk=%d SavePad=0x%04X LoadPad=0x%04X\n",
        g_state.config.enable_mod ? 1 : 0,
        g_state.config.log_enabled ? 1 : 0,
        g_state.config.quick_slot_id,
        g_state.config.quick_save_vk,
        g_state.config.quick_load_vk,
        static_cast<unsigned>(g_state.config.quick_save_controller_mask),
        static_cast<unsigned>(g_state.config.quick_load_controller_mask));
    const std::string locale_name = NarrowWide(g_state.config.locale.c_str());
    Log("[i] Locale: %s\n", locale_name.c_str());

    if (!ResolveDispatchMessages()) {
        return false;
    }

    if (!g_state.config.enable_mod) {
        Log("[i] Mod disabled in config\n");
        return true;
    }

    if (!InstallHooks()) {
        Log("[E] Failed to initialize runtime hooks\n");
        return false;
    }

    g_state.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_state.stop_event == nullptr) {
        Log("[E] Failed to create hotkey stop event gle=%lu\n", GetLastError());
        return false;
    }

    g_state.input_thread = CreateThread(nullptr, 0, &HotkeyThreadProc, nullptr, 0, nullptr);
    if (g_state.input_thread == nullptr) {
        Log("[E] Failed to create hotkey worker gle=%lu\n", GetLastError());
        CloseHandle(g_state.stop_event);
        g_state.stop_event = nullptr;
        return false;
    }

    Log("[+] Runtime ready\n");
    return true;
}

void Shutdown() {
    if (g_state.stop_event != nullptr) {
        SetEvent(g_state.stop_event);
    }
    if (g_state.input_thread != nullptr) {
        WaitForSingleObject(g_state.input_thread, 1000);
        CloseHandle(g_state.input_thread);
        g_state.input_thread = nullptr;
    }
    if (g_state.stop_event != nullptr) {
        CloseHandle(g_state.stop_event);
        g_state.stop_event = nullptr;
    }
    if (g_state.game_window != nullptr && g_state.original_wndproc != nullptr) {
        SetWindowLongPtrW(g_state.game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_state.original_wndproc));
        g_state.original_wndproc = nullptr;
    }
    g_state.game_window = nullptr;
    g_state.raw_input_target_hwnd.store(0);
    ClearCachedLoadUiRoot();
    g_state.quick_save_message = 0;
    g_state.quick_load_message = 0;
    g_state.quick_load_confirm_modal_active.store(false);

    if (g_state.hooks_installed) {
        MH_DisableHook(MH_ALL_HOOKS);
        MH_Uninitialize();
        g_state.hooks_installed = false;
    }

    resolver::Shutdown();
    CloseLog();
}

}  // namespace quicksave
