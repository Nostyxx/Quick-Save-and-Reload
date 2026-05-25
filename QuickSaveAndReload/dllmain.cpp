#include "pch.h"

#include "include/runtime.h"

namespace {

constexpr const wchar_t* kTargetExeName = L"CrimsonDesert.exe";

bool IsTargetProcess() {
    wchar_t exe_path[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe_path, MAX_PATH) == 0) {
        return false;
    }

    const wchar_t* name = std::wcsrchr(exe_path, L'\\');
    name = name != nullptr ? name + 1 : exe_path;
    return _wcsicmp(name, kTargetExeName) == 0;
}

DWORD WINAPI InitializeThread(void* module) {
    qsr::Initialize(static_cast<HMODULE>(module));
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (!IsTargetProcess()) {
            return TRUE;
        }

        HANDLE thread = CreateThread(nullptr, 0, &InitializeThread, module, 0, nullptr);
        if (thread != nullptr) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved == nullptr) {
        qsr::Shutdown();
    }

    return TRUE;
}

