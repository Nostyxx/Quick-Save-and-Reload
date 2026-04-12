#include "pch.h"

#include <cwchar>

#include "mod_runtime.h"

namespace {

constexpr const wchar_t* kTargetExeName = L"CrimsonDesert.exe";

bool IsTargetProcess() {
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        return false;
    }

    const wchar_t* name = std::wcsrchr(exe_path, L'\\');
    name = (name != nullptr) ? (name + 1) : exe_path;
    return _wcsicmp(name, kTargetExeName) == 0;
}

DWORD WINAPI InitThreadProc(void* module) {
    quicksave::Initialize(static_cast<HMODULE>(module));
    return 0;
}

} 

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);

        if (!IsTargetProcess()) {
            return TRUE;
        }

        HANDLE thread = CreateThread(nullptr, 0, InitThreadProc, hModule, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (reserved == nullptr) {
            quicksave::Shutdown();
        }
    }

    return TRUE;
}
