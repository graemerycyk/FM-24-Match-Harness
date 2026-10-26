#!/bin/sh
# Makes a macOS release of fm24ctl and FM24Viewer.app: a fresh universal build, signed with a
# Developer ID and the hardened runtime, notarized and stapled, zipped into dist/.
#   scripts/release_macos.sh --version v1.1.0-macos.1 \
#     --identity "Developer ID Application: Your Name (TEAMID)" --notary-profile fm24-notary
#   scripts/release_macos.sh --version try --identity -   (ad-hoc signed, not notarized: a dry run)
# The notary profile is one saved in the keychain by `xcrun notarytool store-credentials`, so no
# credential is passed here. A Developer ID release needs a clean checkout. See
# docs/DEVELOPMENT.md, "Making a macOS release".
set -e
. "$(dirname "$0")/env.sh"
set -u

usage() {
	echo "usage: scripts/release_macos.sh --version <tag> --identity <Developer ID name, or ->" \
		"[--notary-profile <profile>]" >&2
	exit 2
}

version=
identity=
profile=
while [ $# -gt 0 ]; do
	case "$1" in
	--version) [ $# -ge 2 ] || usage; version=$2; shift 2 ;;
	--identity) [ $# -ge 2 ] || usage; identity=$2; shift 2 ;;
	--notary-profile) [ $# -ge 2 ] || usage; profile=$2; shift 2 ;;
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
	if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
		echo "commit or stash your changes first: a release is built from a clean checkout" >&2
		exit 1
	fi
fi

name="FM24-Harness-macOS-$version"
work="$ROOT/build/release-macos"
stage="$work/stage/$name"
app="$stage/FM24Viewer.app"
dist="$ROOT/dist"
rm -rf "$work"
mkdir -p "$stage" "$dist"

# A fresh universal build, with the viewer for every slice, and the build's own release checks.
"$CMAKE" -S "$ROOT" -B "$work/build" -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release \
	-DFM24_BUILD_TESTS=ON -DFM24_VIEWER=ON -DFM24_ROSETTA_TESTS=OFF
"$CMAKE" --build "$work/build" --parallel
ctest --test-dir "$work/build" --output-on-failure -R '^(release|viewer_release)$'
"$CMAKE" --install "$work/build" --config Release --prefix "$stage" --component Runtime
"$CMAKE" --install "$work/build" --config Release --prefix "$stage" --component Viewer
strip -x "$stage/fm24ctl"

# Inside out: the viewer's own fm24ctl, then the bundle, then the loose fm24ctl. The harness that
# fm24ctl carries is data in a section, not code that runs in fm24ctl, so it keeps its own signature.
sign() {
	if [ "$identity" = - ]; then
		codesign --force --options runtime --sign - "$1"
	else
		codesign --force --options runtime --timestamp --sign "$identity" "$1"
	fi
}
sign "$app/Contents/MacOS/fm24ctl"
sign "$app"
sign "$stage/fm24ctl"
codesign --verify --strict --deep "$app"
codesign --verify --strict "$stage/fm24ctl"

source_url=$(git -C "$ROOT" remote get-url origin 2>/dev/null | sed 's/\.git$//')
commit=$(git -C "$ROOT" rev-parse --short HEAD)
cat >"$stage/READ ME.txt" <<EOF
FM24 match-engine harness for macOS, $version ($commit)

FM24Viewer.app exports, shows, edits and simulates fixtures; fm24ctl is the command line.
Keep this folder outside Desktop, Documents and Downloads (your home folder is fine), and
follow the quick start in the README:
$source_url/blob/$commit/README.md#quick-start

Needs FM24 24.4.2 from Steam. Licensed under GPL-3.0-or-later; the source is at
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
	# A loose executable cannot hold a ticket: Gatekeeper looks fm24ctl's up online on first run.
	xcrun stapler staple "$app"
	xcrun stapler validate "$app"
	spctl --assess --type execute --verbose "$app"
fi

rm -f "$dist/$name.zip" "$dist/$name.zip.sha256"
ditto -c -k --keepParent "$stage" "$dist/$name.zip"
(cd "$dist" && shasum -a 256 "$name.zip" >"$name.zip.sha256")
echo "made dist/$name.zip"
cat "$dist/$name.zip.sha256"
