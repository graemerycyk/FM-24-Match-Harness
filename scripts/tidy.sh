#!/bin/sh
# Runs clang-tidy (with .clang-tidy) over every .c file the platform's build compiles, using
# build/'s compile database: the MinGW-w64 clang-tidy on Windows, Homebrew's on macOS. Build first
# (scripts/build.sh) so build/compile_commands.json exists.
set -e
. "$(dirname "$0")/env.sh"

if [ ! -f "$BUILD_DIR/compile_commands.json" ]; then
	echo "build/compile_commands.json missing: run scripts/build.sh first" >&2
	exit 1
fi
# Only the files in the compile database (the other platform's sources are not built here) under
# src/ of the source tree build/ was configured from, so no tests are linted wherever the tree is.
# On Windows the database's paths are native, with JSON-escaped backslashes; the cache's have
# slashes.
source=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD_DIR/CMakeCache.txt" | tr -d '\r')
files=$(tr -d '\r' <"$BUILD_DIR/compile_commands.json" |
	sed -n 's/^ *"file": "\(.*\.c\)",*$/\1/p' | sed 's/\\\\/\\/g' |
	while IFS= read -r file; do
		case $(printf '%s\n' "$file" | tr '\\' /) in
			("$source"/src/*) printf '%s\n' "$file" ;;
		esac
	done | sort -u)
if [ -z "$files" ]; then
	echo "no src/ sources in build/compile_commands.json" >&2
	exit 1
fi
if [ "$(uname -s)" = Darwin ]; then
	# clang-tidy checks one compilation at a time, so lint each slice of the universal build.
	sysroot=$(xcrun --show-sdk-path)
	database=$(mktemp -d)
	trap 'rm -rf "$database"' EXIT
	for arch in arm64 x86_64; do
		sed "s/-arch arm64 -arch x86_64/-arch $arch/" "$BUILD_DIR/compile_commands.json" \
			>"$database/compile_commands.json"
		printf '%s\n' "$files" | tr '\n' '\0' |
			xargs -0 "$CLANG_TIDY" -p "$database" --quiet --extra-arg=-isysroot"$sysroot" "$@"
	done
else
	printf '%s\n' "$files" | tr '\n' '\0' | xargs -0 "$CLANG_TIDY" -p "$BUILD_DIR" --quiet "$@"
fi
