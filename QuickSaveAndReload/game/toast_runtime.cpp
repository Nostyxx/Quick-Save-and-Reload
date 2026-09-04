#include "pch.h"

#include <MinHook.h>

#include "include/log.h"
#include "include/text_runtime.h"
#include "include/toast_runtime.h"

namespace qsr::toast {
namespace {

using NativeToastCreateStringFn = void* (__fastcall*)(const char*);
using NativeToastPushFn = void(__fastcall*)(void*, void**, unsigned int);
using NativeToastReleaseStringFn = void(__fastcall*)(void*);
using NativeToastShowAlertFn = std::int64_t(__fastcall*)(void*, const char*, unsigned int);
using ToastUiEventFn = void(__fastcall*)(std::int64_t, std::int64_t, double);

void** g_root_global = nullptr;
std::uint32_t g_outer_offset = 0;
std::ptrdiff_t g_manager_offset = 0;
NativeToastCreateStringFn g_create_string = nullptr;
NativeToastPushFn g_push = nullptr;
NativeToastReleaseStringFn g_release_string = nullptr;
NativeToastShowAlertFn g_show_alert = nullptr;
ToastUiEventFn g_ui_event_original = nullptr;
std::uintptr_t g_ui_event_target = 0;
std::mutex g_queue_mutex;
std::atomic<bool> g_message_queued{false};
char g_queued_message[512] = {};
bool g_enabled = false;

std::uintptr_t ScanUniqueExecutablePattern(
    HMODULE module,
    const std::uint8_t* pattern,
    const char* mask,
    std::size_t pattern_size,
    std::size_t conditional_branch_offset = 0,
    std::uintptr_t conditional_branch_target = 0) {
    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (module == nullptr || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return 0;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return 0;
    }

    std::uintptr_t first = 0;
    std::size_t hits = 0;
    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned short i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0
            || section->Misc.VirtualSize < pattern_size) {
            continue;
        }
        const auto section_base = reinterpret_cast<std::uintptr_t>(base) + section->VirtualAddress;
        const auto* memory = reinterpret_cast<const std::uint8_t*>(section_base);
        const std::size_t last = section->Misc.VirtualSize - pattern_size;
        for (std::size_t offset = 0; offset <= last; ++offset) {
            bool matches = true;
            for (std::size_t j = 0; j < pattern_size; ++j) {
                if (mask[j] == 'x' && memory[offset + j] != pattern[j]) {
                    matches = false;
                    break;
                }
            }
            if (!matches) {
                continue;
            }

            const std::uintptr_t candidate = section_base + offset;
            if (conditional_branch_target != 0) {
                const auto* branch = reinterpret_cast<const std::uint8_t*>(
                    candidate + conditional_branch_offset);
                if (branch[0] != 0x0F || branch[1] != 0x84) {
                    continue;
                }

                std::int32_t displacement = 0;
                std::memcpy(&displacement, branch + 2, sizeof(displacement));
                const std::uintptr_t target = candidate + conditional_branch_offset + 6 + displacement;
                if (target != conditional_branch_target) {
                    continue;
                }
            }

            first = first == 0 ? candidate : first;
            if (++hits > 1) {
                return 0;
            }
        }
    }
    return hits == 1 ? first : 0;
}

bool TryResolveAlertBridge(HMODULE module) {
    constexpr std::uint8_t kShowAlertPattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20,
        0x48, 0x8B, 0xFA, 0x41, 0x8B, 0xD8, 0x48, 0x8D, 0x15, 0, 0, 0, 0,
        0xE8, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x0F, 0x84, 0, 0, 0, 0,
        0x4C, 0x8B, 0x80, 0xA0, 0x00, 0x00, 0x00
    };
    constexpr char kShowAlertMask[] = "xxxxxxxxxxxxxxxxxxx????x????xxxxx????xxxxxxx";
    constexpr std::uint8_t kUiEventPattern[] = {
        0x48, 0x8B, 0x42, 0x08, 0x44, 0x8B, 0x40, 0x14, 0x44, 0x39, 0x05,
        0, 0, 0, 0, 0x0F, 0x84, 0, 0, 0, 0, 0xC3
    };
    constexpr char kUiEventMask[] = "xxxxxxxxxxx????xx????x";
    constexpr std::uint8_t kMinimapUiPattern[] = {
        0x48, 0x89, 0x5C, 0x24, 0,
        0x48, 0x89, 0x4C, 0x24, 0,
        0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
        0x48, 0x8D, 0xAC, 0x24, 0, 0, 0xFF, 0xFF,
        0x48, 0x81, 0xEC, 0, 0x03, 0x00, 0x00,
        0x48, 0x83, 0xB9, 0xC0, 0x03, 0x00, 0x00, 0x00,
        0x0F, 0x84, 0, 0, 0x00, 0x00,
        0xC5, 0xFA, 0x10, 0x99, 0, 0x05, 0x00, 0x00,
        0x48, 0x8B, 0x42, 0x18,
        0x44, 0x8B, 0x40, 0x04
    };
    constexpr char kMinimapUiMask[] =
        "xxxx?"
        "xxxx?"
        "xxxxxxxxxxx"
        "xxxx??xx"
        "xxx?xxx"
        "xxxxxxxx"
        "xx??xx"
        "xxxx?xxx"
        "xxxx"
        "xxxx";
    static_assert(sizeof(kShowAlertMask) == sizeof(kShowAlertPattern) + 1);
    static_assert(sizeof(kUiEventMask) == sizeof(kUiEventPattern) + 1);
    static_assert(sizeof(kMinimapUiMask) == sizeof(kMinimapUiPattern) + 1);

    const std::uintptr_t show_alert = ScanUniqueExecutablePattern(
        module, kShowAlertPattern, kShowAlertMask, std::size(kShowAlertPattern));
    const std::uintptr_t minimap_ui = ScanUniqueExecutablePattern(
        module, kMinimapUiPattern, kMinimapUiMask, std::size(kMinimapUiPattern));
    const std::uintptr_t ui_event = ScanUniqueExecutablePattern(
        module,
        kUiEventPattern,
        kUiEventMask,
        std::size(kUiEventPattern),
        15,
        minimap_ui);
    if (show_alert == 0 || minimap_ui == 0 || ui_event == 0) {
        return false;
    }

    g_show_alert = reinterpret_cast<NativeToastShowAlertFn>(show_alert);
    g_ui_event_target = ui_event;
    log::Write("[toast] AlertSystem bridge resolved show=%p ui_event=%p minimap=%p\n",
        reinterpret_cast<void*>(show_alert),
        reinterpret_cast<void*>(ui_event),
        reinterpret_cast<void*>(minimap_ui));
    return true;
}

std::uintptr_t ReadRipRelative(std::uintptr_t instruction) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(instruction);
    if (bytes[0] != 0x48 || bytes[1] != 0x8B || bytes[2] != 0x05) {
        return 0;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes + 3, sizeof(displacement));
    return instruction + 7 + displacement;
}

std::uintptr_t ReadCallTarget(std::uintptr_t instruction) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(instruction);
    if (bytes[0] != 0xE8) {
        return 0;
    }

    std::int32_t displacement = 0;
    std::memcpy(&displacement, bytes + 1, sizeof(displacement));
    return instruction + 5 + displacement;
}

bool TryBuildBridge(std::uintptr_t site) {
    const auto* p = reinterpret_cast<const std::uint8_t*>(site);

    std::uint32_t outer_offset = 0;
    std::uint32_t manager_offset = 0;
    std::size_t manager_load = 0;
    __try {
        if (p[7] != 0x48 || p[8] != 0x8B) {
            return false;
        }

        const std::uint8_t outer_modrm = p[9];
        if (outer_modrm == 0x48) {
            outer_offset = p[10];
            manager_load = 11;
        } else if (outer_modrm == 0x88) {
            std::memcpy(&outer_offset, p + 10, sizeof(outer_offset));
            manager_load = 14;
        } else {
            return false;
        }

        if (p[manager_load] != 0x48 || p[manager_load + 1] != 0x8B
            || p[manager_load + 2] != 0x99) {
            return false;
        }
        std::memcpy(&manager_offset, p + manager_load + 3, sizeof(manager_offset));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    std::uintptr_t create_fn = 0;
    std::uintptr_t push_fn = 0;
    std::uintptr_t release_fn = 0;
    for (std::size_t k = manager_load + 7; k + 8 <= 0x90; ++k) {
        if (create_fn == 0
            && p[k + 0] == 0x48 && p[k + 1] == 0x8B && p[k + 2] == 0xC8
            && p[k + 3] == 0xE8) {
            create_fn = ReadCallTarget(site + k + 3);
            continue;
        }
        if (push_fn == 0
            && p[k + 0] == 0x48 && p[k + 1] == 0x8B && p[k + 2] == 0xCB
            && p[k + 3] == 0xE8) {
            push_fn = ReadCallTarget(site + k + 3);
            continue;
        }
        if (release_fn == 0
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
    if (root_global == 0 || outer_offset == 0 || manager_offset == 0
        || create_fn == 0 || push_fn == 0 || release_fn == 0) {
        return false;
    }

    g_root_global = reinterpret_cast<void**>(root_global);
    g_outer_offset = outer_offset;
    g_manager_offset = static_cast<std::ptrdiff_t>(manager_offset);
    g_create_string = reinterpret_cast<NativeToastCreateStringFn>(create_fn);
    g_push = reinterpret_cast<NativeToastPushFn>(push_fn);
    g_release_string = reinterpret_cast<NativeToastReleaseStringFn>(release_fn);
    return true;
}

bool ResolveBridge() {
    HMODULE module = GetModuleHandleW(nullptr);
    if (module == nullptr) {
        return false;
    }

    if (TryResolveAlertBridge(module)) {
        return true;
    }

    auto* base = reinterpret_cast<std::uint8_t*>(module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned short i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        const std::uintptr_t section_base = reinterpret_cast<std::uintptr_t>(base) + section->VirtualAddress;
        const auto* mem = reinterpret_cast<const std::uint8_t*>(section_base);
        const std::size_t size = static_cast<std::size_t>(section->Misc.VirtualSize);
        if (size < 0x90) {
            continue;
        }

        for (std::size_t offset = 0; offset + 0x90 <= size; ++offset) {
            const auto* p = mem + offset;
            if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05) continue;
            if (TryBuildBridge(section_base + offset)) {
                return true;
            }
        }
    }

    return false;
}

void* ResolveManager() {
    if (g_root_global == nullptr || g_outer_offset == 0 || g_manager_offset == 0
        || g_create_string == nullptr || g_push == nullptr || g_release_string == nullptr) {
        return nullptr;
    }

    __try {
        void* root = *g_root_global;
        if (root == nullptr) {
            return nullptr;
        }
        void* outer = *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(root) + g_outer_offset);
        if (outer == nullptr) {
            return nullptr;
        }
        return *reinterpret_cast<void**>(reinterpret_cast<std::uint8_t*>(outer) + g_manager_offset);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* ResolveUiRootNoThrow(void* ui_object) {
    __try {
        void** vtable = *reinterpret_cast<void***>(ui_object);
        if (vtable != nullptr && vtable[34] != nullptr) {
            using GetUiRootFn = void* (__fastcall*)(void*);
            return reinterpret_cast<GetUiRootFn>(vtable[34])(ui_object);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return nullptr;
}

bool ShowAlertNoThrow(void* ui_root, const char* message) {
    __try {
        g_show_alert(ui_root, message, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void FlushQueuedAlert(void* ui_object) {
    if (g_show_alert == nullptr || ui_object == nullptr || !g_message_queued.load()) {
        return;
    }

    void* ui_root = ResolveUiRootNoThrow(ui_object);
    if (ui_root == nullptr) {
        return;
    }

    char message[sizeof(g_queued_message)] = {};
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (!g_message_queued.load()) {
            return;
        }
        strcpy_s(message, g_queued_message);
        g_queued_message[0] = '\0';
        g_message_queued.store(false);
    }

    if (ShowAlertNoThrow(ui_root, message)) {
        log::Write("[toast] native alert shown message=%s\n", message);
    } else {
        log::Write("[toast] native alert exception message=%s\n", message);
    }
}

void __fastcall ToastUiEventHook(std::int64_t self, std::int64_t event_context, double value) {
    if (g_ui_event_original != nullptr) {
        g_ui_event_original(self, event_context, value);
    }
    FlushQueuedAlert(reinterpret_cast<void*>(self));
}

}  // namespace

bool Initialize(bool enabled) {
    Shutdown();
    g_enabled = enabled;
    if (!g_enabled) {
        log::Write("[toast] disabled by configuration\n");
        return false;
    }

    if (!ResolveBridge()) {
        log::Write("[toast] native bridge unresolved\n");
        return false;
    }
    return true;
}

bool Ready() {
    const bool legacy_ready = g_root_global != nullptr
        && g_outer_offset != 0
        && g_manager_offset != 0
        && g_create_string != nullptr
        && g_push != nullptr
        && g_release_string != nullptr;
    const bool alert_ready = g_show_alert != nullptr && g_ui_event_target != 0;
    return g_enabled && (legacy_ready || alert_ready);
}

bool InstallUiHook() {
    if (!Ready() || g_show_alert == nullptr) {
        return Ready();
    }
    if (g_ui_event_original != nullptr) {
        return true;
    }

    const MH_STATUS create_status = MH_CreateHook(
        reinterpret_cast<LPVOID>(g_ui_event_target),
        reinterpret_cast<LPVOID>(&ToastUiEventHook),
        reinterpret_cast<LPVOID*>(&g_ui_event_original));
    if (create_status != MH_OK) {
        log::Write("[toast] MH_CreateHook failed status=%d target=%p\n",
            static_cast<int>(create_status),
            reinterpret_cast<void*>(g_ui_event_target));
        g_ui_event_original = nullptr;
        return false;
    }
    const MH_STATUS enable_status = MH_EnableHook(reinterpret_cast<LPVOID>(g_ui_event_target));
    if (enable_status != MH_OK) {
        log::Write("[toast] MH_EnableHook failed status=%d target=%p\n",
            static_cast<int>(enable_status),
            reinterpret_cast<void*>(g_ui_event_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_ui_event_target));
        g_ui_event_original = nullptr;
        return false;
    }
    log::Write("[toast] AlertSystem UI hook installed target=%p\n",
        reinterpret_cast<void*>(g_ui_event_target));
    return true;
}

void ShutdownUiHook() {
    if (g_ui_event_original != nullptr && g_ui_event_target != 0) {
        MH_DisableHook(reinterpret_cast<LPVOID>(g_ui_event_target));
        MH_RemoveHook(reinterpret_cast<LPVOID>(g_ui_event_target));
    }
    g_ui_event_original = nullptr;
}

void Show(const char* message) {
    if (message == nullptr || message[0] == '\0' || !Ready()) {
        return;
    }

    if (g_show_alert != nullptr) {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        strncpy_s(g_queued_message, message, _TRUNCATE);
        g_message_queued.store(true);
        return;
    }

    void* manager = ResolveManager();
    if (manager == nullptr) {
        log::Write("[toast] manager unavailable message=%s\n", message);
        return;
    }

    void* handle = g_create_string(message);
    if (handle == nullptr) {
        log::Write("[toast] create string failed message=%s\n", message);
        return;
    }

    g_push(manager, &handle, 0);
    g_release_string(handle);
}

void Show(text::TextId id) {
    Show(text::Get(id));
}

void ShowFormatted(text::TextId id, unsigned int value) {
    const std::string message = text::Format(id, value);
    Show(message.c_str());
}

void Shutdown() {
    ShutdownUiHook();
    g_root_global = nullptr;
    g_outer_offset = 0;
    g_manager_offset = 0;
    g_create_string = nullptr;
    g_push = nullptr;
    g_release_string = nullptr;
    g_show_alert = nullptr;
    g_ui_event_target = 0;
    g_message_queued.store(false);
    g_queued_message[0] = '\0';
    g_enabled = false;
}

}  // namespace qsr::toast
