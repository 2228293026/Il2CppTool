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
Write-Host "全部 $($results.Count) 条检查都验证过：能红，也能绿。" -ForegroundColor Green
exit 0
