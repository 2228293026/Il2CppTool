# CI 用哪个 NDK —— 版本**只有一个出处**：.ci/clang-tidy-baseline.txt 的 NDK-TIDY 行。
#
# 为什么要抽成脚本而不是在 ci.yml 里各写一份（第 140 轮）：
#
#   原来两个 job 各自内联一份「取 SDK ndk/ 目录下版本号最大的一个」。
#   我改了 build 那份，static-analysis 那份**忘了**——而它们本来就该
#   是同一个规则。「重复的检查逻辑」和「漏改一处」是同一件事。
#   ci.yml 里凡是判断，都应该指向一个可以被单独反向验证的脚本。
#
# 为什么不能只信任 runner 上装好的 NDK：
#
#   原写法是「装了什么就用什么」，实际拿到 r27.3，
#   而 README 声明 r29、baseline 也是 r29 的产物。
#   代码从来没在 r29 上编译过；baseline 里的 4 条 r29 特有告警
#   也从来没被 CI 判过。而两次 CI 都报「新增 0 条」——
#   因为「新增」的定义是「不在 baseline 里的」，而 r27 少报的那些
#   不构成「不在 baseline 里」，差 4 条这件事没有任何人会发现。
#
# 参数：
#   -Install   顺带用 sdkmanager 装上缺的版本（CI 用）
#   写 ANDROID_NDK_HOME / NDK_PATH 到 $env:GITHUB_ENV（CI 用）
param([switch]$Install)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$baseline = Join-Path $root '.ci/clang-tidy-baseline.txt'

# ---- 唯一的版本出处 ----
if (-not (Test-Path $baseline)) { throw "找不到 $baseline" }
$revLine = Get-Content -Encoding UTF8 $baseline |
    Where-Object { $_ -match '^\s*NDK-TIDY\s*:' } | Select-Object -First 1
if (-not $revLine) {
    throw "baseline 没有声明 NDK-TIDY: 版本 —— 不知道该用哪个 NDK。`n$baseline`n换 NDK 请同时重建 baseline：.ci\run-clang-tidy.ps1 -NDKBASE <ndk>"
}
$want = ($revLine -split ':', 2)[1].Trim()
Write-Host "Baseline 要求 clang-tidy: NDK $want"

# ---- 收集候选：环境变量 + SDK 的 ndk/ 目录 ----
$candidates = @()
foreach ($envName in 'ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT') {
    $value = [Environment]::GetEnvironmentVariable($envName)
    if ($value) { $candidates += $value }
}
$installed = @()
$sdks = @($env:ANDROID_HOME, $env:ANDROID_SDK_ROOT) | Where-Object { $_ }
foreach ($sdk in $sdks) {
    $ndkRoot = Join-Path $sdk 'ndk'
    if (-not (Test-Path $ndkRoot)) { continue }
    $found = @(Get-ChildItem $ndkRoot -Directory)
    $candidates += $found | ForEach-Object { $_.FullName }
    $installed += $found | ForEach-Object {
        $r = Get-Content (Join-Path $_.FullName 'source.properties') -Encoding UTF8 -ErrorAction SilentlyContinue |
            Where-Object { $_ -match '^\s*Pkg\.Revision\s*=' } | Select-Object -First 1
        if ($r) { $_.FullName + '  (' + ($r -split '=', 2)[1].Trim() + ')' }
    }
}

# ---- 挑出 rev 字面相等的那一个 ----
#
# 比的是 **Pkg.Revision**，不是目录名。目录名 `27.3.13750724` 恰好等于
# rev 是 sdkmanager 的规矩，但把两件事分开比，将来万一不守规矩，
# 报错信息里看得出是哪个不符。
function Get-NdkRev([string]$p) {
    if (-not (Test-Path (Join-Path $p 'source.properties'))) { return $null }
    # 显式声明编码（第 92 轮的教训）：不带 -Encoding 时
    # Windows PowerShell 5.1 按系统 ANSI 读，这台机器是 GBK。
    $r = Get-Content (Join-Path $p 'source.properties') -Encoding UTF8 |
        Where-Object { $_ -match '^\s*Pkg\.Revision\s*=' } | Select-Object -First 1
    if (-not $r) { return $null }
    return ($r -split '=', 2)[1].Trim()
}

$ndk = $candidates |
    Where-Object { $_ -and (Get-NdkRev $_) -eq $want } |
    Select-Object -First 1

if (-not $ndk) {
    Write-Host "runner 上没有 NDK $want。"
    if ($installed) {
        Write-Host '已装的是：'
        $installed | ForEach-Object { Write-Host "    $_" }
    } else {
        Write-Host '一个 NDK 都没找到（ANDROID_NDK_HOME / ANDROID_SDK_ROOT 都空）。'
    }

    if (-not $Install) { throw "需要 NDK $want。" }

    if ($sdks.Count -eq 0) { throw 'runner 上找不到 Android SDK，装不了。' }
    $sdk = $sdks[0]
    $sm = Join-Path $sdk 'cmdline-tools/latest/bin/sdkmanager.bat'
    if (-not (Test-Path $sm)) { throw "找不到 sdkmanager: $sm" }
    Write-Host "Installing NDK $want into $sdk"
    # 版本号写死，不写 latest —— 「latest」的意思就是每次可能拿到不同的编译器。
    & $sm "ndk;$want" 'channel-id=0' 2>&1 | Out-String | Write-Host
    if ($LASTEXITCODE -ne 0) { throw "sdkmanager 安装 NDK $want 失败（rc=$LASTEXITCODE）" }

    $ndk = Join-Path (Join-Path $sdk 'ndk') $want
    if ((Get-NdkRev $ndk) -ne $want) {
        throw "装完了但 rev 对不上：$ndk 是 $(Get-NdkRev $ndk)，要的是 $want。"
    }
}

Write-Host "Using NDK: $ndk"
if ($env:GITHUB_ENV) {
    "ANDROID_NDK_HOME=$ndk" >> $env:GITHUB_ENV
    "NDK_PATH=$ndk" >> $env:GITHUB_ENV
} else {
    Write-Host '(不在 CI 里，只打印；调用方自己设 NDK_PATH)'
}