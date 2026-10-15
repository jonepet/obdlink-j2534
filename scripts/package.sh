#!/bin/sh
set -eu

version=$(sed -n 's/^project(obdlink_j2534 VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
name="obdlink-stn-j2534-${version}"
stage="dist/${name}"

for arch in i686:win32 x86_64:win64; do
    toolchain="cmake/mingw-w64-${arch%%:*}.cmake"
    build="build/${arch##*:}"
    cmake -S . -B "${build}" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="${toolchain}"
    cmake --build "${build}"
done

rm -rf "${stage}" "dist/${name}.zip"
mkdir -p "${stage}"
cp build/win32/obdstn32.dll build/win64/obdstn64.dll scripts/register.ps1 README.md LICENSE "${stage}/"
(cd dist && cmake -E tar cf "${name}.zip" --format=zip "${name}")
(cd dist && sha256sum "${name}.zip" > "${name}.zip.sha256")
echo "dist/${name}.zip"
