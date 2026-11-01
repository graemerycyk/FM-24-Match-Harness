cmake_minimum_required(VERSION 3.20)
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG" CACHE STRING "C compiler flags for Release builds")
# macOS: one universal build for both slices of the universal game (Apple silicon and Intel), no
# newer than the game's own minimum system version. Ignored elsewhere.
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures to build")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Oldest macOS to support")
project(fm_match_harness C)
if(WIN32)
	enable_language(RC)
elseif(NOT APPLE)
	message(FATAL_ERROR "The harness supports Windows (MinGW-w64) and macOS")
endif()

# FM_EDITION and the names of its edition (EDITION_*, and edition.h).
include(cmake/edition.cmake)

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
	# An edition whose game is a Unity app reads its names from its Info.plist.
	if(EDITION_ENGINE_PLUGIN)
		target_sources(common PRIVATE src/common/info_plist.c)
	endif()
	# Linked into the dylib, which exports nothing.
	set_target_properties(common PROPERTIES C_VISIBILITY_PRESET hidden)
endif()
target_include_directories(common PUBLIC src/common "${EDITION_INCLUDE_DIR}")
target_compile_options(common PRIVATE ${warnings})
if(WIN32)
	target_compile_definitions(common PRIVATE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
endif()

# The harness's sources but for the platform's: also built into the stand-in harness (tests).
set(harness_sources
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
add_library(harness SHARED ${harness_sources})
set_target_properties(harness PROPERTIES PREFIX "" OUTPUT_NAME "${EDITION_HARNESS}-${HARNESS_PROTOCOL}")
target_link_libraries(harness PRIVATE common)
target_compile_options(harness PRIVATE ${warnings})
# The build tables of the edition's supported builds (see src/dll/game_macos.h).
if(FM_EDITION STREQUAL "24")
	set(edition_tables src/dll/game_macos_tables.c)
else()
	set(edition_tables src/dll/game_macos_tables_fm${FM_EDITION}.c)
endif()
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
		${edition_tables}
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
	add_executable(${EDITION_CLI}
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
	# The CLI carries a stripped copy of the DLL as a resource and extracts it before injecting (see
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
	target_include_directories(${EDITION_CLI} PRIVATE src/runner src/ui)
	target_link_libraries(${EDITION_CLI} PRIVATE common comctl32 user32 gdi32 ole32 shell32 uuid)
	target_compile_definitions(${EDITION_CLI} PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
	target_compile_options(${EDITION_CLI} PRIVATE ${warnings})
	target_link_options(${EDITION_CLI} PRIVATE -municode -static -static-libgcc)
else()
	add_executable(${EDITION_CLI}
		${runner_sources}
		src/runner/client_posix.c
		src/runner/embedded_macos.c
		src/runner/inject_macos.c
		src/runner/macos_game.c
		src/runner/macos_viewer.c
		src/runner/paths_posix.c
		src/runner/probe_macos.c
	)
	# The CLI carries a stripped, ad-hoc signed copy of the universal dylib in a section and installs
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
	add_dependencies(${EDITION_CLI} embedded_harness)
	set_property(TARGET ${EDITION_CLI} APPEND PROPERTY LINK_DEPENDS "${embedded_dll}")
	target_include_directories(${EDITION_CLI} PRIVATE src/runner src/ui)
	target_link_libraries(${EDITION_CLI} PRIVATE common)
	target_compile_options(${EDITION_CLI} PRIVATE ${warnings})
	target_link_options(${EDITION_CLI} PRIVATE "LINKER:-sectcreate,__TEXT,${EDITION_SECTION},${embedded_dll}")
endif()

install(TARGETS ${EDITION_CLI} RUNTIME DESTINATION . COMPONENT Runtime)

option(FM24_BUILD_TESTS "Build regression checks" OFF)
if(FM24_BUILD_TESTS)
	enable_testing()
	# The tests and their stand-ins carry the edition in their names (EDITION_LOWER).
	set(tests ${EDITION_LOWER}_tests)
	if(WIN32)
		set(test_suites job dll fixture_data runner runner_main fixture_export fixture_view fixture_window fixture_load player_inject)
		add_executable(${tests}
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
		target_link_libraries(${tests} PRIVATE common comctl32 user32 gdi32 ole32 shell32 uuid)
		target_compile_definitions(${tests} PRIVATE UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
		target_link_options(${tests} PRIVATE -static -static-libgcc)
	else()
		set(test_suites job dll fixture_data runner runner_main fixture_export fixture_view player_inject macos match)
		add_executable(${tests}
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
			tests/match_test.c
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
			${edition_tables}
			src/dll/memory_macos.c
			src/dll/site_lookup.c
			src/dll/site_members.c
			src/dll/tactic_flags.c
		)
		target_link_libraries(${tests} PRIVATE common)
		target_compile_definitions(${tests} PRIVATE TESTS_DLL TESTS_RUNNER TESTS_MACOS)
		# The launcher test starts these next to the tests in place of the game, the harness and
		# Steam's overlay.
		set(stand_in_game ${EDITION_LOWER}_stand_in_game)
		set(stand_in_library ${EDITION_LOWER}_stand_in_library)
		add_executable(${stand_in_game} tests/macos_stand_in_game.c)
		add_library(${stand_in_library} SHARED tests/macos_stand_in_library.c)
		foreach(stand_in ${stand_in_game} ${stand_in_library})
			target_include_directories(${stand_in} PRIVATE "${EDITION_INCLUDE_DIR}")
			target_compile_options(${stand_in} PRIVATE ${warnings})
		endforeach()
		add_dependencies(${tests} ${stand_in_game} ${stand_in_library})
		target_compile_definitions(${tests} PRIVATE
			"STAND_IN_GAME=\"$<TARGET_FILE_NAME:${stand_in_game}>\""
			"STAND_IN_LIBRARY=\"$<TARGET_FILE_NAME:${stand_in_library}>\""
		)
		install(TARGETS ${stand_in_game} ${stand_in_library}
			RUNTIME DESTINATION . COMPONENT Tests
			LIBRARY DESTINATION . COMPONENT Tests
		)
		if(EDITION_ENGINE_PLUGIN)
			# The plugin test has the stand-in game load the stand-in harness and load and unload a
			# stand-in engine plugin, or another build of it: the harness built with tables for the
			# plugin (its UUIDs and offsets come from the linked plugin), serving a test socket.
			set(stand_in_plugin ${EDITION_LOWER}_stand_in_plugin)
			set(stand_in_other_plugin ${EDITION_LOWER}_stand_in_other_plugin)
			set(stand_in_harness ${EDITION_LOWER}_stand_in_harness)
			foreach(plugin ${stand_in_plugin} ${stand_in_other_plugin})
				add_library(${plugin} MODULE tests/macos_stand_in_plugin.c)
				set_target_properties(${plugin} PROPERTIES PREFIX "" SUFFIX "")
				target_include_directories(${plugin} PRIVATE src/common src/dll "${EDITION_INCLUDE_DIR}")
				target_compile_options(${plugin} PRIVATE ${warnings})
			endforeach()
			target_compile_definitions(${stand_in_other_plugin} PRIVATE STAND_IN_OTHER_BUILD)
			set(stand_in_offsets "${EDITION_INCLUDE_DIR}/macos_stand_in_offsets.h")
			add_custom_command(
				OUTPUT "${stand_in_offsets}"
				COMMAND "${CMAKE_COMMAND}" "-DPLUGIN=$<TARGET_FILE:${stand_in_plugin}>"
					"-DOUTPUT=${stand_in_offsets}" "-DARCHITECTURES=${CMAKE_OSX_ARCHITECTURES}"
					"-DOBJDUMP=${CMAKE_OBJDUMP}" "-DNM=${CMAKE_NM}"
					-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/macos_stand_in_offsets.cmake"
				DEPENDS ${stand_in_plugin} tests/macos_stand_in_offsets.cmake
				VERBATIM
			)
			add_library(${stand_in_harness} SHARED
				${harness_sources}
				src/common/buffer.c
				src/common/channel.c
				src/common/files.c
				src/common/info_plist.c
				src/common/platform_posix.c
				src/common/tactic_names.c
				src/dll/game_macos.c
				src/dll/memory_macos.c
				src/dll/profiler_macos.c
				tests/macos_stand_in_harness.c
				"${stand_in_offsets}"
			)
			target_include_directories(${stand_in_harness} PRIVATE
				src/common src/dll tests "${EDITION_INCLUDE_DIR}"
			)
			target_compile_options(${stand_in_harness} PRIVATE ${warnings})
			set_target_properties(${stand_in_harness} PROPERTIES
				C_VISIBILITY_PRESET hidden INTERPROCEDURAL_OPTIMIZATION OFF
			)
			add_dependencies(${tests} ${stand_in_plugin} ${stand_in_other_plugin} ${stand_in_harness})
			target_compile_definitions(${tests} PRIVATE
				"STAND_IN_PLUGIN=\"$<TARGET_FILE_NAME:${stand_in_plugin}>\""
				"STAND_IN_OTHER_PLUGIN=\"$<TARGET_FILE_NAME:${stand_in_other_plugin}>\""
				"STAND_IN_HARNESS=\"$<TARGET_FILE_NAME:${stand_in_harness}>\""
			)
			install(TARGETS ${stand_in_plugin} ${stand_in_other_plugin} ${stand_in_harness}
				LIBRARY DESTINATION . COMPONENT Tests
			)
		endif()
	endif()
	target_include_directories(${tests} PRIVATE src/runner src/ui)
	target_compile_options(${tests} PRIVATE ${warnings})
	install(TARGETS ${tests} RUNTIME DESTINATION . COMPONENT Tests)
	foreach(test ${test_suites})
		add_test(NAME ${test} COMMAND ${tests} ${test})
		set_tests_properties(${test} PROPERTIES TIMEOUT 300)
	endforeach()
	# The editions' names: distinct, all in edition.h, and in no other source that should not have them.
	add_test(NAME edition
		COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
			"-DEDITION_HEADER=${EDITION_INCLUDE_DIR}/edition.h" "-DFM_EDITION=${FM_EDITION}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/edition_test.cmake"
	)
	# The edition's build tables are what tools/macos/gen_tables.py makes of its mappings, which also
	# checks the mappings' coverage and, for a plugin engine, that their layouts are the header's.
	if(APPLE)
		find_package(Python3 COMPONENTS Interpreter QUIET)
		if(Python3_Interpreter_FOUND)
			add_test(NAME tables
				COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
					"-DPYTHON=${Python3_EXECUTABLE}" "-DFM_EDITION=${FM_EDITION}"
					"-DTABLES=${CMAKE_CURRENT_SOURCE_DIR}/${edition_tables}"
					"-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/tables_test.c"
					-P "${CMAKE_CURRENT_SOURCE_DIR}/tests/tables_test.cmake"
			)
			# The mapping tools' own tests: the frame derivation and what the generator refuses.
			add_test(NAME align
				COMMAND "${Python3_EXECUTABLE}" -I -m unittest discover
					-s "${CMAKE_CURRENT_SOURCE_DIR}/tools/macos/align" -p "test_*.py"
			)
		else()
			message(STATUS "Python 3 not found: the tables and align tests are left out")
		endif()
	endif()
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
			add_test(NAME x86_64 COMMAND /usr/bin/arch -x86_64 $<TARGET_FILE:${tests}>)
			set_tests_properties(x86_64 PROPERTIES TIMEOUT 600)
		else()
			set_tests_properties(macos PROPERTIES ENVIRONMENT "${EDITION_ENV_PREFIX}ROSETTA_TESTS=OFF")
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
				"-DRUNNER=$<TARGET_FILE:${EDITION_CLI}>"
				"-DSECTION=${EDITION_SECTION}"
				"-DSTRIP=${CMAKE_STRIP}"
				"-DOBJDUMP=${CMAKE_OBJDUMP}"
				-P "${release_script}"
			${release_configurations}
		)
	endif()
endif()

# The native macOS fixture viewer, EDITION_VIEWER_APP (FM24_VIEWER: AUTO, ON or OFF).
if(APPLE)
	include(cmake/macos_viewer.cmake)
endif()
