#!/usr/bin/env bash
set -euo pipefail
build_dir=$(realpath "${1:?Build directory required}")
output_dir=${2:?New validation output directory required}
[[ ! -e "$output_dir" ]] || { echo 'Validation output directory must be new'; exit 1; }
qt_libs=$(qmake -query QT_INSTALL_LIBS)
qt_plugins=$(qmake -query QT_INSTALL_PLUGINS)
project_root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$output_dir/validation/lib" "$output_dir/validation/plugins/platforms"
output_dir=$(realpath "$output_dir")
validation="$output_dir/validation"
for program in zc_tests zc_backup_tests; do
    install -m 755 "$build_dir/tests/$program" "$validation/$program"
    # A clean runtime container must not resolve dependencies from the build kit.
    patchelf --remove-rpath "$validation/$program"
done
cp -L "$qt_libs/libQt6Test.so.6" "$validation/lib/"
cp "$qt_plugins/platforms/libqoffscreen.so" "$validation/plugins/platforms/"
cp "$project_root/tests/linux/run-desktop-smoke.sh" "$validation/"
tar -C "$output_dir" -czf "$output_dir/validation.tar.gz" validation
