#!/bin/sh
# Builds unstripped Release binaries into build/, then installs stripped copies into bin/ (on
# macOS with FM24Viewer.app).
#   scripts/build.sh
#   FM24_VIEWER=ON scripts/build.sh   (macOS: AUTO, the default, ON or OFF; see below)
#   FM24_ROSETTA_TESTS=OFF scripts/build.sh   (Apple silicon: no x86_64 tests under Rosetta)
set -e
. "$(dirname "$0")/env.sh"

protocol=$(sed -n 's/^#define HARNESS_PROTOCOL_TEXT "\([0-9.]*\)"$/\1/p' "$ROOT/src/common/protocol.h")

if [ "$(uname -s)" = Darwin ]; then
	# The generator is chosen when build/ is first configured; delete build/ to switch.
	if [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
		set --
	elif [ -n "$NINJA" ]; then
		set -- -G Ninja -DCMAKE_MAKE_PROGRAM="$NINJA"
	else
		set -- -G "Unix Makefiles"
	fi
	# FM24_VIEWER: AUTO builds FM24Viewer.app for the slices this toolchain can build, ON requires
	# every slice (CI), OFF leaves it out.
	"$CMAKE" -S "$ROOT" -B "$BUILD_DIR" "$@" -DCMAKE_BUILD_TYPE=Release -DFM24_BUILD_TESTS=ON \
		-DFM24_VIEWER="${FM24_VIEWER:-AUTO}" -DFM24_ROSETTA_TESTS="${FM24_ROSETTA_TESTS:-ON}"
	"$CMAKE" --build "$BUILD_DIR" --parallel
	dll="fm24-harness-$protocol.dylib"
	"$CMAKE" --install "$BUILD_DIR" --config Release --prefix "$ROOT/bin"
	# Stripping invalidates the linker's ad-hoc signature, which arm64 requires: sign again. Strip
	# while that signature is there: Xcode 16's strip cannot process a binary whose signature
	# codesign removed.
	for binary in "$ROOT/bin/fm24ctl" "$ROOT/bin/fm24_tests"; do
		strip -x "$binary"
		codesign --force --sign - "$binary"
	done
	runner=fm24ctl
else
	"$CMAKE" -S "$ROOT" -B "$BUILD_DIR" -G Ninja \
		-DCMAKE_MAKE_PROGRAM="$(cygpath -m "$NINJA")" \
		-DCMAKE_TOOLCHAIN_FILE="$(cygpath -m "$ROOT/cmake/mingw64.cmake")" \
		-DCMAKE_BUILD_TYPE=Release \
		-DFM24_BUILD_TESTS=ON
	"$CMAKE" --build "$BUILD_DIR"
	dll="fm24-harness-$protocol.dll"
	"$CMAKE" --install "$BUILD_DIR" --config Release --prefix "$(cygpath -m "$ROOT/bin")"
	# CMake's --strip does not propagate strip failures.
	strip=$(sed -n 's/^CMAKE_STRIP:[^=]*=//p' "$BUILD_DIR/CMakeCache.txt")
	"$strip" --strip-unneeded "$ROOT/bin/fm24ctl.exe" "$ROOT/bin/fm24_tests.exe"
	runner=fm24ctl.exe
fi

# The runner carries the DLL, so a loose copy in bin/ would override it (see inject.c and
# macos_game.c); older protocols' DLLs in build/ are never injected again. Deleting one still loaded
# in fm.exe fails; it goes on a later build once the game has restarted.
for old in "$BUILD_DIR"/fm24-harness-*.dll "$BUILD_DIR"/fm24_harness_p*.dll "$BUILD_DIR"/fm24-harness-*.dylib; do
	[ -e "$old" ] && [ "$(basename "$old")" != "$dll" ] && { rm -f "$old" 2>/dev/null || true; }
done
for old in "$ROOT"/bin/fm24-harness-*.dll "$ROOT"/bin/fm24_harness_p*.dll "$ROOT"/bin/fm24-harness-*.dylib; do
	[ -e "$old" ] && { rm -f "$old" 2>/dev/null || true; }
done
echo "built $runner with harness $protocol"
ls "$ROOT/bin"
