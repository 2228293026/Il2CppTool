# 结构完整性：花括号必须配平。
#
# 为什么需要它
#
# 第 74 轮改撤销功能时，用行号搬代码（插入多行之后行号全部位移），
# 误删了一个 `});`，编译器只说「后面全乱了」
# （`FilterState not allowed inside a function`、`namespaces can only be
# defined in global or namespace scope`）—— 报错位置离真正出错的地方
# 隔了 500 行。最后是**逐函数比深度**才定位到「布尔分支少一个 `}`」。
#
# 这类损坏的共同点是：**编译错误出现在远处，现场却在近处**。
# 而配平检查能直接指出「这个文件少一个右括号」，
# 比从报错信息往回推快一个数量级。
#
# 为什么要剥离字符串和注释
#
# 项目里带了 asmjit / imgui / Roboto 等第三方代码，它们大量在字符串里
# 用花括号（`"{#%u}"`、`" {Done}"`）。朴素计数在这些文件上是错的 ——
# 实测 imgui_demo.cpp 天生就差 1。所以必须先剥掉注释和字面量。
param()

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

# 只覆盖**我们自己写的**代码：第三方目录（asmjit/imgui/roboto）
# 既有宏生成的怪结构，也不在我们的控制范围内。
$dirs = @(
    (Join-Path $root 'app/src/main/jni/Tool'),
    (Join-Path $root 'app/src/main/jni/Il2cpp'),
    (Join-Path $root 'app/src/main/jni/Menu'),
    (Join-Path $root 'app/src/main/jni/Includes')
)
$files = foreach ($d in $dirs) {
    if (Test-Path $d) { Get-ChildItem $d -Recurse -Include *.cpp, *.h -File }
}
$rootCpp = Join-Path $root 'app/src/main/jni'
if (Test-Path $rootCpp) {
    $files += Get-ChildItem $rootCpp -Filter *.cpp -File
}

# 逐字符扫描一遍，剥掉：行注释、块注释、字符串字面量、字符字面量。
# 一次线性扫描搞定，不依赖正则（正则处理「字符串里出现 // 」这类情况
# 会出错，而本项目恰好有 `logf("http://...")` 这种写法）。
function Strip-NonCode([string]$text) {
    $sb = [Text.StringBuilder]::new($text.Length)
    $i = 0
    $n = $text.Length
    while ($i -lt $n) {
        $c = $text[$i]
        $c2 = if ($i + 1 -lt $n) { $text[$i + 1] } else { [char]0 }
        if ($c -eq '/' -and $c2 -eq '/') {
            while ($i -lt $n -and $text[$i] -ne "`n") { $i++ }
            continue
        }
        if ($c -eq '/' -and $c2 -eq '*') {
            $i += 2
            while ($i + 1 -lt $n -and -not ($text[$i] -eq '*' -and $text[$i + 1] -eq '/')) { $i++ }
            $i += 2
            continue
        }
        if ($c -eq '"' -or $c -eq "'") {
            $quote = $c
            $i++
            while ($i -lt $n) {
                if ($text[$i] -eq '\') { $i += 2; continue }
                if ($text[$i] -eq $quote) { $i++; break }
                # 跨行字符串（raw string）直接放行到本行末，避免误吞后面所有代码
                if ($text[$i] -eq "`n") { break }
                $i++
            }
            # 用空格替掉，保持列数不变，错误位置仍然是原来那一行
            [void]$sb.Append('""')
            continue
        }
        [void]$sb.Append($c)
        $i++
    }
    return $sb.ToString()
}

$bad = 0
foreach ($f in ($files | Sort-Object FullName -Unique)) {
    $text = [IO.File]::ReadAllText($f.FullName)
    $code = Strip-NonCode $text
    $open = ([regex]::Matches($code, '\{')).Count
    $close = ([regex]::Matches($code, '\}')).Count
    if ($open -ne $close) {
        Write-Host ("  [不平衡] {0}  开={1} 闭={2} 差={3}" -f $f.Name, $open, $close, ($open - $close)) `
            -ForegroundColor Red
        # 指出最后一个**深度不为零**的位置，方便直接定位
        $lines = $code -split "`n"
        $depth = 0
        for ($i = 0; $i -lt $lines.Count; $i++) {
            $depth += ([regex]::Matches($lines[$i], '\{')).Count
            $depth -= ([regex]::Matches($lines[$i], '\}')).Count
        }
        Write-Host ("            文件末尾深度 = {0}" -f $depth) -ForegroundColor Red
        $bad++
    }
}

Write-Host ""
if ($bad -gt 0) {
    Write-Host "$bad 个文件的花括号不配平 —— 结构已经损坏。" -ForegroundColor Red
    exit 1
}
Write-Host ("结构完整（{0} 个文件的花括号配平）。" -f ($files | Sort-Object FullName -Unique).Count) `
    -ForegroundColor Green
exit 0
