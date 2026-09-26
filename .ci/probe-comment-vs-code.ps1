# 「注释说的」vs「代码做的」——按**函数**比对。
#
# 背景
#
# 第 44/49 轮：界面上出现 literally 的 ** 星号，因为 ImGui 不解析 markdown，
#             而我一直在渲染字符串里用 ** 强调。
# 第 68 轮：一条专门抓「注释声称 vs 代码实际」的门禁，**被函数里那段
#             解释性的注释骗过去了** —— 那段注释本身就写着
#             ensureIfValueType，所以把真正的调用删掉，检查依然绿。
#
# 两件事的共同根源：
#
#     **对文本搜索来说，注释里的字和代码里的调用没有区别。**
#
# 这条检查把这件事变成可查的：对每个函数，收集**代码行**里出现过的
# 标识符；再逐条看**注释行**里提到的标识符，如果注释提到的东西
# 这个函数体里根本没调用过，就报出来。
#
# 报出来不代表一定是 bug —— 注释可能在解释「为什么不这么做」，
# 那类会明确出现「不要 / 旧代码 / 原来 / 不能」等否定词。先看噪声有多大，
# 再决定要不要当门禁。
param()

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$dirs = @(
    (Join-Path $root 'app/src/main/jni/Tool'),
    (Join-Path $root 'app/src/main/jni/Il2cpp'),
    (Join-Path $root 'app/src/main/jni/Menu'),
    (Join-Path $root 'app/src/main/jni/Includes')
)
$files = foreach ($d in $dirs) {
    if (Test-Path $d) { Get-ChildItem $d -Recurse -Include *.cpp, *.h -File }
}

# 注释里提到「做了什么」时，习惯上会带上这些词。用它们缩小范围，
# 否则「注释提到了某个类型名」会产生海量噪音。
$verbPattern = '\b(Ensure|Parse|Write|Read|Request|Resolve|Free|Record|Add|Remove|Clear|Update|Init|Is|Has|Get|Set|Save|Load|Draw|Report|Release|Refresh|Register|Collect|Apply|Handle|Process|Build|Make|Create|Query|Lookup|Find|Scan|Dump|Patch|Freeze|Retry|Abort|Cancel|Reset|Start|Stop|Spawn|Wait|Lock|Unlock|Begin|End)\w*\b'

$findings = @()

foreach ($f in $files) {
    $lines = Get-Content -Encoding UTF8 $f.FullName
    # 粗略切分函数体：顶格 '}' 结束一段
    $inFunc = $false
    $funcStart = -1
    $bodyCode = New-Object System.Collections.Generic.HashSet[string]
    $bodyComments = @()

    for ($i = 0; $i -lt $lines.Count; $i++) {
        $raw = $lines[$i]
        $t = $raw.Trim()

        if (-not $inFunc) {
            if ($t -match '^[A-Za-z_].*\(' -and $t -notmatch '^\s*//' ) {
                # 可能是函数/控制结构头。看它后面有没有 {
                $n = $i
                while ($n -lt $lines.Count -and $lines[$n].Trim() -ne '{' -and $lines[$n].Trim() -notmatch ';$') { $n++ }
                if ($n -lt $lines.Count -and $lines[$n].Trim() -eq '{') {
                    $inFunc = $true; $funcStart = $i
                    $bodyCode = New-Object System.Collections.Generic.HashSet[string]
                    $bodyComments = @()
                }
            }
            continue
        }

        if ($t -eq '}') {
            # 结算这一段
            foreach ($c in $bodyComments) {
                foreach ($m in [regex]::Matches($c.Text, $verbPattern)) {
                    $id = $m.Value
                    if ($bodyCode.Contains($id)) { continue }
                    $findings += [pscustomobject]@{
                        File = $f.Name; Line = $c.Line; Id = $id
                        Text = ($c.Text.Substring(0, [Math]::Min(72, $c.Text.Length)))
                    }
                }
            }
            $inFunc = $false
            continue
        }

        if ($t -match '^(//|\*|/\*)') { $bodyComments += [pscustomobject]@{ Line = $i + 1; Text = $t }; continue }
        # 代码行：抽出所有标识符
        foreach ($m in [regex]::Matches($t, '\b[A-Za-z_]\w{3,}\b')) { [void]$bodyCode.Add($m.Value) }
    }
}

Write-Host "注释提到、但所在函数体里没调用过的标识符：$($findings.Count) 处" -ForegroundColor Cyan
$byId = $findings | Group-Object Id | Sort-Object Count -Descending | Select-Object -First 25
foreach ($g in $byId) {
    $s = $g.Group[0]
    Write-Host ("  {0,-22} x{1,-3} {2}:{3}" -f $g.Name, $g.Count, $s.File, $s.Line) -ForegroundColor DarkGray
    Write-Host ("      {0}" -f $s.Text) -ForegroundColor DarkGray
}
exit 0
