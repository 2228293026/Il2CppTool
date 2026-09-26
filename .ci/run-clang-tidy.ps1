#!/usr/bin/env pwsh
<#
.SYNOPSIS
  静态检查：clang-tidy (bugprone-*, cert-*)
.DESCRIPTION
  本工具是注入到别人进程里跑的代码，一个越界写 / 空指针解引用，
  崩掉的是用户的游戏进程而不是我们。ndk-build 能编过 != 安全。
  只检查项目自己的源文件；imgui / asmjit / frida-gum / xdl / nlohmann
  这些第三方代码的告警既多又不由我们修。
  门禁策略：新增告警 = 失败（存量告警走 baseline 抑制）。
.PARAMETER WriteBaseline
  把当前告警集合写入 baseline。确认存量告警无害时用一次。
#>
param(
    [switch]$WriteBaseline
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$jni = Join-Path $repoRoot 'app/src/main/jni'
$ndk = $env:NDK_PATH
if (-not $ndk) { throw 'NDK_PATH 未设置，请先解析 NDK' }

# 受检文件：只含我们自己写的 .cpp
$targets = @(
    'Includes/Logger.cpp',
    'Includes/Utils.cpp',
    'Menu/ImGui.cpp',
    'Tool/ObjectDrawManager.cpp',
    'Tool/Keyboard.cpp',
    'Tool/Unity.cpp',
    'Tool/Util.cpp',
    'Tool/Tool.cpp',
    'Il2cpp/il2cpp-class.cpp',
    'Il2cpp/Il2cpp.cpp'
) | ForEach-Object { Join-Path $jni $_ }

$missing = $targets | Where-Object { -not (Test-Path $_) }
if ($missing) { throw "找不到源文件: $($missing -join ', ')" }

$tidy = Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang-tidy.exe'
if (-not (Test-Path $tidy)) { throw "找不到 clang-tidy: $tidy" }

# 临时 compile database：只需要给 clang-tidy 正确的头文件搜索路径，
# 不参与真实构建，所以不依赖 ndk-build 生成的产物。
$sysroot = (Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/sysroot') -replace '\\', '/'
# RUNNER_TEMP 只在 CI 有；本地跑时退回系统临时目录，方便本地复现。
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
$dbDir = Join-Path $tempRoot 'il2cpp-tidy'
$dbFile = Join-Path $dbDir 'compile_commands.json'
New-Item -ItemType Directory -Path $dbDir -Force | Out-Null

$entries = $targets | ForEach-Object {
    $rel = $_.Substring($jni.Length + 1) -replace '\\', '/'
    [ordered]@{
        directory = ($jni -replace '\\', '/')
        command    = "clang++ --sysroot=$sysroot -target aarch64-linux-android28 -std=c++20 -w -I. -Iimgui -Iimgui/backends -IFrida/arm64-v8a -Iasmjit/include -IDobby -c $rel"
        file       = $rel
    }
}
$entries | ConvertTo-Json -Depth 4 | Set-Content -Path $dbFile -Encoding UTF8

Write-Host "clang-tidy: $($targets.Count) 个文件"

$checks = '-*,bugprone-*,cert-*,-bugprone-easily-swappable-parameters'

# 本文件顶部是 $ErrorActionPreference = 'Stop'。而 clang-tidy 会把逐文件进度
# （"[1/10] Processing file ..."）写到 **stderr** —— PowerShell 把原生命令的
# stderr 转成错误记录（NativeCommandError），在 'Stop' 下**直接终止脚本**。
# 于是「正常跑完」和「脚本崩了」变成同一件事，退出码 1，而真正的告警列表
# 根本没机会被解析。
#
# 表现：本机跑失败、CI 上却一直是绿的（那边 -quiet 的输出时机不同）。
# 最坏的一种不一致 —— 本地红、CI 绿，两边都不知道该信谁。
#
# 做法：调用期间临时放宽偏好，之后恢复；成败只看**退出码**。
$prevPref = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
try {
    $output = & $tidy -p $dbDir -quiet "--checks=$checks" @($targets -replace '\\','/') 2>&1 | Out-String
}
finally {
    $ErrorActionPreference = $prevPref
}

# 只保留指向我们自己源文件的告警（第三方头文件里的不进来）
$projectPattern = '(Includes|Menu|Tool|Il2cpp)[/\\][A-Za-z0-9_.-]*\.(cpp|h):\d+:\d+: warning:'
$findings = ($output -split "`r?`n") | Where-Object {
    $_ -match $projectPattern -and $_ -notmatch 'imgui[/\\]|asmjit[/\\]|frida-gum|xdl[/\\]|nlohmann[/\\]'
}

$baselineFile = Join-Path $PSScriptRoot 'clang-tidy-baseline.txt'
$known = @()
if (Test-Path $baselineFile) {
    $known = Get-Content $baselineFile | Where-Object { $_.Trim() }
}

$keys = $findings | ForEach-Object {
    # 归一化掉行号：代码一动行号就变，不该因此重新报一遍
    ($_ -replace '^(\s*)', '') -replace ':\d+:\d+: warning:', '|warning:'
} | Sort-Object -Unique

$new = $keys | Where-Object { $known -notcontains $_ }

# -WriteBaseline：把当前集合写成 baseline。确认存量告警无害时用一次即可。
if ($WriteBaseline) {
    $keys | Set-Content -Path $baselineFile -Encoding UTF8
    Write-Host "已写入 baseline: $baselineFile（$($keys.Count) 条）"
    exit 0
}

Write-Host ''
Write-Host "clang-tidy 告警: $(($keys).Count) 条（baseline $(($known).Count) 条，新增 $(($new).Count) 条）"
if ($keys) {
    Write-Host '--- 当前告警 ---'
    $keys | ForEach-Object { Write-Host "  $_" }
}

if ($new) {
    Write-Host ''
    Write-Host '新增告警（会导致 CI 失败）:'
    $new | ForEach-Object { Write-Host "  $_" }
    Write-Host ''
    Write-Host '修复它；或确认无害后重新生成 baseline:'
    Write-Host '  .\.ci\run-clang-tidy.ps1 -WriteBaseline'
    exit 1
}

Write-Host ''
Write-Host '静态检查通过（无新增告警）'
exit 0
