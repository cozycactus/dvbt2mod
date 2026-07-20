#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

usage() {
  printf '%s\n' \
    "usage: $0 /path/to/dvbt2mod.exe /path/to/new-runtime-directory" \
    '       /path/to/new-corresponding-source-directory' >&2
}

if [[ $# -ne 3 ]]; then
  usage
  exit 2
fi

: "${MINGW_PREFIX:?run this script inside an MSYS2 MinGW environment}"

executable=$1
bundle=$2
source_bundle=$3

if [[ ! -f "$executable" ]]; then
  printf 'package_windows: executable not found: %s\n' "$executable" >&2
  exit 1
fi
if [[ -e "$bundle" ]]; then
  printf 'package_windows: refusing to replace existing path: %s\n' "$bundle" >&2
  exit 1
fi
if [[ -e "$source_bundle" ]]; then
  printf 'package_windows: refusing to replace existing path: %s\n' \
    "$source_bundle" >&2
  exit 1
fi

for command_name in bsdtar curl cygpath git ntldd pacman sha256sum; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    printf 'package_windows: required command not found: %s\n' "$command_name" >&2
    exit 1
  fi
done

dependency_report=$(mktemp)
checksum_report=$(mktemp)
trap 'rm -f "$dependency_report" "$checksum_report"' EXIT

mingw_bin="$MINGW_PREFIX/bin"
mingw_bin_windows=$(cygpath -aw "$mingw_bin")
windows_root=$(cygpath -u "${SYSTEMROOT:-C:\\Windows}")

# Build the DLL closure with direct ntldd scans instead of asking it to recurse
# through Windows. Recursive ntldd also walks the runner's System32
# implementation and reports optional/private Windows components as missing.
# They are not imports of dvbt2mod or of any DLL that we ship. Stopping at the
# Windows boundary mirrors the loader contract: bundle every UCRT64 dependency,
# but let Windows resolve its own system DLL graph.
declare -A dependency_paths=()
declare -A queued_dependencies=()
declare -A missing_dependencies=()

scan_queue=("$executable")
scan_index=0
: >"$dependency_report"
while ((scan_index < ${#scan_queue[@]})); do
  scanned_path=${scan_queue[$scan_index]}
  scan_index=$((scan_index + 1))
  printf '%s:\n' "${scanned_path##*/}" >>"$dependency_report"

  scanned_path_windows=$(cygpath -aw "$scanned_path")
  if ! direct_report=$(
    ntldd -D "$mingw_bin_windows" "$scanned_path_windows" 2>&1
  ); then
    printf 'package_windows: could not inspect PE file: %s\n%s\n' \
      "$scanned_path" "$direct_report" >&2
    exit 1
  fi
  while IFS= read -r direct_line; do
    direct_line=${direct_line%$'\r'}
    printf '  %s\n' "$direct_line" >>"$dependency_report"
    if [[ $direct_line =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+=[\>][[:space:]]+(.+)[[:space:]]+\(0x[[:xdigit:]]+\)$ ]]; then
      imported_dll=${BASH_REMATCH[1]}
      resolved_path=${BASH_REMATCH[2]}
      unix_path=$(cygpath -u "$resolved_path")
      shopt -s nocasematch
      if [[ "$unix_path" == "$mingw_bin"/*.dll ]]; then
        filename=${unix_path##*/}
        imported_key=${filename,,}
        if [[ -n ${dependency_paths[$imported_key]:-} &&
              ${dependency_paths[$imported_key]} != "$unix_path" ]]; then
          printf 'package_windows: duplicate DLL name resolves to two paths: %s\n' \
            "$filename" >&2
          exit 1
        fi
        dependency_paths[$imported_key]=$unix_path
        if [[ -z ${queued_dependencies[$imported_key]:-} ]]; then
          queued_dependencies[$imported_key]=1
          scan_queue+=("$unix_path")
        fi
      elif [[ "$unix_path" != "$windows_root"/* ]]; then
        printf 'package_windows: dependency resolved outside UCRT64 and Windows: %s\n' \
          "$unix_path" >&2
        exit 1
      fi
      shopt -u nocasematch
    elif [[ $direct_line =~ ^[[:space:]]*([^[:space:]]+)[[:space:]]+=[\>][[:space:]]+not[[:space:]]+found$ ]]; then
      imported_dll=${BASH_REMATCH[1]}
      imported_key=${imported_dll,,}
      if [[ ! $imported_key =~ ^(api|ext)-[a-z0-9-]+-l[0-9]+-[0-9]+-[0-9]+\.dll$ ]]; then
        missing_dependencies["${scanned_path##*/} -> $imported_dll"]=1
      fi
    elif [[ -n $direct_line ]]; then
      printf 'package_windows: unrecognized ntldd output for %s: %s\n' \
        "$scanned_path" "$direct_line" >&2
      exit 1
    fi
  done <<<"$direct_report"
done

printf '%s\n' 'Resolved PE runtime dependencies:'
sed 's/^/  /' "$dependency_report"

if [[ ${#missing_dependencies[@]} -ne 0 ]]; then
  printf '%s\n' 'package_windows: unresolved non-system DLL imports:' >&2
  printf '%s\n' "${!missing_dependencies[@]}" | LC_ALL=C sort >&2
  exit 1
fi

mkdir -p "$bundle"
cp -- "$executable" "$bundle/dvbt2mod.exe"
cp -- README.md LICENSE "$bundle/"

if [[ ${#dependency_paths[@]} -eq 0 ]]; then
  printf 'package_windows: PE scan found no UCRT64 runtime DLLs\n' >&2
  exit 1
fi

mapfile -t dependency_keys < <(
  printf '%s\n' "${!dependency_paths[@]}" | LC_ALL=C sort
)
for key in "${dependency_keys[@]}"; do
  cp -- "${dependency_paths[$key]}" "$bundle/"
done
printf '%s\n' "${dependency_keys[@]}" >"$bundle/RUNTIME-DLLS.txt"

declare -A package_set=()
for key in "${dependency_keys[@]}"; do
  package_name=$(pacman -Qqo "${dependency_paths[$key]}")
  package_set[$package_name]=1
done
mapfile -t package_names < <(
  printf '%s\n' "${!package_set[@]}" | LC_ALL=C sort
)

{
  printf '# Exact MSYS2 packages owning the bundled runtime DLLs\n'
  for package_name in "${package_names[@]}"; do
    pacman -Q "$package_name"
  done
} >"$bundle/THIRD-PARTY-PACKAGES.txt"

source_repository=${GITHUB_SERVER_URL:-https://github.com}
if [[ -n ${GITHUB_REPOSITORY:-} ]]; then
  source_repository+="/${GITHUB_REPOSITORY}"
else
  source_repository+="/<owner>/<repository>"
fi
source_revision=${GITHUB_SHA:-$(git rev-parse HEAD)}

mkdir -p "$source_bundle/msys2"
source_manifest="$source_bundle/MSYS2-SOURCES.txt"
{
  printf '# Exact source archives for packages owning bundled runtime DLLs\n'
  printf '# binary-package binary-version | source-archive | sha256 | source-url\n'
} >"$source_manifest"

declare -A source_urls=()
declare -A package_source_paths=()
for package_name in "${package_names[@]}"; do
  package_version=$(pacman -Q "$package_name" | awk '{ print $2 }')
  package_page=$(curl \
    --fail --location --silent --show-error \
    --proto '=https' --proto-redir '=https' \
    --retry 3 \
    "https://packages.msys2.org/packages/$package_name")
  if [[ $package_page =~ (https://mirror\.msys2\.org/mingw/sources/([A-Za-z0-9._+-]|%[0-9A-Fa-f]{2})+\.src\.tar\.zst) ]]; then
    source_url=${BASH_REMATCH[1]}
  else
    printf 'package_windows: exact source archive not found for %s\n' \
      "$package_name" >&2
    exit 1
  fi

  source_file=${source_url##*/}
  if [[ -n ${source_urls[$source_file]:-} &&
        ${source_urls[$source_file]} != "$source_url" ]]; then
    printf 'package_windows: duplicate source filename has two URLs: %s\n' \
      "$source_file" >&2
    exit 1
  fi
  source_urls[$source_file]=$source_url

  source_path="$source_bundle/msys2/$source_file"
  package_source_paths[$package_name]=$source_path
  if [[ ! -f "$source_path" ]]; then
    temporary_source="$source_path.part"
    curl \
      --fail --location --silent --show-error \
      --proto '=https' --proto-redir '=https' \
      --retry 3 \
      --output "$temporary_source" \
      "$source_url"
    archive_listing=$(bsdtar -tf "$temporary_source")
    if ! grep -Eq '/(\.SRCINFO|PKGBUILD)$' <<<"$archive_listing"; then
      printf 'package_windows: invalid MSYS2 source archive: %s\n' \
        "$source_file" >&2
      exit 1
    fi
    mv -- "$temporary_source" "$source_path"
  fi
  source_info=$(bsdtar -xOf "$source_path" '*/.SRCINFO')
  if ! grep -Fq "pkgname = $package_name" <<<"$source_info"; then
    printf 'package_windows: %s does not build binary package %s\n' \
      "$source_file" "$package_name" >&2
    exit 1
  fi
  source_pkgver=$(awk '$1 == "pkgver" && $2 == "=" { print $3; exit }' \
    <<<"$source_info")
  source_pkgrel=$(awk '$1 == "pkgrel" && $2 == "=" { print $3; exit }' \
    <<<"$source_info")
  source_epoch=$(awk '$1 == "epoch" && $2 == "=" { print $3; exit }' \
    <<<"$source_info")
  source_version="$source_pkgver-$source_pkgrel"
  if [[ -n "$source_epoch" ]]; then
    source_version="$source_epoch:$source_version"
  fi
  if [[ "$source_version" != "$package_version" ]]; then
    printf 'package_windows: source version %s does not match %s %s\n' \
      "$source_version" "$package_name" "$package_version" >&2
    exit 1
  fi
  source_hash=$(sha256sum "$source_path" | awk '{ print $1 }')
  printf '%s %s | %s | %s | %s\n' \
    "$package_name" "$package_version" "$source_file" "$source_hash" \
    "$source_url" >>"$source_manifest"
done

for package_name in "${package_names[@]}"; do
  copied_license=0
  while IFS= read -r installed_path; do
    case "$installed_path" in
      */share/licenses/*)
        if [[ -f "$installed_path" ]]; then
          relative_path=${installed_path#*/share/licenses/}
          destination="$bundle/licenses/$package_name/installed/$relative_path"
          mkdir -p "${destination%/*}"
          cp -- "$installed_path" "$destination"
          copied_license=1
        fi
        ;;
    esac
  done < <(pacman -Qlq "$package_name")

  # A few runtime packages (notably GMP) do not install their licence files.
  # Their exact MSYS2 source package contains the complete upstream archive, so
  # extract licence notices from that archive rather than shipping an
  # unattributed DLL or relying on a mutable web link.
  if [[ $copied_license -eq 0 ]]; then
    source_path=${package_source_paths[$package_name]}
    mapfile -t nested_archives < <(
      bsdtar -tf "$source_path" \
        | grep -Ei '\.(tar\.(gz|bz2|xz|zst)|tgz|tbz2?|txz|zip)$' || true
    )
    for nested_archive in "${nested_archives[@]}"; do
      nested_listing=$(
        bsdtar -xOf "$source_path" "$nested_archive" | bsdtar -tf -
      )
      mapfile -t license_entries < <(
        grep -Ei \
          '(^|/)(copying([._-][^/]*)?|licen[cs]e([._-][^/]*)?|notice([._-][^/]*)?)$' \
          <<<"$nested_listing" || true
      )
      nested_label=${nested_archive##*/}
      nested_label=${nested_label//[^A-Za-z0-9._+-]/_}
      for license_entry in "${license_entries[@]}"; do
        if [[ "$license_entry" == /* || "$license_entry" == *../* ]]; then
          printf 'package_windows: unsafe licence path in %s: %s\n' \
            "$nested_archive" "$license_entry" >&2
          exit 1
        fi
        license_relative=${license_entry#*/}
        if [[ "$license_relative" == "$license_entry" ]]; then
          license_relative=${license_entry##*/}
        fi
        destination="$bundle/licenses/$package_name/upstream/$nested_label/$license_relative"
        mkdir -p "${destination%/*}"
        bsdtar -xOf "$source_path" "$nested_archive" \
          | bsdtar -xOf - "$license_entry" >"$destination"
        if [[ ! -s "$destination" ]]; then
          printf 'package_windows: empty upstream licence %s in %s\n' \
            "$license_entry" "$nested_archive" >&2
          exit 1
        fi
        copied_license=1
      done
    done
  fi
  if [[ $copied_license -eq 0 ]]; then
    printf 'package_windows: no licence file found for %s\n' \
      "$package_name" >&2
    exit 1
  fi
done

project_source="$source_bundle/dvbt2mod-$source_revision.tar.gz"
git archive \
  --format=tar.gz \
  --prefix="dvbt2mod-$source_revision/" \
  --output="$project_source" \
  "$source_revision"

{
  printf 'dvbt2mod corresponding source bundle\n'
  printf '====================================\n\n'
  printf 'Build revision: %s\n' "$source_revision"
  printf 'Source repository: %s\n\n' "$source_repository"
  printf 'The dvbt2mod archive is the exact Git revision used for this build.\n'
  printf 'The msys2 directory contains the complete exact .src.tar.zst archive\n'
  printf 'for every package that owns a bundled runtime DLL. These archives\n'
  printf 'include upstream source, MSYS2 build recipes, patches, and licence data.\n'
  printf 'MSYS2-SOURCES.txt maps every binary package and version to its source.\n'
} >"$source_bundle/README-SOURCES.txt"

(
  cd "$source_bundle"
  find . -type f ! -name SHA256SUMS.txt -print0 |
    LC_ALL=C sort -z |
    xargs -0 sha256sum
) >"$source_bundle/SHA256SUMS.txt"

{
  printf 'dvbt2mod Windows portable bundle\n'
  printf '================================\n\n'
  printf 'Architecture: Windows x86-64, MSYS2 UCRT64\n'
  printf 'GNU Radio package: '
  pacman -Q mingw-w64-ucrt-x86_64-gnuradio
  printf 'Source revision: %s\n' "$source_revision"
  printf 'Source repository: %s\n\n' "$source_repository"
  printf 'Run:\n'
  printf '  dvbt2mod.exe input.ts output.cf32 --profile 1\n'
  printf '  dvbt2mod.exe --help\n\n'
  printf 'No MSYS2 or GNU Radio installation is required at runtime. Keep the DLLs\n'
  printf 'beside dvbt2mod.exe. See README.md for formats, parameters, and limits.\n'
} >"$bundle/README-WINDOWS.txt"

{
  printf 'Corresponding source and build information\n'
  printf '========================================\n\n'
  printf 'Exact corresponding source is distributed beside this runtime as:\n'
  printf '  %s.zip\n\n' "${source_bundle##*/}"
  printf 'It contains dvbt2mod revision %s and each complete MSYS2 source archive\n' \
    "$source_revision"
  printf 'corresponding to the package versions in THIRD-PARTY-PACKAGES.txt.\n'
  printf 'Repository view: %s/tree/%s\n' "$source_repository" "$source_revision"
} >"$bundle/SOURCE-CODE.txt"

(
  cd "$bundle"
  find . -type f ! -name SHA256SUMS.txt -print0 |
    LC_ALL=C sort -z |
    xargs -0 sha256sum
) >"$checksum_report"
mv -- "$checksum_report" "$bundle/SHA256SUMS.txt"

printf 'package_windows: bundled %d runtime DLLs from %d packages in %s\n' \
  "${#dependency_keys[@]}" "${#package_names[@]}" "$bundle"
printf 'package_windows: saved %d exact MSYS2 source archives in %s\n' \
  "${#source_urls[@]}" "$source_bundle"
