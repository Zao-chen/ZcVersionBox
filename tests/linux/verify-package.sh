#!/usr/bin/env bash
set -euo pipefail
# This script intentionally installs/purges packages and must only run in a fresh container.
[[ -f /.dockerenv && $(id -u) == 0 ]] || { echo 'Use a disposable runtime Docker container as root'; exit 1; }
package=$(realpath "${1:?Application DEB required}")
validation_archive=$(realpath "${2:?Validation archive required}")
mkdir -p "${3:?Results directory required}"
results=$(realpath "$3")
exec > >(tee "$results/package-verification.log") 2>&1
cat /etc/os-release
sha256sum "$package"
[[ ! -e /opt/Qt && ! -e /workspace/app && ! -e /work/validation ]]
[[ $(dpkg-deb -f "$package" Package) == zcversionbox ]]
[[ $(dpkg-deb -f "$package" Architecture) == amd64 ]]
tar -xzf "$validation_archive" -C /work
chown -R tester:tester /work/validation "$results"

# User fixtures predate installation. They are never used by the test services.
runuser -u tester -- bash -c '
    set -e
    mkdir -p "$HOME/Documents/ZcVersionBox/Backup" "$HOME/.config/autostart" "$HOME/.local/share/nautilus/scripts"
    printf "%s\n" "preserve-backup" > "$HOME/Documents/ZcVersionBox/Backup/sentinel"
    printf "%s\n" "preserve-settings" > "$HOME/Documents/ZcVersionBox/config.ini"
    printf "%s\n" "preserve-autostart" > "$HOME/.config/autostart/com.zc.versionbox.desktop"
    printf "%s\n" "preserve-script" > "$HOME/.local/share/nautilus/scripts/添加到 ZcVersionBox"
'
assert_user_data()
{
    grep -qx 'preserve-backup' /home/tester/Documents/ZcVersionBox/Backup/sentinel
    grep -qx 'preserve-settings' /home/tester/Documents/ZcVersionBox/config.ini
    grep -qx 'preserve-autostart' /home/tester/.config/autostart/com.zc.versionbox.desktop
    grep -qx 'preserve-script' '/home/tester/.local/share/nautilus/scripts/添加到 ZcVersionBox'
}

# A metadata-only predecessor exercises a real dpkg upgrade, not an assumed old release.
dpkg-deb -R "$package" /work/predecessor
sed -i 's/^Version:.*/Version: 0.0.0~compat-fixture/' /work/predecessor/DEBIAN/control
dpkg-deb --build --root-owner-group /work/predecessor /work/predecessor.deb
apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 update
apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 install -y --no-install-recommends /work/predecessor.deb
assert_user_data
apt-get install -y --no-install-recommends "$package"
assert_user_data
[[ $(dpkg-query -W -f='${Version}' zcversionbox) == "$(dpkg-deb -f "$package" Version)" ]]
test -x /usr/bin/zcversionbox
desktop-file-validate /usr/share/applications/com.zc.versionbox.desktop
test -f /usr/share/icons/hicolor/512x512/apps/zcversionbox.png
while IFS= read -r -d '' binary; do
    readelf -h "$binary" >/dev/null 2>&1 || continue
    dependencies=$(env -u LD_LIBRARY_PATH ldd "$binary")
    [[ "$dependencies" != *'not found'* ]] || { echo "$dependencies"; exit 1; }
    [[ "$dependencies" != *'/opt/Qt/'* && "$dependencies" != *'/workspace/'* ]]
done < <(find /opt/zcversionbox -type f -print0)
if find /opt/zcversionbox -name '*Qt6Test*' -o -name 'zc_*tests*' | grep -q .; then
    echo 'Test dependencies must not ship in the application'; exit 1
fi

# Both test executables use temporary repositories, fake AI and isolated Git config.
# The only extra Qt files in the validation archive are Qt Test and offscreen QPA.
runuser -u tester -- env LANG=C.UTF-8 \
    ZCVERSIONBOX_EXPECTED_ICON=zcversionbox \
    LD_LIBRARY_PATH=/opt/zcversionbox/lib:/work/validation/lib \
    QT_PLUGIN_PATH=/opt/zcversionbox/plugins:/work/validation/plugins \
    bash -c '
        set -euo pipefail
        [[ $(id -u) != 0 ]]
        QT_QPA_PLATFORM=offscreen /work/validation/zc_tests -o "$1/regression.txt,txt"
        /work/validation/zc_backup_tests -o "$1/backup_core.txt,txt"
        bash /work/validation/run-desktop-smoke.sh /work/validation/zc_tests "$1/native"
    ' _ "$results"
grep 'Totals:' "$results/regression.txt" "$results/backup_core.txt" "$results/native/"*.txt
apt-get purge -y zcversionbox
assert_user_data
[[ ! -e /usr/bin/zcversionbox && ! -e /opt/zcversionbox/bin/ZcVersionBox ]]
[[ ! -e /usr/share/applications/com.zc.versionbox.desktop ]]
[[ ! -e /usr/share/icons/hicolor/512x512/apps/zcversionbox.png ]]
echo 'PASS: install, upgrade, private runtime, regression, backup_core, X11, Wayland and purge preserve user data'
