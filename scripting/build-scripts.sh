#!/usr/bin/env bash
# ===========================================================================
#  build-scripts.sh — compile the gameplay assembly to a native shared library
#  the engine can dlopen.
#
#    ./scripting/build-scripts.sh desktop    -> linux-x64
#    ./scripting/build-scripts.sh android    -> linux-bionic-arm64
#    ./scripting/build-scripts.sh android x86_64
#
#  Output lands in build/scripts/<rid>/libneoasis_scripts.so.
# ===========================================================================
set -euo pipefail

TARGET="${1:-desktop}"
ARCH="${2:-arm64}"

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PROJECT="$ROOT/scripting/NeoAsis.Scripting/NeoAsis.Scripting.csproj"
OUT="$ROOT/build/scripts"

if ! command -v dotnet >/dev/null 2>&1; then
  echo "error: the .NET SDK is not installed." >&2
  echo "       install .NET 10: https://dotnet.microsoft.com/download" >&2
  exit 1
fi

case "$TARGET" in
  desktop)
    RID="linux-x64"
    ;;
  android)
    case "$ARCH" in
      arm64|arm64-v8a) RID="linux-bionic-arm64" ;;
      x64|x86_64)      RID="linux-bionic-x64"   ;;
      *) echo "error: unknown Android arch '$ARCH' (use arm64 or x86_64)" >&2; exit 1 ;;
    esac
    ;;
  *)
    echo "error: unknown target '$TARGET' (use desktop or android)" >&2
    exit 1
    ;;
esac

echo "==> Building Neo-Asis scripts for $RID"
dotnet publish "$PROJECT" \
  -c Release \
  -r "$RID" \
  --self-contained true \
  -p:PublishAot=true \
  -p:NativeLib=Shared \
  -o "$OUT/$RID"

LIB="$OUT/$RID/libneoasis_scripts.so"
if [ -f "$LIB" ]; then
  SIZE=$(du -h "$LIB" | cut -f1)
  echo "==> OK: $LIB ($SIZE)"
  echo "    Copies into the APK by the Gradle native-libs task, or beside the"
  echo "    desktop player for hot reload."
else
  echo "error: expected $LIB was not produced" >&2
  exit 1
fi
