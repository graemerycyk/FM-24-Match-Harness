# SPDX-License-Identifier: GPL-3.0-or-later
# The texts of the CLI and the harness that the stand-in prints for the viewer's tests
# (tests/viewer_stand_in_cli.c, its `static const char <name>Text[]` strings) must be the real
# ones: each must be a whole string literal in the sources that print it, once the edition's names
# (EDITION_HEADER, the build's edition.h) are put in and adjacent literals are joined as the
# compiler joins them. Both must also line-buffer their standard output.
set(stand_in_file "${SOURCE_DIR}/tests/viewer_stand_in_cli.c")
set(real_files
	src/dll/job_execute.c
	src/runner/client_posix.c
	src/runner/commands.c
	src/runner/macos_game.c
	src/runner/session.c
)
file(STRINGS "${EDITION_HEADER}" edition_lines REGEX "^#define EDITION_[A-Z_]+ \"[^\"]*\"")
if(NOT edition_lines)
	message(FATAL_ERROR "no edition names in ${EDITION_HEADER}")
endif()
# The edition's names as the literals they stand for, adjacent literals as one, and the characters
# CMake lists treat specially as others: only whole strings are compared.
macro(read_joined file out)
	file(READ "${file}" ${out})
	foreach(line IN LISTS edition_lines)
		string(REGEX MATCH "^#define (EDITION_[A-Z_]+) (\"[^\"]*\")" edition_match "${line}")
		string(REGEX REPLACE "([^A-Za-z0-9_])${CMAKE_MATCH_1}([^A-Za-z0-9_])" "\\1${CMAKE_MATCH_2}\\2"
			${out} "${${out}}"
		)
	endforeach()
	string(REGEX REPLACE "\"[ \t\r\n]*\"" "" ${out} "${${out}}")
	string(REPLACE ";" "<semicolon>" ${out} "${${out}}")
	string(REPLACE "[" "<open>" ${out} "${${out}}")
	string(REPLACE "]" "<close>" ${out} "${${out}}")
endmacro()
read_joined("${stand_in_file}" stand_in)
set(real "")
foreach(file IN LISTS real_files)
	read_joined("${SOURCE_DIR}/${file}" text)
	string(APPEND real "${text}\n")
endforeach()

string(REGEX MATCHALL "static const char [A-Za-z]+Text<open><close> =[ \t\r\n]*\"([^\"\\\\]|\\\\.)*\""
	texts "${stand_in}"
)
list(LENGTH texts count)
if(count EQUAL 0)
	message(FATAL_ERROR "no texts found in ${stand_in_file}")
endif()
set(missing)
foreach(text IN LISTS texts)
	string(REGEX MATCH "([A-Za-z]+Text)<open>" name "${text}")
	set(name "${CMAKE_MATCH_1}")
	string(REGEX MATCH "\"([^\"\\\\]|\\\\.)*\"$" literal "${text}")
	string(FIND "${real}" "${literal}" at)
	if(at EQUAL -1)
		list(APPEND missing "${name}: ${literal}")
	endif()
endforeach()
# Its standard output is buffered as the CLI's is, a line at a time, so that the viewer's tests see
# progress arrive as it would from the CLI.
set(buffering "setvbuf(stdout, NULL, _IOLBF, 0)")
foreach(file IN ITEMS tests/viewer_stand_in_cli.c src/runner/main.c)
	file(READ "${SOURCE_DIR}/${file}" text)
	string(FIND "${text}" "${buffering}" at)
	if(at EQUAL -1)
		list(APPEND missing "${file} does not call ${buffering}")
	endif()
endforeach()
if(missing)
	list(JOIN missing "\n" missing)
	string(REPLACE "<semicolon>" ";" missing "${missing}")
	string(REPLACE "<open>" "[" missing "${missing}")
	string(REPLACE "<close>" "]" missing "${missing}")
	message(FATAL_ERROR "The stand-in does not print as the CLI does:\n${missing}")
endif()
message(STATUS "${count} stand-in texts are the CLI's own, and both buffer standard output alike")
