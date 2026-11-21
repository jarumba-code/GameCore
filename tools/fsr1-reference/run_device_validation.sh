#!/data/data/com.termux/files/usr/bin/sh
# Reproducible on-device Magpie M2 test path (Termux/Android ARM64).
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PREFIX=${PREFIX:-/data/data/com.termux/files/usr}
WORK=$(mktemp -d "$PREFIX/tmp/magpie-m2.XXXXXX")
trap 'rm -rf "$WORK"' EXIT HUP INT TERM
cmake -S "$ROOT/tools/fsr1-reference" -B "$WORK/cpu" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$WORK/cpu"
ctest --test-dir "$WORK/cpu" --output-on-failure
"$PREFIX/bin/glslc" -I"$ROOT/third_party/amd-fsr1" "$ROOT/graphics/fsr1/vulkan/easu.comp" -o "$WORK/easu.spv"
"$PREFIX/bin/glslc" -I"$ROOT/third_party/amd-fsr1" "$ROOT/graphics/fsr1/vulkan/rcas.comp" -o "$WORK/rcas.spv"
"$PREFIX/bin/spirv-val" "$WORK/easu.spv"
"$PREFIX/bin/spirv-val" "$WORK/rcas.spv"
"$PREFIX/bin/c++" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wno-missing-field-initializers \
  -I"$ROOT/graphics/fsr1/include" -I"$PREFIX/include" \
  "$ROOT/tools/fsr1-reference/vulkan_runner.cpp" \
  "$ROOT/graphics/fsr1/src/image.cpp" "$ROOT/graphics/fsr1/src/easu.cpp" "$ROOT/graphics/fsr1/src/rcas.cpp" \
  -L/system/lib64 -lvulkan -o "$WORK/fsr1_vulkan"
"$PREFIX/bin/c++" -std=c++17 -O2 -Wall -Wextra -Wpedantic \
  -I"$PREFIX/include" "$ROOT/tools/fsr1-reference/gles_capability_probe.cpp" \
  -L/system/lib64 -lEGL -lGLESv2 -o "$WORK/gles_probe"
"$WORK/gles_probe"
mkdir -p "$WORK/gles_headers"
cp "$ROOT/third_party/amd-fsr1/ffx_a.h" "$WORK/gles_headers/ffx_a.h"
# GLSL ES requires unsigned literals here; adapt only the temporary compile copy.
sed 's/con3\[2\]=con3\[3\]=0;/con3[2]=con3[3]=0u;/; s/con\[2\]=0;/con[2]=0u;/; s/con\[3\]=0;/con[3]=0u;/' \
  "$ROOT/third_party/amd-fsr1/ffx_fsr1.h" > "$WORK/gles_headers/ffx_fsr1.h"
"$PREFIX/bin/glslc" -fshader-stage=compute --target-env=opengl -I"$WORK/gles_headers" \
  "$ROOT/graphics/fsr1/gles/easu.comp" -o "$WORK/easu_es.spv"
"$PREFIX/bin/glslc" -fshader-stage=compute --target-env=opengl -I"$WORK/gles_headers" \
  "$ROOT/graphics/fsr1/gles/rcas.comp" -o "$WORK/rcas_es.spv"
"$PREFIX/bin/spirv-val" --target-env opengl4.5 "$WORK/easu_es.spv"
"$PREFIX/bin/spirv-val" --target-env opengl4.5 "$WORK/rcas_es.spv"
for name in diagonal checker shape; do
  "$WORK/fsr1_vulkan" \
    "$ROOT/tools/fsr1-reference/testdata/${name}_input_160x90.ppm" \
    "$WORK/${name}_amd_fsr1_gpu.ppm" "$WORK/easu.spv" "$WORK/rcas.spv"
done
python3 "$ROOT/tools/fsr1-reference/validate_gpu_outputs.py" "$ROOT/tools/fsr1-reference" "$WORK"
