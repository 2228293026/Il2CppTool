#!/usr/bin/env pwsh
# CI workflow 的结构检查（第 140 轮）。
#
# 为什么需要它
#
# 那一轮我在改 ci.yml 时**真的**留下了这样的东西：
#
#       - name: Pre-commit checks (single entry point)
#         shell: pwsh
#         run: .\.ci\check-all.ps1
#         shell: pwsh            <-- 孤儿：上一项的兄弟节点残留
#         run: .\build.ps1
#       ...
#   static-analysis:              <-- 整个 job 被复制了一份
#     runs-on: windows-latest
#     ...
#     steps:
#       - uses: actions/checkout@v4
#
# 成因是「大段替换」时把边界算错了：旧内容的**尾部**没被吃掉。
# 而 YAML 对这种残留的容忍度取决于它长什么样 —— 重复的 job key
# 在多数解析器里只是「后者覆盖前者」，于是**旧 job 静默消失**，
# CI 照样绿；而孤儿 `run:` 是语法错误，CI 在解析阶段就挂。
#
# 两种结局都不可接受，所以不靠「以后小心点」，直接查。
#
# 为什么自己解析而不用 YAML 库
#
# 本机没有 PyYAML，也没有 node 的 yaml 模块；CI 上为了查一个文件
# 去装依赖不值得。所以这里只做**结构性**判断 —— 它不需要真的解析 YAML：
#
#   1. `jobs:` 下的 key 必须唯一（重复 = 后者覆盖前者 = 旧 job 消失）
#   2. 每个 `- name:` 步骤块里必须恰好有一个 `run:` 或 `uses:`
#   3. `- uses:` 型步骤块里不允许出现 `run:`（那一定是残留）
#
# 这三条覆盖了「重复 job」和「孤儿 run」两种真实事故。
# 它们**不能**证明 workflow 一定能跑 —— 那要 GitHub 自己解析。
# 这里的定位是「别让低级的结构错误静默通过」。
#
# 关于误报：第一版把 `- uses:` 型步骤也算进「必须有 run」，
# 结果 checkout 那两步全被报成失败 —— 又一次「规则把修复判成违规」，
# 判据比事实粗。现在按 `- name:` / `- uses:` 分开处理。

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
$ciYml = Join-Path $root '.github/workflows/ci.yml'
if (-not (Test-Path $ciYml)) { Write-Host '找不到 .github/workflows/ci.yml'; exit 0 }

# 整文件读入时声明编码（第 92 轮的教训）：
# Windows PowerShell 5.1 不带 -Encoding 时按系统 ANSI 读，这台机器是 GBK。
$lines = [IO.File]::ReadAllLines($ciYml, [Text.Encoding]::UTF8)

$problems = @()

# ---- 1. jobs 下的 key 唯一 ----
#
# 两处都得处理，缺一个就漏 —— 而且是**假绿**那种漏（第 140 轮实测）：
#
#   · 顶层**注释**不能当成「离开 jobs」。原写法是
#     `if ($inJobs -and $lines[$i] -match '^\S') { $inJobs = $false }`，
#     而 `jobs:` 后面恰好有一段顶层缩进的说明注释 ——
#     收集器在那行就退出了，后面真正的 `static-analysis:` 再也没被看到。
#     于是我**注入的重复 job 它没报**，报的是另一个（孤儿 run）。
#
#   · 注释行整体跳过：注释不是结构。
$jobKeys = @()
$inJobs = $false
for ($i = 0; $i -lt $lines.Length; $i++) {
    $raw = $lines[$i]
    $t = $raw.Trim()
    if ($t -eq '' -or $t.StartsWith('#')) { continue }
    if ($t -match '^jobs:\s*$') { $inJobs = $true; continue }
    if ($inJobs -and $raw -match '^\S') { $inJobs = $false; continue }
    if ($inJobs -and $raw -match '^  ([A-Za-z0-9_-]+):\s*$') {
        $jobKeys += $Matches[1]
    }
}
$dup = $jobKeys | Group-Object | Where-Object Count -gt 1
if ($dup) {
    foreach ($d in $dup) {
        $problems += "jobs 下 ``$($d.Name)`` 出现了 $($d.Count) 次 —— YAML 里后者会**覆盖**前者，旧的 job 静默消失"
    }
}

# ---- 2/3. steps 里每项的动词数量 ----
$items = @()
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -match '^(\s*)- (name|uses):') {
        $items += ,@($i, $Matches[1], $Matches[2])
    }
}
for ($k = 0; $k -lt $items.Count; $k++) {
    $start = $items[$k][0]
    $indent = $items[$k][1]
    $kind = $items[$k][2]
    $end = if ($k + 1 -lt $items.Count) { $items[$k + 1][0] } else { $lines.Length }
    $label = ($lines[$start] -replace '^\s*- ', '')

    if ($kind -eq 'uses') {
        # uses 型步骤：块里再出现 run: 就是残留
        for ($j = $start + 1; $j -lt $end; $j++) {
            if ($lines[$j] -match ('^' + $indent + '\s+run:')) {
                $problems += "第 $($j + 1) 行：``$label`` 是 uses 型步骤却带 run:（上一项的残留）"
            }
        }
        continue
    }
    $verbs = @()
    for ($j = $start + 1; $j -lt $end; $j++) {
        if ($lines[$j] -match ('^' + $indent + '\s+(run|uses):')) { $verbs += $Matches[1] }
    }
    if ($verbs.Count -eq 0) {
        $problems += "第 $($start + 1) 行：``$label`` 没有任何 run:/uses: —— 这个步骤什么都不会做"
    }
    elseif ($verbs.Count -gt 1) {
        $problems += "第 $($start + 1) 行：``$label`` 有 $($verbs.Count) 个 run:/uses:（$($verbs -join '+')）—— 多出来的是残留"
    }
}

if ($problems.Count -eq 0) {
    Write-Host "CI workflow 结构正常（jobs: $($jobKeys -join ', ')；步骤 $($items.Count) 个）"
    exit 0
}

Write-Host "CI workflow 结构有问题（$($problems.Count) 处）:" -ForegroundColor Red
foreach ($p in $problems) { Write-Host "  - $p" -ForegroundColor Red }
Write-Host ''
Write-Host '重复的 job 会被 YAML 静默合并，孤儿 run: 是语法错误 —— 两者都会让 CI 的结果不可信。'
exit 1