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
    [switch]$WriteBaseline,
    # 用**指定 NDK** 重建 baseline，并把它的版本写进 NDK-TIDY 行。
    # 只在「确认换版本是刻意的」时用：.\.ci\run-clang-tidy.ps1 -NDKBASE D:\android-ndk-r29
    [string]$NDKBASE
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$jni = Join-Path $repoRoot 'app/src/main/jni'
if ($NDKBASE) { $env:NDK_PATH = $NDKBASE }
$ndk = $env:NDK_PATH
if (-not $ndk) { throw 'NDK_PATH 未设置，请先解析 NDK' }

# baseline 是**某个 clang-tidy 版本的产物**，不是宇宙真理。
#
# clang-tidy 的检查集合和措辞都随版本变化，而比对逻辑只看告警文本，
# 不看是谁产生的。于是换 NDK 之后：
#   - 版本独有的告警消失 -> 「新增」仍算 0 -> 差额被静默吞掉
#   - 版本独有的新告警   -> 门禁**看不见**，策略名存实亡
#
# 实际撞上的就是它：baseline 是 NDK 29 生成的（50 条），
# CI 上装的是 NDK 27.3（46 条）。两次跑都报「新增 0 条」，
# 谁也没注意到两个数字不等。
#
# 所以这里把版本钉死：用错版本 = 失败，并说清楚该怎么重建。
$propsPath = Join-Path $ndk 'source.properties'
$actualRev = ''
if (Test-Path $propsPath) {
    $line = Get-Content $propsPath -Encoding UTF8 | Where-Object { $_ -match '^\s*Pkg\.Revision\s*=' } | Select-Object -First 1
    if ($line) { $actualRev = ($line -split '=', 2)[1].Trim() }
}
if (-not $actualRev) { $actualRev = '(读不到 version)' }

# ---- baseline 加载 + 版本守卫（第 140 轮）。**必须在跑 clang-tidy 之前** ----
#
# 放在分析之后的代价是实测过的：失败一次要先白跑一遍 90 秒的全量分析。
# 放在分析之前，失败成本 = 读两个文件。
#
# 读 baseline 有两个坑，都是踩出来的：
#
#   (a) **必须**跳过 `#` 开头的行（第 129 轮）。
#       baseline 带了一段说明「这 50 条是怎么分类、为什么抑制」的注释。
#       而原来只按「非空」过滤，于是那些注释被当成已知告警 ——
#       报出来是「baseline 82 条（45+37），新增 0 条」。
#       那是**假绿**：注释不可能匹配任何告警，于是「新增」永远算 0。
#       一份带注释的 baseline 会把「新增 = 失败」这条门禁整个废掉，
#       而且没有任何人会发现 —— 除非真的去数那个 82。
#
#   (b) **必须**跳过 `NDK-TIDY:` 声明行（这一轮的顺带发现）。
#       它不以 `#` 开头，(a) 的过滤器放它过去，于是它被当成
#       **一条已知告警** —— baseline 凭空多 1 条（50 -> 51），
#       而且「幽灵条目」提示会把它列出来：
#       「baseline 里有 1 条已经不触发了：NDK-TIDY: 29.0.14206865」。
#       看起来像「有条告警被修好了」，实际是元数据混进了告警集合。
#       声明行是元数据，不是告警。
$baselineFile = Join-Path $PSScriptRoot 'clang-tidy-baseline.txt'
$known = @()
$baselineRev = ''
$baselineLines = @()
if (Test-Path $baselineFile) {
    $baselineLines = Get-Content -Encoding UTF8 $baselineFile
    $revLine = $baselineLines |
        Where-Object { $_ -match '^\s*NDK-TIDY\s*:' } |
        Select-Object -First 1
    if ($revLine) { $baselineRev = ($revLine -split ':', 2)[1].Trim() }
    $known = $baselineLines |
        Where-Object {
            $_.Trim() -and
            -not $_.Trim().StartsWith('#') -and
            -not ($_ -match '^\s*NDK-TIDY\s*:')
        }
}

# 版本对不上 = **失败**，不是警告。
#
# 三条都要成立才放行，避免「拿一个变量名比另一个」那种自己骗自己的写法：
#   1. baseline 声明了 NDK-TIDY（没声明 = 这份 baseline 来路不明）
#   2. 这次真的读到了 NDK 版本（读不到 = 无法判定，按失败）
#   3. 两者字面相等
if (-not $NDKBASE -and -not $baselineRev) {
    Write-Host ''
    Write-Host 'baseline 没有声明 NDK-TIDY: 版本 —— 这份清单不知道是哪个版本产生的。'
    Write-Host '按**失败**处理。用当前 NDK 重建一次：'
    Write-Host "  .\.ci\run-clang-tidy.ps1 -NDKBASE $ndk"
    exit 1
}
if (-not $NDKBASE -and $actualRev -ne '(读不到 version)' -and ($actualRev -ne $baselineRev)) {
    Write-Host ''
    Write-Host 'clang-tidy 版本对不上：'
    Write-Host "  baseline 是用 NDK $baselineRev 生成的"
    Write-Host "  这一次跑的是 NDK $actualRev  ($ndk)"
    Write-Host ''
    Write-Host '按**失败**处理。理由：这份 baseline 是那一个版本的告警清单，'
    Write-Host '拿别的版本去比，两个数字永远对不上，而「新增」会算成 0 ——'
    Write-Host '也就是说「新增告警 = 失败」这条策略在换版本之后就不成立了，'
    Write-Host '却没有任何人会发现。'
    Write-Host ''
    Write-Host '如果这是**刻意**换 NDK（确认过新旧告警集合的差异）：'
    Write-Host '  .\.ci\run-clang-tidy.ps1 -NDKBASE <新 NDK 路径>'
    exit 1
}

# 受检文件：只含我们自己写的 .cpp
#
# ---- 第 140 轮：版本必须对得上，**而且要在跑 clang-tidy 之前** ----
#
# 判据放在分析之前（第 41-47 行读版本，第 185-244 行比对），
# 是因为这个检查失败时**一行代码都不必分析**：版本不符必然意味着
# baseline 与现实脱节，而脱节原因只有两种（换了 NDK / 有人手改了
# baseline），两种都不该靠「再跑一遍 90 秒的分析」去确认。
#
# 反过来，如果放在分析之后，每次失败都要先付 90 秒 ——
# 门禁自检里那两条注入验证就得慢一倍，而自检是要反复跑的。
#
# 第 129 轮把剩下 6 个补上了。这个列表**曾经只覆盖 44.9%** 的代码：
# `Tool/ClassesTab.cpp`（5683 行，全项目最大的文件）、
# `Main.cpp`（整个渲染循环 + 配置读写）都不在里面 ——
# 而 clang-tidy 是**唯一**能发现 bugprone/cert 类问题
# （空指针解引用、读了没初始化的值…）的检查。
#
# 也就是说：我在前十几轮里往 ClassesTab.cpp 写的那些代码，
# 一直**没人看着**。这和第 101 轮（UI 门禁漏了整个 Il2cpp 目录）
# 是同一个形状，只是这次漏的是最大的那个文件。
#
# `KittyMemory/*` 故意**不加**：那是 vendored 的第三方代码，
# 和 imgui/asmjit/frida-gum/xdl/nlohmann 一样，不归我们修。
$targets = @(
    'Includes/Logger.cpp',
    'Includes/Utils.cpp',
    'Menu/ImGui.cpp',
    'Tool/ObjectDrawManager.cpp',
    'Tool/Keyboard.cpp',
    'Tool/Unity.cpp',
    'Tool/Util.cpp',
    'Tool/Tool.cpp',
    'Tool/ChangeLog.cpp',
    'Tool/ClassesTab.cpp',
    'Tool/Patcher.cpp',
    'Tool/PopUpSelector.cpp',
    'Tool/SelfCheck.cpp',
    'Il2cpp/il2cpp-class.cpp',
    'Il2cpp/Il2cpp.cpp',
    'Main.cpp'
) | ForEach-Object { Join-Path $jni $_ }

# 列表是**手写**的，所以它一定会漂移：新建一个 .cpp 而忘了加进来，
# 那个文件就又静默地不被检查了。直接对着磁盘核一遍。
$ownDir = Join-Path $jni 'KittyMemory'
$ownCpp = Get-ChildItem $jni -Recurse -Include *.cpp -File |
    Where-Object { $_.FullName -notmatch 'imgui|asmjit|Frida|Dobby|xdl|nlohmann|KittyMemory' } |
    ForEach-Object { $_.FullName.Substring($jni.Length + 1) -replace '\\', '/' } |
    Sort-Object
$listed = $targets | ForEach-Object { $_.Substring($jni.Length + 1) -replace '\\', '/' }
$uncovered = $ownCpp | Where-Object { $listed -notcontains $_ }
if ($uncovered) {
    throw "这些项目自己的 .cpp 没进受检列表（clang-tidy 看不到它们）: $($uncovered -join ', ')"
}

$missing = $targets | Where-Object { -not (Test-Path $_) }
if ($missing) { throw "找不到源文件: $($missing -join ', ')" }

$tidy = Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/bin/clang-tidy.exe'
if (-not (Test-Path $tidy)) { throw "找不到 clang-tidy: $tidy" }

# 临时 compile database：只需要给 clang-tidy 正确的头文件搜索路径，
# 不参与真实构建，所以不依赖 ndk-build 生成的产物。
$sysroot = (Join-Path $ndk 'toolchains/llvm/prebuilt/windows-x86_64/sysroot') -replace '\\', '/'
# RUNNER_TEMP 只在 CI 有；本地跑时退回系统临时目录，方便本地复现。
$tempRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { [IO.Path]::GetTempPath() }
# **每个进程一个独立目录**（第 127 轮）。
#
# 原来这里是固定名 `il2cpp-tidy`，于是两个并发的 run-clang-tidy 会共用同一个
# `compile_commands.json`：一个正在写，另一个已经在读。读到半截 JSON 的那个
# clang-tidy 什么都分析不出来，$output 是空的 ->
# 「告警: 0 条」「静态检查通过」。
#
# 最坏的一类假绿：**它报的「通过」不代表它检查过任何东西**。
# 我是在自己同时跑了两轮 check-all 之后撞见的 —— 第一轮报 26，
# 第二轮报 0，而代码一个字都没改。
$dbDir = Join-Path $tempRoot ("il2cpp-tidy-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
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

# ---- 「一条输出都没有」必须**报错**，不能当成「0 条告警」 ----
#
# 这是上一段那个假绿的**另一半**：就算目录撞名修好了，
# 只要 clang-tidy 因为任何原因没产出（工具缺失、数据库读失败、
# 目标文件编译不过），$output 就是空的。
#
# 而空输出走到下面会变成「告警: 0 条」「静态检查通过」——
# **一次没检查过的检查，在输出上和「检查通过」长得一模一样。**
#
# 判据：clang-tidy 只要真的分析过，就一定会打出
# `N warnings generated.` 或 `N warnings generated.`/`No warnings generated.`
# 这一行（带 -quiet 也一样）。一行都没有 = 它没干活。
if ([string]::IsNullOrWhiteSpace($output)) {
    Write-Host ''
    Write-Host "clang-tidy 没有产出任何输出 —— 无法判断有没有告警，按**失败**处理。"
    Write-Host "这**不是**「0 条告警」：前者是「没检查」，后者是「检查了、没问题」。"
    Write-Host "常见原因：compile database 没写成功 / 目标文件编译不过 / clang-tidy 自己崩了。"
    exit 1
}

# 只保留指向我们自己源文件的告警（第三方头文件里的不进来）
#
# `Main\.cpp` 必须在里面（第 129 轮）：它在 **jni 根目录**下，
# 不带 `Includes/`、`Tool/` 这样的前缀，而旧模式只认那四种 ——
# 于是 Main.cpp 明明进了受检列表，它的告警却被这条模式**整个丢掉**。
# 那种「分析了半天、结果没人看」比不分析更糟：门禁显示覆盖率上去了，
# 实际没人在看。
$projectPattern = '(?:[A-Za-z0-9_./\\-]*[/\\])?(?:Includes|Menu|Tool|Il2cpp)[/\\][A-Za-z0-9_.-]*\.(cpp|h):\d+:\d+: warning:|^Main\.cpp:\d+:\d+: warning:'
$findings = ($output -split "`r?`n") | Where-Object {
    $_ -match $projectPattern -and $_ -notmatch 'imgui[/\\]|asmjit[/\\]|frida-gum|xdl[/\\]|nlohmann[/\\]|KittyMemory'
}

$keys = $findings | ForEach-Object {
    # 归一化掉行号：代码一动行号就变，不该因此重新报一遍
    ($_ -replace '^(\s*)', '') -replace ':\d+:\d+: warning:', '|warning:'
} | Sort-Object -Unique

$new = $keys | Where-Object { $known -notcontains $_ }

# -WriteBaseline / -NDKBASE <ndk>：把当前集合写成 baseline，
# 并把生成它的 clang-tidy 版本一起写进去。确认存量告警无害时用一次即可。
if ($WriteBaseline -or $NDKBASE) {
    $header = @(
        '# clang-tidy 存量告警',
        '#',
        '# 门禁策略：**新增告警 = 失败**。这个文件是「已确认无害的存量」。',
        '#',
        '# 注意：**以 # 开头的行是注释，不算已知告警**（加载时会被跳过）。',
        '#',
        "# 本清单由 NDK $actualRev 的 clang-tidy 生成。换版本 = 必须重建：",
        "#   .\.ci\run-clang-tidy.ps1 -NDKBASE <新 NDK 路径>",
        "# 理由见 run-clang-tidy.ps1 里的同名检查（第 140 轮）。",
        "NDK-TIDY: $actualRev",
        ''
    )
    # 注意：这里**不能**直接覆盖整个文件 —— 上面那段按类说明的注释是
    # 有人读的东西（第 129 轮写的抑制理由），重生成不该把它抹掉。
    # 做法：保住所有注释块，把 NDK-TIDY 换成新值，把告警条目整段替换。
    $kept = @()
    foreach ($l in $baselineLines) {
        if ($l -match '^\s*NDK-TIDY\s*:') { continue }
        $kept += $l
    }
    # 去掉旧的告警条目段（第一个非注释、非空行之后直到文件尾）
    $out = @()
    $inEntries = $false
    foreach ($l in $kept) {
        $isComment = -not $l.Trim() -or $l.Trim().StartsWith('#')
        if (-not $isComment) { $inEntries = $true }
        if (-not $inEntries) { $out += $l }
    }
    while ($out.Count -gt 0 -and -not $out[$out.Count - 1].Trim()) { $out = $out[0..($out.Count - 2)] }
    ($header + $out + '' + $keys) | Set-Content -Path $baselineFile -Encoding UTF8
    Write-Host "已写入 baseline: $baselineFile（$($keys.Count) 条，clang-tidy $actualRev）"
    exit 0
}

Write-Host ''
Write-Host "clang-tidy: NDK $actualRev"
Write-Host "clang-tidy 告警: $(($keys).Count) 条（baseline $(($known).Count) 条，新增 $(($new).Count) 条）"

# 两个数字不等**必须说出来**，不能只在括号里躺着。
#
# 第 140 轮发现的：CI 报「46 条（baseline 50 条）」，两次都是。
# 脚本确实判了「新增 0 条」并放行了 —— 从判据上说它没错
# （没有任何一条不在 baseline 里），但这个**差额本身**说明
# baseline 里有条目已经不触发了，而那意味着：
#   - 多出来的告警会顶掉它们的位置，仍然算「已抑制」
#   - 谁都不知道 baseline 已经和现实脱节了
#
# 现在版本不一致会直接失败（上面），但「同版本下条数不等」
# 仍可能是真的脱节（有人手工改过 baseline，或告警被修复了）。
# 所以：只要条数不等就明确提示，让它成为**看得见**的事实。
if ($keys.Count -ne $known.Count) {
    $ghost = $known | Where-Object { $keys -notcontains $_ }
    Write-Host ''
    Write-Host "注意：当前 $(($keys).Count) 条 / baseline $(($known).Count) 条 —— 两个数字不等。"
    if ($ghost) {
        Write-Host "baseline 里有 $(($ghost).Count) 条**已经不触发**了："
        $ghost | ForEach-Object { Write-Host "    $_" }
        Write-Host '这些条目不拦任何东西，却让 baseline 显得比实际宽松。'
        Write-Host '确认过之后用 -NDKBASE <ndk> 重建。'
    }
    $extra = $keys | Where-Object { $known -notcontains $_ }
    if ($extra) {
        Write-Host "有 $(($extra).Count) 条不在 baseline 里（见下方「新增告警」）。"
    }
}

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
    Write-Host "  .\.ci\run-clang-tidy.ps1 -NDKBASE $ndk"
    exit 1
}

Write-Host ''
Write-Host '静态检查通过（无新增告警）'
exit 0
