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

if ($hits.Count -eq 0) {
    if (-not $Quiet) { Write-Host "界面模式检查通过（SameLine 宽度 / 每帧深拷贝）" -ForegroundColor Green }
    exit 0
}

Write-Host "界面模式检查发现 $($hits.Count) 处：" -ForegroundColor Red
foreach ($h in $hits) {
    Write-Host ("  {0}:{1}  {2}" -f $h.File, $h.Line, $h.Rule)
    Write-Host ("      {0}" -f $h.Text)
}
exit 1
