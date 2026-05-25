#include "pch.h"

#include "include/log.h"
#include "include/text_runtime.h"
#include "include/toast_runtime.h"

namespace qsr::toast {
namespace {

using NativeToastCreateStringFn = void* (__fastcall*)(const char*);
using NativeToastPushFn = void(__fastcall*)(void*, void**, unsigned int);
using NativeToastReleaseStringFn = void(__fastcall*)(void*);

void** g_root_global = nullptr;
std::uint32_t g_outer_offset = 0;
std::ptrdiff_t g_manager_offset = 0;
NativeToastCreateStringFn g_create_string = nullptr;
NativeToastPushFn g_push = nullptr;
NativeToastReleaseStringFn g_release_string = nullptr;
bool g_enabled = false;

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
    __try {
        outer_offset = static_cast<std::uint32_t>(*(p + 10));
        std::memcpy(&manager_offset, p + 14, sizeof(manager_offset));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }

    std::uintptr_t create_fn = 0;
    std::uintptr_t push_fn = 0;
    std::uintptr_t release_fn = 0;
    for (std::size_t k = 18; k + 8 <= 0x90; ++k) {
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
            if (p[7] != 0x48 || p[8] != 0x8B || p[9] != 0x48) continue;
            if (p[11] != 0x48 || p[12] != 0x8B || p[13] != 0x99) continue;
            if (TryBuildBridge(section_base + offset)) {
                return true;
            }
        }
    }

    return false;
}

void* ResolveManager() {
    if (!Ready()) {
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
    return g_enabled
        && g_root_global != nullptr
        && g_outer_offset != 0
        && g_manager_offset != 0
        && g_create_string != nullptr
        && g_push != nullptr
        && g_release_string != nullptr;
}

void Show(const char* message) {
    if (message == nullptr || message[0] == '\0' || !Ready()) {
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
    g_root_global = nullptr;
    g_outer_offset = 0;
    g_manager_offset = 0;
    g_create_string = nullptr;
    g_push = nullptr;
    g_release_string = nullptr;
    g_enabled = false;
}

}  // namespace qsr::toast
