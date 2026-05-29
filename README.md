# Noita Quick Save

A native DLL hijack for Noita that adds simple quicksave and quickload hotkeys.

This project builds a replacement `lua51.dll`. When Noita loads it, the DLL starts a
small background key poller and forwards most of Noita's normal LuaJIT exports to
the original Lua DLL renamed as `lua51_orig.dll`.

## Features

- `F5` triggers Noita's own save sequence and copies the result to the backup,
  capturing your current inventory, health, wands, and perks. Noita restarts
  automatically and lands you back in your run — Continue is clicked for you.
- `F9` restores the quicksave and restarts Noita, also auto-clicking Continue.
- Hotkeys only fire when Noita is the focused window, so pressing F5/F9 in
  another application won't accidentally save or load.
- In-game messages are sent through Noita's own `GamePrint` and
  `GamePrintImportant` Lua functions without requiring a companion mod.
- Backups are stored in a visible `NoitaQuicksave` directory beside `noita.exe`.
- Important events are logged beside the game as `noita_quicksave.log`.
- Built as a 32-bit native C++ DLL to match Noita's 32-bit process.

## Warning

This directly copies and replaces Noita save data. Back up your real save before
testing it.

Quickload deletes the current `save00` directory and replaces it with the last
quicksave. If Steam Cloud is enabled, test carefully and keep an external backup.

## Requirements

- Windows Noita installation.
- Visual Studio or Build Tools with MSVC C++ tools installed.
- MSBuild from the Visual Studio installation.

## Build

From this repository:

```bat
"C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe" NoitaQuicksaveDll\NoitaQuicksaveDll.vcxproj /p:Configuration=Release /p:Platform=Win32
```

The built DLL is written to:

```text
NoitaQuicksaveDll\bin\Win32\Release\lua51.dll
```

## Install

1. Open the folder that contains `noita.exe`.
2. Rename Noita's original `lua51.dll` to `lua51_orig.dll`.
3. Copy the published `lua51.dll` from this project into the same folder.
4. Start Noita normally.

The folder should contain at least:

```text
noita.exe
lua51.dll       # this project's DLL
lua51_orig.dll  # Noita's original Lua DLL
```

## Usage

| Key | Action |
| --- | --- |
| `F5` | Save current state and restart Noita. |
| `F9` | Restore the last quicksave and restart Noita. |

Backups are stored at:

```text
<Noita game folder>\NoitaQuicksave\save00\
```

Important event logs are stored at:

```text
<Noita game folder>\noita_quicksave.log
```

## How It Works

### Save trigger

Noita only writes `player.xml` and `world_state.xml` during its exit sequence —
not while the game is running. When `F5` is pressed the DLL posts `WM_CLOSE` to
the game window, which causes Noita to run its normal save-and-exit path. Once
the process exits, `DLL_PROCESS_DETACH` copies the fully-written save files to
the backup location and launches a new Noita process.

At startup the DLL scans the loaded `noita.exe` image for save-related functions
using debug strings embedded in the binary. These are logged to
`noita_quicksave.log` for diagnostics.

### In-game messages

The DLL intercepts Noita's imported `lua_call` and `lua_pcall` exports. Hotkey
handling runs on a background thread, but it only queues UI messages. The queued
messages are drained from Noita's Lua thread and displayed with Noita's own
`GamePrint` or `GamePrintImportant` functions.

This avoids requiring a companion mod, unsafe mod mode, or file polling.

## Save Locations

The DLL reads Noita's active save from:

```text
%APPDATA%\..\LocalLow\Nolla_Games_Noita\save00
```

For Proton/Linux development paths, the code also supports:

```text
~/.steam/steam/steamapps/compatdata/881100/pfx/drive_c/users/steamuser/AppData/LocalLow/Nolla_Games_Noita/save00
```

## Troubleshooting

### Noita fails to start

Make sure the original Noita DLL was renamed to `lua51_orig.dll` and is in the
same directory as the replacement `lua51.dll`.

### F9 says no quicksave exists

Press `F5` first and confirm that
`<Noita game folder>\NoitaQuicksave\save00\` was created.

### Build fails with Lua unresolved externals

The project creates a linker `.exp` file from `lua51_exports.def` before linking.
Build with MSBuild and the MSVC C++ toolchain so `lib.exe`, `cl.exe`, and
`link.exe` are available.

### Noita shows 0xc000007b on launch

That usually means a 64-bit DLL was copied into Noita's 32-bit process. Build the
`Release|Win32` target and install `NoitaQuicksaveDll\bin\Win32\Release\lua51.dll`.

## Development Notes

- `lua51_exports.def` lists the LuaJIT exports forwarded to `lua51_orig.dll`.
- `NoitaQuicksave.cpp` owns the DLL entry point and hotkey poller.
- `SaveFinder.cpp` scans `noita.exe` at startup for save-related functions and
  posts `WM_CLOSE` when a quicksave is triggered.
- `SaveManager.cpp` handles save path resolution, quicksave, quickload, process
  restart, and the `DLL_PROCESS_DETACH` file copy.
- `GameMessages.cpp` owns the `lua_call` / `lua_pcall` hooks and in-game message
  queue.
- `Logger.cpp` and `Utility.cpp` contain shared logging, encoding, and path
  helpers.
