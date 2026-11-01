# SPDX-License-Identifier: GPL-3.0-or-later
# The editions' names (cmake/edition.cmake): FM24's are the released version's, FM27's the design's,
# the two editions share none, the build's edition.h (EDITION_HEADER, for FM_EDITION) holds every
# one, and outside the files that define or need them, the sources under SOURCE_DIR name neither
# edition.
include("${SOURCE_DIR}/cmake/edition.cmake")

set(failed)
set(expected_24
	"PRODUCT=FM24" "LOWER=fm24" "GAME=Football Manager 2024" "GAME_VERSION=24.4.2"
	"GAME_BUNDLE_ID=com.sigames.eur.FootballManager2024" "GAME_EXECUTABLE=fm" "STEAM_APP_ID=2252570"
	"DATA_FOLDER=Sports Interactive/Football Manager 2024" "ENGINE_PATH=" "CLI=fm24ctl"
	"CLI_ENV=FM24CTL"
	"ENV_PREFIX=FM24_" "HARNESS=fm24-harness" "LAUNCHER=launch-fm24.sh" "SECTION=__fm24_harness"
	"MATCH_CONFIG_FORMAT=fm24-match-config/1" "PLAYER_FORMAT=fm24-player/1"
	"TEMPORARY_FIXTURE=temp-fixture.json" "JOB_INPUT_PREFIX=_job_input-"
	"VIEWER_NAME=FM24 Viewer" "VIEWER_APP=FM24Viewer.app"
	"VIEWER_EXECUTABLE=fm24viewer"
	"VIEWER_ID=io.github.graemerycyk.fm24-match-harness.viewer"
)
set(expected_27
	"PRODUCT=FM27" "LOWER=fm27" "GAME=Football Manager 27" "GAME_VERSION=27.0.0.2486716"
	"GAME_BUNDLE_ID=com.SportsInteractive.FootballManager27" "GAME_EXECUTABLE=Football Manager 27"
	"STEAM_APP_ID=4530370" "DATA_FOLDER=Sports Interactive/Football Manager 27"
	"ENGINE_PATH=game_plugin.bundle/Contents/MacOS/game_plugin" "CLI=fm27ctl"
	"CLI_ENV=FM27CTL" "ENV_PREFIX=FM27_" "HARNESS=fm27-harness" "LAUNCHER=launch-fm27.sh"
	"SECTION=__fm27_harness" "MATCH_CONFIG_FORMAT=fm27-match-config/1" "PLAYER_FORMAT=fm27-player/1"
	"TEMPORARY_FIXTURE=fm27-temp-fixture.json" "JOB_INPUT_PREFIX=fm27-job-input-"
	"VIEWER_NAME=FM27 Viewer" "VIEWER_APP=FM27Viewer.app"
	"VIEWER_EXECUTABLE=fm27viewer"
	"VIEWER_ID=io.github.graemerycyk.fm27-match-harness.viewer"
)
foreach(edition IN LISTS edition_supported)
	list(LENGTH expected_${edition} count)
	list(LENGTH edition_name_list names)
	if(NOT count EQUAL names)
		list(APPEND failed "FM${edition}: ${names} names, but ${count} expected values")
	endif()
	edition_names(${edition})
	foreach(pair IN LISTS expected_${edition})
		string(FIND "${pair}" "=" split)
		string(SUBSTRING "${pair}" 0 ${split} name)
		math(EXPR split "${split} + 1")
		string(SUBSTRING "${pair}" ${split} -1 value)
		if(NOT name IN_LIST edition_name_list)
			list(APPEND failed "FM${edition}: ${name} is not one of edition.h's names")
		elseif(NOT EDITION_${name} STREQUAL value)
			list(APPEND failed "FM${edition}: EDITION_${name} is '${EDITION_${name}}', not '${value}'")
		endif()
		set(value_${edition}_${name} "${EDITION_${name}}")
	endforeach()
	set(plugin_24 0)
	set(plugin_27 1)
	if(NOT EDITION_ENGINE_PLUGIN STREQUAL plugin_${edition})
		list(APPEND failed "FM${edition}: EDITION_ENGINE_PLUGIN is '${EDITION_ENGINE_PLUGIN}', "
			"not '${plugin_${edition}}'"
		)
	endif()
	set(phases_24 1)
	set(phases_27 2)
	if(NOT EDITION_TACTIC_PHASES STREQUAL phases_${edition})
		list(APPEND failed "FM${edition}: EDITION_TACTIC_PHASES is '${EDITION_TACTIC_PHASES}', "
			"not '${phases_${edition}}'"
		)
	endif()
endforeach()
# No file, folder, socket, bundle, format or launch option is shared.
foreach(name IN LISTS edition_name_list)
	if(value_24_${name} STREQUAL value_27_${name})
		list(APPEND failed "both editions have EDITION_${name} '${value_24_${name}}'")
	endif()
endforeach()

# The build's edition.h: every name, with this edition's value, as one literal.
file(STRINGS "${EDITION_HEADER}" header REGEX "^#define (FM_EDITION|EDITION_[A-Z_]+) ")
if(NOT header MATCHES "#define FM_EDITION ${FM_EDITION}(;|$)")
	list(APPEND failed "${EDITION_HEADER} is not for FM_EDITION=${FM_EDITION}")
endif()
edition_names(${FM_EDITION})
foreach(number ENGINE_PLUGIN TACTIC_PHASES)
	if(NOT header MATCHES "#define EDITION_${number} ${EDITION_${number}}(;|$)")
		list(APPEND failed
			"${EDITION_HEADER} lacks #define EDITION_${number} ${EDITION_${number}}"
		)
	endif()
endforeach()
foreach(name IN LISTS edition_name_list)
	set(line "#define EDITION_${name} \"${EDITION_${name}}\"")
	string(FIND "${header}" "${line}" at)
	if(at EQUAL -1)
		list(APPEND failed "${EDITION_HEADER} lacks ${line}")
	endif()
endforeach()

# Names of either edition in the sources. These may have them: the docs and tests (not scanned),
# cmake/edition.cmake, each edition's build tables, layouts, synthetic sites and supported builds
# (and the names of those files, wherever they are included), the mapping tools and
# experiments under tools/, and the original project's Windows sources (which build FM24 only).
# CMake's FM24_* settings and variables keep the repository's prefix in both editions, as does the
# viewer's Swift class Fm24ctl, which runs the CLI.
file(GLOB_RECURSE sources LIST_DIRECTORIES false
	"${SOURCE_DIR}/src/*" "${SOURCE_DIR}/cmake/*" "${SOURCE_DIR}/scripts/*" "${SOURCE_DIR}/.github/*"
)
list(APPEND sources "${SOURCE_DIR}/CMakeLists.txt")
set(allowed
	cmake/edition.cmake cmake/mingw64.cmake cmake/mingw64-cross.cmake
	src/common/macos_builds.c src/dll/game_macos_tables.c src/dll/game_macos_tables_fm27.c
	src/dll/game_layout_fm27.h src/dll/sites_fm27.h
	src/common/platform_windows.c src/dll/game.c src/dll/main.c src/dll/memory.c src/dll/profiler.c
	src/runner/client.c src/runner/embedded.c src/runner/fixture_load.c src/runner/fixture_load.h
	src/runner/inject.c src/runner/paths.c src/runner/probe.c src/runner/probe_patterns.c
	src/runner/probe_patterns.h src/runner/resources.h src/ui/fixture_window.c
	src/ui/fixture_window.h src/ui/viewer.manifest src/ui/viewer.rc
)
set(pattern "[Ff][Mm]2[47]|2252570|4530370|Football ?Manager ?(2024|27)")
set(scanned 0)
foreach(source IN LISTS sources)
	file(RELATIVE_PATH relative "${SOURCE_DIR}" "${source}")
	if(relative IN_LIST allowed)
		continue()
	endif()
	math(EXPR scanned "${scanned} + 1")
	file(STRINGS "${source}" lines REGEX "${pattern}")
	foreach(line IN LISTS lines)
		string(REGEX REPLACE
			"FM24_[A-Z0-9_]+|Fm24ctl|(game_layout|game_macos_tables|sites)_fm[0-9]+\\.[ch]" ""
			stripped "${line}"
		)
		if(stripped MATCHES "${pattern}")
			string(STRIP "${line}" line)
			list(APPEND failed "${relative}: ${line}")
		endif()
	endforeach()
endforeach()
if(scanned LESS 100)
	list(APPEND failed "only ${scanned} sources found under ${SOURCE_DIR}")
endif()

if(failed)
	list(JOIN failed "\n" failed)
	message(FATAL_ERROR "Edition names:\n${failed}")
endif()
message(STATUS "Both editions' names are distinct and in edition.h; ${scanned} sources use none")
