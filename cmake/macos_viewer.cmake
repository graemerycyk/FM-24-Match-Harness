# SPDX-License-Identifier: GPL-3.0-or-later
# The macOS fixture viewer, EDITION_VIEWER_APP: SwiftUI views in an AppKit shell over the C core
# (src/ui/fixture_view.h, imported through src/ui/module.modulemap) and the edition's names
# (edition.h, imported as the module Edition), with the same build's CLI inside it. swiftc runs
# once per slice and lipo joins the slices: CMake's own Swift support needs Ninja or Xcode and
# cannot build a universal binary, so this works with every generator.
set(FM24_VIEWER AUTO CACHE STRING "Build the macOS fixture viewer: AUTO, ON (required) or OFF")
set_property(CACHE FM24_VIEWER PROPERTY STRINGS AUTO ON OFF)
set(FM24_VIEWER_DEPLOYMENT_TARGET 13.0 CACHE STRING "Oldest macOS the fixture viewer supports")
# Without the viewer, a bundle an earlier configuration built or installed is removed, so that
# the CLI, which opens the viewer beside it, never hands fixtures to a stale copy.
macro(viewer_absent)
	file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/${EDITION_VIEWER_APP}")
	install(CODE "file(REMOVE_RECURSE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${EDITION_VIEWER_APP}\")"
		COMPONENT Viewer
	)
endmacro()

if(NOT FM24_VIEWER MATCHES "^(AUTO|ON|OFF)$")
	message(FATAL_ERROR "FM24_VIEWER must be AUTO, ON or OFF (not ${FM24_VIEWER})")
elseif(FM24_VIEWER STREQUAL "OFF")
	viewer_absent()
	return()
endif()

set(viewer_source "${CMAKE_CURRENT_SOURCE_DIR}")
set(viewer_dir "${CMAKE_BINARY_DIR}/viewer")
file(MAKE_DIRECTORY "${viewer_dir}")
if(FM24_BUILD_TESTS)
	# The rule that keeps the viewer buildable with the Command Line Tools, checked even when the
	# toolchain cannot build it.
	add_test(NAME viewer_sources
		COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${viewer_source}"
			-P "${viewer_source}/tests/viewer_sources.cmake"
	)
	# The texts the viewer's tests read from the CLI's stand-in are the CLI's own.
	add_test(NAME viewer_stand_in_texts
		COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${viewer_source}"
			"-DEDITION_HEADER=${EDITION_INCLUDE_DIR}/edition.h"
			-P "${viewer_source}/tests/viewer_stand_in_texts.cmake"
	)
endif()

# Fails the configuration for FM24_VIEWER=ON; otherwise the viewer is left out with `reason`.
macro(viewer_unavailable reason)
	if(FM24_VIEWER STREQUAL "ON")
		message(FATAL_ERROR "FM24_VIEWER=ON, but ${reason}")
	endif()
	message(STATUS "Skipping the fixture viewer: ${reason}")
	viewer_absent()
	return()
endmacro()

execute_process(COMMAND xcrun --find swiftc
	OUTPUT_VARIABLE FM24_SWIFTC OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE missing
)
# CMake 4 leaves CMAKE_OSX_SYSROOT empty, and swiftc needs the SDK's path.
execute_process(COMMAND xcrun --sdk macosx --show-sdk-path
	OUTPUT_VARIABLE FM24_MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET
	RESULT_VARIABLE missing_sdk
)
if(missing OR missing_sdk OR NOT FM24_SWIFTC OR NOT FM24_MACOS_SDK)
	viewer_unavailable("swiftc or the macOS SDK cannot be found (install Xcode)")
endif()

# A link of cmake/viewer_probe.swift per slice decides what this toolchain can build: the Command
# Line Tools lack x86_64 Swift libraries for some targets and the macro plugins that newer SDKs'
# @State needs. The result is cached until the compiler's version, the SDK (whose usual path is a
# link that toolchain updates move), the target or the probe changes.
execute_process(COMMAND "${FM24_SWIFTC}" --version
	OUTPUT_VARIABLE swiftc_version ERROR_VARIABLE swiftc_version OUTPUT_STRIP_TRAILING_WHITESPACE
)
string(REGEX REPLACE "[\r\n]+" " " swiftc_version "${swiftc_version}")
file(REAL_PATH "${FM24_MACOS_SDK}" sdk_path)
file(SHA256 "${viewer_source}/cmake/viewer_probe.swift" probe_hash)
set(probe_key
	"${swiftc_version}|${FM24_SWIFTC}|${sdk_path}|${FM24_VIEWER_DEPLOYMENT_TARGET}|${CMAKE_OSX_ARCHITECTURES}|${probe_hash}"
)
if(NOT probe_key STREQUAL "${FM24_VIEWER_PROBE_KEY}")
	set(passed)
	foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
		message(STATUS
			"Checking that Swift can build the fixture viewer for ${arch} (the first check builds "
			"Swift's module cache, which can take a minute or two)"
		)
		execute_process(
			COMMAND "${FM24_SWIFTC}" -swift-version 5 -target ${arch}-apple-macos${FM24_VIEWER_DEPLOYMENT_TARGET}
				-sdk "${FM24_MACOS_SDK}" -module-cache-path "${viewer_dir}/module-cache"
				"${viewer_source}/cmake/viewer_probe.swift" -o "${viewer_dir}/probe-${arch}"
			RESULT_VARIABLE probe ERROR_VARIABLE probe_error OUTPUT_QUIET
		)
		if(probe EQUAL 0)
			list(APPEND passed ${arch})
		else()
			string(REGEX MATCH "[^\n]*error[^\n]*" first_error "${probe_error}")
			message(WARNING "The fixture viewer cannot be built for ${arch} with this toolchain: ${first_error}")
		endif()
	endforeach()
	set(FM24_VIEWER_ARCHITECTURES "${passed}" CACHE INTERNAL "Slices the fixture viewer is built for")
	set(FM24_VIEWER_PROBE_KEY "${probe_key}" CACHE INTERNAL "What the viewer probe checked")
endif()
if(NOT FM24_VIEWER_ARCHITECTURES)
	viewer_unavailable("Swift cannot build it for any slice with this toolchain (install Xcode)")
elseif(FM24_VIEWER STREQUAL "ON" AND NOT FM24_VIEWER_ARCHITECTURES STREQUAL CMAKE_OSX_ARCHITECTURES)
	message(FATAL_ERROR
		"FM24_VIEWER=ON needs every slice (${CMAKE_OSX_ARCHITECTURES}); this toolchain builds "
		"${FM24_VIEWER_ARCHITECTURES} (install Xcode)"
	)
endif()

# The C the viewer calls, as plain Mach-O objects: swiftc's link need not share the C compiler's
# link-time optimisation.
add_library(viewer_core STATIC
	src/common/buffer.c
	src/common/channel.c
	src/common/platform_posix.c
	src/common/tactic_names.c
	src/runner/fixture_data.c
	src/runner/fixture_edit.c
	src/runner/json_reader.c
	src/runner/match_config.c
	src/runner/saves.c
	src/ui/fixture_export.c
	src/ui/fixture_view.c
	src/ui/save_list.c
)
target_include_directories(viewer_core PUBLIC src/common src/runner src/ui "${EDITION_INCLUDE_DIR}")
# The viewer calls the core on Swift's worker threads, whose stacks are 512 KiB: no function may
# keep a FixtureData (about 570 KiB) on its stack, as unoptimised builds would for a temporary.
target_compile_options(viewer_core PRIVATE
	${warnings} -Wframe-larger-than=65536 -Werror=frame-larger-than
)
set_target_properties(viewer_core PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)

file(GLOB_RECURSE viewer_swift CONFIGURE_DEPENDS "${viewer_source}/src/ui/macos/*.swift")
set(viewer_headers
	"${viewer_source}/src/ui/fixture_view.h"
	"${viewer_source}/src/ui/save_list.h"
	"${viewer_source}/src/ui/module.modulemap"
	"${viewer_source}/src/common/channel.h"
	"${EDITION_INCLUDE_DIR}/edition.h"
	"${EDITION_INCLUDE_DIR}/module.modulemap"
)
set(swift_flags -swift-version 5 "$<IF:$<CONFIG:Debug>,-Onone$<SEMICOLON>-g,-O$<SEMICOLON>-wmo>"
	-sdk "${FM24_MACOS_SDK}" -module-cache-path "${viewer_dir}/module-cache"
	-I "${viewer_source}/src/ui" -I "${EDITION_INCLUDE_DIR}"
)

# One command per slice, so that make and ninja compile the slices in parallel.
set(slices)
foreach(arch IN LISTS FM24_VIEWER_ARCHITECTURES)
	set(slice "${viewer_dir}/${EDITION_VIEWER_EXECUTABLE}-${arch}")
	add_custom_command(OUTPUT "${slice}"
		COMMAND "${FM24_SWIFTC}" ${swift_flags} -module-name ${EDITION_PRODUCT}Viewer
			-target ${arch}-apple-macos${FM24_VIEWER_DEPLOYMENT_TARGET}
			${viewer_swift} "$<TARGET_FILE:viewer_core>" -Xlinker -dead_strip -o "${slice}"
		DEPENDS ${viewer_swift} ${viewer_headers} viewer_core
		COMMENT "Building the fixture viewer (${arch})"
		COMMAND_EXPAND_LISTS VERBATIM
	)
	list(APPEND slices "${slice}")
endforeach()

# The bundle, stripped and signed ad hoc from the inside out: the CLI, then the bundle and its
# executable. Its two executables are stripped as it is assembled, while they still carry the
# linker's signatures, which strip handles (Xcode 16's strip cannot process a binary whose signature
# codesign removed); installing it only copies it. The slices and build tree's CLI keep symbols.
set(app "${CMAKE_BINARY_DIR}/${EDITION_VIEWER_APP}")
set(viewer_executable "${app}/Contents/MacOS/${EDITION_VIEWER_EXECUTABLE}")
set(viewer_cli "${app}/Contents/MacOS/${EDITION_CLI}")
configure_file(src/ui/macos/Info.plist.in "${viewer_dir}/Info.plist" @ONLY)
add_custom_command(OUTPUT "${viewer_executable}"
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${app}/Contents/MacOS"
	COMMAND lipo -create ${slices} -output "${viewer_executable}"
	COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${EDITION_CLI}>" "${viewer_cli}"
	COMMAND strip -x "${viewer_executable}"
	COMMAND strip -x "${viewer_cli}"
	COMMAND "${CMAKE_COMMAND}" -E copy "${viewer_dir}/Info.plist" "${app}/Contents/Info.plist"
	COMMAND codesign --force --sign - "${viewer_cli}"
	COMMAND codesign --force --sign - "${app}"
	DEPENDS ${slices} ${EDITION_CLI} "${viewer_dir}/Info.plist"
	COMMENT "Assembling ${EDITION_VIEWER_APP}"
	VERBATIM
)
add_custom_target(${EDITION_VIEWER_EXECUTABLE} ALL DEPENDS "${viewer_executable}")

# Replacing the bundle, not writing into it, leaves a running copy's executable alone.
install(CODE "file(REMOVE_RECURSE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${EDITION_VIEWER_APP}\")"
	COMPONENT Viewer
)
install(DIRECTORY "${app}" DESTINATION . COMPONENT Viewer USE_SOURCE_PERMISSIONS)

if(NOT FM24_BUILD_TESTS)
	return()
endif()

# The Swift tests: the viewer's models (Foundation only) with tests/viewer, linked with the C core
# and the sample fixtures, and run on each slice. They run the CLI's stand-in, which never contacts
# the game, in place of the CLI (named by EDITION_CLI_ENV).
add_library(viewer_samples STATIC tests/fixture_samples.c tests/save_samples.c)
target_link_libraries(viewer_samples PRIVATE viewer_core)
target_compile_options(viewer_samples PRIVATE ${warnings})
set_target_properties(viewer_samples PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF)
set(stand_in_cli ${EDITION_LOWER}_stand_in_${EDITION_CLI})
add_executable(${stand_in_cli} tests/viewer_stand_in_cli.c)
target_link_libraries(${stand_in_cli} PRIVATE viewer_samples viewer_core)
target_compile_options(${stand_in_cli} PRIVATE ${warnings})
set(viewer_test_environment "${EDITION_CLI_ENV}=$<TARGET_FILE:${stand_in_cli}>")
file(GLOB viewer_model_swift CONFIGURE_DEPENDS "${viewer_source}/src/ui/macos/Model/*.swift")
file(GLOB viewer_test_swift CONFIGURE_DEPENDS "${viewer_source}/tests/viewer/*.swift")
set(test_slices)
foreach(arch IN LISTS FM24_VIEWER_ARCHITECTURES)
	set(slice "${viewer_dir}/${EDITION_VIEWER_EXECUTABLE}_tests-${arch}")
	add_custom_command(OUTPUT "${slice}"
		COMMAND "${FM24_SWIFTC}" ${swift_flags} -module-name ${EDITION_PRODUCT}ViewerTests
			-target ${arch}-apple-macos${FM24_VIEWER_DEPLOYMENT_TARGET}
			-I "${viewer_source}/tests/viewer" ${viewer_model_swift} ${viewer_test_swift}
			"$<TARGET_FILE:viewer_samples>" "$<TARGET_FILE:viewer_core>" -o "${slice}"
		DEPENDS ${viewer_model_swift} ${viewer_test_swift} ${viewer_headers}
			"${viewer_source}/tests/fixture_samples.h" "${viewer_source}/tests/save_samples.h"
			"${viewer_source}/tests/viewer/module.modulemap"
			viewer_core viewer_samples
		COMMENT "Building the fixture viewer's tests (${arch})"
		COMMAND_EXPAND_LISTS VERBATIM
	)
	list(APPEND test_slices "${slice}")
endforeach()
set(viewer_tests "${CMAKE_BINARY_DIR}/${EDITION_VIEWER_EXECUTABLE}_tests")
add_custom_command(OUTPUT "${viewer_tests}"
	COMMAND lipo -create ${test_slices} -output "${viewer_tests}"
	DEPENDS ${test_slices}
	VERBATIM
)
# Named apart from the file, which Makefiles would otherwise take for a rule that depends on itself.
add_custom_target(viewer_tests ALL DEPENDS "${viewer_tests}")
add_dependencies(viewer_tests ${stand_in_cli})
add_test(NAME viewer COMMAND "${viewer_tests}")
set_tests_properties(viewer PROPERTIES TIMEOUT 300 ENVIRONMENT "${viewer_test_environment}")
# Under Rosetta if CMakeLists.txt found that it works (FM24_ROSETTA_TESTS).
if("x86_64" IN_LIST FM24_VIEWER_ARCHITECTURES AND rosetta_works)
	add_test(NAME viewer_x86_64 COMMAND /usr/bin/arch -x86_64 "${viewer_tests}")
	set_tests_properties(viewer_x86_64 PROPERTIES
		TIMEOUT 600 ENVIRONMENT "${viewer_test_environment}"
	)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR "Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
	set(release_configurations)
	if(CMAKE_CONFIGURATION_TYPES)
		set(release_configurations CONFIGURATIONS Release)
	endif()
	string(REPLACE ";" "," viewer_architectures "${FM24_VIEWER_ARCHITECTURES}")
	add_test(NAME viewer_release
		COMMAND "${CMAKE_COMMAND}"
			"-DBUILD_DIR=${CMAKE_BINARY_DIR}"
			"-DCONFIG=$<CONFIG>"
			"-DAPP=${app}"
			"-DDLL=${embedded_dll}"
			"-DARCHITECTURES=${viewer_architectures}"
			"-DVIEWER_MINIMUM=${FM24_VIEWER_DEPLOYMENT_TARGET}"
			"-DRUNNER_MINIMUM=${CMAKE_OSX_DEPLOYMENT_TARGET}"
			"-DIDENTIFIER=${EDITION_VIEWER_ID}"
			"-DVIEWER_NAME=${EDITION_VIEWER_NAME}"
			"-DVIEWER_EXECUTABLE=${EDITION_VIEWER_EXECUTABLE}"
			"-DPRODUCT=${EDITION_PRODUCT}"
			"-DCLI=${EDITION_CLI}"
			"-DSECTION=${EDITION_SECTION}"
			"-DPROTOCOL=${HARNESS_PROTOCOL}"
			-P "${viewer_source}/tests/release_viewer_test.cmake"
		${release_configurations}
	)
	# A dry run of scripts/release_macos.sh on this build, and the zip it makes.
	add_test(NAME release_package
		COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${viewer_source}" "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
			"-DFM_EDITION=${FM_EDITION}" -P "${viewer_source}/tests/release_package_test.cmake"
		${release_configurations}
	)
	set_tests_properties(release_package PROPERTIES TIMEOUT 300)
endif()
