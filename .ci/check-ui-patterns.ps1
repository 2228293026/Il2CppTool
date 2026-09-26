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
    (Join-Path $root 'app/src/main/jni/Includes')
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

$hits = @()

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

if ($hits.Count -eq 0) {
    if (-not $Quiet) { Write-Host "界面模式检查通过（SameLine 宽度 / 每帧深拷贝 / workflow 缩进）" -ForegroundColor Green }
    exit 0
}

Write-Host "界面模式检查发现 $($hits.Count) 处：" -ForegroundColor Red
foreach ($h in $hits) {
    Write-Host ("  {0}:{1}  {2}" -f $h.File, $h.Line, $h.Rule)
    Write-Host ("      {0}" -f $h.Text)
}
exit 1
