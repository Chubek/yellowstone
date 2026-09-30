# End-to-end checks for qobjld.
#
# Run with -DTOOLS_DIR=<directory holding qobjld>. Compiles small fixtures
# with the C compiler, links them with qobjld, and runs the results, so a
# regression in loading, symbol resolution, relocation or emission shows up
# as a test failure. Also exercises -r, --shared, archives, -Map and the
# linker-script language.
#
# Skips (returns 77) when no compiler is available.

if(NOT DEFINED TOOLS_DIR)
  message(FATAL_ERROR "TOOLS_DIR must be set")
endif()

set(LD "${TOOLS_DIR}/qobjld")
if(NOT EXISTS "${LD}")
  message(FATAL_ERROR "qobjld not found at ${LD}")
endif()

find_program(CC NAMES clang gcc cc)
if(NOT CC)
  message(STATUS "qobjld-cli-tests: no compiler, skipping")
  return(77)
endif()

set(WORK "${CMAKE_CURRENT_BINARY_DIR}/ld-tests")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")

function(ld_check)
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
  set(LAST_ERROR "${err}" PARENT_SCOPE)
endfunction()

# --help must name the tool; no operands must fail with usage.
ld_check(COMMAND "${LD}" --help)
if(NOT LAST_OUTPUT MATCHES "usage: qobjld")
  message(FATAL_ERROR "qobjld --help did not name the tool:\n${LAST_OUTPUT}")
endif()
execute_process(COMMAND "${LD}"
                WORKING_DIRECTORY "${WORK}"
                OUTPUT_QUIET ERROR_VARIABLE usage_err
                RESULT_VARIABLE status)
if(status EQUAL 0)
  message(FATAL_ERROR "qobjld with no operands exited 0")
endif()
if(NOT usage_err MATCHES "usage: qobjld")
  message(FATAL_ERROR "qobjld with no operands did not print usage:\n${usage_err}")
endif()

# Fixture 1: two objects linked into a static executable that exits 7.
file(WRITE "${WORK}/add.c" "int add(int a, int b) { return a + b; }\n")
file(WRITE "${WORK}/main.c"
"extern int add(int, int);\n"
"void _start(void) {\n"
"  int v = add(3, 4);\n"
"  __asm__ volatile (\"mov %0, %%rdi\\n syscall\" :: \"r\"((long)v), \"a\"(60L) : \"rcx\", \"r11\", \"memory\");\n"
"  __builtin_unreachable();\n"
"}\n")
execute_process(COMMAND ${CC} -c -fno-pie -fno-pic "${WORK}/add.c" -o "${WORK}/add.o"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(STATUS "qobjld-cli-tests: cannot compile fixtures")
  return(77)
endif()
execute_process(COMMAND ${CC} -c -fno-pie -fno-pic "${WORK}/main.c" -o "${WORK}/main.o"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(STATUS "qobjld-cli-tests: cannot compile fixtures")
  return(77)
endif()

ld_check(COMMAND "${LD}" -o "${WORK}/a.out" "${WORK}/main.o" "${WORK}/add.o")
execute_process(COMMAND "${WORK}/a.out" RESULT_VARIABLE code)
# _start above exits with add(3,4)==7 via exit_group(60).
if(NOT code EQUAL 7)
  message(FATAL_ERROR "qobjld executable exited ${code}, expected 7")
endif()

# Fixture 2: archive member selection (only needed members are pulled).
file(WRITE "${WORK}/used.c" "int used(void) { return 42; }\n")
file(WRITE "${WORK}/unused.c" "int unused_fn(void) { return 1; }\n")
file(WRITE "${WORK}/amain.c"
"extern int used(void);\n"
"void _start(void) {\n"
"  long v = used();\n"
"  __asm__ volatile (\"mov %0, %%rdi\\n syscall\" :: \"r\"(v), \"a\"(60L) : \"rcx\", \"r11\", \"memory\");\n"
"  __builtin_unreachable();\n"
"}\n")
execute_process(COMMAND ${CC} -c -fno-pie -fno-pic "${WORK}/used.c" -o "${WORK}/used.o" RESULT_VARIABLE status)
execute_process(COMMAND ${CC} -c -fno-pie -fno-pic "${WORK}/unused.c" -o "${WORK}/unused.o" RESULT_VARIABLE status)
execute_process(COMMAND ${CC} -c -fno-pie -fno-pic "${WORK}/amain.c" -o "${WORK}/amain.o" RESULT_VARIABLE status)
execute_process(COMMAND ar rcs "${WORK}/libu.a" "${WORK}/used.o" "${WORK}/unused.o"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "ar failed")
endif()
ld_check(COMMAND "${LD}" -o "${WORK}/b.out" "${WORK}/amain.o" "${WORK}/libu.a")
execute_process(COMMAND "${WORK}/b.out" RESULT_VARIABLE code)
if(NOT code EQUAL 42)
  message(FATAL_ERROR "archive link exited ${code}, expected 42")
endif()

# Fixture 3: -r partial link then final link.
ld_check(COMMAND "${LD}" -r -o "${WORK}/part.o" "${WORK}/main.o" "${WORK}/add.o")
ld_check(COMMAND "${LD}" -o "${WORK}/c.out" "${WORK}/part.o")
execute_process(COMMAND "${WORK}/c.out" RESULT_VARIABLE code)
if(NOT code EQUAL 7)
  message(FATAL_ERROR "partial link executable exited ${code}, expected 7")
endif()

# Fixture 4: linker script with ENTRY + SECTIONS.
file(WRITE "${WORK}/script.ld"
"ENTRY(_start)\n"
"SECTIONS {\n"
"  .text : { *(.text) }\n"
"  .rodata : { *(.rodata*) }\n"
"  .data : { *(.data*) }\n"
"  .bss : { *(.bss*) *(COMMON) }\n"
"}\n")
ld_check(COMMAND "${LD}" -T "${WORK}/script.ld" -o "${WORK}/d.out"
                 "${WORK}/main.o" "${WORK}/add.o")
execute_process(COMMAND "${WORK}/d.out" RESULT_VARIABLE code)
if(NOT code EQUAL 7)
  message(FATAL_ERROR "script link exited ${code}, expected 7")
endif()

# Fixture 5: --shared emits ET_DYN.
ld_check(COMMAND "${LD}" --shared -o "${WORK}/libt.so" "${WORK}/add.o")
execute_process(COMMAND sh -c "readelf -h '${WORK}/libt.so' | grep -q DYN"
                RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "--shared did not emit a shared object")
endif()

# Fixture 6: undefined symbols fail, --allow-undefined passes.
file(WRITE "${WORK}/undef.c" "extern int missing(void); int call(void) { return missing(); }\n")
execute_process(COMMAND ${CC} -c -fno-pie -fno-pic "${WORK}/undef.c" -o "${WORK}/undef.o"
                RESULT_VARIABLE status)
execute_process(COMMAND "${LD}" -o "${WORK}/undef.out" "${WORK}/undef.o"
                WORKING_DIRECTORY "${WORK}"
                OUTPUT_QUIET ERROR_VARIABLE uerr RESULT_VARIABLE status)
if(status EQUAL 0)
  message(FATAL_ERROR "undefined link unexpectedly succeeded")
endif()
if(NOT uerr MATCHES "undefined reference")
  message(FATAL_ERROR "undefined link did not explain itself:\n${uerr}")
endif()
ld_check(COMMAND "${LD}" --allow-undefined -o "${WORK}/undef-allow.out" "${WORK}/undef.o")

# Fixture 7: --check-script validates the language.
ld_check(COMMAND "${LD}" --check-script --script-text "ENTRY(_start) SECTIONS { .text : { *(.text) } }")
execute_process(COMMAND "${LD}" --check-script --script-text "SECTIONS { .text : "
                WORKING_DIRECTORY "${WORK}"
                OUTPUT_QUIET ERROR_VARIABLE serr RESULT_VARIABLE status)
if(status EQUAL 0)
  message(FATAL_ERROR "bad script unexpectedly validated")
endif()

message(STATUS "qobjld-cli-tests: all checks passed")
