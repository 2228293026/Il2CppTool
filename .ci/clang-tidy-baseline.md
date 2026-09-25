# clang-tidy baseline 说明

`run-clang-tidy.ps1` 的门禁策略是「**新增告警 = 失败**」。这个文件记录当前
已知的存量告警，脚本会把它们从「新问题」里排除。

告警按 `文件|warning: 描述 [check]` 归一化（**去掉行号**）—— 代码一动行号就变，
不该因此重新报一遍。

## 重新生成

确认存量告警无害、或修完一批想收缩基线时：

```powershell
$env:NDK_PATH = 'D:\android-ndk-r29'
.\.ci\run-clang-tidy.ps1 -WriteBaseline
```

## 存量告警为什么留着

大多数是噪音或与本项目场景无关：

- `cert-err58-cpp`（静态初始化可能抛异常）—— 全局 `ImGuiTextBuffer` /
  `std::vector` / `std::regex` 之类的惯用法。真要改成函数内 static 只是形式，
  且 Android 启动阶段 OOM 本来也无解。
- `bugprone-narrowing-conversions` —— 计数用的 `int`，UI 坐标 `int -> float`。
- `cert-err33-c`（忽略 `fclose` / `sprintf` 返回值）—— 已知无害。
- `cert-dcl50-cpp`（C 风格可变参函数）—— 日志宏就是要 printf 风格。
- `bugprone-branch-clone`（`if (M) LOGPTR(...) else LOGE(...)`）—— 两个分支
  调的是不同宏，clang-tidy 只看调用形状，误报。
- `bugprone-easily-swappable-parameters` —— 已在脚本里直接关掉。
- `cert-msc30-c` / `rand()` —— ESP 对象的随机颜色，无安全性要求。

## 值得后续处理的

这些是真问题，只是暂时在基线里：

- `bugprone-macro-parentheses`、`bugprone-suspicious-semicolon`（`Il2cpp.cpp`）
- `bugprone-switch-missing-default-case`（`Il2cpp.cpp`）
- `bugprone-reserved-identifier`（`_il2cpp_type_is_byref`，与上游 il2cpp 头一致）
- `bugprone-empty-catch`（故意吞掉）
- `bugprone-nondeterministic-pointer-iteration-order`（按指针排序方法表，
  顺序不影响正确性）
