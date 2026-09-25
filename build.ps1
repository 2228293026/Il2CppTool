# ================================================================
#  Il2CppTool - one-click build via NDK ndk-build
#  (no Gradle / no Android SDK required)
#
#  Usage:
#    .\build.ps1                         use ANDROID_NDK_HOME (or ANDROID_NDK_ROOT)
#    .\build.ps1 D:\android-ndk-r29      explicit NDK path
#    .\build.ps1 D:\android-ndk-r29 -Debug    full D-level logging
#    .\build.ps1 APP_ABI=arm64-v8a       extra make args
#    .\build.ps1 D:\android-ndk-r29 APP_ABI=x    combined
#
#    If execution policy blocks scripts, run once:
#      powershell -ExecutionPolicy Bypass -File .\build.ps1
#
#  Output: app\src\main\libs\<abi>\libIl2CppTool.so
#
#  Logging:
#    Release (default): W/E/I always go to logcat and the in-app log window;
#                       D-level is silenced (it's very chatty).
#    -Debug:            D-level enabled too. Use this when diagnosing why
#                       something didn't happen.
# ================================================================
# NOTE: no declared -param block on purpose; $args collects arguments VERBATIM so
# build args like "APP_ABI=arm64-v8a" are never mistaken for PowerShell switches.
$Arguments = @($args)
$ErrorActionPreference = 'Stop'

# ---- -Debug enables the D-level log macro ----
# Passed as a **make command-line variable**, not an environment variable:
# ndk-build does not forward the environment into make's $(VAR) namespace, so
# an exported env var silently evaluates to empty and -D__DEBUG__ never reaches
# the compiler. Command-line variables are always defined.
$DebugBuild = $false
if ($Arguments -contains '-Debug') {
    $DebugBuild = $true
    $Arguments = @($Arguments | Where-Object { $_ -ne '-Debug' })
}

# ---- resolve NDK from env vars ----
$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk) { $ndk = $env:ANDROID_NDK_ROOT }

$makeArgs = @()

# ---- first arg may be an explicit NDK path ----
if ($Arguments.Count -gt 0 -and (Test-Path "$($Arguments[0])\ndk-build.cmd")) {
    $ndk = $Arguments[0]
    for ($i = 1; $i -lt $Arguments.Count; $i++) { $makeArgs += $Arguments[$i] }
}
else {
    $makeArgs = $Arguments
}

if (-not $ndk) {
    Write-Host '[ERROR] NDK not found. Set ANDROID_NDK_HOME or pass a path, e.g.:'
    Write-Host '  .\build.ps1 D:\android-ndk-r29'
    Write-Host '  setx ANDROID_NDK_HOME "D:\android-ndk-r29"   (then reopen terminal)'
    exit 1
}

$ndkBuild = Join-Path $ndk 'ndk-build.cmd'
if (-not (Test-Path $ndkBuild)) {
    # Unix / Termux style NDK
    $ndkBuild = Join-Path $ndk 'ndk-build'
}
if (-not (Test-Path $ndkBuild)) {
    Write-Host "[ERROR] ndk-build(.cmd) not found under: $ndk"
    exit 1
}

$jobs = if ($env:NUMBER_OF_PROCESSORS) { $env:NUMBER_OF_PROCESSORS } else { 8 }
if ($DebugBuild) {
    $makeArgs += 'IL2CPPTOOL_DEBUG=1'
    Write-Host '[build] __DEBUG__ enabled (verbose D-level logging)'
}

Push-Location (Join-Path $PSScriptRoot 'app\src\main')
try {
    Write-Host "[build] ndk-build : $ndkBuild"
    Write-Host "[build] workdir   : $(Get-Location)"
    Write-Host "[build] args      : -j$jobs $($makeArgs -join ' ')"
    Write-Host
    # NOTE: splat a single combined array. Splatting an *empty* $makeArgs to a
    # .cmd/ native command on PS 5.1 adds a phantom empty arg that garbles -jN.
    $invokeArgs = @("-j$jobs") + @($makeArgs)
    & $ndkBuild @invokeArgs
    $rc = $LASTEXITCODE

    if ($rc -eq 0 -and (Test-Path 'libs\arm64-v8a\libIl2CppTool.so')) {
        Write-Host
        Write-Host "[DONE] artifact: $((Get-Location))\libs\arm64-v8a\libIl2CppTool.so"
    }
}
finally {
    Pop-Location
}
exit $rc