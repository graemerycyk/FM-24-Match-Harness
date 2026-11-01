# SPDX-License-Identifier: GPL-3.0-or-later
# cmake/build_info.cmake (SCRIPT) against repositories made in WORK with GIT: the commit and whether
# tracked files changed, "unknown" outside a repository's top, and no rewrite while nothing changed.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/repo/folder")
set(output "${WORK}/build_info.c")
# The user's Git configuration (signing, hooks) stays out of the test's commits.
set(git "${GIT}" -c user.name=fm24 -c user.email=fm24@example.invalid -c commit.gpgsign=false
	-c core.hooksPath=hooks-none -c init.defaultBranch=main
)

macro(run_git)
	execute_process(COMMAND ${git} ${ARGN} WORKING_DIRECTORY "${WORK}/repo"
		RESULT_VARIABLE failed OUTPUT_VARIABLE git_output ERROR_VARIABLE git_error
	)
	if(failed)
		message(FATAL_ERROR "git ${ARGN} failed: ${git_error}")
	endif()
endmacro()

# Generates the file for `source` and checks its commit and dirty flag.
function(expect source with_git commit dirty)
	set(found_git "")
	if(with_git)
		set(found_git "${GIT}")
	endif()
	execute_process(
		COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${source}" "-DOUTPUT=${output}" "-DGIT=${found_git}"
			-P "${SCRIPT}"
		RESULT_VARIABLE failed ERROR_VARIABLE error
	)
	if(failed)
		message(FATAL_ERROR "the generator failed for ${source}: ${error}")
	endif()
	file(READ "${output}" text)
	string(FIND "${text}" "const char buildInfo_commit[] = \"${commit}\";\n" at_commit)
	string(FIND "${text}" "const int buildInfo_dirty = ${dirty};\n" at_dirty)
	if(at_commit EQUAL -1 OR at_dirty EQUAL -1)
		message(FATAL_ERROR "${source}: expected ${commit} and ${dirty}, got:\n${text}")
	endif()
endfunction()

file(WRITE "${WORK}/repo/tracked.txt" "one\n")
run_git(init -q .)
run_git(add tracked.txt)
run_git(commit -q -m first)
run_git(rev-parse HEAD)
string(STRIP "${git_output}" head)

expect("${WORK}/repo" ON "${head}" 0)
# An unchanged state leaves the file alone.
file(TIMESTAMP "${output}" before "%s")
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 1.1)
expect("${WORK}/repo" ON "${head}" 0)
file(TIMESTAMP "${output}" after "%s")
if(NOT before STREQUAL after)
	message(FATAL_ERROR "the generator rewrote an unchanged file")
endif()
# Untracked files do not make the tree dirty; changed or newly added tracked ones do.
file(WRITE "${WORK}/repo/untracked.txt" "x\n")
expect("${WORK}/repo" ON "${head}" 0)
file(WRITE "${WORK}/repo/tracked.txt" "two\n")
expect("${WORK}/repo" ON "${head}" 1)
run_git(checkout -q -- tracked.txt)
expect("${WORK}/repo" ON "${head}" 0)
run_git(add untracked.txt)
expect("${WORK}/repo" ON "${head}" 1)
run_git(commit -q -m second)
run_git(rev-parse HEAD)
string(STRIP "${git_output}" second)
expect("${WORK}/repo" ON "${second}" 0)
if(second STREQUAL head)
	message(FATAL_ERROR "the second commit has the first one's hash")
endif()
# A folder inside a repository is not its top, and without Git nothing is known.
expect("${WORK}/repo/folder" ON unknown -1)
expect("${WORK}/repo" OFF unknown -1)
file(REMOVE_RECURSE "${WORK}")
message(STATUS "build_info.cmake reads the commit and the dirty flag")
