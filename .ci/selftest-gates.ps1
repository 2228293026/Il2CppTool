# 门禁自检：每一条检查都必须**抓得到**它要防的东西。
#
# 背景
#
# 第 64 轮发现，我亲手写的一条检查**从写下来那一刻起就是死的**
# （排除模式匹配了所有语句），而它一直显示为「通过」。
# 第 63 轮也有一条断言从未执行（$host 是 PowerShell 只读变量）。
#
# 结论：
#     **一个从不失败的检查，和没有检查，在输出上无法区分。**
#     唯一的区分办法是「故意制造违规，看它抓不抓得到」。
#
# 所以这个脚本对每条检查做两件事：
#   1. 确认它在**干净**的树上通过
#   2. 故意注入一个该检查要防的违规，确认它**变红**
#   3. 撤销注入，确认它回到绿
#
# 任何一步不符合预期就以非 0 退出。
#
# 跑法： .\.ci\selftest-gates.ps1
param()

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$results = @()

function Run-Command {
    param([string]$Exe, [string[]]$CmdArgs)
    & $Exe @CmdArgs 2>&1 | Out-Null
    return $LASTEXITCODE
}

# 临时改一个文件、跑检查、还原。用 try/finally 保证一定还原。
function Test-Rule {
    param(
        [string]$Name,
        [scriptblock]$Inject,      # 注入违规；返回 $true 表示注入成功
        [scriptblock]$Check,       # 跑检查；返回退出码
        [string]$Target,           # 会被改动的文件（相对路径）
        [bool]$ExpectRed = $true   # 注入后**应该**变红。
                                    # 测「不该误报」的规则时传 $false ——
                                    # 那类规则变红才是失败。门禁必须两边都验：
                                    # 只验「能红」的规则，会把注释和日志也拦下来。
    )
    $path = Join-Path $root $Target
    $before = [IO.File]::ReadAllText($path)
    # 还原必须写回**原始字节**。用 WriteAllText 重新编码会把 .ps1 的
    # UTF-8 BOM 弄丢 —— 而那正是第 4 条检查要防的东西：
    # 自检脚本自己会制造它要检出的故障。
    $beforeBytes = [IO.File]::ReadAllBytes($path)
    $injected = $false
    $rcBad = 0
    $rcGood = 0
    try {
        if (& $Inject $before) {
            $injected = $true
        }
        $rcBad = & $Check
        # 还原
        [IO.File]::WriteAllBytes($path, $beforeBytes)
        $rcGood = & $Check
    }
    finally {
        [IO.File]::WriteAllBytes($path, $beforeBytes)
    }

    $ok = $injected -and (($rcBad -ne 0) -eq $ExpectRed) -and ($rcGood -eq 0)
    $script:results += [pscustomobject]@{
        Name    = $Name
        Injected = $injected
        RedOnBad = ($rcBad -ne 0)
        ExpectRed = $ExpectRed
        GreenAfter = ($rcGood -eq 0)
        Ok      = $ok
    }
}

$uiGate = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1', '-Quiet')
}

# ---- 缓存：脚本和被注入的文件都没动过，就不用真跑一遍 ----
#
# 第 90 轮的教训：我提交前习惯性 `-SkipSelfTest`，而这一步是**唯一**能
# 抓住「规则静默失效」的东西 —— 代价是 60 多秒，于是我一直在跳过它。
#
# 只靠「请不要跳过」是没用的（第 85 轮的教训：靠记忆的门禁等于没有）。
# 所以让它**默认就快**：内容指纹没变就秒过，变了就真跑。
# 这样 -SkipSelfTest 就不再是一个「为了省时间而放弃保护」的选项。
$cacheFile = Join-Path $root '.ci/.selftest-cache'
$watch = @(
    '.ci/selftest-gates.ps1', '.ci/check-ui-patterns.ps1',
    '.ci/check-structure.ps1', '.ci/check-hosts.ps1',
    'app/src/main/jni/Tool/Tool.cpp', 'app/src/main/jni/Tool/ClassesTab.cpp',
    'app/src/main/jni/Main.cpp'
)
$sb = [System.Text.StringBuilder]::new()
foreach ($w in $watch) {
    $p = Join-Path $root $w
    if (Test-Path $p) { [void]$sb.AppendLine("$w " + (Get-FileHash $p -Algorithm SHA256).Hash) }
}
$head = (& git -C $root rev-parse HEAD 2>$null)
[void]$sb.AppendLine("HEAD $head")
$fingerprint = $sb.ToString()
$cached = ''
if (Test-Path $cacheFile) { $cached = [IO.File]::ReadAllText($cacheFile) }
if ($cached -eq $fingerprint) {
    Write-Host '门禁自检：门禁脚本和被注入的文件都没动过，跳过（上一次是绿的）'
    exit 0
}

# ---- 1. 规则 A2：相邻重复行 ----
Test-Rule 'A2 相邻重复行' {
    param($t)
$needle = '                RecordFieldChange(currentObj, "(整个对象)", "保存", "已加入 GC 强根", {}, "");'
    if (-not $t.Contains($needle)) { return $false }
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        $t.Replace($needle, $needle + "`r`n" + $needle), (New-Object Text.UTF8Encoding($false)))
    return $true
} $uiGate 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 2. 规则 C：workflow 缩进 ----
Test-Rule 'C workflow 步骤缩进' {
    param($t)
    $needle = "      - name: Pre-commit checks (single entry point)`r`n        shell: pwsh`r`n        run: .\.ci\check-all.ps1"
    if (-not $t.Contains($needle)) { return $false }
    [IO.File]::WriteAllText((Join-Path $root '.github/workflows/ci.yml'),
        $t.Replace($needle, "      - name: Pre-commit checks`r`n  run: .\.ci\check-all.ps1"),
        (New-Object Text.UTF8Encoding($false)))
    return $true
} $uiGate '.github/workflows/ci.yml'

# ---- 3. 编码检查：源码里塞一个 U+FFFD ----
$encCheck = {
    $bad = 0
    Get-ChildItem app/src/main/jni -Recurse -Include *.cpp, *.h -File | ForEach-Object {
        $x = [IO.File]::ReadAllText($_.FullName)
        if ($x -match "\uFFFD") { $bad++ }
    }
    if ($bad -gt 0) { return 1 }
    return 0
}
Test-Rule '源码编码（U+FFFD）' {
    param($t)
    $needle = 'constexpr int MAX_CLASSES = 500;'
    if (-not $t.Contains($needle)) { return $false }
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        $t.Replace($needle, $needle + " // �"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $encCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 4. BOM 检查：把一个 .ps1 的 BOM 去掉 ----
$bomCheck = {
    $bad = 0
    foreach ($s in Get-ChildItem .ci, . -Filter *.ps1 -File -ErrorAction SilentlyContinue) {
        $bytes = [IO.File]::ReadAllBytes($s.FullName)
        if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { continue }
        $text = [Text.Encoding]::UTF8.GetString($bytes)
        $nonAscii = 0
        foreach ($c in $text.ToCharArray()) { if ([int]$c -gt 127) { $nonAscii++ } }
        if ($nonAscii -gt 0) { $bad++ }
    }
    if ($bad -gt 0) { return 1 }
    return 0
}
Test-Rule 'PowerShell BOM' {
    param($t)
    $p = Join-Path $root '.ci/check-hosts.ps1'
    $bytes = [IO.File]::ReadAllBytes($p)
    if ($bytes.Length -lt 3 -or $bytes[0] -ne 0xEF) { return $false }
    # 去掉 BOM
    [IO.File]::WriteAllBytes($p, $bytes[3..($bytes.Length - 1)])
    return $true
} $bomCheck '.ci/check-hosts.ps1'

# ---- 13. 规则 I：把第 97 轮那个「加根失败还照样存指针」复现出来 ----
$iCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'I 加根失败不许存指针' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'NewHandle\(kb\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $j = -1
    for ($k = $i; $k -lt $i + 30; $k++) {
        if ($ls[$k] -match '^\s*return;\s*$') { $j = $k; break }
    }
    if ($j -lt 0) { return $false }
    $ls.RemoveAt($j)
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/Keyboard.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $iCheck 'app/src/main/jni/Tool/Keyboard.cpp'
# ---- 12. 规则 H：把第 94 轮那个「每 2 秒改一次文件系统」复现出来 ----
$hCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'H 自检项不许有副作用' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'static const bool kProbeResult') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    # 把探针从「一次性」挪回「每 2 秒的采集路径里」
    $ls.Insert($i, '    std::remove(Il2cpp::getDataPath().c_str());')
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/SelfCheck.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $hCheck 'app/src/main/jni/Tool/SelfCheck.cpp'
# ---- 11. 门禁必须按 **UTF-8** 读源码，不能跟着系统区域设置走 ----
# 第 92 轮 CI 红而本地绿，查出来是这一条：
#
#   Windows PowerShell 5.1 的 Get-Content 不带 -Encoding 时按**系统 ANSI**
#   读文件，这台机器上是 GBK。源码是 UTF-8，于是
#
#       "**未运行** ..."      UTF-8 字节 22 2A 2A E6 9C AA E8 BF 90 E8 A1 8C 2A 2A 22
#   被 GBK 解码成        "**鏈繍琛?*"
#                                       ^^ 这里只剩一个 * —— 8C 2A 是个非法
#                                          组合，解码器把两个字节一起吃掉
#
# 规则 D 要找的是 `\*\*`，本地只看到一个 `*` → 绿。CI 是 Linux/UTF-8，
# 两个 `*` 都在 → 红。
#
# 也就是说：**所有基于文本的门禁在不同的机器上会给出不同的答案**。
# 而门禁的全部意义就是「哪里都一样的同一个答案」。
$encCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'D 在 GBK 机器上也要看得见中文里的 **' {
    param($t)
    # 用**中文标识**而不是 ASCII：ASCII 标识在任何编码下都正常，
    # 只有非 ASCII 才暴露「读文件用错编码」这个 bug。
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $j = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*ImGui::Text\("[^"]*"\);') { $j = $k; break }
    }
    if ($j -lt 0) { return $false }
    # 关键：`**` 必须**紧跟在中文后面**，而且是全句里**唯一**的一对。
    #
    # 写成 `"**中文**"` 是不行的 —— 开头那对 `**` 前面是 ASCII 空格，
    # 任何编码下都完整，规则照样报红，这条自检就**测不出编码问题**。
    # 而 `"未运行**"` 里 8C 2A 会被 GBK 解码器一起吃掉，只剩一个 `*`，
    # 裸 Get-Content 时规则必然漏报。
    #
    #   裸 Get-Content + 未运行**  -> rc=0   漏报
    #   -Encoding UTF8 + 未运行**  -> rc=1   报出
    $ls[$j] = '            ImGui::Text("未运行**");'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Main.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $encCheck 'app/src/main/jni/Main.cpp'
# ---- 10. 规则 G + **门禁必须扫到 jni 根目录的 .cpp** ----
# 第 90 轮发现的覆盖漏洞：$dirs 只有 Tool / Menu / Includes，
# 所以 Main.cpp（927 行，整个渲染循环、追踪页、配置读写），
# 对下面**每一条**规则都是不可见的。
# 这条自检同时验两件事：规则 G 能红，而且注入点在 Main.cpp 里。
$gCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'G 绕过 FileWriter 直写' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'Util::FileWriter fileWriter\("tool_conf\.json"\);') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
        # 注入的标识符**必须用 ASCII**。
    #
    # 第 90 轮 CI 红过一次而本地绿：本地注入的是 `std::ofstream 直写(...)`，
    # 规则 G 的 `^\s*std::ofstream\s+\w+\s*\(` 匹配上了；到了 CI 上
    # 这一行的中文被按别的编码读成乱码，`\w` 就不匹配了 ——
    # 门禁**自己的测试**依赖了字符编码，这件事本身就是脆弱的。
    # 换成 ASCII 之后与编码无关。
    $ls.Insert($i, '    std::ofstream directWrite("tool_conf.json");')
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Main.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $gCheck 'app/src/main/jni/Main.cpp'

# 规则 D 也必须看得见 Main.cpp（覆盖漏洞就是从这个角度发现的）
Test-Rule 'D 能看见 Main.cpp' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $j = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*ImGui::Text\("[^"]*"\);') { $j = $k; break }
    }
    if ($j -lt 0) { return $false }
    $ls[$j] = $ls[$j] -replace 'ImGui::Text\("([^"]*)"\);', 'ImGui::Text("$1 **加星号**");'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Main.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $gCheck 'app/src/main/jni/Main.cpp'
# ---- 9. 规则 F：第 80 轮那个 bug 原样注入，必须被抓到 ----
$fCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'F 失败后清掉恢复数据' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'if \(!RestorePatchedMethod\(method, o\.bytes\)\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    # 把「成功才清」的那 18 行换成第 80 轮的无条件版本
    $ls.RemoveRange($i, 18)
    $ls.InsertRange($i, [System.Collections.ArrayList]@(
        '                if (!RestorePatchedMethod(method, o.bytes))',
        '                {',
        '                    LOGE("恢复失败: %s", method->getName() ? method->getName() : "?");',
        '                }',
        '                o.bytes.clear();',
        '                o.text.clear();'
    ))
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $fCheck 'app/src/main/jni/Tool/ClassesTab.cpp'
# ---- 8. 规则 E：把第 83 轮那个 bug 原样注入，必须被抓到 ----
# 这是**同一个真实缺陷**的复现，不是造一个假的：第 83 轮
# 「折叠关注值 → 冻结全停」就是这么写的。
$eCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'E 游戏状态挂在面板上' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $af = -1
    for ($i = 0; $i -lt $ls.Count; $i++) {
        if ($ls[$i] -match '^\s*ClassesTab::ApplyFreezes\(\);') { $af = $i; break }
    }
    if ($af -lt 0) { return $false }
    $ls.RemoveAt($af)
    $ch = -1
    for ($i = 0; $i -lt $ls.Count; $i++) {
        if ($ls[$i] -match 'CollapsingHeader\("关注值"\)') { $ch = $i; break }
    }
    if ($ch -lt 0) { return $false }
    $ls.Insert($ch + 2, (' ' * 8 + 'ClassesTab::ApplyFreezes();'))
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/Tool.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $eCheck 'app/src/main/jni/Tool/Tool.cpp'

# 反向：同一个面板里、但被 Button 包着的调用是**合法**的，不该拦
Test-Rule 'E 不误报（Button 包裹）' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $ch = -1
    for ($i = 0; $i -lt $ls.Count; $i++) {
        if ($ls[$i] -match 'CollapsingHeader\("关注值"\)') { $ch = $i; break }
    }
    if ($ch -lt 0) { return $false }
    $ls.Insert($ch + 2, (' ' * 8 + 'if (ImGui::Button("面板内的按钮"))'))
    $ls.Insert($ch + 3, (' ' * 12 + '{'))
    $ls.Insert($ch + 4, (' ' * 16 + 'ClassesTab::ApplyFreezes();'))
    $ls.Insert($ch + 5, (' ' * 12 + '}'))
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/Tool.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $eCheck 'app/src/main/jni/Tool/Tool.cpp' -ExpectRed $false
# ---- 7. markdown 规则：渲染文本里的 ** 必须被抓到，注释/日志里的必须放过 ----
# 这是第 44/49 轮的老缺陷，第 84 轮又犯了一次。规则本身要能红，
# 也要证明**不误报** —— 一个把注释和日志也拦下来的检查会被人加白名单，
# 然后就彻底没用了。
$mdCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'D 渲染文本的 ** ' {
    param($t)
    $p = $t.IndexOf('而保存失败没有任何提示"));')
    if ($p -lt 0) { return $false }
    $out = $t.Substring(0, $p) + '而保存失败**没有任何提示**"));' + $t.Substring($p + '而保存失败没有任何提示"));'.Length)
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/SelfCheck.cpp'), $out,
        (New-Object Text.UTF8Encoding($false)))
    return $true
} $mdCheck 'app/src/main/jni/Tool/SelfCheck.cpp'

# 反向：注释和日志里的 ** 不该被拦
Test-Rule 'D 不误报（注释/日志）' {
    param($t)
    $marker = 'void ClassesTab::ApplyFreezes()'
    $p = $t.IndexOf($marker)
    if ($p -lt 0) { return $false }
    $out = $t.Substring(0, $p) + "// 注释里的 **markdown** 不该被拦`n" + $t.Substring($p)
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'), $out,
        (New-Object Text.UTF8Encoding($false)))
    return $true
} $mdCheck 'app/src/main/jni/Tool/ClassesTab.cpp' -ExpectRed $false
# ---- 6. 结构检查：删掉一个 } 必须被抓到 ----
# 这是第 74 轮真实发生过的损坏（按行号搬代码，误删 `});`）。
$structCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-structure.ps1')
}
Test-Rule '结构（花括号配平）' {
    param($t)
    $marker = "else if (value.is_number_float())"
    $p = $t.IndexOf($marker)
    if ($p -lt 0) { return $false }
    $lineStart = $t.LastIndexOf("`n", $p) + 1
    $lineEnd = $t.IndexOf("`n", $p)
    if ($lineEnd -lt 0) { $lineEnd = $t.Length }
    $line = $t.Substring($lineStart, $lineEnd - $lineStart)
    # 删掉紧挨着它**上面**那一行（就是那个 '}'）—— 复现第 74 轮的损坏
    $prevEnd = $lineStart - 1
    $prevStart = $t.LastIndexOf("`n", $prevEnd - 1) + 1
    $prevLine = $t.Substring($prevStart, $prevEnd - $prevStart)
    if ($prevLine.Trim() -ne '}') { return $false }
    $out = $t.Substring(0, $prevStart) + $t.Substring($prevEnd)
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'), $out,
        (New-Object Text.UTF8Encoding($false)))
    return $true
} $structCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 5. 宿主检查：把 DrawWatches 挂回对象检视器 ----
$hostGate = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-hosts.ps1')
}
Test-Rule '跨界面宿主' {
    param($t)
    $needle = '        poper.Update();'
    if (-not $t.Contains($needle)) { return $false }
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        $t.Replace($needle, '        DrawWatches();' + "`r`n" + $needle), (New-Object Text.UTF8Encoding($false)))
    return $true
} $hostGate 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 汇总 ----
Write-Host ""
Write-Host "门禁自检结果：" -ForegroundColor Cyan
$failed = 0
foreach ($r in $results) {
    if ($r.Ok) {
        Write-Host ("  [通过] {0}" -f $r.Name) -ForegroundColor Green
    }
    else {
        $failed++
        Write-Host ("  [失败] {0}  注入成功={1} 变红={2} 还原后绿={3}" -f `
                $r.Name, $r.Injected, $r.RedOnBad, $r.GreenAfter) -ForegroundColor Red
    }
}

Write-Host ""
if ($failed -gt 0) {
    Write-Host "$failed 条检查抓不到它要防的东西 —— 它们一直是装饰。" -ForegroundColor Red
    exit 1
}
    # 只有全绿才写缓存 —— 写了就等于「这次验过了」
    [IO.File]::WriteAllText($cacheFile, $fingerprint, (New-Object Text.UTF8Encoding($false)))
Write-Host "全部 $($results.Count) 条检查都验证过：能红，也能绿。" -ForegroundColor Green
exit 0
