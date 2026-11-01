# SPDX-License-Identifier: GPL-3.0-or-later
# macOS: a dry run of scripts/release_macos.sh on the build in BUILD_DIR, for FM_EDITION, which
# packages that build as it is (--build, signed ad hoc), and the zip it makes: named after the
# edition, with its CLI, its viewer (and the viewer's own CLI), both universal and signed, and the
# READ ME pointing at the edition's quick start, nothing named after the other edition, and a
# SHA-256 file that checks out. The build's own outputs are left as they were.
include("${SOURCE_DIR}/cmake/edition.cmake")
if(FM_EDITION STREQUAL "24")
	set(other 27)
else()
	set(other 24)
endif()
edition_names(${other})
set(other_names "${EDITION_PRODUCT}" "${EDITION_CLI}" "${EDITION_VIEWER_APP}" "${EDITION_LOWER}")
edition_names(${FM_EDITION})

set(dist "${BUILD_DIR}/release-package-test")
set(name "${EDITION_PRODUCT}-Harness-macOS-test")
set(zip "${dist}/${name}.zip")
file(REMOVE_RECURSE "${dist}")
file(SHA256 "${BUILD_DIR}/${EDITION_CLI}" cli_before)

execute_process(
	COMMAND "${CMAKE_COMMAND}" -E env --unset=FM_EDITION "CMAKE=${CMAKE_COMMAND}"
		/bin/sh "${SOURCE_DIR}/scripts/release_macos.sh" --version test --identity -
		--build "${BUILD_DIR}" --dist "${dist}"
	RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE output
)
if(result)
	message(FATAL_ERROR "scripts/release_macos.sh failed (${result}):\n${output}")
endif()
if(NOT output MATCHES "made ${dist}/${name}\\.zip")
	message(FATAL_ERROR "scripts/release_macos.sh did not make ${name}.zip:\n${output}")
endif()
execute_process(COMMAND shasum -a 256 -c "${name}.zip.sha256" WORKING_DIRECTORY "${dist}"
	RESULT_VARIABLE mismatch OUTPUT_QUIET ERROR_QUIET
)
if(mismatch)
	message(FATAL_ERROR "${name}.zip.sha256 does not check out")
endif()

# What the zip holds.
execute_process(COMMAND zipinfo -1 "${zip}" OUTPUT_VARIABLE listing COMMAND_ERROR_IS_FATAL ANY)
string(REPLACE "\n" ";" entries "${listing}")
set(app "${name}/${EDITION_VIEWER_APP}")
foreach(entry IN ITEMS "${name}/${EDITION_CLI}" "${name}/READ ME.txt" "${app}/Contents/Info.plist"
		"${app}/Contents/MacOS/${EDITION_VIEWER_EXECUTABLE}" "${app}/Contents/MacOS/${EDITION_CLI}")
	if(NOT entry IN_LIST entries)
		message(FATAL_ERROR "${name}.zip lacks ${entry}:\n${listing}")
	endif()
endforeach()
foreach(entry IN LISTS entries)
	if(entry AND NOT entry MATCHES "^${name}/")
		message(FATAL_ERROR "${name}.zip holds ${entry} outside its folder")
	endif()
	foreach(other_name IN LISTS other_names)
		string(FIND "${entry}" "${other_name}" at)
		if(NOT at EQUAL -1)
			message(FATAL_ERROR "${name}.zip holds ${entry}, named after the other edition")
		endif()
	endforeach()
endforeach()

# Unpacked: universal, signed, and a READ ME for this edition.
set(unpacked "${dist}/unpacked")
execute_process(COMMAND ditto -x -k "${zip}" "${unpacked}" COMMAND_ERROR_IS_FATAL ANY)
set(folder "${unpacked}/${name}")
foreach(binary IN ITEMS "${folder}/${EDITION_CLI}"
		"${folder}/${EDITION_VIEWER_APP}/Contents/MacOS/${EDITION_CLI}")
	execute_process(COMMAND lipo -archs "${binary}" OUTPUT_VARIABLE archs COMMAND_ERROR_IS_FATAL ANY)
	if(NOT archs MATCHES "x86_64" OR NOT archs MATCHES "arm64")
		message(FATAL_ERROR "${binary}: expected a universal binary, got ${archs}")
	endif()
endforeach()
execute_process(COMMAND codesign --verify --strict --deep "${folder}/${EDITION_VIEWER_APP}"
	RESULT_VARIABLE invalid ERROR_VARIABLE why
)
execute_process(COMMAND codesign --verify --strict "${folder}/${EDITION_CLI}"
	RESULT_VARIABLE invalid_cli ERROR_VARIABLE why_cli
)
if(invalid OR invalid_cli)
	message(FATAL_ERROR "the release's signatures are not valid: ${why}${why_cli}")
endif()
if(EXISTS "${SOURCE_DIR}/README-${EDITION_PRODUCT}.md")
	set(readme "README-${EDITION_PRODUCT}.md")
else()
	set(readme "README.md")
endif()
file(READ "${folder}/READ ME.txt" text)
foreach(expected IN ITEMS "${EDITION_PRODUCT} match-engine harness for macOS, test ("
		"${EDITION_VIEWER_APP} exports" "${EDITION_CLI} is the command line" "/${readme}#quick-start"
		"Needs ${EDITION_PRODUCT} ${EDITION_GAME_VERSION} from Steam")
	string(FIND "${text}" "${expected}" at)
	if(at EQUAL -1)
		message(FATAL_ERROR "READ ME.txt lacks '${expected}':\n${text}")
	endif()
endforeach()

file(SHA256 "${BUILD_DIR}/${EDITION_CLI}" cli_after)
if(NOT cli_before STREQUAL cli_after)
	message(FATAL_ERROR "the dry run changed the build's ${EDITION_CLI}")
endif()
file(REMOVE_RECURSE "${dist}" "${BUILD_DIR}/release-macos")
