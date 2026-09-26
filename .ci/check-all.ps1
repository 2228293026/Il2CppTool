# 提交前的全部检查，一个入口。
#
# 为什么要合并成一个脚本
#
# 到第 59 轮为止，检查散落在各处：手敲的命令、写进提交信息的 PowerShell 片段、
# CI 的独立步骤。每加一个检查就要记得在**两个地方**都加上 ——
# 而第 58 轮的教训恰恰是「我改了 A 路径，没改 B 路径」。
#
# 现在只有这一个入口，CI 和本地跑的是**同一份**逻辑。
param(
    [switch]$SkipClangTidy,
    [switch]$SkipSelfTest
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$failed = @()

# 注意：检查体里**不能**用 `exit`。在 `& $Body` 里 exit 会终止整个脚本，
# 后面几项检查根本不会跑 —— 而最后一行还留下一个"成功"的退出码，
# 看起来一切正常。检查体只设置 $script:rc，由外层决定去留。
$script:rc = 0

function Invoke-Check {
    param([string]$Name, [scriptblock]$Body)
    Write-Host ""
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    $script:rc = 0
    & $Body
    if ($script:rc -ne 0) {
        $script:failed += $Name
        Write-Host "  [失败] $Name" -ForegroundColor Red
    }
    else {
        Write-Host "  [通过] $Name" -ForegroundColor Green
    }
}

Invoke-Check '界面模式（SameLine 宽度 / 每帧深拷贝 / workflow 缩进）' {
    powershell -ExecutionPolicy Bypass -File .\.ci\check-ui-patterns.ps1 -Quiet
    if ($LASTEXITCODE -ne 0) { $script:rc = 1 }
}

if (-not $SkipClangTidy) {
    Invoke-Check 'clang-tidy 静态分析' {
        # NDK 路径：CI 里由 workflow 设好，本地没设才用默认路径。
        # 写死绝对路径的话，CI 上会用错的路径（那里根本没有 D:\）。
        if (-not $env:NDK_PATH) { $env:NDK_PATH = 'D:\android-ndk-r29' }

        # run-clang-tidy.ps1 内部是 $ErrorActionPreference = 'Stop'，
        # 而 clang-tidy 会把进度（[1/10] Processing...）写到 **stderr** ——
        # PowerShell 把原生命令的 stderr 变成错误记录，'Stop' 下直接抛异常，
        # 于是「有告警」和「脚本崩了」变成同一件事，退出码 1。
        #
        # 所以在子进程里把 stderr 合并进 stdout，并临时放宽偏好，
        # 只按**退出码**判断成败。
        $prev = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        try {
            $out = & powershell -ExecutionPolicy Bypass -File .\.ci\run-clang-tidy.ps1 2>&1 | Out-String
            $code = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $prev
        }
        # 只回显结论那几行，clang-tidy 的逐文件进度刷屏没意义
        ($out -split "`r?`n") | Where-Object { $_ -match '告警:|静态检查|通过|新增' } | ForEach-Object { "    $_" }
        if ($code -ne 0) { $script:rc = 1 }
    }
}

# 最后一步：验证**上面那些检查自己**抓得到它们要防的东西。
#
# 第 64 轮的一条检查从写下来起就是死的，却一直显示「通过」；
# 第 63 轮的一条断言从未执行，同样显示「通过」。
# 一个从不失败的检查和没有检查，在输出上无法区分 ——
# 所以必须真的去制造一次违规，看它会不会红。
#
# 这一步会临时改几个文件再改回来，耗时几十秒，所以可以 -SkipSelfTest 跳过。
if (-not $SkipSelfTest) {
    Invoke-Check '门禁自检（每条检查都必须能红）' {
        powershell -ExecutionPolicy Bypass -File .\.ci\selftest-gates.ps1
        if ($LASTEXITCODE -ne 0) { $script:rc = 1 }
    }
}

Invoke-Check '结构完整（花括号配平）' {
    powershell -ExecutionPolicy Bypass -File .\.ci\check-structure.ps1
    if ($LASTEXITCODE -ne 0) { $script:rc = 1 }
}
Invoke-Check '跨界面内容的宿主（挂错宿主 = 功能消失）' {
    powershell -ExecutionPolicy Bypass -File .\.ci\check-hosts.ps1
    if ($LASTEXITCODE -ne 0) { $script:rc = 1 }
}

Invoke-Check '源码编码（无 U+FFFD / 无控制字符）' {
    $bad = 0
    Get-ChildItem app/src/main/jni -Recurse -Include *.cpp, *.h -File | ForEach-Object {
        $t = [IO.File]::ReadAllText($_.FullName)
        if ($t -match "\uFFFD" -or $t -match "[\x00-\x08\x0B\x0C\x0E-\x1F]") {
            Write-Host "    编码问题: $($_.Name)"
            $bad++
        }
    }
    if ($bad -gt 0) { $script:rc = 1 }
}

Invoke-Check '含非 ASCII 的 PowerShell 脚本必须带 UTF-8 BOM' {
    # Windows PowerShell 5.1 读无 BOM 的 .ps1 会按 ANSI 解码，
    # 中文注释会把语法彻底搞坏。踩过两次了。
    $bad = 0
    foreach ($s in Get-ChildItem .ci, . -Filter *.ps1 -File -ErrorAction SilentlyContinue) {
        $b = [IO.File]::ReadAllBytes($s.FullName)
        $hasBom = ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
        if ($hasBom) { continue }
        if ($true) {
            # 只对**含非 ASCII 字符**的脚本提要求：全 ASCII 的脚本
            # 没有 BOM 也完全正常（build.ps1 就是），一视同仁只会产生噪音。
            $text = [Text.Encoding]::UTF8.GetString($b)
            $nonAscii = 0
            foreach ($c in $text.ToCharArray()) { if ([int]$c -gt 127) { $nonAscii++ } }
            if ($nonAscii -gt 0) { Write-Host "    含 $nonAscii 个非 ASCII 字符却没有 BOM: $($s.Name)"; $bad++ }
        }
    }
    if ($bad -gt 0) { $script:rc = 1 }
}

Write-Host ""
if ($failed.Count -gt 0) {
    Write-Host "以下检查未通过：$($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "全部检查通过。" -ForegroundColor Green
exit 0
