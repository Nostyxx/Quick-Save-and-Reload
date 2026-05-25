#include "pch.h"

#include "include/config.h"

#include <cwctype>

namespace qsr::config {
namespace {

constexpr WORD kXInputGamepadDpadUp = 0x0001;
constexpr WORD kXInputGamepadDpadDown = 0x0002;
constexpr WORD kXInputGamepadDpadLeft = 0x0004;
constexpr WORD kXInputGamepadDpadRight = 0x0008;
constexpr WORD kXInputGamepadStart = 0x0010;
constexpr WORD kXInputGamepadBack = 0x0020;
constexpr WORD kXInputGamepadLeftShoulder = 0x0100;
constexpr WORD kXInputGamepadRightShoulder = 0x0200;
constexpr WORD kXInputGamepadA = 0x1000;
constexpr WORD kXInputGamepadB = 0x2000;
constexpr WORD kXInputGamepadX = 0x4000;
constexpr WORD kXInputGamepadY = 0x8000;

bool BuildIniPath(HMODULE self_module, wchar_t* out_path, std::size_t capacity) {
    if (out_path == nullptr || capacity == 0) {
        return false;
    }

    const DWORD length = GetModuleFileNameW(self_module, out_path, static_cast<DWORD>(capacity));
    if (length == 0 || length >= capacity) {
        return false;
    }

    wchar_t* separator = std::wcsrchr(out_path, L'\\');
    if (separator == nullptr) {
        return false;
    }

    separator[1] = L'\0';
    return wcscat_s(out_path, capacity, L"QuickSaveAndReload.ini") == 0;
}

std::wstring TrimConfigText(const wchar_t* value) {
    if (value == nullptr) {
        return {};
    }

    const wchar_t* first = value;
    while (*first != L'\0' && iswspace(*first)) {
        ++first;
    }

    const wchar_t* last = first + std::wcslen(first);
    while (last > first && iswspace(*(last - 1))) {
        --last;
    }

    return std::wstring(first, last);
}

int KeyNameToVK(const wchar_t* name, int fallback) {
    std::wstring trimmed = TrimConfigText(name);
    if (trimmed.empty()) {
        return 0;
    }
    const wchar_t* key = trimmed.c_str();

    if (_wcsicmp(key, L"NONE") == 0 || _wcsicmp(key, L"UNBOUND") == 0) return 0;
    if (_wcsicmp(key, L"F1") == 0) return VK_F1;
    if (_wcsicmp(key, L"F2") == 0) return VK_F2;
    if (_wcsicmp(key, L"F3") == 0) return VK_F3;
    if (_wcsicmp(key, L"F4") == 0) return VK_F4;
    if (_wcsicmp(key, L"F5") == 0) return VK_F5;
    if (_wcsicmp(key, L"F6") == 0) return VK_F6;
    if (_wcsicmp(key, L"F7") == 0) return VK_F7;
    if (_wcsicmp(key, L"F8") == 0) return VK_F8;
    if (_wcsicmp(key, L"F9") == 0) return VK_F9;
    if (_wcsicmp(key, L"F10") == 0) return VK_F10;
    if (_wcsicmp(key, L"F11") == 0) return VK_F11;
    if (_wcsicmp(key, L"F12") == 0) return VK_F12;
    if (_wcsicmp(key, L"INSERT") == 0) return VK_INSERT;
    if (_wcsicmp(key, L"DELETE") == 0) return VK_DELETE;
    if (_wcsicmp(key, L"HOME") == 0) return VK_HOME;
    if (_wcsicmp(key, L"END") == 0) return VK_END;
    if (_wcsicmp(key, L"PGUP") == 0) return VK_PRIOR;
    if (_wcsicmp(key, L"PGDN") == 0) return VK_NEXT;

    if (trimmed.size() == 1) {
        const wchar_t ch = trimmed[0];
        if (ch >= L'a' && ch <= L'z') {
            return ch - L'a' + L'A';
        }
        if (ch >= L'A' && ch <= L'Z') {
            return ch;
        }
    }

    return fallback;
}

WORD ControllerTokenToMask(const wchar_t* token) {
    if (token == nullptr || token[0] == L'\0') {
        return 0;
    }

    if (_wcsicmp(token, L"dpad_up") == 0 || _wcsicmp(token, L"up") == 0) return kXInputGamepadDpadUp;
    if (_wcsicmp(token, L"dpad_down") == 0 || _wcsicmp(token, L"down") == 0) return kXInputGamepadDpadDown;
    if (_wcsicmp(token, L"dpad_left") == 0 || _wcsicmp(token, L"left") == 0) return kXInputGamepadDpadLeft;
    if (_wcsicmp(token, L"dpad_right") == 0 || _wcsicmp(token, L"right") == 0) return kXInputGamepadDpadRight;
    if (_wcsicmp(token, L"a") == 0 || _wcsicmp(token, L"cross") == 0) return kXInputGamepadA;
    if (_wcsicmp(token, L"b") == 0 || _wcsicmp(token, L"circle") == 0) return kXInputGamepadB;
    if (_wcsicmp(token, L"x") == 0 || _wcsicmp(token, L"square") == 0) return kXInputGamepadX;
    if (_wcsicmp(token, L"y") == 0 || _wcsicmp(token, L"triangle") == 0) return kXInputGamepadY;
    if (_wcsicmp(token, L"lb") == 0 || _wcsicmp(token, L"l1") == 0) return kXInputGamepadLeftShoulder;
    if (_wcsicmp(token, L"rb") == 0 || _wcsicmp(token, L"r1") == 0) return kXInputGamepadRightShoulder;
    if (_wcsicmp(token, L"start") == 0 || _wcsicmp(token, L"options") == 0) return kXInputGamepadStart;
    if (_wcsicmp(token, L"back") == 0 || _wcsicmp(token, L"share") == 0 || _wcsicmp(token, L"select") == 0) {
        return kXInputGamepadBack;
    }

    return 0;
}

WORD ParseControllerCombo(const wchar_t* text, WORD fallback) {
    std::wstring trimmed = TrimConfigText(text);
    if (trimmed.empty()) {
        return 0;
    }
    if (_wcsicmp(trimmed.c_str(), L"NONE") == 0 || _wcsicmp(trimmed.c_str(), L"UNBOUND") == 0) {
        return 0;
    }

    wchar_t copy[128] = {};
    wcsncpy_s(copy, trimmed.c_str(), _TRUNCATE);
    WORD mask = 0;
    wchar_t* context = nullptr;
    for (wchar_t* token = wcstok_s(copy, L"+|, ", &context); token != nullptr; token = wcstok_s(nullptr, L"+|, ", &context)) {
        mask = static_cast<WORD>(mask | ControllerTokenToMask(token));
    }
    return mask != 0 ? mask : fallback;
}

void WriteDefaultConfig(const wchar_t* ini_path) {
    WritePrivateProfileStringW(L"General", L"EnableMod", L"1", ini_path);
    WritePrivateProfileStringW(L"General", L"LogEnabled", L"0", ini_path);
    WritePrivateProfileStringW(L"General", L"ToastNotification", L"1", ini_path);
    WritePrivateProfileStringW(L"General", L"QuickLoadConfirmation", L"1", ini_path);
    WritePrivateProfileStringW(L"General", L"HotkeyQuickSave", L"F5", ini_path);
    WritePrivateProfileStringW(L"General", L"HotkeyQuickLoad", L"F6", ini_path);
    WritePrivateProfileStringW(
        L"General",
        L"_HotkeyOptions",
        L"Leave blank to disable. F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z",
        ini_path);
    WritePrivateProfileStringW(L"Locale", L"Language", L"en_US", ini_path);
    WritePrivateProfileStringW(L"Locale", L"_LanguageOptions", L"en_US, ko_KR, fr_FR, pt_BR, ru_RU", ini_path);
    WritePrivateProfileStringW(L"Hotkeys", L"ControllerHotkeyQuickSave", L"lb+a", ini_path);
    WritePrivateProfileStringW(L"Hotkeys", L"ControllerHotkeyQuickLoad", L"lb+y", ini_path);
    WritePrivateProfileStringW(
        L"Hotkeys",
        L"_ControllerHotkeyOptions",
        L"Leave blank to disable. Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back",
        ini_path);
    WritePrivateProfileStringW(L"SaveRuntime", L"QuickSaveSlotCount", L"1", ini_path);
}

void EnsureConfigValue(const wchar_t* ini_path, const wchar_t* section, const wchar_t* key, const wchar_t* value) {
    wchar_t buffer[64] = {};
    GetPrivateProfileStringW(section, key, L"__MISSING__", buffer, static_cast<DWORD>(std::size(buffer)), ini_path);
    if (wcscmp(buffer, L"__MISSING__") == 0) {
        WritePrivateProfileStringW(section, key, value, ini_path);
    }
}

int ClampInt(int value, int minimum, int maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

}  // namespace

bool Load(HMODULE self_module, Settings& settings) {
    settings = {};

    wchar_t ini_path[MAX_PATH] = {};
    if (!BuildIniPath(self_module, ini_path, MAX_PATH)) {
        return false;
    }

    if (GetFileAttributesW(ini_path) == INVALID_FILE_ATTRIBUTES) {
        WriteDefaultConfig(ini_path);
    }

    EnsureConfigValue(ini_path, L"General", L"EnableMod", L"1");
    EnsureConfigValue(ini_path, L"General", L"LogEnabled", L"0");
    EnsureConfigValue(ini_path, L"General", L"ToastNotification", L"1");
    EnsureConfigValue(ini_path, L"General", L"QuickLoadConfirmation", L"1");
    EnsureConfigValue(ini_path, L"General", L"HotkeyQuickSave", L"F5");
    EnsureConfigValue(ini_path, L"General", L"HotkeyQuickLoad", L"F6");
    EnsureConfigValue(
        ini_path,
        L"General",
        L"_HotkeyOptions",
        L"Leave blank to disable. F1-F12, INSERT, DELETE, HOME, END, PGUP, PGDN, or single letter A-Z");
    EnsureConfigValue(ini_path, L"Locale", L"Language", L"en_US");
    EnsureConfigValue(ini_path, L"Locale", L"_LanguageOptions", L"en_US, ko_KR, fr_FR, pt_BR, ru_RU");
    EnsureConfigValue(ini_path, L"Hotkeys", L"ControllerHotkeyQuickSave", L"lb+a");
    EnsureConfigValue(ini_path, L"Hotkeys", L"ControllerHotkeyQuickLoad", L"lb+y");
    EnsureConfigValue(
        ini_path,
        L"Hotkeys",
        L"_ControllerHotkeyOptions",
        L"Leave blank to disable. Use dpad_up/down/left/right + a/b/x/y/lb/rb/start/back");

    settings.enable_mod = GetPrivateProfileIntW(L"General", L"EnableMod", 1, ini_path) != 0;
    settings.log_enabled = GetPrivateProfileIntW(L"General", L"LogEnabled", 0, ini_path) != 0;
    settings.toast_notification = GetPrivateProfileIntW(L"General", L"ToastNotification", 1, ini_path) != 0;
    settings.quick_load_confirmation = GetPrivateProfileIntW(L"General", L"QuickLoadConfirmation", 1, ini_path) != 0;

    EnsureConfigValue(ini_path, L"SaveRuntime", L"QuickSaveSlotCount", L"1");
    settings.quick_slot_id = 108;
    settings.quick_slot_count = ClampInt(
        GetPrivateProfileIntW(L"SaveRuntime", L"QuickSaveSlotCount", 1, ini_path),
        1,
        8);

    wchar_t locale_text[64] = {};
    GetPrivateProfileStringW(L"Locale", L"Language", L"", locale_text, static_cast<DWORD>(std::size(locale_text)), ini_path);
    if (locale_text[0] == L'\0') {
        GetPrivateProfileStringW(L"General", L"Locale", L"en_US", locale_text, static_cast<DWORD>(std::size(locale_text)), ini_path);
    }
    settings.language = locale_text[0] != L'\0' ? locale_text : L"en_US";

    wchar_t hotkey_text[64] = {};
    GetPrivateProfileStringW(L"General", L"HotkeyQuickSave", L"F5", hotkey_text, static_cast<DWORD>(std::size(hotkey_text)), ini_path);
    settings.quick_save_vk = KeyNameToVK(hotkey_text, VK_F5);
    GetPrivateProfileStringW(L"General", L"HotkeyQuickLoad", L"F6", hotkey_text, static_cast<DWORD>(std::size(hotkey_text)), ini_path);
    settings.quick_load_vk = KeyNameToVK(hotkey_text, VK_F6);
    GetPrivateProfileStringW(L"Hotkeys", L"ControllerHotkeyQuickSave", L"", hotkey_text, static_cast<DWORD>(std::size(hotkey_text)), ini_path);
    settings.quick_save_controller_mask = ParseControllerCombo(hotkey_text, 0);
    GetPrivateProfileStringW(L"Hotkeys", L"ControllerHotkeyQuickLoad", L"", hotkey_text, static_cast<DWORD>(std::size(hotkey_text)), ini_path);
    settings.quick_load_controller_mask = ParseControllerCombo(hotkey_text, 0);
    return true;
}

}  // namespace qsr::config
