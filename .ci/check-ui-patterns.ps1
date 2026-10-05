# 界面的两类「不可见」缺陷检查器。
#
# 背景
#
# 这两类缺陷的性质是：**代码完全正确，编译干净，逻辑也对**，
# 但在真实设备上表现为「按不到」「看不见」「过一会才更新」。
# 静态检查看不见它们，只有真机能发现 —— 而真机验证成本很高。
#
# 所以退一步：**把它们的代码模式变成机械检查**。凡是命中，一律拒绝提交。
# 宁可误报（多花 30 秒），也不要靠「我记得上次犯过」。
#
# 模式 A：SameLine 跟在**长度不受控**的文本后面
#
#     ImGui::TextUnformatted(it->target.c_str());   // target 最多 220 字符
#     ImGui::SameLine();
#     ImGui::TextUnformatted(it->detail.c_str());   // 被顶到屏幕外，看不见
#
# 后面那个才是这一行**真正要看的东西**。所以规则是：
# SameLine 前面那条如果宽度不受限，就是命中。
# 短字面量（"共 5 条:"）后面跟 SameLine 是正常的，不算。
#
# 模式 B：每帧绘制函数里直接用**深拷贝**的返回值
#
#     void DrawUI() {
#         const auto list = Snapshot();   // 512 条 × 2 个 string = 每帧 1024 次堆分配
#
# 逻辑完全正确，规模一大就卡。规则：Draw* 函数体的开头几行里
# 出现「调用返回容器的函数并直接赋值」就命中，除非它走了缓存。

param(
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$dirs = @(
    (Join-Path $root 'app/src/main/jni/Tool'),
    (Join-Path $root 'app/src/main/jni/Menu'),
    (Join-Path $root 'app/src/main/jni/Includes'),
    (Join-Path $root 'app/src/main/jni/Il2cpp')
)

$files = foreach ($d in $dirs) {
    if (Test-Path $d) { Get-ChildItem $d -Recurse -Include *.cpp, *.h -File }
}

# jni **根目录**下的 .cpp 也要扫。
#
# 第 90 轮发现的：这里原本只有三个子目录（Tool / Menu / Includes），
# 于是 `Main.cpp` —— 927 行，整个渲染循环、追踪页、配置读写都在里面 ——
# 对下面每一条规则都是**不可见**的。
#
# 这和第 63 轮「排除模式吃掉所有语句」、第 85 轮「markdown 规则根本不存在」
# 是同一类失败：门禁绿着，但有一大片代码它从来没看过。
# 区别只在于那次是我写完就发现了，这次是**反向验证抓到的** ——
# 注入的缺陷正好在被漏掉的那个文件里。
$rootCpp = Join-Path $root 'app/src/main/jni'
$files += Get-ChildItem $rootCpp -Filter *.cpp -File
# 根目录下的 **.h** 也要扫（第 131 轮）。
#
# 上面那行只取 `*.cpp`，于是根目录的 `OpenGL.h`（243 行，ESP / wallhack
# 那套着色器和开关都在里面）对下面每一条规则都是**不可见**的。
# 同一个根目录、同一个理由，只补一半 —— 这就是「补了 A 路径没补 B 路径」
# （第 58 轮）换了个方向又出现一次。
$files += Get-ChildItem $rootCpp -Filter *.h -File

# ---- 受检范围必须**覆盖全部**项目源文件（第 131 轮）----
#
# `$dirs` 是手写的，所以它一定会漂移：新建一个目录、或在 jni 根目录放一个
# 文件而忘了改这里，那一片代码就静默地不被检查了 ——
# 而门禁照样全绿。
#
# 第 90/101/129 轮都是同一类：漏掉的是**一整个目录 / 整个最大文件**，
# 三次都是靠「反向验证注入的缺陷正好在被漏掉的文件里」才发现的。
# 那说明它不能靠人记得，所以这里直接对着磁盘核一遍。
$skipPattern = 'imgui|asmjit|Frida|Dobby|xdl|nlohmann|KittyMemory'
$ownSource = Get-ChildItem (Join-Path $root 'app/src/main/jni') -Recurse -Include *.cpp, *.h -File |
    Where-Object { $_.FullName -notmatch $skipPattern } |
    ForEach-Object { $_.FullName.ToLower() }
$scanned = $files | ForEach-Object { $_.FullName.ToLower() }
$unscanned = $ownSource | Where-Object { $scanned -notcontains $_ }
if ($unscanned) {
    Write-Host '门禁扫描范围不完整：这些项目源文件没被任何规则看到：'
    $unscanned | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    $script:gateCoverageFailed = $true
}

# ---- 没人 include 的头文件 = 死代码（第 132 轮）----
#
# 一个 `.h` 如果**没有任何**文件 include 它，那么：
#   · 它从来没被编译过 —— 里面的错误、笔误、过期 API 一次都没被检查
#   · 它不在 clang-tidy 的受检范围里（那边只列 .cpp）
#   · 但它**看起来**是代码，所以读代码的人会以为它在起作用
#
# 本项目有 3 个这样的文件，合计 **14309 行**：
#   Includes/Roboto-Regular.h   14026 行  旧的字体资源；现役字体是 OPPOSans
#   OpenGL.h                       243 行  被 ObjectDrawManager 取代的旧 ESP 实现
#   Menu/get_device_api_level_inlines.h  40 行  NDK 头文件的内联片段
#
# 其中 `OpenGL.h` 是第 131 轮刚发现的：它连「有没有人 include」这件事
# 都没人查过，所以它那 243 行 wallhack / 彩虹着色器代码，
# 在 130 轮里**一次都没被看过**。
#
# 这里不删它们（删不删是产品决定，不是门禁该做的），
# 但要**记下来**并且挡住新增 —— 否则「没人看」这件事永远没人知道。
$knownDeadHeaders = @('Roboto-Regular.h', 'OpenGL.h', 'get_device_api_level_inlines.h')
$ownHeaders = $ownSource | Where-Object { $_ -like '*.h' } | ForEach-Object { Split-Path $_ -Leaf }
$allIncludeText = ($files | ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join "`n"
$deadHeaders = $ownHeaders | Where-Object {
    $name = $_
    $knownDeadHeaders -notcontains $name -and
    $allIncludeText -notmatch ('#\s*include\s*[<"][^>"]*' + [regex]::Escape($name) + '[>"]')
}

$hits = @()

if ($deadHeaders) {
    $hits += [pscustomobject]@{
        File = '(死代码)'
        Line = 0
        Rule = 'X: 这个头文件**没有任何文件 include** —— 它从没被编译过，也不在 clang-tidy 范围内。删掉，或确认它确实是现役的'
        Text = ($deadHeaders -join ', ')
    }
}

foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^\s*(//|\*|/\*)') { continue }

        # ---- 模式 A ----
        if ($t -match '^ImGui::SameLine\(\)') {
            # 往上找最近一条非空、非注释的语句
            for ($k = $i - 1; $k -ge [Math]::Max(0, $i - 3); $k--) {
                $p = $lines[$k].Trim()
                if (-not $p -or $p -match '^\s*(//|\*|/\*)') { continue }
                if ($p -match '^ImGui::(Text|TextUnformatted|TextDisabled|TextColored|TextWrapped|Button|SmallButton)\(') {
                    # 参数里含变量（.c_str() / 变量名）=> 宽度不受限
                    if ($p -match '\.c_str\(\)|%\s*[a-z_][a-zA-Z0-9_]*|,\s*[a-z_][a-zA-Z0-9_]*\)') {
                        $hits += [pscustomobject]@{
                            File = $f.Name
                            Line = $i + 1
                            Rule = 'A: SameLine 跟在宽度不受限的文本后'
                            Text = "$($lines[$k].Trim())  ||  $t"
                        }
                    }
                    break
                }
                break
            }
        }

        # ---- 模式 B ----
        # Draw* 函数体的前 6 行里出现「调用后直接赋值」，且不是走缓存
        if ($t -match '^(auto|std::vector<[^>]+>|const auto)\s+\w+\s*=\s*\w+::(Snapshot|Get|List|Entries)\w*\(' -and
            $t -notmatch 'cached') {
            # 往上找函数头
            for ($k = $i - 1; $k -ge [Math]::Max(0, $i - 8); $k--) {
                if ($lines[$k] -match '^\s*void\s+\w*Draw\w*\s*\(') {
                    $hits += [pscustomobject]@{
                        File = $f.Name
                        Line = $i + 1
                        Rule = 'B: 绘制函数里直接用深拷贝返回值'
                        Text = $t
                    }
                    break
                }
            }
        }
    }
}

# ---- 模式 A2：**相邻的重复语句** ----
#
# 第 64 轮发现：保存对象的记录行被插入了**两次**（第 57 轮按行号插入时
# 跑了两遍），于是每保存一次对象，改动记录里就出现**两条一模一样的条目**。
#
# 这一页的全部意义就是「如实记录我做了什么」，出现重复条目等于它在说谎。
#
# 规则：相邻两行去掉缩进后完全相同、长度 > 20、且不是注释/收尾符。
# 重复的 `}`、`else {`、空行都是正常写法，所以排除掉。
# 误报风险低，而且这类重复**本来就该有人看一眼**。
# 只扫 .cpp：字体二进制头（Roboto-Regular.h）里全是重复的十六进制行，
# 那不是「重复插入」，是**数据**。这条规则对数据文件是纯噪音。
foreach ($f in ($files | Where-Object { $_.Extension -eq '.cpp' })) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 1; $i -lt $lines.Count; $i++) {
        $a = ($lines[$i - 1] -replace '^\s+', '')
        $b = ($lines[$i] -replace '^\s+', '')
        if ($a.Length -le 20) { continue }
        if ($a -ne $b) { continue }
        # 排除项必须**窄**。第一版写的是 `^(//|\*|\}|\)|\{$|else\b.*\{|.*;?\s*$)` ——
        # 其中 `.*;?\s*$` 匹配**任意以分号结尾的行**，等于把所有语句都排除了，
        # 这条规则从写下来的那一刻起就是死的（而且它「通过」着）。
        # 教训和第 63 轮那条 `$host` 一样：检查自己必须先反向验证过。
        if ($a -match '^(//|\*|\})') { continue }
        if ($a -match '^\)') { continue }
        if ($a -eq '{' -or $a -eq '}') { continue }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'A2: 相邻两行完全相同（很可能是重复插入）'
            Text = $a
        }
    }
}

# ---- 模式 B2：走路径写字段的函数**必须自己**做值类型写回 ----
#
# 第 67 轮抓到：冻结（新增功能）按路径写字段时，把值写进了**装箱副本** ——
# 路径中间经过 struct 的话，游戏里的真实字段一点没变。而写入「成功」了，
# 不报错，界面照常显示，只有值立不住。
#
# 修的时候我又犯了一版：让**调用方**判断要不要写回。于是「写」和「写回」
# 分在两个函数里 —— 谁重构了其中一个，都会**静默**弄坏嵌套 struct。
#
# 规则：任何按 paths 逐段走并写字段的函数（WriteWatchValue），
# 必须在**自己体内**出现 ensureIfValueType。写到别处去不算数。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    $joined = ($lines -join "`n")
    if ($joined -notmatch 'static bool WriteWatchValue') { continue }
    $start = -1
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch '^static bool WriteWatchValue') { continue }
        # 跳过**前向声明**，只认定义。签名可能跨多行，所以要一直往下找到
        # 「以 { 开头」（定义）或「以 ; 结尾」（声明）为止 ——
        # 只看紧邻的下一行的话，多行签名会被当成声明，**整段规则不执行**。
        # （第 68 轮在这上面卡了三轮：先是匹配到声明，再是匹配到注释。）
        $n = $i
        while ($n -lt $lines.Count) {
            $trimmed = $lines[$n].Trim()
            if ($trimmed -eq '') { $n++; continue }
            if ($trimmed.StartsWith('{')) { break }
            if ($trimmed.EndsWith(';')) { break }
            $n++
        }
        if ($n -ge $lines.Count) { continue }
        if (-not $lines[$n].Trim().StartsWith('{')) { continue }  # 那是声明
        $start = $i
        break
    }
    if ($start -lt 0) { continue }
    $body = @()
    for ($i = $start; $i -lt $lines.Count; $i++) {
        $body += $lines[$i]
        if ($lines[$i] -eq '}') { break }
    }
    # **必须先剥掉注释**再匹配。
    # 函数里那段解释「为什么要写回」的注释本身就出现了 ensureIfValueType
    # 这几个字 —— 第 68 轮第一次写这条规则时，就是被自己的注释骗过去的：
    # 把真正的调用删掉，检查依然是绿的。
    # 检查项必须匹配**代码**，不是文本。
    $codeOnly = ($body | Where-Object { $_.Trim() -notmatch '^(//|\*|/\*)' }) -join "`n"
    if ($codeOnly -notmatch 'ensureIfValueType') {
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $start + 1
            Rule = 'B2: 按路径写字段却没有在函数内做值类型写回（嵌套 struct 会静默失效）'
            Text = $lines[$start].Trim()
        }
    }
}

# 曾经试过一条「成员指针默认 = nullptr 却在别处裸解引用」的检查（规则 D），
# **已删除**。
#
# 删的理由和第 68 轮一样：**误报太多就会被人调低、然后删掉**，那比没有更糟。
# 收窄过三轮：
#   1. 只看裸 name->（排除 st->name->）→ 33 处 → 15 处
#   2. 排除 Frida 目录 + 往上两行判空也算 → 仍是 15 处
# 剩下的全是**函数形参和类成员同名**造成的假阳性
# （`ToggleHooker(MethodInfo *method, ...)` 里的 method 撞上某个类的
#  `method = nullptr` 成员）。靠名字区分不开，要区分就得有类型信息。
#
# 而它本来要抓的那个**真 bug**（关掉全部标签页 → 空指针解引用 →
# 用户的游戏进程崩溃）已经在第 70 轮修掉了，并且改法是
# 「值拿不准就别解引用」—— 这条原则不依赖任何检查也能成立。
# ---- 模式 D：ImGui 会**原样显示** `**`，它不解析 markdown ----
#
# ImGui 的 Text 系列只是把字符串丢进字库，没有任何 markdown 处理。
# 所以中文文案里习惯性写的 `**重点**` 会变成界面上 literally 的星号：
#
#     无法写入该目录 —— **参数预设和配置都不会被保存**
#                                        ^^^^^^^^^^ 会原样显示
#
# 这个缺陷在第 44/49 轮犯过两次，第 **84 轮又犯了一次**
#（新加的自检项里写了 `**`，而当时我以为已经有门禁 ——
#  实际上并没有，只有一句注释提醒）。
#
# 「我记得上次犯过」不是门禁。犯过三次的东西必须有机械检查。
#
# 排除项：
#   注释        —— 不是渲染文本
#   LOGx(...)   —— 日志是纯文本，`**` 在那里没问题
#   第三方目录  —— imgui / asmjit 自己就有 `**DebugBreak**`
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -match 'LOG[DEIWE]\(') { continue }
        if ($t -notmatch '"[^"]*\*\*[^"]*"') { continue }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'D: 渲染文本里的 ** 会原样显示（ImGui 不解析 markdown）'
            Text = $t
        }
    }
}

# ---- 模式 E：改游戏状态的调用**不能**被挂在「面板可不可见」上 ----
#
# 同一个缺陷在两轮里各犯了一次：
#   第 82 轮  DrawUI() 在 BeginTabItem("对象绘制") 里，
#             却把 drawObjects **搬空**了 —— 同帧的 Tick()/DrawAll() 看到空列表，
#             于是「打开那一页 → ESP 全停」。
#   第 83 轮  冻结的每帧写回循环在 DrawWatches() 里，
#             而 DrawWatches() 只在 CollapsingHeader("关注值") **展开**时被调用 ——
#             于是「折叠那一栏 → 冻结全停」。
#
# 两次都**不报错**，按钮还显示着正确状态，只是那个东西不工作了。
#
# 原则：**作用在游戏状态上的东西，不该挂在界面上。**
# 界面是按需渲染的，游戏状态不是。
#
# 规则：改游戏状态的调用，它的**直接父条件**如果是 BeginTabItem /
# CollapsingHeader，就是缺陷。父条件是 Button / Checkbox / MenuItem
# 这些「用户主动触发」的没问题 —— 那些本来就该在点了之后才执行。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # 前视断言里**不能**排除 :: —— ClassesTab::ApplyFreezes() 和
        # Tool::ToggleHooker(...) 全都是限定名调用，排除掉就等于这条规则
        # 只认裸调用，而真实代码里几乎全是限定名。第 87 轮自己踩了一次。
        $isMutator = $t -match '(?<![\w.>])(SetFieldValue|setField|ParseAndSetNumericField|WriteWatchValue|ApplyFreezes|ToggleHooker|DobbyInstrument|DobbyDestroy|NewHandle|FreeHandle|SelectObject|RemoveDrawObject|ClearAllDrawObjects|RestorePatchedMethod|ProcessScannedObjects)\s*\('
        if (-not $isMutator) { continue }
        # 函数**定义**那一行会以 { 结尾（不是调用），要排除 ——
        # 不然 ool ToggleHooker(MethodInfo*, int) { 会被当成调用点。
        if ($t -match '\{\s*$') { continue }
        if ($t -match 'static\s+(void|bool|Il2CppObject)') { continue }
        $ind = $lines[$i] -replace '^(\s*).*', '$1'
        $indLen = $ind.Length
        # 往上找**直接父条件**：第一个缩进比它浅、且是 if/else if 的行
        for ($k = $i - 1; $k -ge 0; $k--) {
            $u = $lines[$k].Trim()
            if ($u -eq '' -or $u -match '^(//|\*|/\*)') { continue }
            $uInd = ($lines[$k] -replace '^(\s*).*', '$1').Length
            if ($uInd -ge $indLen) { continue }
            if ($u -match '^\}\s*else if\b' -or $u -match '^else if\b' -or $u -match '^if\s*\(') {
                if ($u -match 'ImGui::(BeginTabItem|CollapsingHeader)\(') {
                    $hits += [pscustomobject]@{
                        File = $f.Name
                        Line = $i + 1
                        Rule = 'E: 改游戏状态的调用被挂在面板可见性上（折叠/切页即失效，且不报错）'
                        Text = $t
                    }
                }
                break
            }
        }
    }
}

# ---- 模式 F：失败之后无条件清掉**恢复要用的那份数据** ----
#
# 第 80 轮的真实 bug：
#
#     if (!RestorePatchedMethod(method, o.bytes)) { LOGE("恢复失败"); }
#     o.bytes.clear();          // ← 无条件
#     o.text.clear();
#
# 恢复失败（mprotect 失败等）之后：补丁**还在内存里生效**，
# 而**原字节已经被清掉了** —— 退不回去、也不能重试；
# `patched = !o.bytes.empty()` 变成 false，界面上还显示「没打补丁」。
#
# 之所以能做这条规则，是因为这个形状足够窄：
# **条件里那个调用的实参，在同一个 if 之后被无条件清掉**。
# `o.bytes` 既是「恢复函数的输入」，又是「失败后被销毁的东西」——
# 这一条就足以判定，不需要理解业务。
#
# 故意收窄：
#   - 只看实参，不看「附近有什么」（那会误报一堆合法的缓存清理）
#   - 条件里必须有 `else` 才放过（写成 else 就说明作者想过这件事）
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -notmatch '^if\s*\(!?\s*[\w:.]+\(') { continue }
        $m = [regex]::Match($t, '\(([^)]*)\)')
        if (-not $m.Success) { continue }
        $args = @($m.Groups[1].Value -split ',' |
                  ForEach-Object { $_.Trim() } |
                  Where-Object { $_ -match '^[\w\.\->]+$' })
        if ($args.Count -eq 0) { continue }
        $ind = ($lines[$i] -replace '^(\s*).*', '$1').Length
        $j = $i
        while ($j -lt $lines.Count) {
            if (($lines[$j] -replace '^(\s*).*', '$1').Length -eq $ind -and $lines[$j].Trim() -eq '}') { break }
            $j++
        }
        if ($j -ge $lines.Count) { continue }
        # 有 else = 作者考虑过失败路径，放过
        for ($k = $j + 1; $k -le [Math]::Min($j + 2, $lines.Count - 1); $k++) {
            if (($lines[$k] -replace '^(\s*).*', '$1').Length -eq $ind -and $lines[$k].Trim() -match '^else\b') {
                $j = -1
                break
            }
        }
        if ($j -lt 0) { continue }
        for ($k = $j + 1; $k -le [Math]::Min($j + 4, $lines.Count - 1); $k++) {
            $u = $lines[$k].Trim()
            if (($lines[$k] -replace '^(\s*).*', '$1').Length -ne $ind) { continue }
            foreach ($a in $args) {
                $esc = [regex]::Escape($a)
                if ($u -match "^$esc(\.clear\(\)|\.erase\(| = 0;| = nullptr;)") {
                    $hits += [pscustomobject]@{
                        File = $f.Name
                        Line = $k + 1
                        Rule = 'F: 恢复函数失败后无条件清掉了它的实参（原数据没了，退不回去）'
                        Text = $u
                    }
                }
            }
        }
    }
}

# ---- 模式 G：所有文件写入都必须走 FileWriter（原子写在它里面）----
#
# 第 89 轮发现：导出 .cs 有原子写（`.part` + rename），class_tabs.json 没有；
# 第 90 轮发现 tool_conf.json 也没有。**同一个项目里三份持久化、三套纪律。**
#
# 修法不是「给每个调用点各写一遍原子写」—— 两份实现迟早会不一致
# （第 89 轮就手写了一份，第 90 轮发现还得再来一份）。而是把原子写放进
# **FileWriter 本身**：那是所有写入必经的入口。
#
# 所以要拦的是：绕过 FileWriter 直接 ofstream/fopen 写正式文件。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # Util.cpp 里的就是实现本身；dump 走的是自己的进度/取消通道
        if ($f.Name -eq 'Util.cpp') { continue }
        if ($f.Name -eq 'Il2cpp.cpp') { continue }
        # 标识符用 [^\s(]+ 而不是 \w —— C++ 完全允许 UTF-8 标识符
        # （写个「写文件」当变量名是合法的），而 \w 在某些编码下
        # 匹配不到非 ASCII 字母，会让这条规则**静默漏掉**它们。
        # 第 90 轮 CI 就是在这一点上红过一次。
        #
        # 另一个坑（本轮自己踩的）：`(o|f)stream` **匹配不到 ofstream** ——
        # ofstream 是 o+f+stream，两个都试一遍也对不上。
        # 「让规则更通用」的小改动，反而让它彻底静默失效 ——
        # 和第 63 轮那次是同一个错误形状。
        if ($t -match '^\s*std::(of|o|f|i)stream\s+[^\s(]+\s*\(') {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'G: 绕过 FileWriter 直接写文件（原子写在 FileWriter 里，绕过去就没有）'
                Text = $t
            }
        }
    }
}

# ---- 模式 H：自检项**不许有副作用**，尤其不许改文件系统 ----
#
# 第 94 轮：我自己在第 84 轮加的「数据目录可写」探针，每 2 秒
# （Collect() 的刷新周期）在游戏的数据目录里建一个文件、写入、删掉。
# 别的自检项都是只读的，只有这一条在**改文件系统** —— 而且
# 目录可不可写是个**几乎不变**的属性，没有任何理由反复探测。
#
# 形状很好认：自检项里出现 fopen / remove / rename / copy_file，
# 而它本来应该只是「读状态、给结论」。
#
# 排除：Util.cpp（那是 FileWriter 的实现本身）。
# dump 的临时文件在 Il2cpp.cpp（它有自己的进度/取消通道）。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    if ($f.Name -ne 'SelfCheck.cpp') { continue }
    # 允许**一次性的**探测：`static const bool x = []() { ... }();`
    # 这种写法整个进程只执行一次，是「探一下就够」的正确形状。
    # 真正要拦的是：写在**每 2 秒跑一次**的采集路径里的副作用。
    $inOnceInit = $false
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -match '^static\s+.*=\s*\[\]') { $inOnceInit = $true }
        elseif ($t -match '^\}\(\);') { $inOnceInit = $false }
        if ($inOnceInit) { continue }
        if ($t -match '\b(fopen|remove|rename|copy_file|unlink|mkdir|system|popen)\s*\(') {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'H: 自检项有副作用（自检应该只读状态并给结论，不该改文件系统）'
                Text = $t
            }
        }
    }
}

# ---- 模式 I：加根失败之后**不许**把对象指针存下来 ----
#
# 第 97 轮在 Keyboard.cpp 里发现的：
#
#     openedKeyboardHandle = GC::NewHandle(kb);
#     if (openedKeyboardHandle == 0) { LOGW("...可能读到已回收对象"); }
#     openedKeyboard = kb;          // ← 明知没有根，还是存下来了
#
# 后果不是「读到脏数据」，是**崩游戏**：
# updateImpl() 每帧解引用 openedKeyboard（get_status / get_text），
# 而它跑在渲染线程的 eglSwapBuffers 钩子里。没有 GC 根的对象
# 随时可能被回收 —— 解引用它 = use-after-free。
#
# 旧代码那句 LOGW 其实**已经知道**这件事了，但没有改变行为。
# 这就是「失败路径没有出口」：它记录了失败，然后继续往下走。
#
# 形状：NewHandle 的结果被判过、并且失败分支只是记日志，
# 紧接着却把同一个对象赋给一个「会被解引用」的全局/成员。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # 找到 `<var>Handle = ...NewHandle(<obj>)`
        $m = [regex]::Match($t, '^\s*(?:[\w:]+::)?(\w*[Hh]andle)\s*=\s*[\w:]*NewHandle\(\s*(\w+)\s*\)')
        if (-not $m.Success) { continue }
        $handleVar = $m.Groups[1].Value
        $obj = $m.Groups[2].Value
        # 往后 8 行：必须有一个 `return`（或 throw）在赋值之前
        # 窗口要够宽 —— 失败分支里通常有一大段说明加清理调用；
        # 第一次写 8 行，而赋值在第 28 行，规则**从来没被验证过**。
        for ($k = $i + 1; $k -le [Math]::Min($i + 30, $lines.Count - 1); $k++) {
            $u = $lines[$k].Trim()
            if ($u -match '^(//|\*|/\*)') { continue }
            # 赋值给了另一个变量（不是同一个 obj）
            if ($u -match "^\s*(?:[\w:]+::)?(\w+)\s*=\s*$obj\s*;") {
                $dest = $Matches[1]
                if ($dest -eq $obj) { continue }
                # 赋值之前出现过 return 就算有出口
                $before = $lines[$i..($k-1)] -join "`n"
                $hasExit = $before -match '\breturn\b'
                if (-not $hasExit) {
                    $hits += [pscustomobject]@{
                        File = $f.Name
                        Line = $k + 1
                        Rule = 'I: 加根失败后仍把对象存下来（无 GC 根的对象会被解引用 = 崩游戏）'
                        Text = $u
                    }
                }
                break
            }
        }
    }
}

# ---- 模式 J：解引用托管对象之前**必须**走句柄，不能退回裸指针 ----
#
# 第 98 轮在 ObjectDrawManager 里发现的：
#
#     static Il2CppObject* ResolveGameObject(const GameObjectInfo& info) {
#         if (info.gameObjectHandle) return GetHandleTarget(info.gameObjectHandle);
#         return info.gameObject;      // ← 加根失败时退回「没有根的裸指针」
#     }
#
# 而 RootGameObject() 在加根失败时**只打一行 LOGW 就继续**，
# 于是这个分支实际会发生：
#
#     加根失败 → 句柄 0 → Resolve 返回裸指针
#            → DrawAll 每帧 transform->invoke_method(g_GetPosition)
#            → 对一个随时会被 GC 回收的对象解引用 = 崩游戏
#
# 讽刺的是这个函数**上面的注释写的正好是反过来的**：
#     「对象已回收时返回 nullptr —— 此时绝不能去解引用 info.gameObject 那个旧地址」
#
# 形状：函数体里 `return <裸指针成员>;`（而不是 nullptr），
# 且该成员是某个 Il2CppObject* / Transform* 类型的裸指针。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # 只看形如 `return info.<字段>;` 的裸退回
        # 两种形状都要覆盖（第 99 轮的教训）：
        #   `return info.gameObject;`  —— 成员访问（ObjectDrawManager）
        #   `return obj;`              —— 裸参数  （ClassesTab::ResolveSaved）
        # 第一版只写了前一种，于是 ResolveSaved **从门禁底下溜过去了**，
        # 而它和前一个是**完全同一个 bug**。
        # 三种形状（第 101 轮补上第三种）：
        #   `return info.gameObject;`                       成员访问
        #   `return obj;`                                   裸参数
        #   `h ? GetHandleTarget(h) : m_objects[i];`       三元表达式
        #
        # 第三种是 `liveObjects()` 里的，规则 J 第一版**没抓到它** ——
        # 而它和前两个是完全同一个 bug。所以每加一种形状都要重新想一遍：
        # 「这个 bug 还可能长成什么别的样子？」
        if ($t -notmatch '^return\s+(\w+\.(gameObject|transform|gameObjectHandle|transformHandle)|obj|object)\s*;') {
            # else 分支必须**不是** nullptr —— `: nullptr` 恰恰是**正确**的写法
            #（ClassesTab 里有两处 `handle ? GetHandleTarget(handle) : nullptr`），
            # 第一版没收紧，把这两处也报出来了。
            # 而且第一版的否定预查还踩了一个**回溯**坑：`:` 后面的 `\s*`
            # 可以把空格吐回去，于是预查看到的是 " nullptr" 而不是 nullptr，
            # 照样成立。**预查必须放在 `\s*` 前面**。
            if ($t -notmatch 'GetHandleTarget\([^)]*\)\s*:(?!\s*nullptr)\s*[\w\.\[\]]') { continue }
        }
        # 必须紧跟着「if (handle) return GetHandleTarget(...)」这种形状才值得报
        # GetHandleTarget 可能在**前面**也可能在**后面**：
        #   `if (handle) return GetHandleTarget(...); return obj;`   （前）
        #   `if (!handle) return obj; return GetHandleTarget(...);` （后）
        # 第 99 轮第一版只往前看，于是后一种形状又溜过去了。
        $from = [Math]::Max(0, $i - 8)
        $to = [Math]::Min($i + 8, $lines.Count - 1)
        $ctx = $lines[$from..$to] -join "`n"
        if ($ctx -notmatch 'GetHandleTarget') { continue }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'J: 加根失败时退回裸指针（该对象可能被 GC 回收，调用方会解引用它）'
            Text = $t
        }
    }
}

# ---- 模式 L：不可恢复的条目**不许**只用一个看不见的占位 ----
#
# 第 104 轮：改动记录页里，一条「已经恢复过」的记录和一条
# 「还能恢复」的记录，渲染出来**完全一样** ——
# 区别只是最后那格的按钮换成了一个 ImGui::Dummy。
#
# 于是：
#   · 逐条点「恢复」，那一行在界面上没有任何变化
#   · 点「全部恢复」，整个列表看起来一模一样
#
# 而用户要回答的问题是「现在游戏里还挂着哪些改动」。
# 列表长一个样，这个问题就没法回答。
#
# 形状：`if (可恢复) { 画按钮 } else { ImGui::Dummy(...) }` ——
# else 分支只画一个不可见的占位，等于「这一行没有任何状态」。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -notmatch '^ImGui::Dummy\(') { continue }
        # else 分支里只有 Dummy：往后 4 行里没有任何可见控件
        $visible = $false
        for ($k = $i + 1; $k -le [Math]::Min($i + 4, $lines.Count - 1); $k++) {
            $u = $lines[$k].Trim()
            if ($u -match '^(\}|\{)$') { break }
            if ($u -match 'ImGui::(Text|Button|SmallButton|Checkbox|MenuItem|TextDisabled|TextColored|Image|Separator)\w*\s*\(') {
                $visible = $true
                break
            }
        }
        if (-not $visible) {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'L: 不可恢复的条目只画了不可见占位（用户看不出这一行的状态）'
                Text = $t
            }
        }
    }
}

# ---- 模式 N：钩子替换函数（Dobby / REPLACE 家族）必须**整个函数体**都在异常边界里 ----
#
# 第 108 轮的发现，三处：
#
#   Menu/ImGui.cpp   swapbuffers_hook  的 try **只包住了 menuAddress()**，
#                    而 setupMenu / ImGui::NewFrame / ImGui::Render /
#                    ImGui_ImplOpenGL3_RenderDrawData / PublishMenuRect
#                    全在边界**之外**。
#   Tool/ClassesTab.cpp  hookerHandler  **一个 try 都没有**，而
#                    `trace.name = buffer` 是 std::string 赋值。
#
# 而 Menu/ImGui.cpp 里那段注释自己就写着「栈上没有有效的 unwind info
# （Dobby 的 trampoline 是手写汇编），异常一路逃出去就是
# std::terminate —— 用户的游戏直接没了」，还点名了
# 「std::string/vector 的 bad_alloc」。
# 也就是：**理由写在那里，边界没覆盖到**。
#
# 为什么不做成「函数名里带 hook 字样」：
#   那样会误报 HookerView / SampleHookRates / ToggleHooker /
#   HookInput / UninstallInputHooks —— 五个都不是 Dobby 回调
#   （PowerShell 的 -match 还是大小写不敏感的，HookerView 会被
#    `hook` 匹配上）。实测 5 个误报 : 0 个真报。
#
# 正确的做法是从**注册点**拿名单：
#   DobbyHook(目标, 替换, 原函数)
#   DobbyInstrument(目标, 回调, 原函数)
# 第二个参数就是被 Dobby 装上去的替换函数，全项目就这几个，
# 名单是**精确**的，不靠命名猜。
$hookNames = @{}
# 只在 .cpp 里找注册点。头文件里全是假的：
#   dobby.h        DobbyHook / DobbyInstrument **自己的声明**
#   il2cpp-class.h 模板 DobbyHook(methodPointer, (void *)func, ...)
#   OpenGL.h       reinterpret_cast 转发的十几个 GL 钩子
# 那三个来源实测把名单撑成 6 个，其中 5 个报的是无关函数。
# 同一族的另外两个模板：进托管代码的入口。
#
# `MethodInfo::invoke` / `invoke_static` 是**唯一**几处「从 C++ 跳进托管」的地方，
# 而托管抛的异常会被 il2cpp 翻成 C++ 异常。第 110 轮发现
# `get_touchCount` 里的 `invoke_static_method<UnityEngine_Touch>("GetTouch", 0)`
# 没有任何边界 —— 手指在「数出个数」和「取第 0 个」之间收回去就会抛，
# 那个栈上是**游戏的 get_touchCount**，逃出去 = 整局游戏崩掉。
#
# 只包住其中两个重载没用：第三个调用点照样能崩。所以这里要求
# **每个重载各自都有** try。
$invFamily = @('invoke_static', 'invoke')
$castNames = @('reinterpret_cast', 'const_cast', 'static_cast', 'dynamic_cast')
foreach ($f in $files) {
    if ($f.Extension -ne '.cpp') { continue }
    $txt = Get-Content -Encoding UTF8 $f.FullName -Raw
    # Dobby 家族
    foreach ($m in [regex]::Matches($txt, 'Dobby(?:Hook|Instrument)\s*\([^,]*,\s*(?:\(\s*void\s*\*\s*\)|\(\s*[\w:]*callback\w*\s*\))?\s*&?\s*(\w+)')) {
        $n = $m.Groups[1].Value
        if ($castNames -contains $n) { continue }
        $hookNames[$n] = $true
    }
    # KittyMemory 的行内替换家族（第 110 轮补上）
    #
    # REPLACE_NAME_ORIG("类名", "方法名", 替换函数, 原函数)
    # REPLACE("类名", "方法名", 替换函数)
    #
    # 这两个和 DobbyHook 是**同一类东西**：都把游戏自己的方法换成我们的函数，
    # 都在游戏的执行路径上跑。它装的是 Input.get_touchCount 和
    # Input.GetMouseButton —— 游戏每帧各调一次，是全项目调用最频繁的
    # 两个 hook。第 108 轮只按 Dobby 取名单，正好漏掉了它们。
    foreach ($m in [regex]::Matches($txt, 'REPLACE(?:_NAME)?(?:_ORIG)?\s*\([^,]+,[^,]+,\s*&?\s*(\w+)')) {
        $hookNames[$m.Groups[1].Value] = $true
    }
}
# ---- 模式 O：日志函数**不许往外抛异常** ----
#
# 第 112 轮。LOGW/LOGE/LOGI 全部展开成 `logger::AddLog(...)`，而 AddLog 里是
#
#     new char[modifiedFmtLength + 2];      // bad_alloc
#     Buf.appendfv(...)                     // 内部要分配
#     LineOffsets.push_back(...)            // bad_alloc
#     TrimLocked() 里的 std::string tail    // bad_alloc
#
# 四处都会抛。而 AddLog 是从**哪里**被调的？
#
#   · hook 回调里（hookerHandler / get_touchCount / swapbuffers_hook）
#   · 第 110/111 轮我刚加的那些 **catch 块里面**
#
# 在 catch 里抛 = **边展开边抛 = std::terminate**。
# 也就是说：为了让「异常不弄崩游戏」而加的 catch，如果自己打日志时
# 内存不够，会**比不加 catch 更糟** —— 直接 terminate。
#
# 一个日志函数必须做到：记不下来就算了，绝不能把调用方带崩。
# 所以本规则要求 Includes/Logger.cpp 里每个函数体都有边界。
foreach ($f in $files) {
    if ($f.Name -ne 'Logger.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # 命名空间内的函数定义（缩进 4、空格、无模板、无分号）。
        #
        # 缩进要拿**没 trim 的那一行**去匹配：先把行 trim 掉再要求 `^ {4}`
        # 永远不可能成立 —— 第一版就是这么写的，于是一条都没匹配上。
        if ($lines[$i] -notmatch '^ {4}\S' ) { continue }
        if ($t -notmatch '^(const\s+)?[\w:]+[\w\s:<>,\*&]*\s\**(\w+)\s*\([^;]*$') { continue }
        # `{` 常常在**下一行**（这个项目里大多数函数定义都是这样写的）。
        # 第一版只认同行，结果一条都没匹配上 —— 又一条「显示为通过的假检查」。
        $openAt = $i
        if ($t -notmatch '\{\s*$') {
            if (($i + 1) -lt $lines.Count -and $lines[$i + 1].Trim() -eq '{') { $openAt = $i }
            else { continue }
        }
        $ind = ($lines[$openAt] -replace '^(\s*).*', '$1').Length
        $body = @()
        for ($m = $openAt; $m -lt $lines.Count; $m++) {
            $body += $lines[$m]
            if ($lines[$m].Trim() -eq '}' -and ($lines[$m] -replace '^(\s*).*', '$1').Length -eq $ind) { break }
        }
        $bt = $body -join "`n"
        if ($bt -notmatch '\btry\s*\{') {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'O: 日志函数可能往外抛（它是在 catch 里被调的，抛出去就是 terminate）'
                Text = $t
            }
        }
    }
}
foreach ($name in $invFamily) {
    foreach ($f in $files) {
        if ($f.Extension -ne '.cpp' -and $f.Extension -ne '.h') { continue }
        $lines = Get-Content -Encoding UTF8 $f.FullName
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $t = $lines[$i].Trim()
            if ($t -match '^(//|\*|/\*)') { continue }
            if ($t -notmatch ('\b' + [regex]::Escape($name) + '\(.*\)\s*$')) { continue }
            # `{` 可以在**下一行**（这三个模板的定义都是这样写的）。
            # 一开始只认同行，结果规则一条都没匹配上 —— 静默地什么都查不到。
            $openAt = $i
            if ($t -notmatch '\{\s*$') {
                if (($i + 1) -lt $lines.Count -and $lines[$i + 1].Trim() -eq '{') { $openAt = $i }
                else { continue }
            }
            $ind = ($lines[$openAt] -replace '^(\s*).*', '$1').Length
            $body = @()
            for ($m = $openAt; $m -lt $lines.Count; $m++) {
                $body += $lines[$m]
                if ($lines[$m].Trim() -eq '}' -and ($lines[$m] -replace '^(\s*).*', '$1').Length -eq $ind) { break }
            }
            $bt = $body -join "`n"
            if ($bt -notmatch '\btry\s*\{') {
                $hits += [pscustomobject]@{
                    File = $f.Name
                    Line = $i + 1
                    Rule = "N: 进托管代码的 $name 没有异常边界（托管异常会被 il2cpp 翻成 C++ 异常，逃出去就崩游戏）"
                    Text = $t
                }
            }
        }
    }
}

foreach ($name in $hookNames.Keys) {
    foreach ($f in $files) {
        $lines = Get-Content -Encoding UTF8 $f.FullName
        if ($f.Extension -ne '.cpp') { continue }
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $t = $lines[$i].Trim()
            if ($t -match '^(//|\*|/\*)') { continue }
            # 找这个替换函数的**定义**（不是声明：声明行以 ; 结尾）
            if ($t -notmatch ("\b" + [regex]::Escape($name) + "\s*\(")) { continue }
            if ($t -match ';') { continue }
            $ind = ($lines[$i] -replace '^(\s*).*', '$1').Length
            $body = @()
            $closed = $false
            for ($m2 = $i; $m2 -lt $lines.Count; $m2++) {
                $body += $lines[$m2]
                if ($lines[$m2].Trim() -eq '}' -and ($lines[$m2] -replace '^(\s*).*', '$1').Length -eq $ind) { $closed = $true; break }
            }
            if (-not $closed) { continue }
            $bt = $body -join "`n"
            if ($bt -notmatch '\btry\s*\{') {
                $hits += [pscustomobject]@{
                    File = $f.Name
                    Line = $i + 1
                    Rule = 'N: 钩子替换函数没有异常边界（异常逃进游戏执行路径 = std::terminate = 崩游戏）'
                    Text = $t
                }
            }
            break
        }
    }
}
# ---- 模式 P：往趋势/曲线里取样时，不许把**读失败**当成 0 ----
#
# 第 114 轮给关注值加了行内迷你趋势图之后想到的。
#
# 取样那一侧最容易犯的错是「读不到就填个 0」。而图表比文字更容易骗人：
# 文字是一个孤零零的「0」，图是「刚刚还好好的，现在突然归零」——
# 用户看到的是**正在掉血**。
#
# 形状：history.push_back(...) / push_back(...) 紧跟在一次可能失败的
# 读取之后，而**中间没有**任何判空/判定成功的分支。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -notmatch '^\w+\.history\.push_back\(') { continue }
        # 往上找这段取样的起点：必须是「读取成功之后」而不是 catch/失败分支
        $hasGuard = $false
        for ($k = $i; $k -ge 0; $k--) {
            $u = $lines[$k].Trim()
            # **注释不能算守卫**。第 114 轮就栽在这：我把 `isfinite(v)` 去掉之后，
            # 规则还是绿的 —— 因为上面那行说明文字里有 `//   isfinite(v)`。
            # 第 90 轮的教训：匹配之前先把注释剥掉。
            if ($u -match '^(//|\*|/\*)') { continue }
            if ($u -match 'catch\s*\(|<读取失败|<非标量|isfinite|\*endp ==') { $hasGuard = $true; break }
            if ($u -match '^\}\s*$' -or $u -match '^(try|for|while)\b') { break }
        }
        if (-not $hasGuard) {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'P: 取样进趋势图之前没有排除「读取失败」（图上会出现凭空的下坠）'
                Text = $t
            }
        }
    }
}

# ---- 模式 Q：复制到剪贴板必须走那个**不抛**的助手 ----
#
# 第 118 轮。现在全项目有 6 处「复制」，其中 4 处要在渲染线程上
# 拼一整份几百行的字符串（导出改动记录、关注值整份、自检报告、日志全文）。
# 那个 `+=` 循环会抛 std::bad_alloc，而异常一路逃出渲染路径的后果
# 不是「这次复制没成功」，而是**这一整帧的菜单都不画** ——
# 用户看到的是界面闪一下、剪贴板没变，完全无从判断原因。
#
# 所以统一走 `CopyToClipboard(fn, &err)`，它保证不抛，失败时如实上报。
# 直接调 `ImGui::SetClipboardText` 的只允许出现在 Includes/Utils.cpp 里
# （就是那个助手本身）。
foreach ($f in $files) {
    if ($f.Name -eq 'Utils.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -notmatch 'ImGui::SetClipboardText\s*\(') { continue }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'Q: 直接调 SetClipboardText（这一帧在渲染线程上，异常逃出去 = 整帧菜单不画）。请走 CopyToClipboard'
            Text = $t
        }
    }
}

# ---- 模式 R：UI 层的「保存」必须看返回值 ----
#
# 第 119 轮。ConfigSave 以前是 void，调用方无从知道写盘有没有成功。
# 而三个调用点都在**渲染线程**上，且 `classesTabs` 在调用它**之前**
# 就已经被改了（预设加进去了、筛选结果认领了、标签页建好了）。
#
# 于是保存失败时的表现是：
#     界面上它还在  +  用户以为存上了  +  下次启动没了
# 也就是**静默丢数据**，而且丢得很彻底。
#
# 所以：保存类函数一律返回 bool，UI 层的调用点一律包在 if 里。
# 本规则只查 UI 那个文件 —— Tool.cpp 里 ConfigInit 的首次创建
# 忽略返回值是合理的（还没有东西可丢）。
foreach ($f in $files) {
    if ($f.Name -ne 'ClassesTab.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -notmatch '^Tool::ConfigSave\(\);') { continue }
        $prev = if ($i -gt 0) { $lines[$i - 1].Trim() } else { '' }
        if ($prev -match 'if\s*\(\s*!Tool::ConfigSave\(\)\s*\)') { continue }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'R: UI 层的保存忽略了返回值（内存已改而没落盘 = 静默丢数据）'
            Text = $t
        }
    }
}

# ---- 模式 S：FileWriter 写完**必须**看 ok() ----
#
# 第 119/120 轮。`FileWriter::ok()` 是第 90 轮就有的，
# 但**一个调用点都没用** —— 「写盘失败了」这件事从来传不上来。
#
# 而所有写盘点都在**用户看得见的地方**：
#     导出对象 json      写失败照样打 "Done save"，用户以为文件在
#     tool_conf.json     设置改完立刻生效，用户以为存上了
#     class_tabs.json    参数预设/筛选结果，用户以为存上了
#
# 最糟的是**静默丢数据**：界面上它已经变了，而下次启动回到旧值，
# 中间没有任何一步说过「没写成功」。
#
# 形状：函数里 `Util::FileWriter X(...)` 出现了，但整个函数里
# 从头到尾没读过 `X.ok()`。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    # 按函数切块（缩进 4 的定义行，或更粗的）
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        $m = [regex]::Match($t, 'Util::FileWriter\s+(\w+)\s*\(')
        if (-not $m.Success) { continue }
        $var = $m.Groups[1].Value
        # 往后看到函数结束（缩进 <= 定义行缩进且以 } 结尾）
        $ind = ($lines[$i] -replace '^(\s*).*', '$1').Length
        $body = @()
        for ($k = $i; $k -lt $lines.Count; $k++) {
            $body += $lines[$k]
            $kt = $lines[$k].Trim()
            if ($k -gt $i -and $kt -match '^\}' -and
                ($lines[$k] -replace '^(\s*).*', '$1').Length -le $ind) { break }
        }
        $bt = $body -join "`n"
        if ($bt -notmatch ('\b' + [regex]::Escape($var) + '\.ok\(\)')) {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = "S: FileWriter ``$var`` 写完没有看 ok()（写盘失败会被当成成功 = 静默丢数据）"
                Text = $t
            }
        }
    }
}

# ---- 模式 T：`savedSet.insert` 之前**必须**先加根成功 ----
#
# 第 122 轮。`ClassesTab.cpp` 的「Save」按钮原来长这样：
#
#     savedSet[currentObj->klass].insert(currentObj);   // 先躺进去
#     SaveObjectWithRoot(currentObj);                    // 再加根，结果丢弃
#
# 加根失败时 savedSet 里就多了一个**没有根的裸指针**：
# 检视器照样列得出来，ResolveSaved 原样返回它，而它随时可能被 GC 回收 ——
# 界面上看得见的每个字都正常，然后解引用它的时候崩游戏。
#
# 这是第 99 轮在**另一个**调用点修掉的同一个 bug，一直留在这里。
# 规则 J 抓不到它：J 盯的是「退回裸指针」的**形状**（三元 / return），
# 而这里是**顺序**。
#
# 形状：同一个块里 `savedSet[...].insert(...)` 出现在
# `SaveObjectWithRoot(...)` 之前。
foreach ($f in $files) {
    if ($f.Name -ne 'ClassesTab.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -notmatch 'savedSet\[.+\]\.insert\(') { continue }
        # 往后 6 行内必须出现加根；出现得比 insert 晚 = 顺序错了
        for ($k = $i + 1; $k -le [Math]::Min($i + 6, $lines.Count - 1); $k++) {
            $u = $lines[$k].Trim()
            if ($u -match '^(//|\*|/\*)') { continue }
            if ($u -match 'SaveObjectWithRoot\(') {
                $hits += [pscustomobject]@{
                    File = $f.Name
                    Line = $i + 1
                    Rule = 'T: 先 insert 再加根 —— 加根失败时 savedSet 里会留下一个没有根的裸指针（解引用它 = 崩游戏）'
                    Text = $t
                }
                break
            }
        }
    }
}

# ---- 模式 U：自检说「可用」之前，那个能力得**真的**可判定 ----
#
# 第 124 轮。`ApiResolved()` 查了 domain / class / thread / is_vm_thread
# 四个符号，自检据此说「il2cpp API 已解析 ✓」。
#
# 但 `NewHandle` 用的是另外三个（gchandle_new / get_target / free），
# 一个都没查。三个全缺时 ApiResolved() 照样 true、自检照样绿，
# 而 NewHandle 对**每一个**对象返回 0，liveObjects() 把它们全跳过 ——
# 用户看到「Find Objects 秒完成，一个都没找到」。
#
# il2cpp_gchandle_new 在老版本 il2cpp 上就可能缺，这不是理论问题。
#
# 形状：自检里用 ApiResolved() 判「核心能力可用」，
# 而 GC::NewHandle 依赖的符号没有任何一项单独判定。
foreach ($f in $files) {
    if ($f.Name -ne 'SelfCheck.cpp') { continue }
    $text = Get-Content -Encoding UTF8 $f.FullName -Raw
    if ($text -notmatch 'ApiResolved\(\)') { continue }
    # 必须**真的**是一个判定分支，不能是 `false && GcHandleApiResolved()`
    # 这种「调用还在、判定没了」的写法 —— 只查「文本里出现过这个名字」的话，
    # 那个注入正好能骗过它（第 124 轮自己踩了一次）。
    # 两项**各自**判定，不能是「有其中一项就放过」。
    #
    # 第一版写成了两个连续的 `if (...match...) { continue }`，那是**或**：
    # 只要 gchandle 那项在位，浏览那项被架空也照样放过 —— 我注入
    # `if (false && Il2cpp::BrowserApiResolved())` 验证时它没红（第 125 轮）。
    # 和第 115/124 轮同一个错：把「存在」当成了「每一项都成立」。
    $gcOk = $text -match 'if\s*\(\s*Il2cpp::GcHandleApiResolved\(\)\s*\)'
    $browserOk = $text -match 'if\s*\(\s*Il2cpp::BrowserApiResolved\(\)\s*\)'
    if ($gcOk -and $browserOk) { continue }
    $hits += [pscustomobject]@{
        File = $f.Name
        Line = 0
        Rule = 'U: 自检用 ApiResolved() 判「核心能力可用」，但 GC 句柄 / 浏览路径那些符号没人单独判定（缺了照样一片绿：一个留不住对象，一个打开类就崩）'
        Text = 'ApiResolved() 不含 il2cpp_gchandle_* / 浏览路径符号'
    }
}

# ---- 模式 V：说「缺符号会崩」的能力，界面上必须真的挡一下 ----
#
# 第 126 轮。`BrowserApiResolved()` 那几个符号的包装函数是**裸调**的
# （`GetIsMethodInflated` -> `il2cpp_method_is_inflated`），
# 而它在方法列表的渲染循环里每一行都要走：符号为空 = 空指针调用 = SIGSEGV。
#
# 只在自检里报出来是不够的 —— 用户得先打开一个类才会踩到。
# 所以**凡是**用 BrowserApiResolved 判定过的能力，
# 依赖它的那个界面就得先挡一下，给一句能看懂的话。
#
# 三处必须挡：类页面的方法列表、getMethodList、枚举下拉。
# Pattern 是**字面量**，不在这里写正则转义。
#
# 第一版我把 `klass->getFields\(\)` 这样的「已转义」串又套了一层
# `[regex]::Escape`，于是模式变成 `klass\->getFields\\\(\\\)` ——
# **一条都匹配不上**，而规则还报「通过」。
# 这就是第 15 次「规则看着对、其实没在跑」。
$needBrowserGuard = @(
    @{ File = 'ClassesTab.cpp'; Pattern = 'MethodViewer(klass, method, paramsInfo);'; Why = '方法列表每个方法都要走一遍裸调用' },
    @{ File = 'ClassesTab.cpp'; Pattern = 'klass->getMethods()'; Why = '类一打开就走的建缓存路径' },
    @{ File = 'PopUpSelector.cpp'; Pattern = 'klass->getFields()'; Why = '枚举下拉，点一下就崩' }
)
foreach ($g in $needBrowserGuard) {
    $file = $files | Where-Object { $_.Name -eq $g.File }
    if (-not $file) { continue }
    $text = Get-Content -Encoding UTF8 $file.FullName -Raw
    if ($text -notmatch [regex]::Escape($g.Pattern)) { continue }
    # 那个调用**之前** 60 行内必须出现过 BrowserApiResolved 的判定
    $lines = Get-Content -Encoding UTF8 $file.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch [regex]::Escape($g.Pattern)) { continue }
        $from = [Math]::Max(0, $i - 60)
        $seg = ($lines[$from..$i] -join "`n")
        if ($seg -match '!Il2cpp::BrowserApiResolved\(\)') { break }
        $hits += [pscustomobject]@{
            File = $g.File
            Line = $i + 1
            Rule = "V: 自检说这类能力缺符号会崩，但界面上没挡（$($g.Why)）"
            Text = $lines[$i].Trim()
        }
        break
    }
}

# ---- 模式 Y：跨线程「交接标志」不许用 relaxed ----
#
# 第 133 轮。`BulkResult` 里
#     done.store(true, relaxed)     工作线程
#     done.load(relaxed)            界面线程 -> 接着读 ok / failed
# relaxed **不建立 happens-before**：不同原子对象之间没有顺序保证，
# arm64 这种弱内存序平台上完全允许界面线程先看见 done==true、
# 却读到还没更新的 ok/failed -> 界面上显示「成功 0 个，失败 0 个」。
#
# 这不是崩溃，是**给用户看一个假数** —— 讽刺的是正是第 121 轮
# 想消灭的那类问题（「界面上一个看起来权威、实际不对的数字」）。
#
# 判据（按名字区分，因为「这个标志到底盖着哪些数据」没法静态推出来）：
#   交接标志 done / ready / finished / completed  -> 必须 release / acquire
#   请求标志 cancel / stop / quit / request       -> relaxed 是对的
#
# 请求标志为什么对：它只是个「请停下」的单向信号，别的数据**不靠它发布**，
# 工作线程轮询它、最终会看到就够了。而 relaxed 原子量按标准保证
# 每个对象自身的修改顺序和最终可见 —— 对这种用法正是最省的选择。
# （Dump().cancelRequested 就是这一类，别把它一起改掉。）
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        $m = [regex]::Match($t, '\b(done|ready|finished|completed)\.store\(\s*true\s*,\s*std::memory_order_relaxed')
        if (-not $m.Success) { continue }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'Y: 交接标志用 relaxed —— 读到这个标志之后的那些数据可能还是旧值（界面上会显示假数）'
            Text = $t
        }
    }
}

# ---- 模式 Z：能力判定失败**不许**写进缓存 ----
#
# 第 134 轮。第 126 轮给浏览路径加挡的时候，我在 `buildMethodMap` 里写成：
#
#     if (!Il2cpp::BrowserApiResolved()) {
#         methodCache[klass] = empty;     <-- 把一次暂时的状态缓存成了永久结果
#         return methodCache[klass];
#     }
#
# 只要在那一次调用时符号还没解析出来，这个类的方法列表就**永远是空的**，
# 即使符号后来解析好了也不会重建 —— 而「没有方法」和「看不了」在界面上
# 长得几乎一样（都画 0 个方法），用户不会知道差在哪。
#
# 「符号不会晚点才解析出来」听起来成立（dlsym 不会二次成功），
# 但「菜单会不会在 API 解析完成之前被画出来」是**时序**，
# 而时序是最不靠得住的一类假设。
#
# 形状：`if (!<某个能力判定>)` 的分支体里出现 `<某个缓存容器>[...] =`。
foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -notmatch '^if\s*\(\s*!\s*\w+(::\w+)*\s*\(\s*\)\s*\)') { continue }
        $name = [regex]::Match($t, '!\s*(\w+(::\w+)*)').Groups[1].Value
        # 跳过「不该缓存」的那些（不是能力判定）
        if ($name -match '^(Empty|empty)$') { continue }
        # **必须按花括号深度扫，不能见 } 就 break**（第 134 轮）。
        #
        # 第一版就是见 } 就 break，于是被保护块里**嵌套**的
        #     if (!g_warnedAboutBrowserApi) { ... }   <-- 它的 } 把扫描截断了
        # 真正要抓的那行在它后面，于是规则永远是绿的。
        # 注入真缺陷验证时才发现 —— 又一次「规则看着对、其实没在跑」。
        # 深度从 0 起、**进块之后**才算 —— 因为这个 `if` 的 `{` 通常在**下一行**，
        # 而我在第一版里既预设了 1 又数了那个 `{`，等于数了两次，
        # 于是深度永远回不到 0，扫描一路冲进整个函数（第一版误报了
        # 下面那个 `methodCache.size() >= kMethodCacheLimit` 的清空逻辑）。
        $depth = 0
        $entered = $false
        for ($k = $i + 1; $k -lt $lines.Count; $k++) {
            $u = $lines[$k].Trim()
            if ($u -notmatch '^(//|\*|/\*)') {
                $depth += ([regex]::Matches($u, '\{')).Count - ([regex]::Matches($u, '\}')).Count
            }
            if (-not $entered -and $depth -gt 0) { $entered = $true }
            if ($entered -and $depth -le 0) { break }
            if (-not $entered) { continue }
            if ($u -match '^(//|\*|/\*)') { continue }
            # 写进任何「缓存」容器
            if ($u -match '\b(methodCache|objectCache|newObjectMap|collectCache)\b.*=' -and
                $u -notmatch '^\s*(==|!=)') {
                $hits += [pscustomobject]@{
                    File = $f.Name
                    Line = $k + 1
                    Rule = 'Z: 能力判定失败时写进缓存 —— 一次暂时的状态会变成**永久**结果（符号补齐后也不重建）'
                    Text = $u
                }
                break
            }
        }
    }
}

# ---- 模式 AA：能力判定失败**不许**用早退绕过状态清理 ----
#
# 第 135 轮。第 126 轮在 `PopUpSelector::Update()` 里加的挡是这样的：
#
#     if (!Il2cpp::BrowserApiResolved()) {
#         ImGui::TextColored(...);       "枚举列表画不出来"
#         ImGui::EndPopup();
#         return;                        // <-- 直接走人
#     }
#
# 但 `lastCallback` 平时**只在 `Do()` 里清空**，而这条路径跳过了 `Do()`。
# 后果不是这个弹窗关不掉（EndPopup 已经调了），而是：
#     `Update()` 每帧都跑 -> `if (lastCallback)` 一直成立
#     -> `Do()` 再也不会被调用 -> 那个「正在选值」的交互状态永远退不出来
#     -> 用户以为界面卡住了
#
# 早退本身没错（它挡住了崩溃）。错的是**早退绕过了它负责的状态清理**。
#
# 形状：`if (!<能力判定>)` 的块里出现 `return;`，而该块**没有**任何
# 对 `lastCallback` / `needOpen` / `userData` 的清理。
foreach ($f in $files) {
    if ($f.Name -ne 'PopUpSelector.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        if ($t -notmatch '^if\s*\(\s*!\s*\w+(::\w+)*\s*\(\s*\)\s*\)') { continue }
        $name = [regex]::Match($t, '!\s*(\w+(::\w+)*)').Groups[1].Value
        if ($name -match '^(Empty|empty)$') { continue }
        $depth = 0
        $entered = $false
        $hasReturn = $false
        $hasCleanup = $false
        for ($k = $i + 1; $k -lt $lines.Count; $k++) {
            $u = $lines[$k].Trim()
            if ($u -notmatch '^(//|\*|/\*)') {
                $depth += ([regex]::Matches($u, '\{')).Count - ([regex]::Matches($u, '\}')).Count
            }
            if (-not $entered -and $depth -gt 0) { $entered = $true }
            if ($entered -and $depth -le 0) { break }
            if (-not $entered -or $u -match '^(//|\*|/\*)') { continue }
            if ($u -match '^\s*return\s*;') { $hasReturn = $true }
            if ($u -match '\b(lastCallback|needOpen|userData)\s*=\s*(nullptr|""|\"\")') {
                $hasCleanup = $true
            }
        }
        if ($hasReturn -and -not $hasCleanup) {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'AA: 能力判定失败用早退绕过状态清理 —— lastCallback 一直是「已设置」，Update() 每帧走这条分支，交互状态永远退不出来'
                Text = $t
            }
        }
    }
}

# ---- 模式 AB：记一次结果的 Begin 必须有地方读出来（第 136 轮）----
#
# 第 121 轮我加了 `g_bulkResult`（Begin / Add / Finish / Report），
# 给三个批量操作各记一次：取消追踪全部 / 追踪全部 / 恢复全部。
# 但 `Report()` **只在一处**调过。
#
# 后果比「少报一个数」更阴：没被读出来的结果**积在 done 标志里**，
# 等下次点别的按钮时和别的操作的结果**一起报出来**，
# 而且贴在完全不相干的界面上 —— 用户会以为那是刚才那次操作的果。
#
# 也就是说「记了但没人读」不会消失，只会**攒着一次在错误的时机冒出来**。
#
# 形状：`Begin(` 的个数 > `Report()` 的个数。
$bulkBegin = 0
$bulkReport = 0
foreach ($f in $files) {
    if ($f.Name -ne 'ClassesTab.cpp') { continue }
    $text = Get-Content -Encoding UTF8 $f.FullName -Raw
    $bulkBegin  = ([regex]::Matches($text, 'g_bulkResult\.Begin\s*\(')).Count
    $bulkReport = ([regex]::Matches($text, 'g_bulkResult\.Report\s*\(')).Count
}
# 注意：**不能**简单比 Begin 的个数和 Report 的个数。
# 第一版就是这么写的，然后它把「修复」判成了违规 ——
# 因为「取消追踪全部」和「追踪全部」的 Begin 在**同一个函数**里，
# 而那个函数里只有**一个** Report()：它是每帧读一次「当前挂着的那一条」，
# 两个 Begin 天然共用它。3 Begin / 2 Report 是对的。
#
# 真正要抓的是更窄的一种：**某个函数里有 Begin，却没有 Report** ——
# 那才是「记了没人读」。所以下面按函数统计。
$functionsWithBegin = @()
$functionsWithReport = @()
foreach ($f in $files) {
    if ($f.Name -ne 'ClassesTab.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    $cur = '<文件作用域>'
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(?:[\w:<>*&]+\s+)+([A-Za-z_]\w*(?:::\w+)*)\s*\([^;]*$') { $cur = $Matches[1] }
        if ($t -match '^\s*g_bulkResult\.Begin\s*\(') { $functionsWithBegin += $cur }
        if ($t -match '^\s*g_bulkResult\.Report\s*\(\s*\)\s*;') { $functionsWithReport += $cur }
    }
}
foreach ($fn in ($functionsWithBegin | Sort-Object -Unique)) {
    if ($functionsWithReport -notcontains $fn) {
        $hits += [pscustomobject]@{
            File = 'ClassesTab.cpp'
            Line = 0
            Rule = "AB: 函数 ``$fn`` 里记了批量结果却没有 Report —— 它会一直积着，在下一次别的操作之后贴错地方报出来"
            Text = $fn
        }
    }
}

# ---- 模式 AC：跨线程发布「消息」不许用会被释放的缓冲 ----
#
# 第 137 轮。`GetFilterFailure()` 是这样把失败原因交出去的：
#
#     std::lock_guard guard(filterState->mutex);
#     return filterState->failure;        // const char *
#
# 加锁只保护了**读指针**这一步；调用方拿到指针之后锁就放了，
# 而工作线程随时可能在同一把锁下改写它。
#
# 现在**恰好**没问题，因为三处赋值全是**字符串字面量**（静态存储期），
# 指针的值永远有效 —— 所以这是「**结论正确、理由没写下来**」：
# 下一个人把某处改成 `failure = someStdString.c_str()` 就静默变成野指针，
# 而且编译器一句都不会说。
#
# 这正是第 134 轮那个教训的另一半：
# 那里是「靠一个说不清的时序成立」，这里是「靠一个没写下来的约定成立」。
#
# 形状：`const char *`（或 `char *`）类型的状态字段被赋值，
# 且右值不是字面量 / nullptr / 别的裸指针转发。
$acRaw = ($files | ForEach-Object { [IO.File]::ReadAllText($_.FullName) }) -join "`n"

# 第 138 轮：`GetFilterFailure()` 已经改成返回 std::string，
# 那个裸指针字段现在**只在 FilterState 内部**流通。
# 如果哪天它又变成了某个**公开**返回值，说明有人把指针又递出去了 ——
# 那正是第 137 轮要根治的东西，不该悄悄回来。
#
# 这一段**必须在循环外面**：第一版把它塞进了赋值扫描的循环体里，
# 于是它前面任何一句 continue 都能让它整段跳过 —— 规则「看着在跑、
# 其实从没执行过」。同样的错第 15 次：存在 ≠ 生效。
if ($acRaw -match 'const\s+char\s*\*\s*\w*Get\w*Failure\s*\(\s*\)\s*;') {
    $hits += [pscustomobject]@{
        File = 'ClassesTab.h'
        Line = 0
        Rule = 'AC: 又把失败原因以裸指针返回了 —— 调用方拿到的是「出了锁就没人管」的指针，第 138 轮已改成 std::string'
        Text = 'const char *Get...Failure()'
    }
}

foreach ($f in $files) {
    # **头文件也要扫**（第 138 轮）。
    # 第一版只扫 .cpp，于是「公开返回值改回裸指针」这种注入**抓不到** ——
    # 因为那行声明在 .h 里。而「公开 API 递出裸指针」恰恰是最该抓的一处。
    if ($f.Name -notin @('ClassesTab.cpp', 'ClassesTab.h')) { continue }
    $rawTextCache = Get-Content -Encoding UTF8 $f.FullName -Raw
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # 形如  st->failure = <expr>;   或   failure = <expr>;（局部变量）
        # 第二种也是要查的：它在 DoFilterWork 里，第 137 轮注入验证时
        # 我只写了第一种，于是规则「看着有、其实没在跑」——
        # 而真正会被改坏的是那个局部变量。
        $m = [regex]::Match($t, '^(?:st->)?(\w+)\s*=\s*([^;]+);\s*$')
        if (-not $m.Success) { continue }
        $field = $m.Groups[1].Value
        $rhs   = $m.Groups[2].Value.Trim()
        if ($field -notmatch 'failure|reason|errorText') { continue }
        if ($rhs -match '^"|^nullptr$') { continue }
        # 转发一个局部变量是允许的（那个局部变量的所有赋值处都要是字面量）
        if ($rhs -match '^[A-Za-z_]\w*$') { continue }
        # **只查 char\* 类型的字段**（第 137 轮）。
        # 第一版只按名字匹配，于是 `rootFailures = it->second;`（一个 size_t）
        # 也被报了 —— 那不是消息，是个计数。
        #
        # 所以先扫出全文所有 `const char *name` / `char *name` 的声明，
        # 只对**这些名字**生效。名字匹配用来定位，类型匹配用来定性。
        if ($rawTextCache -notmatch ('\b(?:const\s+)?char\s*\*\s*(?:[\w:>]+\s*,\s*)*' + [regex]::Escape($field) + '\b')) {
            continue
        }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'AC: 跨线程发布的失败原因赋了非字面量 —— 调用方在锁外用它，一旦右边指向会被释放的缓冲就是野指针'
            Text = $t
        }
    }
}

# ---- 模式 AD：「线程没起来」的回调必须留下可见痕迹 ----
#
# 第 139 轮。`SpawnDetached(body, name, onFail)` 的第三个参数
# 是在 `std::thread` 构造抛异常时才会被调用的。
#
# 四处调用点里，有两处只做了「状态退回去」：
#
#     [processingFlag]() { *processingFlag = false; }
#
# 于是界面上按钮恢复正常、进度条消失、**什么异常都没有**，
# 而实际效果是「一个方法都没恢复 / 一个方法都没追踪」。
# 用户会以为是自己点的时机不对，或者以为「本来就没有要恢复的东西」，
# 于是**再点一次**。
#
# > 「静默什么都没发生」比报错更难查：报错至少有个人会去看日志。
#
# 形状：`SpawnDetached` 的第三个参数（无参 lambda）体里，
# 除了 `*processingFlag = false;` 这类状态复位之外，
# 没有任何一处会**被界面读到**（写 bulkResult / pendingXxx / LOGE）。
foreach ($f in $files) {
    if ($f.Name -ne 'ClassesTab.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        # 形如  [processingFlag]()     <- 无参的失败回调
        # 捕获列表里可能有多个东西（[this, processingFlag]()），\w+ 只吃一个，+ 也不够 ——
        # 第一版写的是 ^\[\w+\]，[this, processingFlag]() 一个都匹配不上，于是规则静默。
        if ($t -notmatch '^\[[\w\s,*&]+\]\s*\(\s*\)\s*$') { continue }
        # 往下找到它的函数体（第一行就是 { 的紧邻行）
        $j = $i + 1
        while ($j -lt $lines.Count -and $lines[$j].Trim() -ne '{') {
            if ($lines[$j].Trim() -match '^\}') { break }
            $j++
        }
        if ($j -ge $lines.Count -or $lines[$j].Trim() -ne '{') { continue }
        # 扫函数体
        $depth = 0
        $entered = $false
        $body = @()
        for ($k = $j; $k -lt $lines.Count; $k++) {
            $u = $lines[$k].Trim()
            if ($u -notmatch '^(//|\*|/\*)') {
                $depth += ([regex]::Matches($u, '\{')).Count - ([regex]::Matches($u, '\}')).Count
            }
            if (-not $entered -and $depth -gt 0) { $entered = $true }
            if ($entered -and $depth -le 0) { break }
            if ($entered -and $u -notmatch '^(//|\*|/\*)$') { $body += $u }
        }
        $text = $body -join "`n"
        # 「可见」= 界面能读到的状态，或者至少一条日志
        $visible = $text -match 'g_bulkResult|g_pending\w+|LOGE\('
        if (-not $visible) {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'AD: 「线程没起来」的回调什么痕迹都不留 —— 界面看起来一切正常，用户只会以为「本来就没有要做的事」然后再点一次'
                Text = $t
            }
        }
    }
}

# ---- 模式 AE：函数级 static 不许缓存「可能失败」的解析结果 ----
#
# 第 141 轮。`Keyboard.cpp` 的 updateImpl() 里：
#
#     static MethodInfo *get_statusMethod = TouchScreenKeyboard->getMethod("get_status");
#     if (!get_statusMethod) { LOGE(...); return Reset(); }
#
# `static` 的初值**只算一次**。所以那一刻如果 getMethod 返回 nullptr
# （元数据还没注册完 / 这个 Unity 版本没这个方法），
# 之后**永远**是 nullptr —— 每帧都走失败分支直接 Reset()。
#
# 于是键盘永远打不开，而界面上一点提示都没有：用户看到「点了没反应」。
#
# 讽刺的是同一个文件的 `Open()` 里专门写了「类解析失败可以重试」，
# 说明「暂时拿不到」在这份代码里**确实会发生**。
# 同一件事两处处理不同 —— 必然有一处是错的（第 134 轮同一个形状）。
#
# 形状：`static <指针类型> <名字> = <可能失败的调用>(...)`
foreach ($f in $files) {
    if ($f.Extension -ne '.cpp') { continue }
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # static 指针 = 一次调用（不是字面量、不是 nullptr、不是别的变量转发）
        #
        # 调用方要同时认 `A::b()` 和 `a->b()` 两种写法。
        # 第一版只写了 `(?:::\w+)*`，而真身是
        # `TouchScreenKeyboard->getMethod(...)` —— 中间是 `->` 不是 `::`，
        # 于是**一条都没匹配上**，规则报「通过」。
        # 第 16 次「规则看着对、其实没在跑」，靠注入真缺陷才发现。
        $m = [regex]::Match($t, '^static\s+[A-Za-z_][\w:]*\s*\*\s*(\w+)\s*=\s*([A-Za-z_]\w*(?:(?:::|->)[A-Za-z_]\w*)*)\s*\(')
        if (-not $m.Success) { continue }
        $name = $m.Groups[1].Value
        $call = $m.Groups[2].Value
        # 只查「解析类」的调用：getMethod / FindClass / getField 之类
        if ($call -notmatch '(?i)get|find|resolve|lookup') { continue }
        # 紧接着必须有判空 —— 没有的话静态解析器压根不会被检查
        $guard = ''
        for ($k = $i + 1; $k -lt [Math]::Min($i + 4, $lines.Count); $k++) {
            if ($lines[$k].Trim() -match ('if\s*\(\s*!\s*' + [regex]::Escape($name) + '\s*\)')) { $guard = $lines[$k].Trim(); break }
        }
        if (-not $guard) { continue }
        $hits += [pscustomobject]@{
            File = $f.Name
            Line = $i + 1
            Rule = 'AE: 函数级 static 缓存了**可能失败**的解析结果 —— static 只初始化一次，那一刻返回空就永久是空（判空分支会每次都走，功能等于废掉）'
            Text = $t
        }
    }
}

# ---- 模式 AF：版本号只有一个出处 ----
#
# 第 142 轮。这份代码里一度有**三个**互不相同的版本说法：
#
#     app/build.gradle   versionName "3.2"
#     Main.cpp           窗口标题硬编码 "v0.9"
#     SelfCheck.cpp      自检页硬编码 "v0.9"
#     仓库里还放着      Tool_v0.9.zip
#
# 而自检页那两行的存在意义**恰恰是**「用户报『菜单没反应』时，
# 让我一眼确认他跑的是哪个版本」。它自己报的是错的版本 ——
# 于是用户如实抄来的报告里，最关键的那一栏是假的。
#
# 修法：版本号的唯一出处是仓库根目录的 VERSION.txt，
# build.ps1 生成 app/src/main/jni/Includes/Version.h，源码只读那个头文件。
#
# 形状：.cpp / .h 里出现字面量形式的版本号（v0.9 / "0.9" 之类）
foreach ($f in $files) {
    if ($f.Extension -notin @('.cpp', '.h')) { continue }
    if ($f.Name -eq 'Version.h') { continue }   # 生成物
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($i = 0; $i -lt $lines.Count; $i++) {
        $t = $lines[$i].Trim()
        if ($t -match '^(//|\*|/\*)') { continue }
        # 形如 "v0.9" / 'v0.9' / v0.9 | HitMargin
        #
        # 注意**不要**加「同一行出现 IL2CPPTOOL_VERSION 就放过」这种豁免：
        # 真实的形状恰恰是 `OBFUSCATE("Il2CppTool v0.9") + IL2CPPTOOL_VERSION` ——
        # 同一行里两者都有。加了豁免之后这条规则就漏掉了**唯一**的真实案例，
        # 而手工注入验证时才发现（我第 142 轮自己踩的）。
        #
        # Version.h 是生成物，已经在上面按文件名排除了，不需要这层。
        if ($t -match '\bv\d+\.\d+(\.\d+)?\b') {
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'AF: 源码里写死了版本号 —— 唯一出处是 VERSION.txt（由 build.ps1 生成 Version.h）。自检页报错版本比不报更糟'
                Text = $t
            }
        }
    }
}

# ---- 模式 AG：空列表不许直接消失，要指路 ----
#
# 第 142 轮。`DrawWatches()` 第一行原来是：
#
#     if (g_watches.empty()) { return; }
#
# 空的时候整个关注值区域消失，面板里只剩一个空的折叠块。
# 而关注值是工具的核心工作流之一（「盯着一个数值，随它变」）。
# **一个不出现的功能等于不存在** —— 用户没理由知道它存在，
# 也没理由知道该去哪儿加（添加入口藏在对象字段的右键菜单里，
# 要先有对象、先展开字段，才知道可以右键）。
#
# 第 121 轮的教训是「把东西放在对的宿主上，否则功能消失」，
# 这里是它的镜像：**宿主对了，但内容在不该消失的时候消失了**。
#
# ## 为什么用白名单，而不是「像不像 UI」的启发式
#
# 这一条我试了三版正则判据，每一版都错，而且是**同时**误报和漏报：
#
#   v1  `X.empty()` + `return`         -> 误报 13 处（bytes/classes/data…
#                                         全是逻辑函数，空了就返回是对的）
#   v2  「if 前后 30 行内有 ImGui::」   -> 误报 1 处、且漏掉真缺陷
#   v3  「分支之后 30 行内有 ImGui::」  -> 同样误报 2 处
#
# v2/v3 的致命之处：`patched.empty()` 那几行**外面就套着 ImGui 代码**，
# 而真正的缺陷（删掉关注值的引导块）分支体里恰好**没有** ImGui ——
# 同一个条件既造成误报又造成漏报。
#
# 三版都在猜「这段代码是不是 UI」。而项目里已经有不靠猜的答案：
# `.ci/check-hosts.ps1` 的函数白名单。列表区域就那么几个，
# 显式列出来比启发式可靠 —— **判据宁可窄，不要每次重调**。
#
# 白名单是「用户可见的列表区域」：空时该显示引导，而不是消失。
# 不包括：搜索结果（空 = 没匹配到，本身就是答案）、错误提示条
# （空 = 没错误，正确地不占位）、lambda 里的失败分支。
$emptyStateLists = @(
    @{ Func = 'DrawWatches';     File = 'app/src/main/jni/Tool/ClassesTab.cpp' }
)
foreach ($f in $files) {
    if ($f.Extension -ne '.cpp') { continue }
    $rel = $f.FullName.Substring($root.Length + 1).Replace('\', '/')
    $lines = Get-Content -Encoding UTF8 $f.FullName
    for ($e = 0; $e -lt $emptyStateLists.Count; $e++) {
        $spec = $emptyStateLists[$e]
        if ($rel -ne $spec.File) { continue }
        # 定位该函数体。
        # 匹配函数**定义行** `void ClassesTab::DrawWatches()`，
        # 而不是任何 `::DrawWatches(` —— 后者会匹配到**注释里的引用**和调用点，
        # 扫描范围就从那里一路扫到文件末尾（第一版白名单一次报出 7 处）。
        #
        # `{` 在**下一行**（Allman 风格），所以只匹配到右括号为止。
        $fnAt = -1
        for ($i = 0; $i -lt $lines.Count; $i++) {
            if ($lines[$i] -match ('^[\w\s\*&:]+::' + $spec.Func + '\s*\([^;]*\)\s*$')) { $fnAt = $i; break }
        }
        if ($fnAt -lt 0) { continue }
        # 函数体范围：从开括号到配对的闭括号，别越界扫到别的函数
        $depth = 0; $fnEnd = $lines.Count - 1
        for ($i = $fnAt; $i -lt $lines.Count; $i++) {
            $depth += ([regex]::Matches($lines[$i], '\{')).Count - ([regex]::Matches($lines[$i], '\}')).Count
            if ($depth -eq 0 -and $i -gt $fnAt) { $fnEnd = $i; break }
        }
        for ($i = $fnAt; $i -le $fnEnd; $i++) {
            $t = $lines[$i].Trim()
            if ($t -notmatch '^if\s*\(\s*\w+\s*\.\s*empty\s*\(\s*\)\s*\)\s*$') { continue }
            $j = $i + 1
            while ($j -lt $lines.Count -and $lines[$j].Trim() -eq '') { $j++ }
            if ($j -ge $lines.Count -or $lines[$j].Trim() -ne '{') { continue }
            $depth = 0; $body = @()
            for ($k = $j; $k -lt $lines.Count; $k++) {
                $body += $lines[$k]
                $depth += ([regex]::Matches($lines[$k], '\{')).Count - ([regex]::Matches($lines[$k], '\}')).Count
                if ($depth -eq 0 -and $k -gt $j) { break }
            }
            $btext = $body -join ' '
            if ($btext -notmatch '\breturn\b') { continue }
            if ($btext -match 'TextDisabled|TextColored|TextWrapped') { continue }
            $hits += [pscustomobject]@{
                File = $f.Name
                Line = $i + 1
                Rule = 'AG: 列表为空时直接 return —— 该区域会**整块消失**。空状态应该指路（怎么用、入口在哪），而不是留白'
                Text = $t
            }
        }
    }
}
# ---- 模式 C：workflow 里 run:/shell: 的缩进不对 ----
#
# 上一轮我加这个检查时把 YAML 缩进写错了（2 空格而不是 6），
# 结果 **CI 在 0 秒就失败** —— workflow 本身语法不合法，一个步骤都没跑。
# 而当时的门禁只检查「文件里有没有出现 check-ui-patterns 这个字符串」，
# 字符串是在的，所以门禁放行了。
#
# 0 秒失败是「workflow 语法错误」的指纹，不是某个步骤失败。
#
# 规则刻意做得**很粗**：jobs.steps 的键（run/shell/with/id）必须缩进 >= 6
# （jobs=0, steps=2, 键=4... 实际文件里是 8）。不检查更细的关系 ——
# 试过逐步骤比对缩进，`static-analysis:` 这种 job 级的键会误报，
# 而**误报的检查会被调低或干脆删掉**，那才是真正的损失。
$workflow = Join-Path $root '.github/workflows/ci.yml'
if (Test-Path $workflow) {
    $wl = Get-Content -Encoding UTF8 $workflow
    for ($i = 0; $i -lt $wl.Count; $i++) {
        $cur = $wl[$i]
        $trim = $cur.Trim()
        if ($trim -eq '' -or $trim.StartsWith('#')) { continue }
        if ($trim -notmatch '^(run|shell|with|id|env|if|continue-on-error):') { continue }
        $ind = $cur.Length - $cur.TrimStart().Length
        if ($ind -lt 6) {
            $hits += [pscustomobject]@{
                File = 'ci.yml'
                Line = $i + 1
                Rule = "C: 步骤键缩进 $ind < 6（workflow 语法会直接报错，CI 0 秒失败）"
                Text = $cur.Trim()
            }
        }
    }
}

if ($hits.Count -eq 0 -and -not $script:gateCoverageFailed) {
    if (-not $Quiet) { Write-Host "界面模式检查通过（SameLine 宽度 / 每帧深拷贝 / workflow 缩进）" -ForegroundColor Green }
    exit 0
}

# 扫描范围不完整**必须**让门禁红（第 131 轮）。
#
# 这一点专门写出来，是因为它先被漏掉过一次：我加了这个检查、
# 设了 `$script:gateCoverageFailed`，却没接进上面的判定 ——
# 于是它「跑过了」，而门禁照样绿。又一次「存在 ≠ 生效」。
if ($script:gateCoverageFailed -and $hits.Count -eq 0) {
    Write-Host "界面模式检查失败：扫描范围不完整（见上面的红字）" -ForegroundColor Red
    exit 1
}

Write-Host "界面模式检查发现 $($hits.Count) 处：" -ForegroundColor Red
foreach ($h in $hits) {
    Write-Host ("  {0}:{1}  {2}" -f $h.File, $h.Line, $h.Rule)
    Write-Host ("      {0}" -f $h.Text)
}
exit 1
