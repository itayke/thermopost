#!/usr/bin/env bash
# Build a release, publish it to the public releases repo, and print the row for the
# Sheet's firmware tab.
#
#   tools/release.sh ["what changed"]
#
# The version is THERMOPOST_VERSION in CMakeLists.txt; bump and commit it first. Needs
# ESP-IDF exported and the gh CLI logged in.

set -euo pipefail

REPO="${THERMOPOST_RELEASES_REPO:-itayke/thermopost-releases}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Its own directory, so the release flag never lingers in the dev build's configure.
BUILD="$ROOT/build-release"
NOTES="${1:-}"

VERSION="$(sed -nE 's/^set\(THERMOPOST_VERSION ([0-9.]+)\)$/\1/p' "$ROOT/CMakeLists.txt")"
[[ -n "$VERSION" ]] || { echo "no THERMOPOST_VERSION in CMakeLists.txt" >&2; exit 1; }
TAG="v$VERSION"

# A version has to name exactly one source tree.
if [[ -n "$(git -C "$ROOT" status --porcelain)" ]]; then
    echo "commit or stash first: a release is built from a clean tree" >&2
    exit 1
fi
if gh release view "$TAG" --repo "$REPO" >/dev/null 2>&1; then
    echo "$TAG is already released; bump THERMOPOST_VERSION" >&2
    exit 1
fi
command -v idf.py >/dev/null || {
    echo "export ESP-IDF first: . ~/esp/esp-idf/export.sh" >&2
    exit 1
}

idf.py -C "$ROOT" -B "$BUILD" -DTHERMOPOST_RELEASE=1 build

BIN="$BUILD/thermopost-$VERSION.bin"
cp "$BUILD/thermopost.bin" "$BIN"
SHA="$(shasum -a 256 "$BIN" | cut -d' ' -f1)"
COMMIT="$(git -C "$ROOT" rev-parse --short HEAD)"

gh release create "$TAG" "$BIN" --repo "$REPO" --title "$TAG" \
    --notes "${NOTES:-Thermopost $VERSION}"$'\n\n'"Source: $COMMIT"

URL="https://github.com/$REPO/releases/download/$TAG/thermopost-$VERSION.bin"
echo
echo "Paste into the firmware tab (version, url, sha256, notes):"
printf '%s\t%s\t%s\t%s\n' "$VERSION" "$URL" "$SHA" "${NOTES:-$COMMIT}"
echo
echo "Then tag the source: git tag $TAG $COMMIT"
