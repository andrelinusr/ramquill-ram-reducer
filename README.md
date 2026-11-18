# RamQuill

[Português (BR)](README.pt-BR.md)

A RAM cleaner and SysMain disabler for **Windows 10/11**, written in **pure C (C17)** on top of the native Windows API. No frameworks, no extra runtime, no installer: a single `.exe` with a dark black-and-blue interface.

> Built with low-memory machines in mind (for example, laptops with 4 GB of RAM), where a quick memory relief or turning off SysMain can make a noticeable difference.

---

## What it is

RamQuill asks Windows to give back memory that applications are holding but not actively using, and to flush several system caches. **It does not close any program or kill any process.** It also has a button to stop and disable the **SysMain** service (formerly Superfetch).

The interface shows RAM usage on an **animated circular progress gauge** with the percentage in the middle, plus a log of every operation below it.

## Features

- Circular progress gauge with smooth animation, glow and rounded ends, software-rendered with antialiasing.
- One-click **Clean RAM**, in two intensities: standard and deep.
- **Disable SysMain** through the Windows Service Control Manager (no `sc.exe` calls).
- **Auto-clean** when RAM usage crosses a threshold (default: 85%), with a 60-second minimum interval between runs.
- **System tray** icon: minimizing sends the app to the tray, with a menu to open, clean or exit.
- Step-by-step log that shows the `NTSTATUS` code when something fails.
- Single instance, automatic DPI scaling and dark title bar.
- Settings stored in `%APPDATA%\RamQuill\RamQuill.ini`.

## How it works

Cleaning runs on a separate worker thread so the window never freezes. Steps run in the order below; a bit mask decides which ones are executed.

| # | Step | How |
|---|------|-----|
| 1 | Working set of all processes | `NtSetSystemInformation` → `SystemMemoryListInformation` (`MemoryEmptyWorkingSets`) |
| 2 | System file cache | `SetSystemFileCacheSize(-1, -1, 0)` |
| 3 | Write cache of fixed volumes | `CreateFileW("\\.\X:")` + `FlushFileBuffers` |
| 4 | Modified page list *(deep)* | `MemoryFlushModifiedList` |
| 5 | Standby list *(deep)* | `MemoryPurgeStandbyList` |
| 6 | Low-priority standby list | `MemoryPurgeLowPriorityStandbyList` |
| 7 | Registry cache | `SystemRegistryReconciliationInformation` (class 155) |
| 8 | Identical page combining | `SystemCombinePhysicalMemoryInformation` (class 130) |

**Details worth knowing**

- `NtSetSystemInformation` is resolved at runtime with `GetProcAddress` from `ntdll.dll`.
- Before cleaning, the app enables `SeProfileSingleProcessPrivilege` and `SeIncreaseQuotaPrivilege` on its own token (`OpenProcessToken`, `LookupPrivilegeValueW`, `AdjustTokenPrivileges`).
- If step 1 fails, a **fallback** walks the process list with `EnumProcesses` and calls `EmptyWorkingSet` on each one, **skipping critical processes** (`csrss`, `smss`, `wininit`, `services`, `lsass`, `winlogon`, `System`, `Registry` and unnamed processes).
- Steps 4 and 5 only run in **deep** mode, because they can cause a brief slowdown right after cleaning (Windows has to read data back from disk).
- The order is deliberate: data written to disk in steps 2–4 can be discarded in the same pass by steps 5–6.
- **SysMain** is handled through the Service Control Manager: `ChangeServiceConfigW` sets the start type to *Disabled* and `ControlService` sends the stop request.

**A note on the numbers:** memory usage comes from `GlobalMemoryStatusEx`, which counts the standby list as available memory. That means the gauge can drop right after a clean even though some of those pages are still physically in RAM, just on a different list.

## Usage

1. Run `RamQuill.exe`. Windows will ask for **Administrator** permission, which is required to touch system memory and services.
2. Use the buttons:
   - **LIMPAR RAM** (clean RAM): runs the cleanup.
   - **DESATIVAR SYSMAIN** (disable SysMain): stops the service and prevents it from starting with Windows.
   - **FAZER TUDO** (do everything): disables SysMain and cleans RAM.
3. Optional switches:
   - **Auto-clean when RAM goes above 85%**: enables automatic cleaning.
   - **Deep clean**: also flushes the standby and modified lists (frees more, with a possible short slowdown).
4. Minimize to send the app to the tray. Click the tray icon to reopen it, or right-click for the menu. The close button exits the program.

> The interface text is currently in Portuguese. Translating the labels is a matter of editing the string literals in `main.c`.

### Configuration file

`%APPDATA%\RamQuill\RamQuill.ini`:

```ini
[RamQuill]
AutoClean=0
DeepClean=0
Threshold=85
```

`Threshold` accepts values from 50 to 99.

## Building

**Requirements:** 64-bit Windows 10/11 and a **MinGW-w64 GCC** toolchain (for example [WinLibs](https://winlibs.com) as a ZIP, or MSYS2).

### Quick way

Put `main.c`, `ramquill.manifest` and `compilar.bat` in the same folder (plus a `ramquill.ico` if you want a custom icon) and double-click `compilar.bat`. It looks for gcc on its own and, if it can't find it, asks for the path of the `bin` folder.

### Manually

```bat
echo 1 24 "ramquill.manifest" > ramquill.rc
windres ramquill.rc -O coff -o ramquill_res.o

gcc -std=gnu17 -O2 -s -municode -mwindows -static -Wall -Wextra ^
    -ffunction-sections -fdata-sections -Wl,--gc-sections ^
    main.c ramquill_res.o -o RamQuill.exe ^
    -lgdi32 -luser32 -ladvapi32 -lpsapi -ldwmapi -lshell32 -lm
```

To embed an icon, add `1 ICON "ramquill.ico"` to `ramquill.rc` before running `windres`.

## DLLs used

All of them ship with Windows: `kernel32`, `user32`, `gdi32`, `advapi32`, `shell32`, `dwmapi` and the system C runtime. `ntdll` is accessed at runtime through `GetProcAddress`. `libgcc` and `winpthread` are embedded by `-static`.

## Project layout

```
main.c               full source code
ramquill.manifest    requests Administrator and declares compatibility and DPI awareness
compilar.bat         Windows build script
ramquill.ico         (optional) application icon
```

## Warnings

- RamQuill **does not terminate processes**, inject code, write into other programs' memory, or use the network.
- Cleaning RAM gives **temporary relief**: programs reclaim memory as you use them, and right after a clean the system may feel slightly slower until caches fill up again. Avoid deep cleaning on virtual machines, databases with large caches, and real-time audio or video work.
- Some steps rely on **undocumented Windows calls**. They may change or be missing on certain versions; when one fails, the log shows the `NTSTATUS`.
- **Reverting SysMain:** run `sc config SysMain start= auto` and `sc start SysMain` in an elevated terminal, or use `services.msc`.
- Antivirus software may flag new programs that touch memory and services (a common false positive). The source code is open for review.
- Use at your own risk.

## Credits and license

Original code, written from the publicly known technique of clearing Windows memory lists, which is also used by utilities such as [Mem Reduct](https://github.com/henrypp/memreduct) (GPLv3). No code from Mem Reduct was copied.

License: set the project license here (for example, MIT) and add a `LICENSE` file.
