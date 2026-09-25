#!/usr/bin/env bash
set -euo pipefail
build_dir=${1:?Build directory required}
output_dir=${2:?New output directory required}
tag=${3:?Release tag required}
architectures=${4:-arm64}
[[ "$tag" =~ ^[0-9A-Za-z][0-9A-Za-z._+-]*$ ]] || { echo 'Invalid tag'; exit 1; }
project_root=$(cd "$(dirname "$0")/.." && pwd)
[[ -d "$build_dir/ZcVersionBox.app" ]] || { echo 'Application bundle missing'; exit 1; }
[[ ! -e "$output_dir" ]] || { echo 'Packaging requires a new, empty output path'; exit 1; }
mkdir -p "$output_dir"
app="$output_dir/ZcVersionBox.app"
cp -R "$build_dir/ZcVersionBox.app" "$app"
frameworks="$app/Contents/Frameworks"
mkdir -p "$frameworks"
cp "$project_root/3rdparty/ZcAILib/lib/libZcAiLib.dylib" "$frameworks/libZcAiLib.1.dylib"
install_name_tool -id '@rpath/libZcAiLib.1.dylib' "$frameworks/libZcAiLib.1.dylib"
# Copy the SDK before deployment so its QtNetwork dependency is also resolved.
macdeployqt "$app" -always-overwrite -verbose=1

licenses="$app/Contents/Resources/licenses"
mkdir -p "$licenses"
cp "$project_root/3rdparty/qlementine/LICENSE" "$licenses/Qlementine.txt"
cp "$project_root/3rdparty/qlementine/UPSTREAM.md" "$licenses/"
cp "$project_root/3rdparty/qlementine/LICENSES/"*.txt "$licenses/"
cp "$project_root/LICENSE" "$licenses/ZcVersionBox.txt"

IFS=';' read -r -a slices <<< "$architectures"
for arch in "${slices[@]}"; do
  lipo -verify_arch "$arch" "$app/Contents/MacOS/ZcVersionBox"
  lipo -verify_arch "$arch" "$frameworks/libZcAiLib.1.dylib"
done
if find "$app" -iname '*elawidget*' -o -iname '*zcwidget*' -o -iname '*qlementine*.dylib' | grep -q .; then
  echo 'Unexpected UI runtime in package'; exit 1
fi
# All Mach-O dependencies must resolve within the bundle or macOS system libraries.
while IFS= read -r -d '' binary; do
  file "$binary" | grep -q 'Mach-O' || continue
  for arch in "${slices[@]}"; do lipo -verify_arch "$arch" "$binary"; done
  if otool -L "$binary" | tail -n +2 | awk '{print $1}' | grep -vE '^(@rpath/|@executable_path/|@loader_path/|/System/Library/|/usr/lib/)' | grep -q .; then
    echo "Unbundled dependency: $binary"; otool -L "$binary"; exit 1
  fi
done < <(find "$app" -type f -print0)
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
arch_label=${architectures//;/_}
hdiutil create -volname ZcVersionBox -srcfolder "$app" -ov -format UDZO "$output_dir/ZcVersionBox-$tag-mac-$arch_label.dmg"
