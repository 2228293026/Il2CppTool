# 「跨界面」内容的宿主检查。
#
# 背景
#
# 第 62 轮和第 63 轮各抓到一个同族的缺陷：代码本身正确，
# 但**挂错了宿主**，于是功能在正常使用中消失或显示在错误的界面上。
#
#   第 62 轮：关注列表挂在 ImGuiJson 里 → 关掉所有对象 tab 就消失并停止刷新
#   第 63 轮：写入失败横幅挂在 ImGuiJson 里 → 参数错误显示在错误的界面上，
#              而且过期清除也跟着消失 → 一分钟前的报错永远挂着
#
# 共同点：**设置方和「显示 / 清除」必须在同一个、一直都在的地方。**
# ImGuiJson 只有在「有打开的对象 tab」时才会被调用，所以它不是合格的宿主。
#
# 规则（刻意保持很短，好处是不会误报到让人想关掉它）：
#   1. 这两个函数在全树**各只有一个调用点**
#   2. 调用点在 Tool.cpp（工具页），不在 ClassesTab.cpp（对象检视器）
param()

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$errors = @()

# 「唯一调用点」+「必须在 Tool.cpp」—— 两个断言合起来才表达得了「宿主正确」。
$expect = @(
    @{ Func = 'DrawFieldErrorBanner'; Host = 'app/src/main/jni/Tool/Tool.cpp' },
    @{ Func = 'DrawWatches';         Host = 'app/src/main/jni/Tool/Tool.cpp' }
)

$sources = Get-ChildItem app/src/main/jni -Recurse -Include *.cpp -File |
    Where-Object { $_.FullName -notmatch 'imgui|asmjit|Frida|xdl' }

foreach ($e in $expect) {
    $sites = @()
    foreach ($f in $sources) {
        $m = Select-String -Path $f.FullName -Encoding UTF8 -Pattern ([regex]::Escape($e.Func) + '\(\);')
        foreach ($hit in $m) { $sites += $hit }
    }
    if ($sites.Count -ne 1) {
        $errors += "$($e.Func)() 有 $($sites.Count) 个调用点（应恰好 1 个）：" +
        (($sites | ForEach-Object { " $($_.Filename):$($_.LineNumber)" }) -join '')
        continue
    }
    $site = $sites[0]
    $rel = $site.Path.Substring($root.Length).TrimStart('\','/').Replace('\','/')
    if ($rel -ne $e.Host) {
        $errors += "$($e.Func)() 的唯一调用点在 $rel，应在 $($e.Host)" +
        "（宿主必须是**一直都在**的页面；ClassesTab.cpp 里的 ImGuiJson 只在有打开的对象 tab 时才跑）"
    }
    else {
        Write-Host "  [通过] $($e.Func)() 唯一挂在 $rel" -ForegroundColor Green
    }
}

if ($errors.Count -gt 0) {
    Write-Host ""
    Write-Host "跨界面宿主检查未通过：" -ForegroundColor Red
    foreach ($e in $errors) { Write-Host "  - $e" }
    exit 1
}
exit 0
