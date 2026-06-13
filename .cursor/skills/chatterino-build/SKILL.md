---
name: chatterino-build
description: Compile this Chatterino fork on Windows with Visual Studio Build Tools, NMake, Qt, and the local run-copy workflow. Use when the user asks to build, compile, run, reconfigure, clean, or diagnose build/link issues in this repository.
---

# Chatterino Build

## Quick Start

Use the project wrapper from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File .\my_build.ps1
```

Run after building:

```powershell
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -Run
```

## Build Workflow

1. Run commands from the repository root: `F:\Stuff\Vibe\chatterino\docker\chatterino2`.
2. Prefer `my_build.ps1` over manually invoking CMake or NMake.
3. The wrapper initializes the VS 2022 x64 toolchain with:
   `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat`
4. The wrapper builds in `build` with `nmake`.
5. After a successful build, it copies `build\bin\chatterino.exe` to `build\bin\chatterino_runme.exe`.
6. `-Run` launches `chatterino_runme.exe`, leaving `chatterino.exe` free for future linker output.
7. For normal compile checks, do not enable copy waiting. If `chatterino_runme.exe` is locked, let the wrapper skip the copy and finish.

## Common Commands

```powershell
powershell -ExecutionPolicy Bypass -File .\my_build.ps1
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -Run
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -Clean
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -Configure
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -Configure -BuildType Debug
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -CopyTimeoutMinutes 10 -CopyRetrySeconds 5
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -WaitForCopy -CopyTimeoutMinutes 10 -CopyRetrySeconds 5
```

Use `-WaitForCopy` only when the user asks for a release build or explicitly asks to wait for the run-copy update.

## Build Type

The wrapper defaults `-Configure` to Release.

The existing build directory can have its own cached type. Check it with:

```powershell
rg "^CMAKE_BUILD_TYPE:STRING=" build\CMakeCache.txt
```

Reconfigure Release explicitly with:

```powershell
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -Configure -BuildType Release
```

## Locked Executable Behavior

If `build\bin\chatterino.exe` is locked, the linker cannot build. This happens when the currently running app was launched directly from `chatterino.exe`.

Close that instance once, then relaunch with:

```powershell
powershell -ExecutionPolicy Bypass -File .\my_build.ps1 -Run
```

After that, normal builds should link `chatterino.exe` while the running app holds `chatterino_runme.exe` instead.

If `chatterino_runme.exe` is locked after a normal build, the wrapper skips the copy and exits successfully. This is the preferred behavior for routine agent compile checks.

Use `-WaitForCopy` only for a requested release build or when the user explicitly asks to wait. With `-WaitForCopy`, the wrapper waits and retries the copy until `CopyTimeoutMinutes` expires.

## Manual Equivalent

Only use this when debugging the wrapper:

```powershell
cmd.exe /d /s /c '"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" && cd /d "F:\Stuff\Vibe\chatterino\docker\chatterino2\build" && nmake'
```
