#!/usr/bin/env sh
# ============================================================
#  Il2CppTool - 一键构建脚本（ndk-build，无需 Gradle / Android SDK）
#  用法:
#     ./build.sh                    读 $ANDROID_NDK_HOME(或 $ANDROID_NDK_ROOT)
#     ./build.sh /opt/android-ndk-r29     显式指定 NDK 路径
#     ./build.sh APP_ABI=armeabi-v7a,arm64-v8a  追加 make 参数
#  产物: app/src/main/libs/<abi>/libIl2CppTool.so
# ============================================================
set -u
cd "$(dirname "$0")/app/src/main" || exit 1

NDK_HOME="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

# 第一个参数若是 NDK 目录，则作为 NDK 路径并使用后面的参数作 make 参数
if [ "$#" -gt 0 ] && [ -d "$1" ]; then
  NDK_HOME="$1"
  shift
fi

if [ -z "$NDK_HOME" ]; then
  echo "[错误] 未找到 NDK。请设置环境变量 ANDROID_NDK_HOME，或: ./build.sh /path/to/ndk" >&2
  exit 1
fi

# Windows 下 Git Bash 无法直接执行 .cmd 时，改用重定向；优先用可执行的 ndk-build
NDKB=""
if [ -x "$NDK_HOME/ndk-build" ]; then
  NDKB="$NDK_HOME/ndk-build"
elif [ -f "$NDK_HOME/ndk-build.cmd" ]; then
  # Git Bash 里通常能直接运行 .cmd（经 cmd.exe）；不行则由用户改用 build.bat
  NDKB="$NDK_HOME/ndk-build.cmd"
elif [ -x "$NDK_HOME/build/ndk-build" ]; then
  NDKB="$NDK_HOME/build/ndk-build"
else
  echo "[错误] $NDK_HOME 下找不到 ndk-build" >&2
  exit 1
fi

JOBS=$(nproc 2>/dev/null || true)
[ -z "$JOBS" ] || [ "$JOBS" -lt 1 ] && JOBS=4

echo "[build] ndk-build : $NDKB"
echo "[build] 工作目录  : $PWD"
echo "[build] 参数      : -j$JOBS $*"
echo

"$NDKB" -j"$JOBS" "$@"
rc=$?

if [ $rc -eq 0 ] && [ -f "libs/arm64-v8a/libIl2CppTool.so" ]; then
  echo
  echo "[完成] 产物: $PWD/libs/arm64-v8a/libIl2CppTool.so"
fi
exit $rc