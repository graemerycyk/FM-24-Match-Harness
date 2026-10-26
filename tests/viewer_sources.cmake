# SPDX-License-Identifier: GPL-3.0-or-later
# The fixture viewer's Swift must build with the Command Line Tools, which lack the plugins for the
# macros of SwiftUI (SwiftUIMacros, PreviewsMacros) and Foundation (FoundationMacros) in newer SDKs:
# @State, @Entry, @Animatable, @AnimatableIgnored, @Previewable, #Preview, #Predicate, #Expression
# and #bundle. View state lives in small ObservableObject models (@StateObject) instead. Observation
# needs macOS 14, so @Observable is kept out too, although the Command Line Tools can expand it.
file(GLOB_RECURSE sources
	"${SOURCE_DIR}/src/ui/macos/*.swift"
	"${SOURCE_DIR}/tests/viewer/*.swift"
	"${SOURCE_DIR}/cmake/viewer_probe.swift"
)
if(NOT sources)
	message(FATAL_ERROR "no viewer Swift sources under ${SOURCE_DIR}")
endif()
set(failed)
foreach(source IN LISTS sources)
	file(READ "${source}" content)
	# One list element per line: characters that CMake lists treat specially are neutralised.
	string(REPLACE "\\" "/" content "${content}")
	string(REPLACE ";" "," content "${content}")
	string(REPLACE "[" "(" content "${content}")
	string(REPLACE "]" ")" content "${content}")
	string(REPLACE "\n" ";" lines "${content}")
	set(number 0)
	foreach(line IN LISTS lines)
		math(EXPR number "${number} + 1")
		if(line MATCHES "@(State|Entry|Animatable|AnimatableIgnored|Previewable|Observable)([^A-Za-z0-9_]|$)"
			OR line MATCHES "#(Preview|Predicate|Expression|bundle)([^A-Za-z0-9_]|$)"
		)
			file(RELATIVE_PATH name "${SOURCE_DIR}" "${source}")
			string(STRIP "${line}" line)
			list(APPEND failed "${name}:${number}: ${line}")
		endif()
	endforeach()
endforeach()
if(failed)
	list(JOIN failed "\n" failed)
	message(FATAL_ERROR "Command Line Tools builds cannot compile these macros:\n${failed}")
endif()
list(LENGTH sources count)
message(STATUS "${count} viewer Swift sources use none of those macros")
