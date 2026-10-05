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

# 备份目录必须在仓库外（否则备份文件本身会被门禁扫到），
# 也不能放在系统 TEMP 里 —— CI 上多份并发跑会互相覆盖。
#
# 后缀用**路径本身**而不是哈希：
#   · `[IO.Path]::GetHashCode` 在 PowerShell 里返回**空**（它不是静态属性，
#     拿到的是方法组），于是目录名变成 `...stash-`，空尾巴；
#   · `String.GetHashCode()` 在 .NET Core 里**每个进程都不一样**
#     （随机化，为了防哈希碰撞攻击），跨进程就找不到上一次留下的备份了
#     —— 而「跨进程」恰恰是这个机制的全部意义。
# 把非法字符替掉就行，稳定、可读、还能一眼看出是哪份仓库。
$stashRoot = Join-Path ([IO.Path]::GetTempPath()) ("il2cpp-selftest-stash-" + ($root -replace '[\\/:*?"<>|]', '_'))
if (-not (Test-Path $stashRoot)) { [void](New-Item -ItemType Directory -Path $stashRoot -Force) }

# ---- 上一次运行被打断过？先把被改坏的文件还原（第 139 轮）----
#
# 这个还原必须在**任何检查之前**跑：门禁要在被改坏的树上给出
# 「这里有个真 bug」的结论，而不是对着自己上次留下的垃圾报错。
function Repair-InjectedFiles {
    if (-not (Test-Path $stashRoot)) { return 0 }
    $n = 0
    foreach ($p in Get-ChildItem $stashRoot -Filter *.path -File) {
        $rel = [IO.File]::ReadAllText($p.FullName).Trim()
        $bak = $p.FullName.Substring(0, $p.FullName.Length - 5)   # 去掉 '.path'
        $target = Join-Path $root $rel
        if (-not (Test-Path $bak)) { continue }
        $bakBytes = [IO.File]::ReadAllBytes($bak)
        $cur = if (Test-Path $target) { [IO.File]::ReadAllBytes($target) } else { @() }
        # 长度不同就一定要写回去。
        #
        # 第一版这里写的是「长度不同就 skip」—— 想省掉逐字节比较，
        # 结果**注入几乎总是会改行长**，于是这一条 skip 把整个机制废掉了：
        # 反向验证时它一个文件都没还原，而我还在看别的地方。
        # （和第 111/125 轮同一个形状：一条为了省事加的守卫，
        #   恰好把「本该走的那条路」关上了。）
        if ($cur.Length -ne $bakBytes.Length) {
            [IO.File]::WriteAllBytes($target, $bakBytes)
            Write-Host "  已还原本次之前被打断留下的注入：$rel"
            $n++
            continue
        }
        # 长度一样再比字节 —— 免得每次都白白碰一遍 mtime。
        $same = $true
        for ($i = 0; $i -lt $bakBytes.Length; $i++) {
            if ($cur[$i] -ne $bakBytes[$i]) { $same = $false; break }
        }
        if ($same) { continue }
        [IO.File]::WriteAllBytes($target, $bakBytes)
        Write-Host "  已还原本次之前被打断留下的注入：$rel"
        $n++
    }
    return $n
}
$repaired = Repair-InjectedFiles
if ($repaired -gt 0) {
    Write-Host "门禁自检：上一次运行被中断，已先还原 $repaired 个文件（否则检查的是自己留下的垃圾）"
}

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
    # try/finally 只在**进程还活着**的时候有用。进程被杀掉（Ctrl+C、
    # 任务管理器、CI 超时）时 finally 一次都不跑，文件就留在被改坏的
    # 状态 —— 第 139 轮真的中了一次：`check-all.ps1` 被中断时正停在
    # 规则 N 的注入阶段，`try` 被换成注释留在了 ClassesTab.cpp 里，
    # 下一轮我还以为那是自己写的 bug，查了半天才发现是上一次跑的残留。
    #
    # 所以在**注入之前**把原始字节另存一份到磁盘。进程死了，
    # 下一次运行时 `Repair-InjectedFiles` 会把它原样写回去。
    # 备份目录必须在仓库外 —— 否则备份本身也会被门禁扫到。
    $stash = Join-Path $stashRoot ($Target -replace '[\\/]', '__')
    [IO.File]::WriteAllBytes($stash, $beforeBytes)
    [IO.File]::WriteAllText($stash + '.path', $Target, (New-Object Text.UTF8Encoding($false)))
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
        # 这条已经还原完了，备份就没用了，留着只会让下次运行
        # 以为「上一次被中断过」。
        # 括号不能省：`Remove-Item $stash, $stash + '.path'` 会被
        # PowerShell 解析成**三个**参数，于是每次都报
        # 「A positional parameter cannot be found that accepts argument '+'」，
        # 备份永远删不掉 —— 也就是「每次运行都以为上一次被中断过」。
        Remove-Item $stash, ($stash + '.path') -Force -ErrorAction SilentlyContinue
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
    # 少了它，改了这个脚本自检也会走缓存 —— 而缓存里的「绿」
    # 是**上一次**的结果（第 140 轮：门禁脚本改了却仍然秒过）。
    '.ci/check-workflow.ps1',
    # 这两个是被注入的目标文件，不在 watch 里等于改了它们不会触发重跑。
    '.github/workflows/ci.yml',
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
    # 注入**真** U+FFFD，不能用 "`u{FFFD}" 那种字面转义 ——
    # 那是六个普通字符，编码检查当然不会红。
    # 这条自检第一版就是这么错的：注入成功=True，变红=False，CI 上白跑一趟。
    # 用 [char]0xFFFD 构造，脚本本身保持纯 ASCII，不再依赖源文件编码。
    $bad = [string][char]0xFFFD
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        $t.Replace($needle, $needle + ' // ' + $bad), (New-Object Text.UTF8Encoding($false)))
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

# ---- 14. 规则 J：把第 98 轮那个「加根失败退回裸指针」复现出来 ----
$jCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'J 不许退回裸指针' {
    param($t)
    # 用行数组拼，避免嵌套 here-string。
    # 锚点按**真实文本**写。函数体里那行注释也在 —— 漏掉它就匹配不上，
    # 而「注入成功=False」意味着这条自检又一次在**测空气**。
    $good = @(
        '    if (!info.gameObjectHandle)',
        '    {',
        '        // 加根没成功。不退回裸指针 —— 那个地址随时可能是野的。',
        '        return nullptr;',
        '    }',
        '    return Il2cpp::GC::GetHandleTarget(info.gameObjectHandle);'
    ) -join "`r`n"
    $bad = @(
        '    if (info.gameObjectHandle)',
        '    {',
        '        return Il2cpp::GC::GetHandleTarget(info.gameObjectHandle);',
        '    }',
        '    return info.gameObject;'
    ) -join "`r`n"
    if ($t.Contains($good)) {
        [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ObjectDrawManager.cpp'),
            $t.Replace($good, $bad), (New-Object Text.UTF8Encoding($false)))
        return $true
    }
    return $false
} $jCheck 'app/src/main/jni/Tool/ObjectDrawManager.cpp'
# ---- 15. 规则 J 扩到「没有成员访问」的形状（ResolveSaved）----
# 第 99 轮：规则 J 第一版只认 return info.gameObject;（成员访问），
# 于是 return obj;（裸参数）**从门禁底下溜过去了** ——
# 而它和前一个是**完全同一个 bug**，只是写的人不同。
#
# 这条自检锚在 ClassesTab::ResolveSaved 上，专门盯这个形状。
$j2Check = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'J 裸参数形状（ResolveSaved）' {
    param($t)
    # 锚点就是那一行 return nullptr —— 换回 return obj 就复现出
    # 第 99 轮那个 bug 的**原样形状**。
    $good = '        return nullptr;'
    $bad  = '        return obj;'
    if ($t.Contains($good)) {
        [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
            $t.Replace($good, $bad), (New-Object Text.UTF8Encoding($false)))
        return $true
    }
    return $false
} $j2Check 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 16. 扫描范围：Il2cpp 目录必须在内（第 101 轮）----
# 第 101 轮最大的发现不是那个 bug，是**这个 bug 所在的文件从来没被扫过**：
# check-ui-patterns.ps1 的 $dirs 少了 Il2cpp，5009 行 Il2cpp.cpp / Il2cpp.h
# 对规则 A / A2 / B2 / D / E / F / G / H / I / J 全都是不可见的。
$covCheck = {
    # -SkipClangTidy 是**必须**的（第 112 轮）。
    #
    # 这一条要验的是「check-all 的扫描范围元检查能发现目录被拿掉」。
    # 跑整个 check-all 会顺带跑 clang-tidy，而 clang-tidy 需要 NDK_PATH。
    # 没设这个环境变量时它会失败 -> 非 0 -> 自检报「还原后绿=False」，
    # 而**代码其实完全正常**。
    #
    # 也就是说这条自检的成败取决于**跑它的人有没有配好 NDK** ——
    # 一个「在我机器上是红的」的检查，比没有更糟。
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-all.ps1', '-SkipSelfTest', '-SkipClangTidy')
}
Test-Rule 'K 扫描范围含 Il2cpp' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $idx = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'jni/Il2cpp') { $idx = $k; break }
    }
    if ($idx -lt 0) { return $false }
    $ls.RemoveAt($idx)
    [IO.File]::WriteAllText((Join-Path $root '.ci/check-ui-patterns.ps1'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($true)))
    return $true
} $covCheck '.ci/check-ui-patterns.ps1'

# ---- 17. 规则 L：把「只画不可见占位」复现出来（第 104 轮）----
$lCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'L 不可恢复的行要有可见状态' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'ImGui::TextDisabled\("已恢复"\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    # 换成一个不可见的占位 = 复现第 104 轮那个「这一行没有任何状态」
    $ls[$i] = '                ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ChangeLog.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $lCheck 'app/src/main/jni/Tool/ChangeLog.cpp'

# ---- 18. 规则 N：把「Dobby 替换函数没有边界」复现出来（第 108 轮）----
#
# 第 108 轮在两个 Dobby 回调上都找到过：swapbuffers_hook 的 try 只包住
# menuAddress()，而 hookerHandler 一个 try 都没有。
$nCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'N Dobby 替换函数要有异常边界' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'void hookerHandler\(void \*address') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    # 从函数开头**往后**找第一个 try —— 用固定偏移会打到函数签名上
    #（try 上面还有一整段说明）。
    $ty = -1
    for ($k = $i; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*try\s*$') { $ty = $k; break }
    }
    if ($ty -lt 0) { return $false }
    $ls[$ty] = '    ; // 第 108 轮之前的样子：一个 try 都没有'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $nCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 19. 规则 N 的另一半：进托管代码的 invoke 也要有边界（第 110 轮）----
#
# 这条一开始**一条都没匹配上**（规则本体把 `{` 要求在定义那一行，
# 而这三个模板的 `{` 都在下一行）—— 也就是又一条「从写下来起就是死的」检查，
# 而且它显示为「通过」。靠反向验证才发现（第 110 轮）。
$n2Check = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'N 进托管代码的 invoke 要有边界' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'T MethodInfo::invoke\(Il2CppObject \*instance\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ty = -1
    for ($k = $i; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*try\s*$') { $ty = $k; break }
    }
    if ($ty -lt 0) { return $false }
    $ls[$ty] = '    ; // 第 110 轮之前的样子'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Il2cpp/il2cpp-class.h'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $n2Check 'app/src/main/jni/Il2cpp/il2cpp-class.h'

# ---- 20. 规则 O：把「日志函数会抛」复现出来（第 112 轮）----
#
# 这条规则写出来之后**第一次跑就是红的**（7 处），因为 Logger.cpp 里的
# 函数确实都会抛。中间还卡了两次：先把 `{` 要求在同一行（没匹配上，
# 显示为「通过」），又先把行 trim 掉再要求 `^ {4}`（同样没匹配上）。
# 两次都是「看起来是绿的」，靠人肉盯着那条 0 命中才发现。
$oCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'O 日志函数不许往外抛' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'void AddLog\(const char \*prefix') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ty = -1
    for ($k = $i; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*try\s*$' -and $ls[$k - 1] -match '它自己不能再是第一个崩的地方') { $ty = $k; break }
    }
    if ($ty -lt 0) { return $false }
    $ls[$ty] = '        ; // 第 112 轮之前的样子'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Includes/Logger.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $oCheck 'app/src/main/jni/Includes/Logger.cpp'

# ---- 21. 规则 P：把「读取失败也取样」复现出来（第 114 轮）----
#
# 这条规则第一版把**注释**当成了守卫 —— 我把 isfinite(v) 去掉之后它还是绿的，
# 因为上面那行说明文字里写着 `//   isfinite(v)`。
# 第 90 轮的教训：匹配之前先把注释剥掉。
$pCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'P 取样不许把读取失败当 0' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $n = 0
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'if \(text != "<非标量>"') { $ls[$k] = '                if (true)'; $n++ }
        if ($ls[$k] -match 'if \(endp != nullptr && endp != text\.c_str\(\) && \*endp ==') { $ls[$k] = '                    if (true)'; $n++ }
        # std::isfinite(v))) 也要去掉 —— 它本身就是 $hasGuard 认得的守卫之一，
        # 只去掉两个 if 的条件，规则仍会从 isfinite 那行判定「有守卫」。
        if ($ls[$k] -match '^\s*std::isfinite\(v\)\)\s*$') { $ls[$k] = '                        0);'; $n++ }
    }
    if ($n -lt 2) { return $false }
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $pCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 22. 规则 Q：把「直接调 SetClipboardText」复现出来（第 118 轮）----
$qCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'Q 复制必须走不抛的助手' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*std::string err;$') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls.Insert($i, '        ImGui::SetClipboardText("x");')
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/SelfCheck.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $qCheck 'app/src/main/jni/Tool/SelfCheck.cpp'

# ---- 23. 规则 R：把「保存忽略返回值」复现出来（第 119 轮）----
$rCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'R UI 层的保存要看返回值' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*if \(!Tool::ConfigSave\(\)\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls[$i] = '            Tool::ConfigSave(); if (false)'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $rCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 24. 规则 S：把「写盘不看 ok()」复现出来（第 120 轮）----
$sCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'S 写盘必须看 ok()' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*if \(!fileWriter\.ok\(\)\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls[$i] = '        ;'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Main.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $sCheck 'app/src/main/jni/Main.cpp'

# ---- 25. 规则 T：把「先 insert 再加根」复现出来（第 122 轮）----
$tCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'T 必须先加根再 insert' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*if \(SaveObjectWithRoot\(currentObj\)\)$') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $j = $i + 2
    $ins = $ls[$j]
    $ls[$j] = $ls[$i]
    $ls[$i] = $ins
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $tCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 26. 规则 U：把「GC 句柄判定被架空」复现出来（第 124 轮）----
$uCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'U 自检要真的判定 GC 句柄' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*if \(Il2cpp::GcHandleApiResolved\(\)\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    # 关键：注入成「调用还在、判定没了」。只查名字出现过的规则会被骗过。
    $ls[$i] = '    if (false && Il2cpp::GcHandleApiResolved())'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/SelfCheck.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $uCheck 'app/src/main/jni/Tool/SelfCheck.cpp'

# ---- 27. 规则 U（第二半）：把「浏览路径判定被架空」复现出来（第 125 轮）----
#
# 上一条只验了 gchandle 那一半，而规则 U 第一版是**或**：
# 只要 gchandle 在位，浏览那半被架空也照样放过。
# 所以这里单独验第二半 —— 两个半必须**各自**成立。
$uCheck2 = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'U 浏览路径判定也要在位' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*if \(Il2cpp::BrowserApiResolved\(\)\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls[$i] = '    if (false && Il2cpp::BrowserApiResolved())'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/SelfCheck.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $uCheck2 'app/src/main/jni/Tool/SelfCheck.cpp'

# ---- 28. 规则 V：把「界面没挡」复现出来（三个要求各自验，第 126 轮）----
#
# 规则 V 第一版把「已转义」的 Pattern 又套了一层 [regex]::Escape，
# 一条都匹配不上而规则报「通过」—— 第 15 次「规则没在跑」。
# 下面是修好之后的三个独立要求：每一个都要能单独红。
$vCheck1 = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
 'app/src/main/jni/Tool/PopUpSelector.cpp'
Test-Rule 'V 枚举下拉要挡' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*if \(!Il2cpp::BrowserApiResolved\(\)\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls[$i] = $ls[$i] -replace 'if \(!Il2cpp::', 'if (false && Il2cpp::'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/PopUpSelector.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $vCheck1 'app/src/main/jni/Tool/PopUpSelector.cpp'

# ---- 29. 门禁扫描范围必须覆盖全部项目源文件（第 131 轮）----
#
# 这是第 90 / 101 / 129 轮同一个形状的第四次：门禁绿着，
# 但有一片代码它从来没看过（这次是 jni 根目录的 OpenGL.h）。
$covAll = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'W 扫描范围要含 jni 根目录的 .h' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'Get-ChildItem \$rootCpp -Filter \*\.h -File') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls[$i] = $ls[$i] -replace '\*\.h', '*.nomatch'
    [IO.File]::WriteAllText((Join-Path $root '.ci/check-ui-patterns.ps1'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($true)))
    return $true
} $covAll '.ci/check-ui-patterns.ps1'

# ---- 30. 规则 X：把「新增的孤儿头文件」复现出来（第 132 轮）----
#
# 这一条不能走 `Test-Rule`：那个函数的注入方式是**改一个已存在的文件**，
# 而规则 X 的违规形态是**多出一个没人 include 的新文件** ——
# 还原要删掉它，`Test-Rule` 的「写回原始字节」管不到新文件。
#
# 所以这里手写，但**按同样的契约**报进 $script:results：
# 注入成功 / 注入后变红 / 还原后变绿，三条都要验。
# 只跑「造个文件看看红不红」是不够的 ——
# 那样的话，规则 X 完全可以一直在报红，而没人发现。
$xCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
$probePath = Join-Path $root 'app/src/main/jni/Tool/ZZOrphan.h'
Write-Host "[自检] 临时造一个孤儿头文件: $probePath"
$injectedX = $false
$rcBadX = 0
$rcGoodX = 0
try {
    [IO.File]::WriteAllText($probePath, "#pragma once`r`nint zzOrphan();`r`n",
        (New-Object Text.UTF8Encoding($false)))
    $injectedX = $true
    $rcBadX = & $xCheck
}
finally {
    Remove-Item $probePath -Force -ErrorAction SilentlyContinue
}
$rcGoodX = & $xCheck
$script:results += [pscustomobject]@{
    Name        = 'X 没人 include 的头文件要报出来'
    Injected    = $injectedX
    RedOnBad    = ($rcBadX -ne 0)
    ExpectRed   = $true
    GreenAfter  = ($rcGoodX -eq 0)
    Ok          = ($injectedX -and ($rcBadX -ne 0) -and ($rcGoodX -eq 0))
}

# ---- 31. 规则 Y：把「交接标志退回 relaxed」复现出来（第 133 轮）----
#
# 还要验**不误报**：Dump().cancelRequested 也是 relaxed，
# 但它只是个「请停下」的单向信号，别的数据不靠它发布 -> 那是**对的**。
# 只验「能红」的门禁会把正确的写法也一起改掉。
$yCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'Y 交接标志不许 relaxed' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match 'done\.store\(true, std::memory_order_release\)') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls[$i] = $ls[$i] -replace 'memory_order_release', 'memory_order_relaxed'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $yCheck 'app/src/main/jni/Tool/ClassesTab.cpp'
Test-Rule 'Y 请求标志 relaxed 不误报' {
    param($t)
    # 往 Tool.cpp 里塞一个**请求**风格的 relaxed 标志。
    # `Dump().cancelRequested` 一直是 relaxed 而没人去改它，就是这一类。
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $ls.Add('void zzCancelProbe(std::atomic<bool> &cancelProbe) {')
    $ls.Add('    cancelProbe.store(true, std::memory_order_relaxed);')
    $ls.Add('}')
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/Tool.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $yCheck 'app/src/main/jni/Tool/Tool.cpp' -ExpectRed $false

# ---- 32. 规则 Z：把「能力判定失败写进缓存」复现出来（第 134 轮）----
#
# 这条规则的第一版**两次都没抓到**，两次都是注入验证抓出来的：
#   1) 见 } 就 break  ->  被嵌套 if 的 } 截断，扫不到目标行
#   2) depth 预设 1 又数了那个 {  -> 深度回不到 0，扫进整个函数（误报）
$zCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'Z 能力判定失败不许写缓存' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $anchor = -1
    $ins = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($anchor -lt 0 -and $ls[$k] -match '^\s*if \(!Il2cpp::BrowserApiResolved\(\)\)') { $anchor = $k; continue }
        if ($anchor -ge 0 -and $ls[$k] -match '^\s*static MethodList empty;\s*$') { $ins = $k; break }
    }
    if ($ins -lt 0) { return $false }
    $ls.Insert($ins + 1, '            methodCache[klass] = empty;')
    $ls[$ins + 2] = '            return methodCache[klass];'
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $zCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 33. 规则 AA：把「早退绕过状态清理」复现出来（第 135 轮）----
$aaCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'AA 早退不许绕过状态清理' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $i = -1
    for ($k = 0; $k -lt $ls.Count; $k++) {
        if ($ls[$k] -match '^\s*auto callback = std::move\(lastCallback\);') { $i = $k; break }
    }
    if ($i -lt 0) { return $false }
    $ls.RemoveAt($i)
    $ls.RemoveAt($i)      # lastCallback = nullptr;
    $ls.RemoveAt($i)      # userData = nullptr;
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/PopUpSelector.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $aaCheck 'app/src/main/jni/Tool/PopUpSelector.cpp'

# ---- 34. 规则 AB：把「记了没人读」复现出来（第 136 轮）----
#
# 注意删的是**最后一个** Report（Draw 里的那个）。
# 删 ClassViewer 里的那个不会变红 —— 因为 Begin 就在同一个函数里，
# 而那一个 Report 每帧读一次，两个 Begin 天然共用它。
# 所以这条规则只能按**函数**统计，不能比全文件总数（第 136 轮踩过）。
$abCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'AB 记了结果必须有地方读' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    $idx = -1
    for ($k = $ls.Count - 1; $k -ge 0; $k--) {
        if ($ls[$k] -match '^\s*g_bulkResult\.Report\(\);\s*$') { $idx = $k; break }
    }
    if ($idx -lt 0) { return $false }
    $ls.RemoveAt($idx)
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $abCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 35. 规则 AC：把「失败原因指向临时缓冲」复现出来（第 137 轮）----
#
# 注意替换的是**局部变量**那一处（`failure = "..."`），
# 不是 `st->failure = failure;` —— 后者转发局部变量，是允许的写法。
# 第一版规则只查 `st->` 前缀，注入验证时立刻发现「规则没在跑」。
$acCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'AC 跨线程消息不许指向临时缓冲' {
    param($t)
    $marker = 'failure = "筛选时发生异常";'
    if ($t.IndexOf($marker) -lt 0) { return $false }
    $out = $t.Replace($marker, 'failure = std::string("筛选时发生异常").c_str();')
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'), $out,
        (New-Object Text.UTF8Encoding($false)))
    return $true
} $acCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 36. 规则 AC 第二面：公开返回值不许退回裸指针（第 138 轮）----
#
# 这是**独立**的一条要求，不能靠第一条顺带抓到：
#   一条管「字段赋值」，一条管「公开 API 的返回类型」。
# 第 111/125 轮那条「每个要求都要能单独红」在这里又用上了。
#
# 注意我第一次写这行时**漏了变量名的 `$`**（写成了 `= {`），
# 于是它变成了给上一行的值重新赋值 —— 而 `$acCheck2` 从来没被定义过，
# 于是 721 行那一句调用的是**别的变量**。
# 报错倒不会立刻出现（PowerShell 对未定义变量当 null），
# 只是这条检查从来没真正跑过 —— 而自检列表里它显示「通过」。
$acCheck2 = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'AC 公开 API 不许返回裸指针' {
    param($t)
    if ($t.IndexOf('std::string GetFilterFailure();') -lt 0) { return $false }
    $out = $t.Replace('std::string GetFilterFailure();', 'const char *GetFilterFailure();')
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.h'), $out,
        (New-Object Text.UTF8Encoding($true)))
    return $true
} $acCheck2 'app/src/main/jni/Tool/ClassesTab.h'

# ---- 37. 规则 AD：把「静默失败」复现出来（第 139 轮）----
#
# 这条规则自己第一版是**完全静默**的：我把捕获列表写成 `^\[\w+\]`，
# 而那两处的真身是 `[this, processingFlag]()` —— 一个都匹配不上。
# 手工注入时立刻发现「删干净了却不红」。
#
# 然后自检这一条也**自己不行**，而且错了两版：
#
# 第一版只删 `Begin` 后面**紧挨着**的那几行。可真实顺序是
#     g_bulkResult.Begin("恢复全部");
#     g_bulkResult.Finish(); // 0 个成功 -> ...
#     LOGE("RestoreAll: 无法创建后台线程，一个方法都没恢复");
# 三行紧挨着，它应该能删干净 —— 但它只删掉了一行就停，
# 因为下一行是注释 `// 0 个成功 -> ...`，而循环上限是 3，
# 删完注释就 `break` 了。Finish 和 LOGE 都还在，
# 「可见痕迹」照样成立，门禁报绿。
#
# 第二版改成「Begin 到 SpawnDetached( 之间整段删」，结果 end=begin+1，
# 只删掉 Begin 一行 —— 因为那个 Begin 是**正常路径**的（工作线程里
# 的那次 Begin），不是失败回调里的。我找错了目标。
#
# 正确做法：**按缩进找回调体**。失败回调的特征是它自己那层大括号
# —— `[this, processingFlag]()` 后面紧跟的 `{`，
# 而工作线程是 `SpawnDetached(` 后面换行才 `{`。
$adCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-ui-patterns.ps1')
}
Test-Rule 'AD 线程没起来必须留痕迹' {
    param($t)
    $ls = [System.Collections.ArrayList](($t -split "`r?`n"))
    # 找失败回调：`[... ]()` 单独一行，紧跟的下一行是 `{`
    $open = -1
    for ($k = 0; $k -lt $ls.Count - 1; $k++) {
        if ($ls[$k].Trim() -notmatch '^\[[\w\s,*&]+\]\s*\(\s*\)\s*$') { continue }
        if ($ls[$k + 1].Trim() -ne '{') { continue }
        $open = $k + 1
        break
    }
    if ($open -lt 0) { return $false }
    # 从 `{` 走到配对的 `}`，整段替换成只复位状态 —— 静默失败的真身。
    $depth = 0
    $close = -1
    for ($k = $open; $k -lt $ls.Count; $k++) {
        $u = $ls[$k]
        if ($u.Trim() -notmatch '^(//|\*|/\*)') {
            $depth += ([regex]::Matches($u, '\{')).Count - ([regex]::Matches($u, '\}')).Count
        }
        if ($depth -le 0 -and $k -gt $open) { $close = $k; break }
    }
    if ($close -lt 0) { return $false }
    $ls.RemoveAt($close)
    for ($k = $close - 1; $k -ge $open; $k--) { $ls.RemoveAt($k) }
    $indent = ([regex]::Match($ls[$open - 1], '^\s*')).Value
    $ls.Insert($open, $indent + '    {')
    $ls.Insert($open + 1, $indent + '        *processingFlag = false;')
    $ls.Insert($open + 2, $indent + '    }')
    [IO.File]::WriteAllText((Join-Path $root 'app/src/main/jni/Tool/ClassesTab.cpp'),
        ($ls -join "`r`n"), (New-Object Text.UTF8Encoding($false)))
    return $true
} $adCheck 'app/src/main/jni/Tool/ClassesTab.cpp'

# ---- 38. CI workflow 结构：重复 job / 孤儿步骤（第 140 轮）----
#
# 两条要求必须**各自**能红，不能只验其中一条：
#   1. jobs 下 key 重复 —— YAML 里后者覆盖前者，旧 job 静默消失
#   2. 步骤里出现多余的 run:/uses: —— 上一项的兄弟节点残留
$wfCheck = {
    Run-Command 'powershell' @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-workflow.ps1')
}
Test-Rule 'wf 重复 job 要报出来' {
    param($t)
    $needle = '  static-analysis:'
    if (-not $t.Contains($needle)) { return $false }
    # 注意注入点：必须在**真正那个** job 之前再插一个完整的，
    # 不能插在它上面那段注释里 —— 那样 `-replace` 会把注释块切断，
    # 后面的注释变成顶层裸文本，注入出来的 YAML 本身就不合法。
    # 我第一次就是这么注入的，检查器没红，我去查检查器才发现是注入错了。
    $inject = "  static-analysis:`n    runs-on: windows-latest`n    timeout-minutes: 30`n    steps:`n      - uses: actions/checkout@v4`n`n  static-analysis:"
    [IO.File]::WriteAllText((Join-Path $root '.github/workflows/ci.yml'),
        $t.Replace($needle, $inject), (New-Object Text.UTF8Encoding($false)))
    return $true
} $wfCheck '.github/workflows/ci.yml'
Test-Rule 'wf 孤儿步骤要报出来' {
    param($t)
    $needle = "        run: .\.ci\check-all.ps1"
    if (-not $t.Contains($needle)) { return $false }
    $out = $t.Replace($needle, $needle + "`n        shell: pwsh`n        run: .\build.ps1")
    [IO.File]::WriteAllText((Join-Path $root '.github/workflows/ci.yml'), $out,
        (New-Object Text.UTF8Encoding($false)))
    return $true
} $wfCheck '.github/workflows/ci.yml'

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
        if ($ls[$k] -match 'RestorePatchedMethod\(method, o\)\)') { $i = $k; break }
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
