#!/usr/bin/env bash
# Build a release, publish it as a GitHub release tagged at the commit it was built
# from, and print the row for the Sheet's firmware tab.
#
#   tools/release.sh ["what changed"]
#
# The version is THERMOPOST_VERSION in CMakeLists.txt; bump and commit it first. Needs
# ESP-IDF exported and the gh CLI logged in.

set -euo pipefail

REPO="${THERMOPOST_RELEASES_REPO:-itayke/thermopost}"
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

COMMIT="$(git -C "$ROOT" rev-parse HEAD)"
# The commit has to be on GitHub for the tag to point at it.
if ! git -C "$ROOT" branch -r --contains "$COMMIT" | grep -q .; then
    echo "push first: the release is tagged at $COMMIT" >&2
    exit 1
fi

idf.py -C "$ROOT" -B "$BUILD" -DTHERMOPOST_RELEASE=1 build

BIN="$BUILD/thermopost-$VERSION.bin"
cp "$BUILD/thermopost.bin" "$BIN"
SHA="$(shasum -a 256 "$BIN" | cut -d' ' -f1)"
gh release create "$TAG" "$BIN" --repo "$REPO" --title "$TAG" --target "$COMMIT" \
    --notes "${NOTES:-Thermopost $VERSION}"

URL="https://github.com/$REPO/releases/download/$TAG/thermopost-$VERSION.bin"
ROW_NOTES="${NOTES:-${COMMIT:0:7}}"
echo
echo "version  $VERSION"
echo "url      $URL"
echo "sha256   $SHA"
echo "notes    $ROW_NOTES"
# Tabs survive the clipboard but not a terminal copy, and Sheets splits a pasted
# tab-separated line across columns.
if command -v pbcopy >/dev/null; then
    printf '%s\t%s\t%s\t%s' "$VERSION" "$URL" "$SHA" "$ROW_NOTES" | pbcopy
    echo
    echo "The row is on the clipboard: paste it into the firmware tab's first empty row."
fi
