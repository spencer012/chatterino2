param(
    [ValidateSet("Debug", "Release")]
    [string] $BuildType = "Release",

    [switch] $Configure,
    [switch] $Clean,
    [switch] $Run,
    [switch] $WaitForCopy,

    [int] $CopyTimeoutMinutes = 5,
    [int] $CopyRetrySeconds = 5
)

$ErrorActionPreference = "Stop"

$repoRoot = $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"
$exePath = Join-Path $buildDir "bin\chatterino.exe"
$runExePath = Join-Path $buildDir "bin\chatterino_runme.exe"
$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
$qtPrefix = "C:\Qt\6.10.1\msvc2022_64"

if (-not (Test-Path $vcvars)) {
    throw "Could not find vcvars64.bat at: $vcvars"
}

if (-not (Test-Path $buildDir)) {
    New-Item -ItemType Directory -Path $buildDir | Out-Null
}

function Test-FileLocked {
    param([string] $Path)

    if (-not (Test-Path $Path)) {
        return $false
    }

    try {
        $stream = [System.IO.File]::Open($Path, "Open", "ReadWrite", "None")
        $stream.Close()
        return $false
    } catch {
        return $true
    }
}

if (Test-FileLocked $exePath) {
    Write-Error "Cannot build because $exePath is locked. Close that Chatterino instance once, then use -Run to launch $runExePath for future runs."
}

function Copy-WithRetry {
    param(
        [string] $Source,
        [string] $Destination,
        [int] $TimeoutMinutes,
        [int] $RetrySeconds,
        [bool] $Wait
    )

    if (-not (Test-Path $Source)) {
        throw "Build output not found: $Source"
    }

    $deadline = (Get-Date).AddMinutes($TimeoutMinutes)

    while ($true) {
        if (-not (Test-FileLocked $Destination)) {
            Copy-Item -Path $Source -Destination $Destination -Force
            Write-Host "Copied build output to $Destination"
            return $true
        }

        if (-not $Wait) {
            Write-Warning "$Destination is locked. Skipping run-copy update; use -WaitForCopy to wait and retry."
            return $false
        }

        if ((Get-Date) -ge $deadline) {
            Write-Error "Timed out after $TimeoutMinutes minute(s) waiting to copy over locked file: $Destination"
        }

        Write-Host "$Destination is locked. Retrying in $RetrySeconds second(s)..."
        Start-Sleep -Seconds $RetrySeconds
    }
}

$commands = @()

if ($Configure) {
    $commands += "cmake -G""NMake Makefiles"" -DCMAKE_BUILD_TYPE=$BuildType -DCHATTERINO_SPELLCHECK=On -DCMAKE_TOOLCHAIN_FILE=""conan_toolchain.cmake"" -DCMAKE_PREFIX_PATH=""$qtPrefix"" .."
}

if ($Clean) {
    $commands += "nmake clean"
}

$commands += "nmake"

$cmd = "`"$vcvars`" && cd /d `"$buildDir`" && " + ($commands -join " && ")
& cmd.exe /d /s /c $cmd

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

$copiedRunExe = Copy-WithRetry -Source $exePath -Destination $runExePath -TimeoutMinutes $CopyTimeoutMinutes -RetrySeconds $CopyRetrySeconds -Wait:$WaitForCopy

if ($Run) {
    if (-not $copiedRunExe) {
        Write-Warning "Not launching because $runExePath was locked and was not updated."
        exit 0
    }

    $env:QT_WIN_DEBUG_CONSOLE = "new"
    $env:QT_LOGGING_RULES = "chatterino.*.debug=true"
    Start-Process -FilePath $runExePath -WorkingDirectory (Join-Path $buildDir "bin")
}
