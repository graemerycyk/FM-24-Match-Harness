# SPDX-License-Identifier: GPL-3.0-or-later
# macOS: installs the Viewer component as a release does and checks FM24Viewer.app. APP is the build
# tree's bundle, DLL the stripped, signed dylib fm24ctl embeds, ARCHITECTURES (comma-separated) the
# slices the viewer was built for, PROTOCOL the harness protocol version (src/common/protocol.h).
string(REPLACE "," ";" architectures "${ARCHITECTURES}")
set(built "${APP}/Contents/MacOS")
file(SHA256 "${built}/fm24viewer" viewer_before)
file(SHA256 "${built}/fm24ctl" runner_before)

set(prefix "${BUILD_DIR}/release-check-viewer")
execute_process(
	COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}" --prefix "${prefix}"
		--component Viewer
	COMMAND_ERROR_IS_FATAL ANY
)
set(app "${prefix}/FM24Viewer.app")
set(installed "${app}/Contents/MacOS")

# A valid ad-hoc signature over the whole bundle, with no entitlements.
execute_process(
	COMMAND codesign --verify --deep --strict "${app}" RESULT_VARIABLE invalid ERROR_VARIABLE why
)
if(invalid)
	message(FATAL_ERROR "FM24Viewer.app: the code signature is missing or invalid: ${why}")
endif()
foreach(code IN ITEMS "${app}" "${installed}/fm24ctl")
	execute_process(
		COMMAND codesign -d --entitlements - "${code}"
		OUTPUT_VARIABLE entitlements ERROR_QUIET COMMAND_ERROR_IS_FATAL ANY
	)
	string(STRIP "${entitlements}" entitlements)
	if(entitlements)
		message(FATAL_ERROR "${code}: expected no entitlements, got ${entitlements}")
	endif()
endforeach()

foreach(binary IN ITEMS fm24viewer fm24ctl)
	set(path "${installed}/${binary}")
	execute_process(COMMAND lipo -archs "${path}" OUTPUT_VARIABLE archs COMMAND_ERROR_IS_FATAL ANY)
	string(STRIP "${archs}" archs)
	string(REPLACE " " ";" archs "${archs}")
	# fm24ctl is always universal; the viewer has the slices this toolchain could build.
	set(expected ${architectures})
	if(binary STREQUAL "fm24ctl")
		set(expected arm64 x86_64)
		set(minimum "${RUNNER_MINIMUM}")
	else()
		set(minimum "${VIEWER_MINIMUM}")
	endif()
	foreach(arch IN LISTS expected)
		if(NOT arch IN_LIST archs)
			message(FATAL_ERROR "${binary}: expected the ${arch} slice, got ${archs}")
		endif()
		execute_process(
			COMMAND vtool -arch ${arch} -show-build "${path}"
			OUTPUT_VARIABLE build COMMAND_ERROR_IS_FATAL ANY
		)
		if(NOT build MATCHES "minos ${minimum}\n")
			message(FATAL_ERROR "${binary} (${arch}): expected minos ${minimum}:\n${build}")
		endif()
	endforeach()
	# Only the system's libraries and frameworks: the bundle carries no Swift runtime.
	execute_process(COMMAND otool -L "${path}" OUTPUT_VARIABLE libraries COMMAND_ERROR_IS_FATAL ANY)
	string(REGEX REPLACE "^[^\n]*:\n" "" libraries "${libraries}")
	string(REGEX REPLACE "\n[^\n]*:\n" "\n" libraries "${libraries}")
	string(REGEX MATCHALL "\t[^ \n]+" libraries "${libraries}")
	foreach(library IN LISTS libraries)
		string(STRIP "${library}" library)
		if(NOT library MATCHES "^/(System/Library|usr/lib)/")
			message(FATAL_ERROR "${binary}: links ${library}, which is not part of macOS")
		endif()
	endforeach()
	execute_process(COMMAND nm -a "${path}" OUTPUT_VARIABLE symbols COMMAND_ERROR_IS_FATAL ANY)
	# Debug stabs (ld's own "OPT radr://5614542" marker is always there).
	if(symbols MATCHES " - [0-9a-f]+ [0-9a-f]+ +(SO|OSO|SOL|FUN|BNSYM|ENSYM|GSYM|STSYM|LSYM) ")
		message(FATAL_ERROR "${binary}: expected a stripped binary without debug symbols")
	endif()
	# Release builds have no debug symbols to begin with: the local ones show that strip -x ran.
	execute_process(COMMAND nm -m "${path}" OUTPUT_VARIABLE symbols COMMAND_ERROR_IS_FATAL ANY)
	string(REGEX MATCH "[^\n]*\\) non-external [^\n]*" local "${symbols}")
	if(local)
		message(FATAL_ERROR "${binary}: expected a binary stripped of local symbols, got ${local}")
	endif()
endforeach()

# The bundle's identity, its version (the harness protocol's) and document types: Open With for
# JSON and binary fixtures, without becoming their default app.
set(plist "${app}/Contents/Info.plist")
execute_process(COMMAND plutil -lint "${plist}" OUTPUT_QUIET COMMAND_ERROR_IS_FATAL ANY)
foreach(check IN ITEMS
	"CFBundleIdentifier=${IDENTIFIER}"
	"CFBundleExecutable=fm24viewer"
	"CFBundleDisplayName=FM24 Viewer"
	"CFBundleShortVersionString=${PROTOCOL}"
	"CFBundleVersion=${PROTOCOL}"
	"LSMinimumSystemVersion=${VIEWER_MINIMUM}"
	"CFBundleDocumentTypes.0.LSHandlerRank=Alternate"
	"CFBundleDocumentTypes.0.CFBundleTypeRole=Editor"
	"CFBundleDocumentTypes.0.LSItemContentTypes.0=public.json"
	"CFBundleDocumentTypes.0.LSItemContentTypes.1=com.apple.macbinary-archive"
)
	string(FIND "${check}" "=" split)
	string(SUBSTRING "${check}" 0 ${split} key)
	math(EXPR split "${split} + 1")
	string(SUBSTRING "${check}" ${split} -1 expected)
	execute_process(
		COMMAND plutil -extract "${key}" raw -o - "${plist}"
		OUTPUT_VARIABLE value OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE missing
	)
	if(missing OR NOT value STREQUAL expected)
		message(FATAL_ERROR "Info.plist: expected ${key} to be ${expected}, got ${value}")
	endif()
endforeach()

# Each slice of the bundled fm24ctl carries the whole universal dylib.
file(SIZE "${DLL}" dll_size)
foreach(arch IN ITEMS x86_64 arm64)
	execute_process(
		COMMAND size -m -arch ${arch} "${installed}/fm24ctl"
		OUTPUT_VARIABLE sections
		COMMAND_ERROR_IS_FATAL ANY
	)
	string(REGEX MATCH "Section __fm24_harness: ([0-9]+)" section_match "${sections}")
	if(NOT section_match OR CMAKE_MATCH_1 LESS dll_size)
		message(FATAL_ERROR "fm24ctl (${arch}) in FM24Viewer.app: the harness dylib is not embedded")
	endif()
endforeach()

file(SHA256 "${built}/fm24viewer" viewer_after)
file(SHA256 "${built}/fm24ctl" runner_after)
if(NOT viewer_before STREQUAL viewer_after OR NOT runner_before STREQUAL runner_after)
	message(FATAL_ERROR "Installing the stripped viewer modified the build outputs")
endif()
