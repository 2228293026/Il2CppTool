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

# ---- 生成 Version.h（版本号的**唯一出处**是 VERSION.txt）----
#
# 第 142 轮加的。之前窗口标题和自检页都硬编码着 "v0.9"，而
# app/build.gradle 里写的是 "3.2"，仓库里还躺着一个 Tool_v0.9.zip。
# 自检页那行的存在意义**就是**「用户报问题时让我确认他跑的是哪个版本」，
# 它自己报的是错的版本 —— 于是报告里最关键的那一栏是假的。
#
# 用一个生成的头文件而不是在 .cpp 里写死：
#   · 版本号只有一处需要改（VERSION.txt）
#   · 改完不重新编译就换不掉版本号 —— 那正是「多个说法」的成因
#   · 顺带把 git 提交和构建日期也带上，比一个过期版本号有用得多
$versionFile = Join-Path $PSScriptRoot 'VERSION.txt'
if (-not (Test-Path $versionFile)) { throw "找不到 VERSION.txt（版本号的唯一出处）: $versionFile" }
$toolVersion = (Get-Content $versionFile -Encoding UTF8 | Select-Object -First 1).Trim()
if (-not $toolVersion) { throw 'VERSION.txt 第一行为空' }

# 提交号：有 git 就带短哈希，没有（发布 tarball）就写 unknown
$commit = 'unknown'
try {
    $head = (& git -C $PSScriptRoot rev-parse --short HEAD 2>$null)
    if ($LASTEXITCODE -eq 0 -and $head) { $commit = $head.Trim() }
} catch { }

$versionHeader = Join-Path $PSScriptRoot 'app\src\main\jni\Includes\Version.h'
# 用 -f 格式化而不是在双引号里写 \" ——
# PowerShell 的转义是**反引号**，\" 只是两个普通字符（第 122 轮踩过：
# 那一轮是反引号 + a 变成 BEL 字符，方向相反、症状一样：脚本解析失败）。
# 写成字面量 '\' 放进单引号字符串里最省心。
$headerBody = @(
    '// 由 build.ps1 自动生成 —— **不要手改，也不要提交**（已在 .gitignore 里）。',
    '// 改版本请改仓库根目录的 VERSION.txt，理由见 docs/版本号.md。',
    '#pragma once',
    '',
    ('#define IL2CPPTOOL_VERSION "{0}"' -f $toolVersion),
    ('#define IL2CPPTOOL_COMMIT "{0}"' -f $commit)
) -join "`n"
[IO.File]::WriteAllText($versionHeader, $headerBody + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Host "[build] version   : $toolVersion ($commit)"

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