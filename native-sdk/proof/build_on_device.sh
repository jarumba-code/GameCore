#!/data/data/com.termux/files/usr/bin/sh
set -eu
repo_dir=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
build_dir=${1:-"${TMPDIR:-/tmp}/magpie-native-sdk-proof"}
cmake -S "$repo_dir/native-sdk/proof" -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" --parallel 2
ctest --test-dir "$build_dir" --output-on-failure
file "$build_dir"/libmagpie_core.so "$build_dir"/libmagpie_fsr.so "$build_dir"/libmagpie_vulkan.so "$build_dir"/libmagpie_gles.so
"$build_dir/magpie_sdk_smoke"
