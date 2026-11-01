# SPDX-License-Identifier: GPL-3.0-or-later
# The edition's build tables (TABLES) are what tools/macos/gen_tables.py, run with PYTHON, generates
# from the edition's mappings under SOURCE_DIR: FM24's one build, or every mapping of FM27's, with
# FM27's synthetic sites (src/dll/sites_fm27.h). The generator refuses mappings that leave out an ID
# or site the harness uses without saying why, frames whose synthetic IDs the mappings do not give,
# and FM27 mappings whose layouts are not src/dll/game_layout_fm27.h's.
if(FM_EDITION STREQUAL "24")
	set(mappings "${SOURCE_DIR}/tools/macos/steam-24.4.2.json")
else()
	file(GLOB mappings "${SOURCE_DIR}/tools/macos/steam-${FM_EDITION}.*.json")
endif()
if(NOT mappings)
	message(FATAL_ERROR "no mappings for FM_EDITION=${FM_EDITION} in ${SOURCE_DIR}/tools/macos")
endif()
execute_process(
	COMMAND "${PYTHON}" -I "${SOURCE_DIR}/tools/macos/gen_tables.py" ${mappings} "${OUTPUT}"
	RESULT_VARIABLE result
	ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
	message(FATAL_ERROR "gen_tables.py refused the mappings:\n${error}")
endif()
file(READ "${TABLES}" committed)
file(READ "${OUTPUT}" generated)
if(NOT committed STREQUAL generated)
	message(FATAL_ERROR "${TABLES} is not what gen_tables.py generates from ${mappings}: regenerate it")
endif()
if(NOT FM_EDITION STREQUAL "24")
	get_filename_component(directory "${OUTPUT}" DIRECTORY)
	file(READ "${SOURCE_DIR}/src/dll/sites_fm${FM_EDITION}.h" committed)
	file(READ "${directory}/sites_fm${FM_EDITION}.h" generated)
	if(NOT committed STREQUAL generated)
		message(FATAL_ERROR "src/dll/sites_fm${FM_EDITION}.h is not what gen_tables.py generates: regenerate it")
	endif()
endif()
list(LENGTH mappings count)
message(STATUS "${TABLES} is generated from its ${count} mapping(s)")
