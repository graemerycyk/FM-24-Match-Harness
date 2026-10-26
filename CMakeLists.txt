cmake_minimum_required(VERSION 3.20)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "C compiler flags for Release builds")
# macOS: one universal build for both slices of the universal game (Apple silicon and Intel), no
# newer than the game's own minimum system version. Ignored elsewhere.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures to build")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS to support")
project(fm24_harness C)
if(WIN32)
	enable_language(RC)
elseif(NOT APPLE)
	message(FATAL_ERROR "The harness supports Windows (MinGW-w64) and macOS")
endif()

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" OR "Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
	include(CheckIPOSupported)
	check_ipo_supported(LANGUAGES C)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()
if(APPLE)
	add_link_options("$<$<CONFIG:Release>:-Wl,-dead_strip>")
else()
	add_link_options("$<$<CONFIG:Release>:-Wl,--gc-sections>")
endif()

# The protocol version is part of the DLL's file name (see src/common/protocol.h). Changing it
# re-runs the configure step, so the DLL is renamed without touching this file.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS src/common/protocol.h)
file(STRINGS src/common/protocol.h protocol_line REGEX "define HARNESS_PROTOCOL_TEXT \"[0-9.]+\"")
string(REGEX MATCH "\"([0-9.]+)\"" protocol_match "${protocol_line}")
set(HARNESS_PROTOCOL "${CMAKE_MATCH_1}")

if(WIN32)
	add_compile_definitions(__USE_MINGW_ANSI_STDIO=1 WIN32_LEAN_AND_MEAN)
else()
	# The trace and the profiler walk frame-pointer chains; keep them in the harness's own code too.
	add_compile_options(-fno-omit-frame-pointer)
endif()

set(warnings -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wconversion -Wno-sign-conversion)

add_library(common STATIC
	src/common/buffer.c
	src/common/files.c
	src/common/tactic_names.c
	src/ui/constants.h
)
if(WIN32)
	target_sources(common PRIVATE src/common/platform_windows.c)
else()
	target_sources(common PRIVATE
		src/common/channel.c
		src/common/macos_builds.c
		src/common/platform_posix.c
	)
	# Linked into the dylib, which exports nothing.
	set_target_properties(common PROPERTIES C_VISIBILITY_PRESET hidden)
endif()
target_include_directories(common PUBLIC src/common)
target_compile_options(common PRIVATE ${warnings})
if(WIN32)
	target_compile_definitions(common PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
endif()

add_library(harness SHARED
	src/dll/config_writer.c
	src/dll/database.c
	src/dll/fixture.c
	src/dll/game_state.c
	src/dll/job.c
	src/dll/job_build.c
	src/dll/job_execute.c
	src/dll/job_inspect.c
	src/dll/job_replace.c
	src/dll/job_teams.c
	src/dll/job_players.c
	src/dll/job_player.c
	src/dll/job_run.c
	src/dll/job_status.c
	src/dll/labels.c
	src/dll/log.c
	src/dll/match.c
	src/dll/match_stats.c
	src/dll/output.c
	src/dll/rng.c
	src/dll/seed_build.c
	src/dll/site_lookup.c
	src/dll/site_members.c
	src/dll/stream.c
	src/dll/tactic_flags.c
	src/dll/trace.c
)
set_target_properties(harness PROPERTIES PREFIX "" OUTPUT_NAME "fm24-harness-${HARNESS_PROTOCOL}")
target_link_libraries(harness PRIVATE common)
target_compile_options(harness PRIVATE ${warnings})
if(WIN32)
	target_sources(harness PRIVATE
		src/dll/game.c
		src/dll/main.c
		src/dll/memory.c
		src/dll/profiler.c
	)
	target_link_options(harness PRIVATE -static -static-libgcc
		"$<$<CONFIG:Release>:-Wl,--exclude-all-symbols>"
	)
else()
	# Loaded into the game with DYLD_INSERT_LIBRARIES: it exports nothing, and its initialiser in
	# main_macos.c starts the job thread.
	target_sources(harness PRIVATE
		src/dll/game_macos.c
		src/dll/game_macos_tables.c
		src/dll/main_macos.c
		src/dll/memory_macos.c
		src/dll/profiler_macos.c
	)
	set_target_properties(harness PROPERTIES C_VISIBILITY_PRESET hidden SUFFIX ".dylib")
endif()

set(runner_sources
	src/runner/commands.c
	src/runner/fixture_data.c
	src/runner/json_reader.c
	src/runner/main.c
	src/runner/match_config.c
	src/runner/player_inject.c
	src/runner/saves.c
	src/runner/session.c
	src/ui/fixture_export.c
)
if(WIN32)
	add_executable(fm24ctl
		${runner_sources}
		src/runner/client.c
		src/runner/embedded.c
		src/runner/fixture_load.c
		src/runner/inject.c
		src/runner/paths.c
		src/runner/probe.c
		src/runner/probe_patterns.c
		src/ui/fixture_window.c
		src/ui/viewer.rc
		"${CMAKE_BINARY_DIR}/harness_dll.rc"
	)
	# fm24ctl carries a stripped copy of the DLL as a resource and extracts it before injecting (see
	# src/runner/embedded.h), so a release is the one executable.
	set(embedded_dll "${CMAKE_BINARY_DIR}/harness-embedded.dll")
	add_custom_command(
		OUTPUT "${embedded_dll}"
		COMMAND "${CMAKE_STRIP}" --strip-unneeded -o "${embedded_dll}" "$<TARGET_FILE:harness>"
		DEPENDS harness
		VERBATIM
	)
	file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/harness_dll.rc"
		CONTENT "#include \"resources.h\"\nIDR_HARNESS_DLL RCDATA \"${embedded_dll}\"\n"
	)
	set_source_files_properties("${CMAKE_BINARY_DIR}/harness_dll.rc" PROPERTIES OBJECT_DEPENDS "${embedded_dll}")
	target_include_directories(fm24ctl PRIVATE src/runner src/ui)
	target_link_libraries(fm24ctl PRIVATE common comctl32 user32 gdi32 ole32 shell32 uuid)
	target_compile_definitions(fm24ctl PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
	target_compile_options(fm24ctl PRIVATE ${warnings})
	target_link_options(fm24ctl PRIVATE -municode -static -static-libgcc)
else()
	add_executable(fm24ctl
		${runner_sources}
		src/runner/client_posix.c
		src/runner/embedded_macos.c
		src/runner/inject_macos.c
		src/runner/macos_game.c
		src/runner/macos_viewer.c
		src/runner/paths_posix.c
		src/runner/probe_macos.c
	)
	# fm24ctl carries a stripped, ad-hoc signed copy of the universal dylib in a section and installs
	# it for the game to load (see src/runner/embedded.h), so a release is the one executable.
	set(embedded_dll "${CMAKE_BINARY_DIR}/harness-embedded.dylib")
	add_custom_command(
		OUTPUT "${embedded_dll}"
		COMMAND "${CMAKE_STRIP}" -x -o "${embedded_dll}" "$<TARGET_FILE:harness>"
		COMMAND codesign --force --sign - "${embedded_dll}"
		DEPENDS harness
		VERBATIM
	)
	add_custom_target(embedded_harness DEPENDS "${embedded_dll}")
	add_dependencies(fm24ctl embedded_harness)
	set_property(TARGET fm24ctl APPEND PROPERTY LINK_DEPENDS "${embedded_dll}")
	target_include_directories(fm24ctl PRIVATE src/runner src/ui)
	target_link_libraries(fm24ctl PRIVATE common)
	target_compile_options(fm24ctl PRIVATE ${warnings})
	target_link_options(fm24ctl PRIVATE "LINKER:-sectcreate,__TEXT,__fm24_harness,${embedded_dll}")
endif()

install(TARGETS fm24ctl RUNTIME DESTINATION . COMPONENT Runtime)

option(FM24_BUILD_TESTS "Build regression checks" OFF)
if(FM24_BUILD_TESTS)
	enable_testing()
	if(WIN32)
		set(test_suites job dll fixture_data runner runner_main fixture_export fixture_view fixture_window fixture_load player_inject)
		add_executable(fm24_tests
			tests/test.c
			tests/job_test.c
			tests/dll_test.c
			tests/fixture_data_test.c
			tests/runner_main_test.c
			tests/runner_test.c
			tests/fixture_export_test.c
			tests/fixture_samples.c
			tests/fixture_view_test.c
			tests/fixture_window_test.c
			tests/fixture_load_test.c
			tests/player_inject_test.c
			src/ui/fixture_export.c
			src/ui/fixture_view.c
			src/runner/embedded.c
			src/runner/fixture_data.c
			src/runner/fixture_edit.c
			src/runner/json_reader.c
			src/runner/match_config.c
			src/runner/paths.c
			src/runner/player_inject.c
			src/runner/probe_patterns.c
			src/runner/saves.c
			src/runner/session.c
			src/dll/memory.c
			src/dll/site_lookup.c
			src/dll/site_members.c
			src/dll/tactic_flags.c
		)
		target_link_libraries(fm24_tests PRIVATE common comctl32 user32 gdi32 ole32 shell32 uuid)
		target_compile_definitions(fm24_tests PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
		target_link_options(fm24_tests PRIVATE -static -static-libgcc)
	else()
		set(test_suites job dll fixture_data runner runner_main fixture_export fixture_view player_inject macos)
		add_executable(fm24_tests
			tests/test.c
			tests/job_test.c
			tests/dll_test.c
			tests/fixture_data_test.c
			tests/runner_main_test.c
			tests/runner_test.c
			tests/fixture_export_test.c
			tests/fixture_samples.c
			tests/fixture_view_test.c
			tests/player_inject_test.c
			tests/macos_test.c
			tests/save_samples.c
			src/ui/fixture_export.c
			src/ui/fixture_view.c
			src/ui/save_list.c
			src/runner/embedded_macos.c
			src/runner/fixture_data.c
			src/runner/fixture_edit.c
			src/runner/json_reader.c
			src/runner/match_config.c
			src/runner/paths_posix.c
			src/runner/player_inject.c
			src/runner/saves.c
			src/runner/session.c
			src/dll/game_macos_tables.c
			src/dll/memory_macos.c
			src/dll/site_lookup.c
			src/dll/site_members.c
			src/dll/tactic_flags.c
		)
		target_link_libraries(fm24_tests PRIVATE common)
		target_compile_definitions(fm24_tests PRIVATE FM24_DLL_TESTS FM24_RUNNER_TESTS FM24_MACOS_TESTS)
		# The launcher test starts these next to fm24_tests in place of the game, the harness and
		# Steam's overlay.
		add_executable(fm24_stand_in_game tests/macos_stand_in_game.c)
		add_library(fm24_stand_in_library SHARED tests/macos_stand_in_library.c)
		foreach(stand_in fm24_stand_in_game fm24_stand_in_library)
			target_compile_options(${stand_in} PRIVATE ${warnings})
		endforeach()
		add_dependencies(fm24_tests fm24_stand_in_game fm24_stand_in_library)
		target_compile_definitions(fm24_tests PRIVATE
			"FM24_STAND_IN_GAME=\"$<TARGET_FILE_NAME:fm24_stand_in_game>\""
			"FM24_STAND_IN_LIBRARY=\"$<TARGET_FILE_NAME:fm24_stand_in_library>\""
		)
		install(TARGETS fm24_stand_in_game fm24_stand_in_library
			RUNTIME DESTINATION . COMPONENT Tests
			LIBRARY DESTINATION . COMPONENT Tests
		)
	endif()
	target_include_directories(fm24_tests PRIVATE src/runner src/ui)
	target_compile_options(fm24_tests PRIVATE ${warnings})
	install(TARGETS fm24_tests RUNTIME DESTINATION . COMPONENT Tests)
	foreach(test ${test_suites})
		add_test(NAME ${test} COMMAND fm24_tests ${test})
		set_tests_properties(${test} PROPERTIES TIMEOUT 300)
	endforeach()
	# On Apple silicon, also check the x86_64 slice (and its build tables, and the viewer's) under
	# Rosetta, if it runs an x86_64 program within ten seconds. FM24_ROSETTA_TESTS=OFF skips the
	# check, and the macos suite then runs nothing under Rosetta either.
	option(FM24_ROSETTA_TESTS "On Apple silicon, also run the x86_64 tests under Rosetta" ON)
	set(rosetta_works OFF)
	if(APPLE AND "x86_64" IN_LIST CMAKE_OSX_ARCHITECTURES AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
		if(FM24_ROSETTA_TESTS)
			message(STATUS "Checking that Rosetta runs x86_64 programs, for the x86_64 tests")
			execute_process(
				COMMAND /usr/bin/arch -x86_64 /usr/bin/true
				RESULT_VARIABLE rosetta OUTPUT_QUIET ERROR_QUIET TIMEOUT 10
			)
			if(rosetta EQUAL 0)
				set(rosetta_works ON)
			else()
				message(STATUS
					"Leaving out the x86_64 tests: Rosetta did not run an x86_64 program (${rosetta}). "
					"Install it with `softwareupdate --install-rosetta`, or skip this check with "
					"FM24_ROSETTA_TESTS=OFF."
				)
			endif()
		else()
			message(STATUS "Leaving out the x86_64 tests (FM24_ROSETTA_TESTS=OFF)")
		endif()
		if(rosetta_works)
			add_test(NAME x86_64 COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:fm24_tests>)
			set_tests_properties(x86_64 PROPERTIES TIMEOUT 600)
		else()
			set_tests_properties(macos PROPERTIES ENVIRONMENT "FM24_ROSETTA_TESTS=OFF")
		endif()
	endif()
	if(CMAKE_BUILD_TYPE STREQUAL "Release" OR "Release" IN_LIST CMAKE_CONFIGURATION_TYPES)
		set(release_configurations)
		if(CMAKE_CONFIGURATION_TYPES)
			set(release_configurations CONFIGURATIONS Release)
		endif()
		if(WIN32)
			set(release_script "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_test.cmake")
		else()
			set(release_script "${CMAKE_CURRENT_SOURCE_DIR}/tests/release_macos_test.cmake")
		endif()
		add_test(NAME release
			COMMAND "${CMAKE_COMMAND}"
				"-DBUILD_DIR=${CMAKE_BINARY_DIR}"
				"-DCONFIG=$<CONFIG>"
				"-DDLL=${embedded_dll}"
				"-DRUNNER=$<TARGET_FILE:fm24ctl>"
				"-DSTRIP=${CMAKE_STRIP}"
				"-DOBJDUMP=${CMAKE_OBJDUMP}"
				-P "${release_script}"
			${release_configurations}
		)
	endif()
endif()

# The native macOS fixture viewer, FM24Viewer.app (FM24_VIEWER: AUTO, ON or OFF).
if(APPLE)
	include(cmake/macos_viewer.cmake)
endif()
