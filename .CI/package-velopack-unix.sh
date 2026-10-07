#!/usr/bin/env bash
set -euo pipefail

platform=""
package_id=""
pack_dir=""
output_dir=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --platform|--package-id|--pack-dir|--output-dir)
            [ "$#" -ge 2 ] || {
                echo "Missing value for $1" >&2
                exit 2
            }
            case "$1" in
                --platform) platform="$2" ;;
                --package-id) package_id="$2" ;;
                --pack-dir) pack_dir="$2" ;;
                --output-dir) output_dir="$2" ;;
            esac
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done
[[ "$package_id" =~ ^[A-Za-z][A-Za-z0-9_.-]*$ ]] || {
    echo "--package-id must be your own package identity" >&2
    exit 2
}
case "$(printf '%s' "$package_id" | tr '[:upper:]' '[:lower:]')" in
    moltobenne.moltorino7|moltobenne.moltorino7updatertest)
        echo "Official Moltorino package identities are reserved" >&2
        exit 2
        ;;
esac
[ -d "$pack_dir" ] && [ -n "$output_dir" ] || {
    echo "--pack-dir and --output-dir are required" >&2
    exit 2
}
repo_root="$(cd "$(dirname "$0")/.." && pwd -P)"
pack_dir="$(cd "$pack_dir" && pwd -P)"
output_dir="$(python3 -c 'import os, sys; print(os.path.realpath(sys.argv[1]))' "$output_dir")"
case "$output_dir/" in
    "$pack_dir/"*)
        echo "The output directory must be outside the staged runtime" >&2
        exit 2
        ;;
esac
mkdir -p "$output_dir"
version="$(sed -nE 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+).*/\1/p' "$repo_root/CMakeLists.txt" | head -n 1)"
[ -n "$version" ] || {
    echo "Unable to read the CMake version" >&2
    exit 1
}

case "$platform:$(uname -s)" in
    macos:Darwin)
        [[ "$pack_dir" == *.app ]] || {
            echo "Stage a native .app bundle" >&2
            exit 2
        }
        executable="$pack_dir/Contents/MacOS/Moltorino7"
        identity="$pack_dir/Contents/MacOS/moltorino-build-identity.json"
        [ -f "$pack_dir/Contents/Frameworks/velopack_libc_osx.dylib" ] &&
            [ -f "$pack_dir/Contents/Resources/licenses/Velopack.txt" ] || {
            echo "Stage the Velopack runtime and license in the app bundle" >&2
            exit 1
        }
        lipo "$executable" -verify_arch x86_64 arm64
        lipo "$pack_dir/Contents/Frameworks/velopack_libc_osx.dylib" -verify_arch x86_64 arm64
        architecture="osx-universal"
        channel="osx-universal-fork"
        ;;
    linux:Linux)
        [ "$(uname -m)" = x86_64 ] || {
            echo "This packager requires Linux x64" >&2
            exit 2
        }
        [[ "$pack_dir" == *.AppDir ]] || {
            echo "Stage a native .AppDir" >&2
            exit 2
        }
        executable="$pack_dir/usr/bin/Moltorino7"
        identity="$pack_dir/usr/bin/moltorino-build-identity.json"
        [ -f "$pack_dir/usr/share/licenses/moltorino/Velopack.txt" ] || {
            echo "Stage the Velopack license in the AppDir" >&2
            exit 1
        }
        [ ! -e "$pack_dir/usr/lib/velopack_libc_linux_x64_gnu.so" ] || {
            echo "Build with the pinned static Linux Velopack runtime" >&2
            exit 1
        }
        architecture="linux-x64"
        channel="linux-x64-fork"
        ;;
    *)
        echo "Package on macOS or Linux with the matching --platform" >&2
        exit 2
        ;;
esac
[ -x "$executable" ] && [ -f "$identity" ] || {
    echo "Stage a MOLTORINO_VELOPACK=ON build before packaging" >&2
    exit 1
}
python3 - "$identity" "$version" "$platform" "$architecture" <<'PY'
import json
import sys

with open(sys.argv[1], encoding='utf-8') as file:
    identity = json.load(file)

expected = {
    'publicVersion': sys.argv[2],
    'platform': sys.argv[3],
    'architecture': sys.argv[4],
    'velopackVersion': '1.2.0',
}
for field, value in expected.items():
    if identity.get(field) != value:
        raise SystemExit(f'Staged build identity mismatch: {field}')
PY
dotnet --list-runtimes | grep -Eq '^Microsoft\.NETCore\.App 8\.' || {
    echo "Velopack 1.2.0 requires the .NET 8 runtime" >&2
    exit 1
}
tool_dir="$repo_root/releases/_tools/vpk/1.2.0"
archive="$tool_dir/vpk.1.2.0.zip"
mkdir -p "$tool_dir"
if [ ! -f "$archive" ]; then
    curl --fail --location --retry 3 --output "$archive.download" \
        'https://github.com/velopack/velopack/releases/download/1.2.0/vpk.1.2.0.nupkg'
    archive_to_check="$archive.download"
else
    archive_to_check="$archive"
fi
if command -v sha256sum >/dev/null 2>&1; then
    archive_hash="$(sha256sum "$archive_to_check" | awk '{print $1}')"
else
    archive_hash="$(shasum -a 256 "$archive_to_check" | awk '{print $1}')"
fi
[ "$archive_hash" = '3e458a676be46d1122e522312db18411f36ea8c70e586f81a676695d43f89dbc' ] || {
    echo "Velopack CLI SHA-256 does not match the pinned release" >&2
    exit 1
}
[ "$archive_to_check" = "$archive" ] || mv "$archive_to_check" "$archive"
unzip -oq "$archive" -d "$tool_dir/package"
vpk="$tool_dir/package/tools/net8.0/any/vpk.dll"
[ -f "$vpk" ] || {
    echo "The pinned archive is missing vpk.dll" >&2
    exit 1
}
args=("$vpk" --skip-updates --yes pack --packId "$package_id"
    --packVersion "$version" --packDir "$pack_dir" --mainExe Moltorino7
    --packTitle "$package_id" --packAuthors "$package_id" --channel "$channel"
    --outputDir "$output_dir" --delta None)
if [ "$platform" = macos ]; then
    VPK_RUNTIME=osx11 dotnet "${args[@]}" --noPortable --signAppIdentity -
else
    dotnet "${args[@]}" --runtime linux-x64
fi
echo "Packages created in $output_dir."
