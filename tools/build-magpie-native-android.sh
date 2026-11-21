#!/data/data/com.termux/files/usr/bin/sh
set -eu
OUT=${1:?usage: build-magpie-native-android.sh OUTPUT_DIR}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK="$ROOT/native-sdk/proof"
FSR="$ROOT/graphics/fsr1"
mkdir -p "$OUT"
NDK=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [ -n "$NDK" ] && [ -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  cmake -S "$ROOT/app/src/main/cpp" -B "$OUT/cmake-arm64" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 \
    -DANDROID_STL=c++_shared -DCMAKE_LIBRARY_OUTPUT_DIRECTORY="$OUT"
  cmake --build "$OUT/cmake-arm64" --parallel
  HOST_TAG=${ANDROID_NDK_HOST_TAG:-}
  if [ -z "$HOST_TAG" ]; then
    case "$(uname -s)" in
      Linux*) HOST_TAG=linux-x86_64 ;;
      Darwin*) HOST_TAG=darwin-x86_64 ;;
      MINGW*|MSYS*|CYGWIN*) HOST_TAG=windows-x86_64 ;;
      *) echo "Set ANDROID_NDK_HOST_TAG for this build host" >&2; exit 1 ;;
    esac
  fi
  cp "$NDK/toolchains/llvm/prebuilt/$HOST_TAG/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$OUT/libc++_shared.so"
  exit 0
fi
CXX=${CXX:-clang++}
COMMON="-std=c++17 -O2 -fPIC -fvisibility=hidden -Wall -Wextra -Wpedantic"
INCLUDES="-I$PREFIX/include -I$SDK/include -I$FSR/include"
LINK="-L$OUT -Wl,-rpath-link,$OUT -Wl,--no-undefined"
SYSTEM_LIBS="-L/system/lib64"
# In a normal desktop build, Android Gradle/NDK CMake is preferred. This script
# is also the reproducible Termux path where the Android SDK's x86 host tools
# cannot execute; Termux clang targets arm64 Android and links device stubs.
$CXX $COMMON $INCLUDES -shared "$SDK/src/core.cpp" -Wl,-soname,libmagpie_core.so -o "$OUT/libmagpie_core.so"
$CXX $COMMON $INCLUDES -shared "$SDK/src/fsr.cpp" "$FSR/src/image.cpp" "$FSR/src/easu.cpp" "$FSR/src/rcas.cpp" $LINK -lmagpie_core -Wl,-soname,libmagpie_fsr.so -o "$OUT/libmagpie_fsr.so"
$CXX $COMMON $INCLUDES -shared "$SDK/src/vulkan_probe.cpp" $SYSTEM_LIBS -lvulkan -Wl,-soname,libmagpie_vulkan.so -o "$OUT/libmagpie_vulkan.so"
$CXX $COMMON $INCLUDES -shared "$SDK/src/gles_probe.cpp" $SYSTEM_LIBS -lEGL -lGLESv3 -Wl,-soname,libmagpie_gles.so -o "$OUT/libmagpie_gles.so"
$CXX $COMMON $INCLUDES -shared "$ROOT/app/src/main/cpp/native_fsr.cpp" $LINK -lmagpie_core -lmagpie_fsr -lmagpie_vulkan -lmagpie_gles $SYSTEM_LIBS -landroid -ljnigraphics -Wl,-soname,libmagpie_android_jni.so -o "$OUT/libmagpie_android_jni.so"
cp "$PREFIX/lib/libc++_shared.so" "$OUT/libc++_shared.so"
