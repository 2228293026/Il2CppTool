@echo off
setlocal enabledelayedexpansion
rem Il2CppTool one-click build via NDK ndk-build (no Gradle / no Android SDK).
rem Usage:
rem   build.bat                         use ANDROID_NDK_HOME (or ANDROID_NDK_ROOT)
rem   build.bat D:\android-ndk-r29       explicit NDK path
rem   build.bat APP_ABI=armeabi-v7a      extra make args
cd /d "%~dp0app\src\main" || exit /b 1

set "NDK_HOME=%ANDROID_NDK_HOME%"
if "%NDK_HOME%"=="" set "NDK_HOME=%ANDROID_NDK_ROOT%"

set "ARGS="
if not "%~1"=="" (
    if exist "%~1\ndk-build.cmd" (
        set "NDK_HOME=%~1"
        set "ARGS=%~2 %~3 %~4 %~5 %~6 %~7 %~8 %~9"
    ) else (
        set "ARGS=%*"
    )
)

if "%NDK_HOME%"=="" (
    echo [ERROR] NDK not found. set ANDROID_NDK_HOME or pass the path, e.g.:
    echo   build.bat D:\android-ndk-r29
    exit /b 1
)
if not exist "%NDK_HOME%\ndk-build.cmd" (
    echo [ERROR] ndk-build.cmd not found under %NDK_HOME%
    exit /b 1
)

echo [build] ndk-build : %NDK_HOME%\ndk-build.cmd
echo [build] workdir   : %CD%
echo [build] args      : -j%NUMBER_OF_PROCESSORS% %ARGS%
echo.
call "%NDK_HOME%\ndk-build.cmd" -j%NUMBER_OF_PROCESSORS% %ARGS%
set "BUILD_RC=%ERRORLEVEL%"

if "%BUILD_RC%"=="0" (
    if exist "libs\arm64-v8a\libIl2CppTool.so" (
        echo.
        echo [DONE] artifact: %CD%\libs\arm64-v8a\libIl2CppTool.so
    )
)
exit /b %BUILD_RC%