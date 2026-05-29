#pragma once

#define WIN32_LEAN_AND_MEAN

#include <windows.h>

#include <filesystem>
#include <string>

namespace noitaqs
{
    std::wstring Widen(const char* value);
    std::string ToUtf8(const std::wstring& value);
    std::filesystem::path GetModuleDirectory(HMODULE module);

    // Top-level visible window owned by the current (Noita) process.
    // Returns nullptr if none is found.
    HWND FindOwnMainWindow();

    // True when the foreground window belongs to the current process. Returns
    // false when there is no foreground window (a safe default for gating hotkeys).
    bool IsOwnWindowForeground();
}
