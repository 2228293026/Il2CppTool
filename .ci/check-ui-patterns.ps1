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

$hits = @()

foreach ($f in $files) {
    $lines = Get-Content $f.FullName
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
    $lines = Get-Content $f.FullName
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
    $lines = Get-Content $f.FullName
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
    $wl = Get-Content $workflow
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
