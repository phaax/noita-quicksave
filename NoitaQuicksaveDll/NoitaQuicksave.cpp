#define WIN32_LEAN_AND_MEAN

#include <windows.h>
#include <process.h>

#include "GameMessages.h"
#include "Logger.h"
#include "SaveFinder.h"
#include "SaveManager.h"
#include "Utility.h"

#include <stdexcept>
#include <string>

namespace
{
    constexpr int VK_F5_KEY = 0x74;
    constexpr int VK_F9_KEY = 0x78;

    HMODULE g_module = nullptr;

    void LogException(const wchar_t* prefix, const std::exception& ex)
    {
        noitaqs::LogAndQueue(std::wstring(prefix) + noitaqs::Widen(ex.what()), true);
    }

    unsigned __stdcall PollThread(void*)
    {
        try
        {
            noitaqs::InitializeSaveManager(g_module);
            noitaqs::InitSaveFinder();
            noitaqs::LogAndQueue(L"Loaded. F5 = quicksave  |  F9 = quickload (restarts Noita)");
        }
        catch (const std::exception& ex)
        {
            LogException(L"Initialization FAILED: ", ex);
        }

        SHORT previousF5 = 0;
        SHORT previousF9 = 0;

        for (;;)
        {
            try
            {
                SHORT f5 = GetAsyncKeyState(VK_F5_KEY);
                SHORT f9 = GetAsyncKeyState(VK_F9_KEY);

                // GetAsyncKeyState reads global keyboard state, so F5/F9 would
                // otherwise fire while the user is in another application. Gate
                // the actions on Noita owning the foreground window. previousF5/F9
                // still update every cycle below regardless of focus: a key held
                // while unfocused looks like a held edge once focus returns, so
                // the next genuine press (release + re-press) is what fires.
                bool focused = noitaqs::IsOwnWindowForeground();

                if (focused && (f5 & 0x8000) != 0 && (previousF5 & 0x8000) == 0)
                {
                    noitaqs::RequestSaveTrigger();
                    noitaqs::LogAndQueue(L"Quicksave requested...");
                }

                if (focused && (f9 & 0x8000) != 0 && (previousF9 & 0x8000) == 0)
                {
                    if (noitaqs::IsQuicksavePending())
                    {
                        // Noita is mid-exit and still holds open handles on save00,
                        // so we can't remove_all it from here. Let the F5 finish
                        // naturally; the user can press F9 in the restarted instance.
                        noitaqs::LogAndQueue(
                            L"Quickload ignored: quicksave in progress. Press F9 again after Noita restarts.",
                            true);
                    }
                    else if (!noitaqs::HasQuicksaveBackup())
                    {
                        noitaqs::LogAndQueue(L"Quickload ignored: no quicksave found. Press F5 first.", true);
                    }
                    else
                    {
                        noitaqs::LogAndQueue(L"Quickload: restoring save...");
                        noitaqs::Quickload();
                        noitaqs::LogAndQueue(L"Quickload restored. Restarting Noita...", true);
                        noitaqs::RestartNoita();
                    }
                }

                previousF5 = f5;
                previousF9 = f9;

                noitaqs::ProcessAutoContinueWatcher();
            }
            catch (const std::exception& ex)
            {
                LogException(L"Error: ", ex);
            }

            Sleep(15);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
        noitaqs::InitializeGameMessages(module);

        uintptr_t thread = _beginthreadex(nullptr, 0, PollThread, nullptr, 0, nullptr);
        if (thread != 0)
            CloseHandle(reinterpret_cast<HANDLE>(thread));
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // lpReserved is non-null when the process is terminating (ExitProcess),
        // null when the DLL is being unloaded via FreeLibrary. Only finalize
        // the quicksave on process termination — that's when the save files are ready.
        if (lpReserved != nullptr)
            noitaqs::FinalizePendingQuicksave();

        noitaqs::ShutdownGameMessages();
        noitaqs::ShutdownLogging();
    }

    return TRUE;
}
