# SPDX-License-Identifier: GPL-3.0-or-later
# The game edition a build is for, every name that tells the editions apart, and where each
# edition's game keeps its match engine. Only this file holds them: CMake writes them into
# edition.h (src/common/edition.h.in), which the C sources include and the viewer's Swift imports
# as the module Edition, and the build and tests use the EDITION_* variables. The editions share
# no file, folder, socket, bundle or launch option, so both can be installed side by side.
#   edition_names(<24|27>)  sets the EDITION_* variables for that edition (also in script mode)

set(edition_supported 24 27)
# The names edition.h defines, in its order; edition_names sets EDITION_<name> for each.
set(edition_name_list
	PRODUCT LOWER GAME GAME_VERSION GAME_BUNDLE_ID GAME_EXECUTABLE STEAM_APP_ID DATA_FOLDER
	ENGINE_PATH CLI CLI_ENV ENV_PREFIX HARNESS LAUNCHER SECTION MATCH_CONFIG_FORMAT PLAYER_FORMAT
	TEMPORARY_FIXTURE JOB_INPUT_PREFIX VIEWER_NAME VIEWER_APP VIEWER_EXECUTABLE VIEWER_ID
)
# EDITION_ENGINE_PLUGIN, a number, is set with them: 1 where the game loads its match engine from a
# plugin bundle once it has started (EDITION_ENGINE_PATH, under the app's Contents/PlugIns), 0
# where the engine is in the game's executable (and EDITION_ENGINE_PATH is empty). So is
# EDITION_TACTIC_PHASES, the phases of the game's tactics: 1 where a plan's formation slots each
# have a position and a role with its duty, and tactical familiarity has seven named categories; 2
# where each slot has a position and a role, without duty, in possession and others out of
# possession, and familiarity has six categories, numbered.

macro(edition_names edition)
	if(NOT "${edition}" IN_LIST edition_supported)
		message(FATAL_ERROR "FM_EDITION must be 24 or 27 (not ${edition})")
	endif()
	if("${edition}" STREQUAL "24")
		set(EDITION_GAME "Football Manager 2024")
		set(EDITION_GAME_VERSION "24.4.2")
		set(EDITION_GAME_BUNDLE_ID "com.sigames.eur.FootballManager2024")
		set(EDITION_GAME_EXECUTABLE "fm")
		set(EDITION_STEAM_APP_ID "2252570")
		# The match engine is in the game's executable.
		set(EDITION_ENGINE_PLUGIN 0)
		set(EDITION_ENGINE_PATH "")
		# One formation, whose roles have duties.
		set(EDITION_TACTIC_PHASES 1)
		# The released version's names, which the original project's Windows viewer also uses.
		set(EDITION_TEMPORARY_FIXTURE "temp-fixture.json")
		set(EDITION_JOB_INPUT_PREFIX "_job_input-")
	else()
		set(EDITION_GAME "Football Manager 27")
		set(EDITION_GAME_VERSION "27.0.0.2486716")
		set(EDITION_GAME_BUNDLE_ID "com.SportsInteractive.FootballManager27")
		set(EDITION_GAME_EXECUTABLE "Football Manager 27")
		set(EDITION_STEAM_APP_ID "4530370")
		# The match engine is in a plugin bundle the game loads once it has started.
		set(EDITION_ENGINE_PLUGIN 1)
		set(EDITION_ENGINE_PATH "game_plugin.bundle/Contents/MacOS/game_plugin")
		# A formation in possession and one out of possession, with roles but no duties.
		set(EDITION_TACTIC_PHASES 2)
		set(EDITION_TEMPORARY_FIXTURE "fm${edition}-temp-fixture.json")
		set(EDITION_JOB_INPUT_PREFIX "fm${edition}-job-input-")
	endif()
	# The rest follow from the edition's number.
	set(EDITION_PRODUCT "FM${edition}")
	set(EDITION_LOWER "fm${edition}")
	set(EDITION_DATA_FOLDER "Sports Interactive/${EDITION_GAME}")
	set(EDITION_CLI "fm${edition}ctl")
	set(EDITION_CLI_ENV "FM${edition}CTL")
	set(EDITION_ENV_PREFIX "FM${edition}_")
	set(EDITION_HARNESS "fm${edition}-harness")
	set(EDITION_LAUNCHER "launch-fm${edition}.sh")
	set(EDITION_SECTION "__fm${edition}_harness")
	set(EDITION_MATCH_CONFIG_FORMAT "fm${edition}-match-config/1")
	set(EDITION_PLAYER_FORMAT "fm${edition}-player/1")
	set(EDITION_VIEWER_NAME "FM${edition} Viewer")
	set(EDITION_VIEWER_APP "FM${edition}Viewer.app")
	set(EDITION_VIEWER_EXECUTABLE "fm${edition}viewer")
	set(EDITION_VIEWER_ID "io.github.graemerycyk.fm${edition}-match-harness.viewer")
endmacro()

if(CMAKE_SCRIPT_MODE_FILE)
	return()
endif()

# The Windows build is the original project's, for FM24 only.
if(WIN32)
	set(edition_default 24)
else()
	set(edition_default 27)
endif()
set(FM_EDITION ${edition_default} CACHE STRING "The game edition to build for: 24 or 27")
set_property(CACHE FM_EDITION PROPERTY STRINGS ${edition_supported})
if(WIN32 AND NOT FM_EDITION STREQUAL "24")
	message(FATAL_ERROR "The Windows build supports FM24 only (FM_EDITION=24, not ${FM_EDITION})")
endif()
# A build folder keeps the edition it was first configured for: the editions' outputs have other
# names, so switching would leave the other edition's beside them. A folder configured before there
# were editions is FM24's.
if(NOT DEFINED CACHE{FM_EDITION_CONFIGURED} AND EXISTS "${CMAKE_BINARY_DIR}/CMakeFiles/fm24ctl.dir")
	set(FM_EDITION_CONFIGURED 24 CACHE INTERNAL "The edition this build folder is for")
endif()
if(DEFINED CACHE{FM_EDITION_CONFIGURED} AND NOT FM_EDITION STREQUAL FM_EDITION_CONFIGURED)
	message(FATAL_ERROR
		"${CMAKE_BINARY_DIR} was configured for FM_EDITION=${FM_EDITION_CONFIGURED}, not "
		"${FM_EDITION}: use another build folder, or delete this one to switch"
	)
endif()
set(FM_EDITION_CONFIGURED ${FM_EDITION} CACHE INTERNAL "The edition this build folder is for")
edition_names(${FM_EDITION})
message(STATUS "Building for ${EDITION_GAME} (FM_EDITION=${FM_EDITION}): ${EDITION_CLI}")

# edition.h, and the module map through which the viewer's Swift imports it.
set(EDITION_INCLUDE_DIR "${CMAKE_BINARY_DIR}/generated")
configure_file(src/common/edition.h.in "${EDITION_INCLUDE_DIR}/edition.h" @ONLY)
file(CONFIGURE OUTPUT "${EDITION_INCLUDE_DIR}/module.modulemap" CONTENT
	"// The edition's names for the viewer's Swift (generated by cmake/edition.cmake).
module Edition {
	header \"edition.h\"
	export *
}
")
