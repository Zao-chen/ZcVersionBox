#!/usr/bin/env bash
set -euo pipefail
build_dir=${1:?Build directory required}
output_dir=${2:?New output directory required}
tag=${3:?Release tag required}
architectures="${4:-arm64;x86_64}"
[[ "$tag" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo 'Invalid tag'; exit 1; }
project_root=$(cd "$(dirname "$0")/.." && pwd)
[[ -d "$build_dir/ZcVersionBox.app" ]] || { echo 'Application bundle missing'; exit 1; }
[[ ! -e "$output_dir" ]] || { echo 'Packaging requires a new, empty output path'; exit 1; }
mkdir -p "$output_dir"
app="$output_dir/ZcVersionBox.app"
cp -R "$build_dir/ZcVersionBox.app" "$app"
frameworks="$app/Contents/Frameworks"
mkdir -p "$frameworks"
[[ -f "$frameworks/libZcAiLib.1.dylib" ]] || { echo 'Built AI SDK missing from bundle'; exit 1; }
install_name_tool -id '@rpath/libZcAiLib.1.dylib' "$frameworks/libZcAiLib.1.dylib"
# Copy the SDK before deployment so its QtNetwork dependency is also resolved.
macdeployqt "$app" -always-overwrite -verbose=1

# Some macdeployqt versions can drop the leading '@' when rewriting the
# install name of a bundled non-Qt dylib. Repair only that known malformed
# spelling before the dependency gate below; unrelated absolute paths must
# still fail the package.
while IFS= read -r -d '' binary; do
  file "$binary" | grep -q 'Mach-O' || continue
  if otool -L "$binary" | awk '{print $1}' | grep -Fxq 'rpath/libZcAiLib.1.dylib'; then
    install_name_tool -change 'rpath/libZcAiLib.1.dylib' \
      '@rpath/libZcAiLib.1.dylib' "$binary"
  fi
done < <(find "$app" -type f -print0)

licenses="$app/Contents/Resources/licenses"
mkdir -p "$licenses"
cp "$project_root/3rdparty/qlementine/LICENSE" "$licenses/Qlementine.txt"
cp "$project_root/3rdparty/efsw/LICENSE" "$licenses/efsw.txt"
cp "$project_root/3rdparty/ZcAILib/LICENSE" "$licenses/ZcAILib.txt"
cp "$project_root/3rdparty/ZcAILib/UPSTREAM.md" "$licenses/ZcAILib-upstream.md"
cp "$project_root/3rdparty/efsw/UPSTREAM.md" "$licenses/efsw-upstream.md"
cp "$project_root/3rdparty/qlementine/UPSTREAM.md" "$licenses/"
cp "$project_root/3rdparty/qlementine/LICENSES/"*.txt "$licenses/"
cp "$project_root/LICENSE" "$licenses/ZcVersionBox.txt"

IFS=';' read -r -a slices <<< "$architectures"
for arch in "${slices[@]}"; do
  lipo "$app/Contents/MacOS/ZcVersionBox" -verify_arch "$arch"
  lipo "$frameworks/libZcAiLib.1.dylib" -verify_arch "$arch"
done
if find "$app" -iname '*elawidget*' -o -iname '*zcwidget*' -o -iname '*qlementine*.dylib' | grep -q .; then
  echo 'Unexpected UI runtime in package'; exit 1
fi
# All Mach-O dependencies must resolve within the bundle or macOS system libraries.
while IFS= read -r -d '' binary; do
  file "$binary" | grep -q 'Mach-O' || continue
  for arch in "${slices[@]}"; do lipo "$binary" -verify_arch "$arch"; done
  if otool -L "$binary" \
      | awk 'NR == 1 || $0 ~ / \(architecture [^)]+\):$/ { next } { print $1 }' \
      | grep -vE '^(@rpath/|@executable_path/|@loader_path/|/System/Library/|/usr/lib/)' \
      | grep -q .; then
    echo "Unbundled dependency: $binary"; otool -L "$binary"; exit 1
  fi
done < <(find "$app" -type f -print0)
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
arch_label=${architectures//;/_}
hdiutil create -volname ZcVersionBox -srcfolder "$app" -ov -format UDZO "$output_dir/ZcVersionBox-$tag-mac-$arch_label.dmg"
