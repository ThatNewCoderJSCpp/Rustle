#!/usr/bin/env bash
set -euo pipefail

zip_file="$1"
app_name="$2"
source_dir="$3"
out="$4"

app_lower="$(echo "$app_name" | tr '[:upper:]' '[:lower:]')"
zip_file="$(cd "$(dirname "$zip_file")" && pwd)/$(basename "$zip_file")"
source_dir="$(cd "$source_dir" && pwd)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

base="$(basename "$zip_file" .zip)"
rest="${base#"$app_name"-}"
version="${rest%%-*}"
platform="${rest#"$version"-}"
case "$platform" in
    windows-i686) arch=x86 ;;
    windows-clangarm64) arch=arm64 ;;
    *) arch=x64 ;;
esac

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
unzip -q "$zip_file" -d "$work/unpacked"
root="$work/unpacked/$base"
if [ ! -f "$root/$app_lower.exe" ]; then
    echo "$zip_file doesn't hold $base/$app_lower.exe" >&2
    exit 1
fi

rm -f "$out/$base.7z"
(cd "$work/unpacked" && 7z a -t7z -mx=9 -bd "$out/$base.7z" "$base" > /dev/null)
7z t -bd "$out/$base.7z"

filelist="$work/files.nsh"
: > "$filelist"
(cd "$root" && find . -type f | sed 's|^\./||' | sort) | while IFS= read -r f; do
    printf 'Delete "$INSTDIR\\%s"\n' "${f//\//\\}" >> "$filelist"
done
(cd "$root" && find . -mindepth 1 -type d | sed 's|^\./||' | awk '{ print length($0) "\t" $0 }' | sort -rn | cut -f2-) | while IFS= read -r d; do
    printf 'RMDir "$INSTDIR\\%s"\n' "${d//\//\\}" >> "$filelist"
done

makensis -V2 -INPUTCHARSET UTF8 \
    "-DVERSION=$version" \
    "-DARCH=$arch" \
    "-DSOURCE=$root" \
    "-DICON=$source_dir/Packaging/windows/$app_lower.ico" \
    "-DFILELIST=$filelist" \
    "-DOUTFILE=$out/$base.exe" \
    "$source_dir/Packaging/windows/installer.nsi"

ls -l "$out/$base.7z" "$out/$base.exe"
