#include "pch.h"

#include "include/log.h"

namespace qsr::log {
namespace {

FILE* g_log_file = nullptr;
bool g_enabled = false;
std::mutex g_lock;

bool BuildLogPath(HMODULE self_module, wchar_t* out_path, std::size_t capacity) {
    const DWORD length = GetModuleFileNameW(self_module, out_path, static_cast<DWORD>(capacity));
    if (length == 0 || length >= capacity) {
        return false;
    }

    wchar_t* separator = std::wcsrchr(out_path, L'\\');
    if (separator == nullptr) {
        return false;
    }

    separator[1] = L'\0';
    return wcscat_s(out_path, capacity, L"QuickSaveAndReload.log") == 0;
}

}  // namespace

bool Open(HMODULE self_module, bool enabled) {
    std::scoped_lock lock(g_lock);
    g_enabled = enabled;
    if (!g_enabled) {
        return true;
    }

    wchar_t log_path[MAX_PATH] = {};
    if (!BuildLogPath(self_module, log_path, MAX_PATH)) {
        return false;
    }

    if (g_log_file != nullptr) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }

    return _wfopen_s(&g_log_file, log_path, L"w") == 0 && g_log_file != nullptr;
}

void Write(const char* format, ...) {
    std::scoped_lock lock(g_lock);
    if (!g_enabled || g_log_file == nullptr || format == nullptr) {
        return;
    }

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    std::fprintf(g_log_file, "[%02u:%02u:%02u.%03u] ",
        static_cast<unsigned>(time.wHour),
        static_cast<unsigned>(time.wMinute),
        static_cast<unsigned>(time.wSecond),
        static_cast<unsigned>(time.wMilliseconds));

    va_list args;
    va_start(args, format);
    std::vfprintf(g_log_file, format, args);
    va_end(args);
    std::fflush(g_log_file);
}

void Close() {
    std::scoped_lock lock(g_lock);
    if (g_log_file != nullptr) {
        std::fclose(g_log_file);
        g_log_file = nullptr;
    }
    g_enabled = false;
}

}  // namespace qsr::log
