#!/usr/bin/env bash
set -euo pipefail

package="$1"
app_name="$2"
source_dir="$3"
out="$4"

app_lower="$(echo "$app_name" | tr '[:upper:]' '[:lower:]')"
package="$(cd "$(dirname "$package")" && pwd)/$(basename "$package")"
source_dir="$(cd "$source_dir" && pwd)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

base="$(basename "$package" .tar.gz)"
rest="${base#"$app_name"-}"
version="${rest%%-*}"
platform="${rest#"$version"-}"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/unpacked"
tar -xzf "$package" -C "$work/unpacked"
root="$work/unpacked/$base"
if [ ! -x "$root/$app_lower" ]; then
    echo "$package doesn't hold $base/$app_lower" >&2
    exit 1
fi

app="$work/root/$app_name.app"
contents="$app/Contents"
mkdir -p "$contents/MacOS" "$contents/Resources"
cp -R "$root/." "$contents/MacOS/"
clashes="$(cd "$contents/MacOS" && ls -1 | tr '[:upper:]' '[:lower:]' | sort | uniq -d)"
if [ -n "$clashes" ]; then
    echo "Contents/MacOS has names that only differ by case, which clash on macOS's case-insensitive disks: $clashes" >&2
    exit 1
fi
executable="$(sed -n '/<key>CFBundleExecutable<\/key>/{n;s/.*<string>\(.*\)<\/string>.*/\1/p;}' "$source_dir/Packaging/macos/Info.plist.in")"
if [ ! -x "$contents/MacOS/$executable" ]; then
    echo "Info.plist runs $executable, but Contents/MacOS has no such program" >&2
    exit 1
fi
cp "$source_dir/Packaging/macos/$app_lower.icns" "$contents/Resources/$app_lower.icns"
sed -e "s/@VERSION@/$version/g" "$source_dir/Packaging/macos/Info.plist.in" > "$contents/Info.plist"
printf 'APPL????' > "$contents/PkgInfo"
plutil -lint "$contents/Info.plist"

if ! codesign --force --deep --sign - "$app" 2> "$work/codesign.txt"; then
    echo "::warning::couldn't ad-hoc sign $app_name.app: $(cat "$work/codesign.txt")"
fi

ln -s /Applications "$work/root/Applications"
dmg="$out/$base.dmg"
rm -f "$dmg"
hdiutil create -volname "$app_name $version" -srcfolder "$work/root" -fs HFS+ -format UDZO -ov "$dmg"
hdiutil verify "$dmg"

rm -f "$work/root/Applications"
(cd "$work/root" && ditto -c -k --sequesterRsrc --keepParent "$app_name.app" "$out/$base-app.zip")

ls -la "$dmg" "$out/$base-app.zip"
