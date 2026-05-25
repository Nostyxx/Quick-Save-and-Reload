#include "pch.h"

#include <Xinput.h>
#include <hidsdi.h>
#include <hidusage.h>

#pragma comment(lib, "hid.lib")

#include "include/hotkey_service.h"
#include "include/load_runtime.h"
#include "include/log.h"
#include "include/text_runtime.h"
#include "include/toast_runtime.h"

namespace qsr::hotkeys {
namespace {

constexpr DWORD kPollIntervalMs = 25;
constexpr ULONGLONG kRawControllerFreshMs = 1500;
constexpr const wchar_t* kQuickLoadDispatchMessageName =
    L"Quick Save and Reload::QuickLoad::5D28E3A8-52AA-49DE-84C5-54EB44827C0D";
constexpr std::uintptr_t kQuickDispatchToken = 0x5153525F44495350ull;
constexpr DWORD kMaxControllerCount = 4;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

HANDLE g_stop_event = nullptr;
HANDLE g_thread = nullptr;
HWND g_game_window = nullptr;
WNDPROC g_original_wndproc = nullptr;
UINT g_quick_load_message = 0;
int g_quick_save_vk = VK_F5;
int g_quick_load_vk = VK_F6;
WORD g_quick_save_controller_mask = 0;
WORD g_quick_load_controller_mask = 0;
HMODULE g_xinput_module = nullptr;
XInputGetStateFn g_xinput_get_state = nullptr;
std::atomic<std::uintptr_t> g_raw_input_target_hwnd{0};
std::atomic<WORD> g_dualsense_buttons{0};
std::atomic<ULONGLONG> g_dualsense_last_input_ms{0};
std::atomic<bool> g_pending_quick_save{false};
std::atomic<bool> g_pending_quick_load{false};

bool IsDualSenseHidDevice(HANDLE device);
void ProcessDualSenseRawInput(HRAWINPUT raw_input_handle);
void RegisterDualSenseRawInput(HWND hwnd);
void UnregisterDualSenseRawInput();

bool IsGameForeground() {
    HWND foreground = GetForegroundWindow();
    if (foreground == nullptr) {
        return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    return pid == GetCurrentProcessId();
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

LRESULT CALLBACK HookedGameWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == WM_INPUT) {
        ProcessDualSenseRawInput(reinterpret_cast<HRAWINPUT>(lparam));
    } else if (msg == WM_INPUT_DEVICE_CHANGE && hwnd == g_game_window && wparam == GIDC_REMOVAL) {
        const HANDLE device = reinterpret_cast<HANDLE>(lparam);
        if (IsDualSenseHidDevice(device)) {
            g_dualsense_buttons.store(0);
            g_dualsense_last_input_ms.store(0);
        }
    }

    if (g_quick_load_message != 0 && msg == g_quick_load_message && wparam == static_cast<WPARAM>(kQuickDispatchToken)) {
        const bool claimed = TakeQuickLoadRequest();
        if (!claimed) {
            return 0;
        }

        if (load_runtime::TryOpenQuickLoadConfirmationModal("wndproc-ui")) {
            return 0;
        }

        log::Write("[quick-load] unavailable reason=confirmation-open-failed direct-fallback-blocked\n");
        toast::Show(text::TextId::ToastQuickLoadFailed);
        return 0;
    }

    if (g_original_wndproc != nullptr) {
        return CallWindowProcW(g_original_wndproc, hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void EnsureGameWindowHooked() {
    HWND hwnd = FindGameWindow();
    if (hwnd == nullptr) {
        return;
    }
    if (hwnd == g_game_window) {
        RegisterDualSenseRawInput(hwnd);
        return;
    }

    if (g_game_window != nullptr && g_original_wndproc != nullptr) {
        SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
        g_original_wndproc = nullptr;
    }

    g_game_window = hwnd;
    g_original_wndproc =
        reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&HookedGameWndProc)));
    RegisterDualSenseRawInput(hwnd);
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
    if (HidP_GetUsages(
            HidP_Input,
            HID_USAGE_PAGE_BUTTON,
            0,
            usages,
            &usage_count,
            preparsed,
            reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)),
            report_len) == HIDP_STATUS_SUCCESS) {
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
    if (HidP_GetUsageValue(
            HidP_Input,
            HID_USAGE_PAGE_GENERIC,
            0,
            HID_USAGE_GENERIC_HATSWITCH,
            &hat_value,
            preparsed,
            reinterpret_cast<PCHAR>(const_cast<BYTE*>(report)),
            report_len) == HIDP_STATUS_SUCCESS) {
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
    auto* raw = reinterpret_cast<RAWINPUT*>(storage.data());
    if (GetRawInputData(raw_input_handle, RID_INPUT, raw, &raw_size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1)) {
        return;
    }
    if (raw->header.dwType != RIM_TYPEHID || !IsDualSenseHidDevice(raw->header.hDevice)) {
        return;
    }

    const ULONG report_size = raw->data.hid.dwSizeHid;
    const ULONG report_count = raw->data.hid.dwCount;
    const BYTE* report_bytes = raw->data.hid.bRawData;
    if (report_size == 0 || report_count == 0 || report_bytes == nullptr) {
        return;
    }

    WORD buttons = 0;
    for (ULONG i = 0; i < report_count; ++i) {
        buttons |= MapDualSenseReportToButtons(raw->header.hDevice, report_bytes + (report_size * i), report_size);
    }

    g_dualsense_buttons.store(buttons);
    g_dualsense_last_input_ms.store(GetTickCount64());
}

void RegisterDualSenseRawInput(HWND hwnd) {
    if (hwnd == nullptr || (g_quick_save_controller_mask == 0 && g_quick_load_controller_mask == 0)) {
        return;
    }

    const auto hwnd_value = reinterpret_cast<std::uintptr_t>(hwnd);
    if (g_raw_input_target_hwnd.load() == hwnd_value) {
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
        g_raw_input_target_hwnd.store(hwnd_value);
    } else {
        log::Write("[hotkey] DualSense raw input unavailable gle=%lu hwnd=%p\n",
            static_cast<unsigned long>(GetLastError()),
            hwnd);
    }
}

void UnregisterDualSenseRawInput() {
    if (g_raw_input_target_hwnd.load() == 0) {
        return;
    }

    RAWINPUTDEVICE devices[2] = {};
    devices[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[0].usUsage = HID_USAGE_GENERIC_GAMEPAD;
    devices[0].dwFlags = RIDEV_REMOVE;
    devices[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[1].usUsage = HID_USAGE_GENERIC_JOYSTICK;
    devices[1].dwFlags = RIDEV_REMOVE;

    if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) {
        log::Write("[hotkey] DualSense raw input unregister failed gle=%lu\n",
            static_cast<unsigned long>(GetLastError()));
    }
    g_raw_input_target_hwnd.store(0);
}

bool QueueQuickLoadMessage(const char* source) {
    if (g_quick_load_message == 0) {
        log::Write("[quick-load] queue failed reason=no-message source=%s\n", source != nullptr ? source : "<null>");
        return false;
    }

    EnsureGameWindowHooked();
    if (g_game_window == nullptr) {
        log::Write("[quick-load] queue failed reason=no-game-window source=%s\n", source != nullptr ? source : "<null>");
        return false;
    }

    if (!PostMessageW(g_game_window, g_quick_load_message, static_cast<WPARAM>(kQuickDispatchToken), 0)) {
        log::Write("[quick-load] queue failed reason=post-message gle=%lu source=%s hwnd=%p\n",
            static_cast<unsigned long>(GetLastError()),
            source != nullptr ? source : "<null>",
            g_game_window);
        return false;
    }

    return true;
}

void ResolveXInput() {
    g_xinput_get_state = nullptr;
    g_xinput_module = nullptr;

    if (g_quick_save_controller_mask == 0 && g_quick_load_controller_mask == 0) {
        return;
    }

    constexpr const wchar_t* kDlls[] = {
        L"xinput1_4.dll",
        L"xinput1_3.dll",
        L"xinput9_1_0.dll",
    };

    for (const wchar_t* dll : kDlls) {
        HMODULE module = LoadLibraryW(dll);
        if (module == nullptr) {
            continue;
        }

        auto* get_state = reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
        if (get_state == nullptr) {
            FreeLibrary(module);
            continue;
        }

        g_xinput_module = module;
        g_xinput_get_state = get_state;
        return;
    }

    log::Write("[hotkey] controller input unavailable reason=xinput-not-found save_mask=0x%04X load_mask=0x%04X\n",
        static_cast<unsigned>(g_quick_save_controller_mask),
        static_cast<unsigned>(g_quick_load_controller_mask));
}

WORD PollControllerButtons() {
    if (g_xinput_get_state != nullptr) {
        for (DWORD index = 0; index < kMaxControllerCount; ++index) {
            XINPUT_STATE state = {};
            if (g_xinput_get_state(index, &state) == ERROR_SUCCESS) {
                return state.Gamepad.wButtons;
            }
        }
    }

    const ULONGLONG last_raw_input = g_dualsense_last_input_ms.load();
    const WORD dualsense_buttons = g_dualsense_buttons.load();
    if (last_raw_input != 0 && (dualsense_buttons != 0 || GetTickCount64() - last_raw_input <= kRawControllerFreshMs)) {
        return dualsense_buttons;
    }

    return 0;
}

DWORD WINAPI WorkerThread(void*) {
    bool previous_save_down = false;
    bool previous_load_down = false;
    bool previous_controller_save_down = false;
    bool previous_controller_load_down = false;
    while (WaitForSingleObject(g_stop_event, kPollIntervalMs) == WAIT_TIMEOUT) {
        EnsureGameWindowHooked();
        if (!IsGameForeground()) {
            previous_save_down = false;
            previous_load_down = false;
            previous_controller_save_down = false;
            previous_controller_load_down = false;
            continue;
        }

        const bool save_down = g_quick_save_vk != 0 && (GetAsyncKeyState(g_quick_save_vk) & 0x8000) != 0;
        if (save_down && !previous_save_down) {
            bool expected = false;
            if (g_pending_quick_save.compare_exchange_strong(expected, true)) {
                log::Write("[hotkey] quick-save requested\n");
            }
        }
        previous_save_down = save_down;

        const bool load_down = g_quick_load_vk != 0 && (GetAsyncKeyState(g_quick_load_vk) & 0x8000) != 0;
        if (load_down && !previous_load_down) {
            bool expected = false;
            if (g_pending_quick_load.compare_exchange_strong(expected, true)) {
                log::Write("[hotkey] quick-load requested\n");
                if (!QueueQuickLoadMessage("keyboard")) {
                    g_pending_quick_load.store(false);
                }
            }
        }
        previous_load_down = load_down;

        const WORD controller_buttons = PollControllerButtons();
        const bool controller_save_down = g_quick_save_controller_mask != 0
            && (controller_buttons & g_quick_save_controller_mask) == g_quick_save_controller_mask;
        if (controller_save_down && !previous_controller_save_down) {
            bool expected = false;
            if (g_pending_quick_save.compare_exchange_strong(expected, true)) {
                log::Write("[hotkey] quick-save requested source=controller buttons=0x%04X mask=0x%04X\n",
                    static_cast<unsigned>(controller_buttons),
                    static_cast<unsigned>(g_quick_save_controller_mask));
            }
        }
        previous_controller_save_down = controller_save_down;

        const bool controller_load_down = g_quick_load_controller_mask != 0
            && (controller_buttons & g_quick_load_controller_mask) == g_quick_load_controller_mask;
        if (controller_load_down && !previous_controller_load_down) {
            bool expected = false;
            if (g_pending_quick_load.compare_exchange_strong(expected, true)) {
                log::Write("[hotkey] quick-load requested source=controller buttons=0x%04X mask=0x%04X\n",
                    static_cast<unsigned>(controller_buttons),
                    static_cast<unsigned>(g_quick_load_controller_mask));
                if (!QueueQuickLoadMessage("controller")) {
                    g_pending_quick_load.store(false);
                }
            }
        }
        previous_controller_load_down = controller_load_down;
    }
    return 0;
}

}  // namespace

bool Start(int quick_save_vk, int quick_load_vk, WORD quick_save_controller_mask, WORD quick_load_controller_mask) {
    g_quick_save_vk = quick_save_vk;
    g_quick_load_vk = quick_load_vk;
    g_quick_save_controller_mask = quick_save_controller_mask;
    g_quick_load_controller_mask = quick_load_controller_mask;
    g_quick_load_message = RegisterWindowMessageW(kQuickLoadDispatchMessageName);
    if (g_quick_load_message == 0) {
        log::Write("[hotkey] RegisterWindowMessage failed gle=%lu\n",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }
    g_pending_quick_save.store(false);
    g_pending_quick_load.store(false);
    ResolveXInput();
    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_stop_event == nullptr) {
        log::Write("[hotkey] CreateEvent failed gle=%lu\n",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }
    g_thread = CreateThread(nullptr, 0, &WorkerThread, nullptr, 0, nullptr);
    if (g_thread == nullptr) {
        log::Write("[hotkey] CreateThread failed gle=%lu\n",
            static_cast<unsigned long>(GetLastError()));
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
        return false;
    }
    return true;
}

bool TakeQuickSaveRequest() {
    return g_pending_quick_save.exchange(false);
}

bool TakeQuickLoadRequest() {
    return g_pending_quick_load.exchange(false);
}

void Stop() {
    if (g_stop_event != nullptr) {
        SetEvent(g_stop_event);
    }
    if (g_thread != nullptr) {
        WaitForSingleObject(g_thread, 1000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop_event != nullptr) {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
    if (g_game_window != nullptr && g_original_wndproc != nullptr) {
        SetWindowLongPtrW(g_game_window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_original_wndproc));
    }
    UnregisterDualSenseRawInput();
    g_game_window = nullptr;
    g_original_wndproc = nullptr;
    g_quick_load_message = 0;
    g_dualsense_buttons.store(0);
    g_dualsense_last_input_ms.store(0);
    if (g_xinput_module != nullptr) {
        FreeLibrary(g_xinput_module);
        g_xinput_module = nullptr;
    }
    g_xinput_get_state = nullptr;
    g_pending_quick_save.store(false);
    g_pending_quick_load.store(false);
}

}  // namespace qsr::hotkeys
