@echo off
setlocal
if "%~1"=="" (
  echo Usage: build-magpie-native-android.bat OUTPUT_DIR 1>&2
  exit /b 2
)
set "OUT=%~1"
set "ROOT=%~dp0.."
if not defined ANDROID_NDK_HOME if defined ANDROID_NDK_ROOT set "ANDROID_NDK_HOME=%ANDROID_NDK_ROOT%"
if not defined ANDROID_NDK_HOME (
  echo ANDROID_NDK_HOME must point to an installed Android NDK. 1>&2
  exit /b 2
)
if not exist "%ANDROID_NDK_HOME%\build\cmake\android.toolchain.cmake" (
  echo Android NDK CMake toolchain not found under ANDROID_NDK_HOME. 1>&2
  exit /b 2
)
cmake -S "%ROOT%\app\src\main\cpp" -B "%OUT%\cmake-arm64" -G Ninja -DCMAKE_TOOLCHAIN_FILE="%ANDROID_NDK_HOME%\build\cmake\android.toolchain.cmake" -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_shared -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="%OUT%"
if errorlevel 1 exit /b %errorlevel%
cmake --build "%OUT%\cmake-arm64" --parallel
if errorlevel 1 exit /b %errorlevel%
copy /Y "%ANDROID_NDK_HOME%\toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\aarch64-linux-android\libc++_shared.so" "%OUT%\libc++_shared.so" >nul
if errorlevel 1 exit /b %errorlevel%
endlocal
