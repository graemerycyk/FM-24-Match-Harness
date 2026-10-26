#!/bin/sh
# Tool locations shared by the scripts; override any of them from the environment.
# Windows (MSYS2 Bash):
#   CMAKE/NINJA:  a native Windows CMake and Ninja (the MSYS ones cannot drive the MinGW compiler),
#                 by default the copies bundled with Visual Studio Build Tools.
#   MINGW:        the MSYS2 MinGW-w64 prefix (gcc, clang-tidy).
#   CLANG_FORMAT: clang-format 22 or newer.
# macOS: CMake from the PATH, Ninja if it is installed (else Makefiles), and clang-format/clang-tidy
# from Homebrew's llvm (22 or newer; `brew install llvm`). The compiler is Xcode's Apple clang.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"

if [ "$(uname -s)" = Darwin ]; then
	: "${CMAKE:=cmake}"
	: "${NINJA:=$(command -v ninja || true)}"
	if [ -z "$LLVM" ]; then
		for formula in llvm llvm@23 llvm@22; do
			LLVM=$(brew --prefix "$formula" 2>/dev/null || true)
			[ -x "$LLVM/bin/clang-format" ] && break
		done
	fi
	: "${CLANG_FORMAT:=$LLVM/bin/clang-format}"
	: "${CLANG_TIDY:=$LLVM/bin/clang-tidy}"
else
	VS_CMAKE_DIR="/c/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/CMake"
	: "${CMAKE:=$VS_CMAKE_DIR/CMake/bin/cmake.exe}"
	: "${NINJA:=$VS_CMAKE_DIR/Ninja/ninja.exe}"
	: "${MINGW:=/c/msys64/mingw64}"
	: "${CLANG_FORMAT:=/c/msys64/usr/bin/clang-format}"
	: "${CLANG_TIDY:=$MINGW/bin/clang-tidy}"
	PATH="$MINGW/bin:$PATH"
	export PATH
fi

# Every C source and header under src/.
sources() {
	find "$ROOT/src" -name '*.c' -o -name '*.h' | sort
}
