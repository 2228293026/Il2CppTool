# 为什么需要它（第 142 轮加）
#
# 过去三轮里我**三次**把 `.ci/selftest-gates.ps1` 的 UTF-8 BOM 弄丢了，
# 三次都是：`edit` 工具重写文件时不保留 BOM。
# 三次里有两次我在同一次输出里**已经看到**门禁报红
# （「含 5852 个非 ASCII 字符却没有 BOM」），却读过去了。
#
# 为什么机制本身没起作用：`check-all.ps1` 是要**人手动跑**的，
# 而我是「改完 → 直接 commit」。CI 会红，但那时已经推送了。
# 一条只有手动运行才存在的检查，在「人不可能每次都记得跑」的现实中，
# 约等于没有（第 90 轮那条教训的又一次）。
#
# 所以把它变成**git 自己会触发**的东西。
#
# 代价与取舍
#
#   · 完整自检要十几分钟（含 40 次双向注入验证），每次提交都跑不现实，
#     所以钩子默认只跑**快速项**（-SkipClangTidy -SkipSelfTest，约 10 秒）。
#   · 需要完整验证时用：git commit 时设 IL2CPPTOOL_FULL_GATE=1。
#   · 要跳过（明知会红、且清楚原因）：git commit --no-verify。
#     跳过会被记进 docs/缺陷类清单.md 那一类的记录里 ——
#     **「跳过」必须是个需要解释的动作**，而不是随手一个 -n。

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$full = $env:IL2CPPTOOL_FULL_GATE -eq '1'
$scriptArgs = @('-ExecutionPolicy', 'Bypass', '-File', '.\.ci\check-all.ps1')
if (-not $full) { $scriptArgs += @('-SkipClangTidy', '-SkipSelfTest') }

Write-Host ''
if ($full) {
    Write-Host '[hook] 完整门禁（含 clang-tidy 与 40 条自检，要十几分钟）...' -ForegroundColor Cyan
}
else {
    Write-Host '[hook] 快速门禁（clang-tidy 与自检已跳过；完整验证：$env:IL2CPPTOOL_FULL_GATE=1）' -ForegroundColor Cyan
}

& powershell @scriptArgs
$rc = $LASTEXITCODE

if ($rc -ne 0) {
    Write-Host ''
    Write-Host '[hook] 门禁未通过，**已阻止本次提交**。' -ForegroundColor Red
    Write-Host ''
    Write-Host '  要一起修就直接改，改完 `git add` 再提交即可。'
    Write-Host '  确实需要跳过：git commit --no-verify（并且请在提交说明里写清为什么）。'
    exit 1
}

Write-Host '[hook] 门禁通过。' -ForegroundColor Green
exit 0
