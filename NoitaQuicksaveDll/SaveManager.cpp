#include "SaveManager.h"

#include "Logger.h"
#include "Utility.h"

#include <filesystem>
#include <stdexcept>

namespace fs = std::filesystem;

namespace noitaqs
{
    namespace
    {
        fs::path g_baseDir;
        fs::path g_save00;
        fs::path g_backup;

        // Passed to the relaunched process on its command line — never written to
        // disk, so a crashed restart can't strand a marker that triggers Continue
        // on the next legitimate manual launch.
        constexpr const wchar_t* kAutoContinueArg = L"--noitaqs-autocontinue";

        // Raw Win32 path copies — safe to access from DLL_PROCESS_DETACH.
        wchar_t g_save00Raw[MAX_PATH * 2] = {};
        wchar_t g_backupRaw[MAX_PATH * 2] = {};
        bool g_rawPathsValid = false;

        volatile LONG g_quicksavePending = 0;

        // Set at DLL init when the launching process passed kAutoContinueArg.
        // Cleared after the trigger fires or the address lookup fails. Only
        // touched from the poll thread.
        bool g_autoContinuePending = false;

        // Auto-continue progresses Waiting (no menu yet) -> Pressing (menu up,
        // re-writing the Continue trigger every frame until the menu dismisses)
        // -> Done. A frame budget bounds Pressing so a changed flag semantic
        // can't loop forever. All three only touched from the poll thread.
        enum class AutoContinueState { Waiting, Pressing, Done };
        AutoContinueState g_acState = AutoContinueState::Waiting;
        int g_acFrames = 0;
        constexpr int kMaxPressFrames = 200; // ~3 s at the 15 ms poll interval

        // Noita's PE preferred base and the addresses (recorded against that base)
        // of the in-main-menu flag and the Continue-pressed trigger. Rebased via
        // the runtime module slide so the feature still works under ASLR.
        constexpr uintptr_t kNoitaImageBase = 0x00400000;
        constexpr uintptr_t kMainMenuFlagVA = 0x0120761B;
        constexpr uintptr_t kContinueTriggerVA = 0x01207618;

        // Pre-created replacement Noita process (CREATE_SUSPENDED). Resumed from
        // DLL_PROCESS_DETACH. Resuming a thread is loader-safe; creating a process
        // from DllMain is not, per Microsoft documentation.
        HANDLE g_pendingProcess = nullptr;
        HANDLE g_pendingThread = nullptr;

        fs::path ResolveSave00()
        {
            wchar_t appData[MAX_PATH]{};
            DWORD length = GetEnvironmentVariableW(L"APPDATA", appData, static_cast<DWORD>(std::size(appData)));
            if (length == 0 || length >= std::size(appData))
                throw std::runtime_error("APPDATA is not available.");

            return fs::path(appData).parent_path()
                / L"LocalLow"
                / L"Nolla_Games_Noita"
                / L"save00";
        }

        bool IsSaveDirectory(const fs::path& path)
        {
            return fs::exists(path / L"world");
        }

        void CopyDirectory(const fs::path& source, const fs::path& destination)
        {
            if (!fs::exists(source))
                throw std::runtime_error("Noita save00 directory was not found.");

            fs::create_directories(destination);

            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(source))
            {
                fs::path relative = fs::relative(entry.path(), source);
                fs::path target = destination / relative;

                if (entry.is_directory())
                {
                    fs::create_directories(target);
                }
                else if (entry.is_regular_file())
                {
                    fs::create_directories(target.parent_path());
                    fs::copy_file(entry.path(), target, fs::copy_options::overwrite_existing);
                }
            }
        }

        // SEH-protected single-byte memory probes. The Continue/menu addresses are
        // rebased from a hardcoded build-time VA — if a future Noita patch shifts
        // those globals out of writable memory, we want a clean disable, not an AV.
        // Kept in their own functions because __try is incompatible with C++ object
        // unwinding (no std::wstring or fs::path locals allowed).
        bool SafeReadByte(const volatile uint8_t* address, uint8_t* outValue)
        {
            __try
            {
                *outValue = *address;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool SafeWriteByte(volatile uint8_t* address, uint8_t value)
        {
            __try
            {
                *address = value;
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                return false;
            }
        }

        bool CommandLineHasAutoContinueArg()
        {
            const wchar_t* cmdLine = GetCommandLineW();
            return cmdLine != nullptr && wcsstr(cmdLine, kAutoContinueArg) != nullptr;
        }

        // Builds: "<exePath>" --noitaqs-autocontinue
        // CreateProcessW may modify lpCommandLine, so callers must pass a writable
        // local buffer. No C++ objects — safe to call from DLL_PROCESS_DETACH.
        bool BuildAutoContinueCommandLine(wchar_t* dst, size_t cap, const wchar_t* exePath)
        {
            return swprintf_s(dst, cap, L"\"%s\" %s", exePath, kAutoContinueArg) > 0;
        }

        // Bring this process's main window to the foreground so Noita (SDL)
        // captures keyboard/mouse input. After a restart the new window is
        // spawned by the dying old process and doesn't reliably receive
        // foreground activation, which sometimes leaves controls dead until the
        // user clicks in-world. The old process calls AllowSetForegroundWindow
        // on us at spawn time so SetForegroundWindow here is honored rather
        // than silently denied by the foreground lock.
        void ActivateOwnWindow()
        {
            HWND hwnd = FindOwnMainWindow();
            if (hwnd == nullptr)
            {
                Log(L"[SaveManager] Auto-continue: main window not found for activation.");
                return;
            }

            ShowWindow(hwnd, SW_SHOW);
            BringWindowToTop(hwnd);
            SetForegroundWindow(hwnd);
            SetActiveWindow(hwnd);
        }

        void TouchAllFiles(const fs::path& root)
        {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);

            for (const fs::directory_entry& entry : fs::recursive_directory_iterator(root))
            {
                if (!entry.is_regular_file())
                    continue;

                HANDLE file = CreateFileW(
                    entry.path().c_str(),
                    FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);

                if (file == INVALID_HANDLE_VALUE)
                    continue;

                SetFileTime(file, nullptr, nullptr, &now);
                CloseHandle(file);
            }
        }
    }

    void InitializeSaveManager(HMODULE module)
    {
        g_baseDir = GetModuleDirectory(module);
        g_save00 = ResolveSave00();
        g_backup = g_baseDir / L"NoitaQuicksave" / L"save00";

        // Set up logging first so subsequent init steps can report status.
        InitializeLogging(g_baseDir / L"noita_quicksave.log");

        // If the launching process passed the auto-continue arg, the user pressed
        // F5 or F9 there and expects the new process to land straight in the run.
        if (CommandLineHasAutoContinueArg())
        {
            g_autoContinuePending = true;
            Log(L"[SaveManager] Auto-continue armed: will press Continue when main menu loads.");
        }

        // Store raw wchar_t copies for use in DLL_PROCESS_DETACH.
        // Refuse to mark the paths valid if either source exceeds the destination
        // buffer — a silently-truncated path passed to RemoveDirectoryW could destroy
        // unrelated user data.
        const std::wstring& save00Str = g_save00.native();
        const std::wstring& backupStr = g_backup.native();
        if (save00Str.size() < std::size(g_save00Raw)
            && backupStr.size() < std::size(g_backupRaw))
        {
            wcsncpy_s(g_save00Raw, save00Str.c_str(), _TRUNCATE);
            wcsncpy_s(g_backupRaw, backupStr.c_str(), _TRUNCATE);
            g_rawPathsValid = true;
        }
        else
        {
            g_save00Raw[0] = L'\0';
            g_backupRaw[0] = L'\0';
            g_rawPathsValid = false;
        }
    }

    bool HasQuicksaveBackup()
    {
        return IsSaveDirectory(g_backup);
    }

    void CopySave00ToBackup()
    {
        if (fs::exists(g_backup))
            fs::remove_all(g_backup);
        CopyDirectory(g_save00, g_backup);
    }

    void Quickload()
    {
        if (!HasQuicksaveBackup())
            throw std::runtime_error("Backup directory not found.");

        if (fs::exists(g_save00))
            fs::remove_all(g_save00);

        CopyDirectory(g_backup, g_save00);
        TouchAllFiles(g_save00);
    }

    void SetQuicksavePending()
    {
        if (!g_rawPathsValid)
        {
            Log(L"[SaveManager] Refusing quicksave finalize: paths exceeded buffer.");
            return;
        }
        InterlockedExchange(&g_quicksavePending, 1);
    }

    bool IsQuicksavePending()
    {
        return InterlockedCompareExchange(&g_quicksavePending, 0, 0) != 0;
    }

    // Win32-only recursive directory removal (safe in DLL_PROCESS_DETACH).
    static void Win32RemoveDir(const wchar_t* path)
    {
        wchar_t pattern[MAX_PATH * 2];
        if (swprintf_s(pattern, L"%s\\*", path) < 0) return;

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            wchar_t child[MAX_PATH * 2];
            if (swprintf_s(child, L"%s\\%s", path, fd.cFileName) < 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                Win32RemoveDir(child);
            else
                DeleteFileW(child);
        } while (FindNextFileW(h, &fd));

        FindClose(h);
        RemoveDirectoryW(path);
    }

    // Win32-only recursive directory copy (safe in DLL_PROCESS_DETACH).
    static void Win32CopyDir(const wchar_t* src, const wchar_t* dst)
    {
        CreateDirectoryW(dst, nullptr);

        wchar_t pattern[MAX_PATH * 2];
        if (swprintf_s(pattern, L"%s\\*", src) < 0) return;

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h == INVALID_HANDLE_VALUE) return;

        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                continue;
            wchar_t srcChild[MAX_PATH * 2], dstChild[MAX_PATH * 2];
            if (swprintf_s(srcChild, L"%s\\%s", src, fd.cFileName) < 0) continue;
            if (swprintf_s(dstChild, L"%s\\%s", dst, fd.cFileName) < 0) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                Win32CopyDir(srcChild, dstChild);
            else
                CopyFileW(srcChild, dstChild, FALSE);
        } while (FindNextFileW(h, &fd));

        FindClose(h);
    }

    bool PrepareSuspendedRestart()
    {
        // Already prepared (e.g., user mashed F5 twice).
        if (g_pendingThread != nullptr)
            return true;

        wchar_t exePath[MAX_PATH]{};
        DWORD exeLen = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (exeLen == 0 || exeLen >= std::size(exePath))
            return false;

        wchar_t workDir[MAX_PATH]{};
        wcsncpy_s(workDir, exePath, _TRUNCATE);
        if (wchar_t* lastSlash = wcsrchr(workDir, L'\\'))
            *lastSlash = L'\0';

        wchar_t cmdLine[MAX_PATH * 2 + 64]{};
        if (!BuildAutoContinueCommandLine(cmdLine, std::size(cmdLine), exePath))
            return false;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE,
                            CREATE_SUSPENDED, nullptr, workDir, &si, &pi))
            return false;

        // Grant the child the right to call SetForegroundWindow on itself once it
        // starts. We are still the foreground process here (the user just pressed
        // F5/F9 in this window), so this permission transfer is allowed.
        AllowSetForegroundWindow(pi.dwProcessId);

        g_pendingProcess = pi.hProcess;
        g_pendingThread = pi.hThread;
        return true;
    }

    void FinalizePendingQuicksave()
    {
        if (InterlockedCompareExchange(&g_quicksavePending, 0, 1) == 0)
            return;
        if (!g_rawPathsValid || g_save00Raw[0] == L'\0' || g_backupRaw[0] == L'\0')
            return;

        Win32RemoveDir(g_backupRaw);
        Win32CopyDir(g_save00Raw, g_backupRaw);

        // Preferred path: a suspended replacement process was prepared while Noita
        // was still alive. ResumeThread is documented as safe in DllMain.
        if (g_pendingThread != nullptr)
        {
            ResumeThread(g_pendingThread);
            CloseHandle(g_pendingThread);
            g_pendingThread = nullptr;
            if (g_pendingProcess) { CloseHandle(g_pendingProcess); g_pendingProcess = nullptr; }
            return;
        }

        // Fallback: pre-creation failed. CreateProcess from DllMain is officially
        // unsafe but has worked in practice; keep the legacy path so a single
        // pre-create failure doesn't strand the user without a restart.
        wchar_t exePath[MAX_PATH]{};
        DWORD exeLen = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (exeLen == 0 || exeLen >= std::size(exePath))
            return;

        wchar_t workDir[MAX_PATH]{};
        wcsncpy_s(workDir, exePath, _TRUNCATE);
        if (wchar_t* lastSlash = wcsrchr(workDir, L'\\'))
            *lastSlash = L'\0';

        wchar_t cmdLine[MAX_PATH * 2 + 64]{};
        if (!BuildAutoContinueCommandLine(cmdLine, std::size(cmdLine), exePath))
            return;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(exePath, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, workDir, &si, &pi))
            AllowSetForegroundWindow(pi.dwProcessId); // best-effort; harmless if denied
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread)  CloseHandle(pi.hThread);
    }

    void RestartNoita()
    {
        wchar_t exePath[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, exePath, static_cast<DWORD>(std::size(exePath)));
        if (length == 0 || length == std::size(exePath))
            throw std::runtime_error("Could not resolve noita.exe path.");

        wchar_t cmdLine[MAX_PATH * 2 + 64]{};
        if (!BuildAutoContinueCommandLine(cmdLine, std::size(cmdLine), exePath))
            throw std::runtime_error("Could not build relaunch command line.");

        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);

        PROCESS_INFORMATION process{};
        BOOL created = CreateProcessW(
            exePath,
            cmdLine,
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            g_baseDir.c_str(),
            &startup,
            &process);

        if (!created)
            throw std::runtime_error("Could not restart Noita.");

        // Let the new instance pull itself to the foreground when it loads (we are
        // still the foreground process until TerminateProcess below).
        AllowSetForegroundWindow(process.dwProcessId);

        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);

        TerminateProcess(GetCurrentProcess(), 0);
    }

    void ProcessAutoContinueWatcher()
    {
        if (!g_autoContinuePending)
            return;

        const uintptr_t moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (moduleBase == 0)
            return;

        const uintptr_t slide = moduleBase - kNoitaImageBase;
        const volatile uint8_t* menuFlag =
            reinterpret_cast<const volatile uint8_t*>(kMainMenuFlagVA + slide);
        volatile uint8_t* continueTrigger =
            reinterpret_cast<volatile uint8_t*>(kContinueTriggerVA + slide);

        uint8_t flagValue = 0;
        if (!SafeReadByte(menuFlag, &flagValue))
        {
            Log(L"[SaveManager] Auto-continue: menu flag address not readable; disabling.");
            g_autoContinuePending = false;
            return;
        }

        switch (g_acState)
        {
        case AutoContinueState::Waiting:
            // Idle until the main menu starts rendering.
            if (flagValue != 0)
            {
                Log(L"[SaveManager] Auto-continue: main menu detected; activating window and pressing Continue.");
                ActivateOwnWindow();
                g_acState = AutoContinueState::Pressing;
                g_acFrames = 0;
            }
            break;

        case AutoContinueState::Pressing:
            if (flagValue == 0)
            {
                // Menu dismissed -> Continue was accepted and the game is loading.
                // Re-activate now: this is the moment Noita needs the window focused
                // to capture input (otherwise controls stay dead until a manual click).
                Log(L"[SaveManager] Auto-continue: Continue accepted; re-activating window for input capture.");
                ActivateOwnWindow();
                g_acState = AutoContinueState::Done;
                g_autoContinuePending = false;
                break;
            }

            // Re-write the trigger every frame the menu is up. A single write can
            // race the menu becoming ready to consume it; retrying makes the press
            // reliable instead of working only some of the time.
            if (!SafeWriteByte(continueTrigger, 1))
            {
                Log(L"[SaveManager] Auto-continue: trigger address not writable; disabling.");
                g_acState = AutoContinueState::Done;
                g_autoContinuePending = false;
                break;
            }

            if (++g_acFrames > kMaxPressFrames)
            {
                Log(L"[SaveManager] Auto-continue: timed out waiting for menu to dismiss; disabling.");
                g_acState = AutoContinueState::Done;
                g_autoContinuePending = false;
            }
            break;

        case AutoContinueState::Done:
            g_autoContinuePending = false;
            break;
        }
    }
}
