#!/usr/bin/env bash
set -euo pipefail
build_dir=$(cd "${1:?Build directory required}" && pwd)
output_dir=${2:?New output directory required}
tag=${3:?Release tag required}
[[ $(uname -m) == x86_64 ]] || { echo 'This package targets x86_64'; exit 1; }
[[ "$tag" =~ ^v?[0-9]+\.[0-9]+\.[0-9]+([.+~-][0-9A-Za-z.+~-]+)?$ ]] || { echo 'Invalid Debian release version'; exit 1; }
[[ ! -e "$output_dir" ]] || { echo 'Packaging requires a new output directory'; exit 1; }
[[ -f "$build_dir/LinuxCPackConfig.cmake" ]] || { echo 'Configure with -DZCVERSIONBOX_DEPLOY_LINUX=ON'; exit 1; }
for tool in cmake cpack dpkg-shlibdeps file readelf desktop-file-validate; do
    command -v "$tool" >/dev/null || { echo "Missing packaging tool: $tool"; exit 1; }
done
project_root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
payload="$output_dir/payload"
app="$payload/opt/zcversionbox"
cmake --install "$build_dir" --prefix "$app"
mkdir -p "$payload/usr/bin" "$payload/usr/share/applications" "$payload/usr/share/icons/hicolor/512x512/apps"
install -m 755 "$project_root/scripts/linux/zcversionbox" "$payload/usr/bin/zcversionbox"
install -m 644 "$project_root/scripts/linux/com.zc.versionbox.desktop" "$payload/usr/share/applications/"
install -m 644 "$project_root/res/img/logo_512.png" "$payload/usr/share/icons/hicolor/512x512/apps/zcversionbox.png"
desktop-file-validate "$payload/usr/share/applications/com.zc.versionbox.desktop"

for required in bin/ZcVersionBox lib/libZcAiLib.so.1 lib/libQt6Core.so.6 \
    lib/libQt6Network.so.6 lib/libQt6Svg.so.6 lib/libQt6Widgets.so.6 \
    plugins/platforms/libqxcb.so plugins/platforms/libqwayland-generic.so \
    plugins/imageformats/libqsvg.so plugins/tls/libqopensslbackend.so; do
    [[ -f "$app/$required" ]] || { echo "Missing runtime: $required"; exit 1; }
done
while IFS= read -r -d '' binary; do
    readelf -h "$binary" >/dev/null 2>&1 || continue
    readelf -h "$binary" | grep -q 'Advanced Micro Devices X86-64' || { echo "Wrong architecture: $binary"; exit 1; }
    dependencies=$(LD_LIBRARY_PATH="$app/lib" ldd "$binary")
    if [[ "$dependencies" == *'not found'* ]]; then echo "$dependencies"; exit 1; fi
    if readelf -d "$binary" | grep -E '(RUNPATH|RPATH)' | grep -qE '/workspace/|/home/|/opt/Qt/'; then
        echo "Build path leaked into runtime: $binary"; exit 1
    fi
done < <(find "$app" -type f -print0)
if find "$app" -type f -name 'zc_*tests*' | grep -q .; then echo 'Tests must not ship in the application'; exit 1; fi

licenses="$app/licenses"
mkdir -p "$licenses"
for dependency in ZcAILib efsw qlementine qwindowkit; do
    cp "$project_root/3rdparty/$dependency/LICENSE" "$licenses/$dependency.txt"
    if [[ -f "$project_root/3rdparty/$dependency/UPSTREAM.md" ]]; then
        cp "$project_root/3rdparty/$dependency/UPSTREAM.md" "$licenses/$dependency-upstream.md"
    fi
done
cp "$project_root/3rdparty/qlementine/LICENSES/"*.txt "$licenses/"
cp "$project_root/LICENSE" "$licenses/ZcVersionBox.txt"
cp "$project_root/3rdparty/qt-licenses/"*.txt "$licenses/"
cp "$project_root/3rdparty/qt-licenses/UPSTREAM.md" "$licenses/Qt-runtime-upstream.md"

ZC_LINUX_PAYLOAD="$payload" ZC_PACKAGE_TAG="$tag" ZC_PACKAGE_VERSION="${tag#v}" \
    cpack --config "$build_dir/LinuxCPackConfig.cmake" -B "$output_dir"
package="$output_dir/ZcVersionBox-$tag-linux-amd64.deb"
[[ -f "$package" ]] || { echo 'Debian package missing'; exit 1; }
dpkg-deb --info "$package"
