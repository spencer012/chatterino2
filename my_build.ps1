param(
    [ValidateSet("Debug", "Release")]
    [string] $BuildType = "Release",

    [switch] $Configure,
    [switch] $Clean,
    # Launch the new build. If it's already running, close that instance first.
    [switch] $Run,
    # Build and run the unit tests (chatterino-test) instead of the app.
    [switch] $Tests,
    # gtest filter for -Tests, e.g. "ChannelEvents.*:ChannelPoints.*".
    [string] $TestFilter = "",
    # Also run the tests that need the local mock servers (httpbox on 9051,
    # PubSub/websocket servers on 9050/9052) even if they aren't running.
    [switch] $AllTests
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

function Update-RunExe {
    param(
        [string] $Source,
        [string] $Destination
    )

    if (-not (Test-Path $Source)) {
        throw "Build output not found: $Source"
    }

    # Windows can't overwrite a running exe, but it can rename one. Stage the
    # new build next to it, move the running one aside, then swap the new one
    # in. A running Chatterino keeps going; the next launch gets the new build.
    $dir = Split-Path $Destination
    $name = [System.IO.Path]::GetFileNameWithoutExtension($Destination)
    $staged = Join-Path $dir "$name.new.exe"

    Copy-Item -Path $Source -Destination $staged -Force

    # Left over from earlier swaps. Still locked while that build is running.
    Get-ChildItem -Path $dir -Filter "$name.old*.exe" |
        Where-Object { -not (Test-FileLocked $_.FullName) } |
        Remove-Item -Force

    $movedAside = $null
    if (Test-Path $Destination) {
        if (Test-FileLocked $Destination) {
            # Pick a name no other still-running build is using.
            $movedAside = Join-Path $dir "$name.old.exe"
            $i = 1
            while (Test-Path $movedAside) {
                $movedAside = Join-Path $dir "$name.old$i.exe"
                $i++
            }
            Move-Item -Path $Destination -Destination $movedAside
        } else {
            # Not running (the running instance was already moved aside by
            # an earlier build), so it can just be replaced.
            Remove-Item $Destination -Force
        }
    }
    Move-Item -Path $staged -Destination $Destination

    if ($movedAside) {
        Write-Host "Updated $Destination (the running instance keeps the previous build until restarted)"
    } else {
        Write-Host "Updated $Destination"
    }
}

function Get-RunExeProcesses {
    param([string] $Path)

    $dir = Split-Path $Path
    $name = [System.IO.Path]::GetFileNameWithoutExtension($Path)
    Get-Process -Name $name, "$name.old*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and (Split-Path $_.Path) -eq $dir }
}

$commands = @()

if ($Configure) {
    $commands += "cmake -G""NMake Makefiles"" -DCMAKE_BUILD_TYPE=$BuildType -DCHATTERINO_SPELLCHECK=On -DCMAKE_TOOLCHAIN_FILE=""conan_toolchain.cmake"" -DCMAKE_PREFIX_PATH=""$qtPrefix"" .."
}

if ($Clean) {
    $commands += "nmake clean"
}

if ($Tests) {
    # BUILD_TESTS is off by default upstream; enabling it only adds the
    # chatterino-test target, normal builds below still only build the app.
    $commands += "cmake -DBUILD_TESTS=On ."
    $commands += "nmake chatterino-test"
    $commands += "cd /d `"$buildDir\bin`""
    # Tests that talk to the local mock servers (httpbox, websocket echo,
    # PubSub). Without the servers they fail slowly (a missing httpbox makes
    # some wait 2 minutes each) and the Lua HTTP test aborts the whole run.
    $serverTests = @(
        "NetworkRequest.*",
        "PluginTest.testHttp",
        "PluginTest.testTcpWebSocket",
        "PluginTest.testTlsWebSocket",
        "WebSocketPool.tcpEcho",
        "WebSocketPool.tlsEcho",
        "TwitchPubSubClient.*",
        "BasicPubSub.*",
        "SeventvEventAPI.*",
        "BttvLiveUpdates.*"
    )
    $httpbox = New-Object System.Net.Sockets.TcpClient
    $serversUp = $false
    try {
        $serversUp = $httpbox.ConnectAsync("127.0.0.1", 9051).Wait(300)
    } catch {
        $serversUp = $false
    } finally {
        $httpbox.Dispose()
    }

    $filter = $TestFilter
    if (-not $AllTests -and -not $serversUp) {
        Write-Host "Mock servers not running on 127.0.0.1:9051, skipping: $($serverTests -join ', ') (use -AllTests to run them anyway)"
        $positive = if ($filter) { $filter } else { "*" }
        $filter = "$positive-$($serverTests -join ':')"
    }
    if ($filter) {
        $commands += ".\chatterino-test.exe --gtest_filter=`"$filter`""
    } else {
        $commands += ".\chatterino-test.exe"
    }
} else {
    $commands += "nmake chatterino"
}

$cmd = "`"$vcvars`" && cd /d `"$buildDir`" && " + ($commands -join " && ")
& cmd.exe /d /s /c $cmd

if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

if ($Tests) {
    exit 0
}

Update-RunExe -Source $exePath -Destination $runExePath

if ($Run) {
    $running = @(Get-RunExeProcesses $runExePath)
    if ($running.Count -gt 0) {
        Write-Host "Closing the running Chatterino..."
        foreach ($process in $running) {
            [void] $process.CloseMainWindow()
        }
        foreach ($process in $running) {
            if (-not $process.WaitForExit(15000)) {
                Write-Warning "Chatterino (pid $($process.Id)) didn't close within 15s; not launching a second instance."
                exit 0
            }
        }
    }

    $env:QT_WIN_DEBUG_CONSOLE = "new"
    $env:QT_LOGGING_RULES = "chatterino.*.debug=true"
    Start-Process -FilePath $runExePath -WorkingDirectory (Join-Path $buildDir "bin")
}
