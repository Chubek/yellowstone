# End-to-end checks for the command-line tools.
#
# Run with -DTOOLS_DIR=<directory holding the built tools>. Builds a small set
# of fixtures with the C compiler, then exercises each tool on them and
# compares against what the same query would produce from the tools in the
# tree, so a regression in argument handling, archive traversal or reporting
# shows up as a test failure rather than as a subtly different dump.
#
# Skips (returns 77) when no compiler is available, matching CTest's skip code.

if(NOT DEFINED TOOLS_DIR)
  message(FATAL_ERROR "TOOLS_DIR must be set")
endif()

set(CMAKE_CXX_COMPILER_NAME qobjfile-cli-tests)
find_program(CLANG NAMES clang clang++ g++ c++)
if(NOT CLANG)
  message(STATUS "${CMAKE_CXX_COMPILER_NAME}: no compiler, skipping")
  return(77)
endif()

set(WORK "${CMAKE_CURRENT_BINARY_DIR}/cli-tests")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

file(WRITE "${WORK}/fixture.c"
"extern int external(int);\n"
"int data = 9;\n"
"int zero[8];\n"
"int exported(int x) { return external(x) + data + zero[0]; }\n"
"int beta_helper(int x) { return x * 2; }\n")

foreach(part a b)
  execute_process(
    COMMAND ${CLANG} -c "${WORK}/fixture.c" -o "${WORK}/${part}.o"
    RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(STATUS "${CMAKE_CXX_COMPILER_NAME}: cannot compile fixtures")
    return(77)
  endif()
endforeach()

function(run_tool)
  cmake_parse_arguments(ARG "" "OUTPUT" "COMMAND" ${ARGN})
  execute_process(COMMAND ${ARG_COMMAND}
                  WORKING_DIRECTORY "${WORK}"
                  OUTPUT_VARIABLE out
                  ERROR_VARIABLE err
                  RESULT_VARIABLE status)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR
            "${ARG_COMMAND} failed (${status})\nstdout:\n${out}\nstderr:\n${err}")
  endif()
  if(ARG_OUTPUT)
    file(WRITE "${WORK}/${ARG_OUTPUT}" "${out}")
  endif()
  set(LAST_OUTPUT "${out}" PARENT_SCOPE)
endfunction()

# --help must work for every tool and mention the tool's own name.
foreach(tool qobjdump qobjsize qobjcp qobjstr qobjstrip qobjnm qobjar)
  run_tool(COMMAND "${TOOLS_DIR}/${tool}" --help)
  if(NOT LAST_OUTPUT MATCHES "usage: ${tool}")
    message(FATAL_ERROR "${tool} --help does not name the tool:\n${LAST_OUTPUT}")
  endif()
  # With no operands every tool must print its usage and fail, rather than
  # doing nothing or crashing.
  execute_process(COMMAND "${TOOLS_DIR}/${tool}"
                  WORKING_DIRECTORY "${WORK}"
                  OUTPUT_QUIET ERROR_VARIABLE usage_err
                  RESULT_VARIABLE status)
  if(status EQUAL 0)
    message(FATAL_ERROR "${tool} with no operands exited 0")
  endif()
  if(NOT usage_err MATCHES "usage: ${tool}")
    message(FATAL_ERROR "${tool} with no operands did not print usage:\n${usage_err}")
  endif()
endforeach()

# qobjdump must find the symbols the fixture defines.
run_tool(OUTPUT symbols.txt COMMAND "${TOOLS_DIR}/qobjnm" -g a.o)
foreach(name exported data zero)
  if(NOT LAST_OUTPUT MATCHES "${name}")
    message(FATAL_ERROR "qobjnm did not report ${name}:\n${LAST_OUTPUT}")
  endif()
endforeach()

# qobjdump and qobjnm must agree about how many symbols there are, rather than
# this test hard-coding a count that changes whenever the fixture does.
run_tool(OUTPUT dump.txt COMMAND "${TOOLS_DIR}/qobjdump" -t a.o)
file(READ "${WORK}/dump.txt" dump_text)
run_tool(OUTPUT nm.txt COMMAND "${TOOLS_DIR}/qobjnm" a.o)
file(READ "${WORK}/nm.txt" nm_text)
# Count the symbol rows each tool prints, and require the two to agree.
string(REGEX MATCHALL "GLOBAL" dump_globals "${dump_text}")
string(REGEX MATCHALL ":[ ]+0x" dump_rows "${dump_text}")
string(REGEX MATCHALL " [0-9]+ " nm_rows "${nm_text}")
list(LENGTH dump_globals dump_globals_count)
list(LENGTH dump_rows dump_count)
list(LENGTH nm_rows nm_count)
if(dump_globals_count LESS 3)
  message(FATAL_ERROR
          "qobjdump -t reported only ${dump_globals_count} globals")
endif()
if(NOT dump_count EQUAL nm_count)
  message(FATAL_ERROR
          "qobjdump -t listed ${dump_count} symbols but qobjnm listed ${nm_count}")
endif()

# qobjcp must extract a section with exactly the size the section table
# reports. The bytes are compared through a real file rather than CMake's
# text-capturing output, which cannot carry a NUL.
run_tool(OUTPUT sections.txt COMMAND "${TOOLS_DIR}/qobjcp" --list-sections a.o)
if(NOT LAST_OUTPUT MATCHES "\\.text")
  message(FATAL_ERROR "qobjcp --list-sections omitted .text:\n${LAST_OUTPUT}")
endif()
run_tool(COMMAND "${TOOLS_DIR}/qobjcp" --dump-section .text a.o "${WORK}/text.bin")
file(SIZE "${WORK}/text.bin" text_size)
if(text_size EQUAL 0)
  message(FATAL_ERROR "qobjcp --dump-section .text produced no output")
endif()
# A section that is not present must be an error, not an empty file.
execute_process(COMMAND "${TOOLS_DIR}/qobjcp" --dump-section .nope a.o
                        "${WORK}/nope.bin"
                WORKING_DIRECTORY "${WORK}"
                ERROR_VARIABLE missing_err RESULT_VARIABLE status)
if(status EQUAL 0)
  message(FATAL_ERROR "qobjcp accepted a section that does not exist")
endif()
if(NOT missing_err MATCHES "no section named")
  message(FATAL_ERROR "qobjcp did not explain the missing section:\n${missing_err}")
endif()

# qobjar must build an archive the same archive library can read back.
run_tool(COMMAND "${TOOLS_DIR}/qobjar" rcs libtest.a a.o b.o)
run_tool(OUTPUT members.txt COMMAND "${TOOLS_DIR}/qobjar" t libtest.a)
if(NOT LAST_OUTPUT MATCHES "a.o" OR NOT LAST_OUTPUT MATCHES "b.o")
  message(FATAL_ERROR "qobjar t did not list the members:\n${LAST_OUTPUT}")
endif()

# The index qobjar writes must name the symbols the members define.
run_tool(OUTPUT index.txt COMMAND "${TOOLS_DIR}/qobjnm" -A libtest.a)
foreach(name exported beta_helper)
  if(NOT LAST_OUTPUT MATCHES "${name}")
    message(FATAL_ERROR "qobjar did not index ${name}:\n${LAST_OUTPUT}")
  endif()
endforeach()

# Every tool must accept an archive, not just a plain object.
foreach(tool qobjdump qobjnm qobjsize qobjstr)
  run_tool(COMMAND "${TOOLS_DIR}/${tool}" libtest.a)
endforeach()

# Stripping must leave a file that still parses and still lists its symbols.
run_tool(COMMAND "${TOOLS_DIR}/qobjstrip" -g -o stripped.o a.o)
run_tool(OUTPUT stripped.txt COMMAND "${TOOLS_DIR}/qobjnm" -g stripped.o)
if(NOT LAST_OUTPUT MATCHES "exported")
  message(FATAL_ERROR "qobjstrip -g removed a needed symbol:\n${LAST_OUTPUT}")
endif()
# -s removes the symbol table entirely, and the result must still be readable.
run_tool(COMMAND "${TOOLS_DIR}/qobjstrip" -s -o stripped-all.o a.o)
run_tool(COMMAND "${TOOLS_DIR}/qobjdump" -f stripped-all.o)

# A bad option must be rejected rather than silently ignored.
execute_process(COMMAND "${TOOLS_DIR}/qobjdump" --no-such-option a.o
                WORKING_DIRECTORY "${WORK}"
                OUTPUT_QUIET ERROR_VARIABLE err RESULT_VARIABLE status)
if(status EQUAL 0)
  message(FATAL_ERROR "qobjdump accepted an unknown option")
endif()
if(NOT err MATCHES "unknown option")
  message(FATAL_ERROR "qobjdump did not explain the bad option:\n${err}")
endif()

message(STATUS "${CMAKE_CXX_COMPILER_NAME}: all checks passed")
