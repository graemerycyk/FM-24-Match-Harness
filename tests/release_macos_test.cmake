# SPDX-License-Identifier: GPL-3.0-or-later
# macOS: DLL is the stripped, signed copy of the dylib fm24ctl embeds; the release installs only RUNNER.
file(SHA256 "${DLL}" dll_before)
file(SHA256 "${RUNNER}" runner_before)

execute_process(
	COMMAND "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --config "${CONFIG}"
		--prefix "${BUILD_DIR}/release-check" --component Runtime
	COMMAND_ERROR_IS_FATAL ANY
)

get_filename_component(runner_name "${RUNNER}" NAME)
set(installed_runner "${BUILD_DIR}/release-check/${runner_name}")
execute_process(COMMAND "${STRIP}" -x "${installed_runner}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND codesign --force --sign - "${installed_runner}" COMMAND_ERROR_IS_FATAL ANY)

foreach(binary IN ITEMS "${DLL}" "${installed_runner}")
	get_filename_component(name "${binary}" NAME)
	# Both slices of the universal game, and a valid (ad-hoc) signature, which arm64 requires.
	execute_process(COMMAND lipo -archs "${binary}" OUTPUT_VARIABLE archs COMMAND_ERROR_IS_FATAL ANY)
	if(NOT archs MATCHES "x86_64" OR NOT archs MATCHES "arm64")
		message(FATAL_ERROR "${name}: expected a universal x86_64 and arm64 binary, got ${archs}")
	endif()
	execute_process(COMMAND codesign --verify "${binary}" RESULT_VARIABLE unsigned)
	if(unsigned)
		message(FATAL_ERROR "${name}: the code signature is missing or invalid")
	endif()
	execute_process(COMMAND nm -a "${binary}" OUTPUT_VARIABLE symbols COMMAND_ERROR_IS_FATAL ANY)
	# Debug stabs (ld's own "OPT radr://5614542" marker is always there).
	if(symbols MATCHES " - [0-9a-f]+ [0-9a-f]+ +(SO|OSO|SOL|FUN|BNSYM|ENSYM|GSYM|STSYM|LSYM) ")
		message(FATAL_ERROR "${name}: expected a stripped binary without debug symbols")
	endif()
endforeach()

# The dylib exports nothing: the game finds nothing in it, and its initialiser starts the job thread.
execute_process(COMMAND nm -gU "${DLL}" OUTPUT_VARIABLE exported COMMAND_ERROR_IS_FATAL ANY)
string(REGEX REPLACE "\n[^\n]*:\n" "\n" exported "\n${exported}")
string(STRIP "${exported}" exported)
if(exported)
	message(FATAL_ERROR "internal dylib symbols must not be exported:\n${exported}")
endif()

# Each slice of the runner carries the whole universal dylib.
file(SIZE "${DLL}" dll_size)
foreach(arch IN ITEMS x86_64 arm64)
	execute_process(
		COMMAND size -m -arch ${arch} "${installed_runner}"
		OUTPUT_VARIABLE sections
		COMMAND_ERROR_IS_FATAL ANY
	)
	string(REGEX MATCH "Section __fm24_harness: ([0-9]+)" section_match "${sections}")
	if(NOT section_match OR CMAKE_MATCH_1 LESS dll_size)
		message(FATAL_ERROR "${runner_name} (${arch}): the harness dylib is not embedded")
	endif()
endforeach()

file(SHA256 "${DLL}" dll_after)
file(SHA256 "${RUNNER}" runner_after)
if(NOT dll_before STREQUAL dll_after OR NOT runner_before STREQUAL runner_after)
	message(FATAL_ERROR "Installing stripped copies modified the build outputs")
endif()
