#!/bin/sh
# Makes a macOS release of an edition's command line and viewer (FM_EDITION, as for scripts/build.sh:
# 27, the default, or 24): a fresh universal build, signed with a Developer ID and the hardened
# runtime, notarized and stapled, zipped into dist/ as FMNN-Harness-macOS-<version>.zip.
#   scripts/release_macos.sh --version v1.0.0 \
#     --identity "Developer ID Application: Your Name (TEAMID)" --notary-profile <profile>
#   FM_EDITION=24 scripts/release_macos.sh --version v1.1.0-macos.1 --identity ... --notary-profile ...
#   scripts/release_macos.sh --version try --identity -   (ad-hoc signed, not notarized: a dry run)
#   scripts/release_macos.sh --version try --identity - --build build --dist /tmp/dist
#     (a dry run that packages a configured build, as it is, instead of building afresh)
# The notary profile is one saved in the keychain by `xcrun notarytool store-credentials`, so no
# credential is passed here. A Developer ID release needs a clean checkout and a fresh build. See
# docs/DEVELOPMENT.md, "Making a macOS release".
set -e
. "$(dirname "$0")/env.sh"
set -u

usage() {
	echo "usage: [FM_EDITION=24|27] scripts/release_macos.sh --version <tag>" \
		"--identity <Developer ID name, or -> [--notary-profile <profile>]" \
		"[--build <configured build, for a dry run>] [--dist <folder>]" >&2
	exit 2
}

version=
identity=
profile=
from=
dist="$ROOT/dist"
while [ $# -gt 0 ]; do
	case "$1" in
	--version) [ $# -ge 2 ] || usage; version=$2; shift 2 ;;
	--identity) [ $# -ge 2 ] || usage; identity=$2; shift 2 ;;
	--notary-profile) [ $# -ge 2 ] || usage; profile=$2; shift 2 ;;
	--build) [ $# -ge 2 ] || usage; from=$2; shift 2 ;;
	--dist) [ $# -ge 2 ] || usage; dist=$2; shift 2 ;;
	*) usage ;;
	esac
done
[ "$(uname -s)" = Darwin ] || { echo "a macOS release is made on macOS" >&2; exit 2; }
[ -n "$version" ] && [ -n "$identity" ] || usage
case "$version" in
*[!A-Za-z0-9._-]*) echo "the version may use letters, digits, '.', '_' and '-' only" >&2; exit 2 ;;
esac
if [ "$identity" != - ]; then
	[ -n "$profile" ] || { echo "a Developer ID release also needs --notary-profile" >&2; exit 2; }
	[ -z "$from" ] || { echo "a Developer ID release is built afresh: leave out --build" >&2; exit 2; }
	if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
		echo "commit or stash your changes first: a release is built from a clean checkout" >&2
		exit 1
	fi
fi
edition=${FM_EDITION:-}

if [ -n "$from" ]; then
	build=$(cd "$from" && pwd)
	[ -f "$build/generated/edition.h" ] ||
		{ echo "$from is not a configured build (it has no generated/edition.h)" >&2; exit 2; }
	built=$(sed -n 's/^#define FM_EDITION \([0-9]*\)$/\1/p' "$build/generated/edition.h")
	if [ -n "$edition" ] && [ "$edition" != "$built" ]; then
		echo "$from is a build for FM_EDITION=$built, not $edition" >&2
		exit 2
	fi
	work="$build/release-macos"
	rm -rf "$work"
else
	work="$ROOT/build/release-macos"
	build="$work/build"
	rm -rf "$work"
	# A fresh universal build of the edition, with the viewer for every slice, and the build's own
	# release checks.
	set --
	[ -n "$edition" ] && set -- "-DFM_EDITION=$edition"
	"$CMAKE" -S "$ROOT" -B "$build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release "$@" \
		-DFM24_BUILD_TESTS=ON -DFM24_VIEWER=ON -DFM24_ROSETTA_TESTS=OFF
	"$CMAKE" --build "$build" --parallel
	ctest --test-dir "$build" --output-on-failure -R '^(release|viewer_release)$'
fi

# The edition's names (the build's edition.h).
name() {
	sed -n "s/^#define EDITION_$1 \"\([^\"]*\)\".*/\1/p" "$build/generated/edition.h"
}
product=$(name PRODUCT)
cli=$(name CLI)
viewer=$(name VIEWER_APP)
game_version=$(name GAME_VERSION)
name="$product-Harness-macOS-$version"
stage="$work/stage/$name"
app="$stage/$viewer"
mkdir -p "$stage" "$dist"
dist=$(cd "$dist" && pwd)

"$CMAKE" --install "$build" --config Release --prefix "$stage" --component Runtime
"$CMAKE" --install "$build" --config Release --prefix "$stage" --component Viewer
[ -d "$app" ] || { echo "the build has no $viewer: configure it with FM24_VIEWER=ON" >&2; exit 1; }
strip -x "$stage/$cli"

# Inside out: the viewer's own CLI, then the bundle, then the loose CLI. The harness that the CLI
# carries is data in a section, not code that runs in the CLI, so it keeps its own signature.
sign() {
	if [ "$identity" = - ]; then
		codesign --force --options runtime --sign - "$1"
	else
		codesign --force --options runtime --timestamp --sign "$identity" "$1"
	fi
}
sign "$app/Contents/MacOS/$cli"
sign "$app"
sign "$stage/$cli"
codesign --verify --strict --deep "$app"
codesign --verify --strict "$stage/$cli"

# The README whose quick start is this edition's: README-<product>.md if there is one, else README.md.
readme=README.md
[ -f "$ROOT/README-$product.md" ] && readme="README-$product.md"
source_url=$(git -C "$ROOT" remote get-url origin 2>/dev/null | sed 's/\.git$//')
commit=$(git -C "$ROOT" rev-parse --short HEAD)
cat >"$stage/READ ME.txt" <<EOF
$product match-engine harness for macOS, $version ($commit)

$viewer exports, shows, edits and simulates fixtures; $cli is the command line.
Keep this folder outside Desktop, Documents and Downloads (your home folder is fine), and
follow the quick start in the README:
$source_url/blob/$commit/$readme#quick-start

Needs $product $game_version from Steam. Licensed under GPL-3.0-or-later; the source is at
$source_url/tree/$commit
EOF

if [ -n "$profile" ]; then
	ditto -c -k --keepParent "$stage" "$work/notarize.zip"
	result=$(xcrun notarytool submit "$work/notarize.zip" --keychain-profile "$profile" --wait 2>&1) ||
		true
	echo "$result"
	if ! echo "$result" | grep -q 'status: Accepted'; then
		echo "notarization did not succeed; xcrun notarytool log <submission id>" \
			"--keychain-profile $profile shows why" >&2
		exit 1
	fi
	# A loose executable cannot hold a ticket: Gatekeeper looks the CLI's up online on first run.
	xcrun stapler staple "$app"
	xcrun stapler validate "$app"
	spctl --assess --type execute --verbose "$app"
fi

rm -f "$dist/$name.zip" "$dist/$name.zip.sha256"
ditto -c -k --keepParent "$stage" "$dist/$name.zip"
(cd "$dist" && shasum -a 256 "$name.zip" >"$name.zip.sha256")
echo "made $dist/$name.zip"
cat "$dist/$name.zip.sha256"
