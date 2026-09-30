# DomTERM -- PTY/TTY management, Terminfo, session recording, connections,
# terminal orchestration and the Termscript automation language.
cmake_minimum_required(VERSION 3.15)
project(domterm LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

# Perl drives scripts/aurocks.pl, which compiles Termscript.g into the
# Termscript parser.  There is no fallback: the grammar is the source of
# truth for the language front end.
find_program(DOMTERM_PERL perl REQUIRED)
set(DOMTERM_AUROCKS ${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/aurocks.pl)

# ── generated Termscript parser ──────────────────────────────────────
# One translation unit, shared by the standalone runtime below.  The
# custom entry point avoids clashing with any other aurocks output; the
# ts_mk_* actions it calls live in ts_runtime.c.
set(DOMTERM_PARSER_C ${CMAKE_CURRENT_BINARY_DIR}/termscript_parser.c)
add_custom_command(
  OUTPUT ${DOMTERM_PARSER_C}
  COMMAND ${DOMTERM_PERL} ${DOMTERM_AUROCKS} --entrypoint ts_gram_parse
          ${CMAKE_CURRENT_SOURCE_DIR}/termscript/Termscript.g
          > ${DOMTERM_PARSER_C}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/termscript/Termscript.g
          ${DOMTERM_AUROCKS}
  COMMENT "Generating Termscript parser (aurocks)"
  VERBATIM
)

# ── libdomterm (core: no Termscript) ─────────────────────────────────
add_library(domterm STATIC
  domterm.c
  src/dt_util.c
  src/dt_terminfo.c
  src/dt_tiexpand.c
  src/dt_tty.c
  src/dt_pty.c
  src/dt_record.c
  src/dt_conn.c
  src/dt_puppeteer.c
)

target_include_directories(domterm PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/domterm>
)
target_include_directories(domterm PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(domterm PUBLIC Threads::Threads)

# ── libtermscript_standalone (embedding + extension API, no DomTERM) ─
add_library(termscript_standalone STATIC
  termscript/ts_runtime.c
  ${DOMTERM_PARSER_C}
)
target_include_directories(termscript_standalone PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_link_libraries(termscript_standalone PRIVATE Threads::Threads)

# ── libtermscript (DomTERM Termscript bindings; links standalone) ─────
add_library(termscript STATIC
  src/dt_termscript.c
)
target_include_directories(termscript PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/domterm>
)
target_include_directories(termscript PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(termscript PUBLIC domterm termscript_standalone)

# ── libtermscript_stdlib (hybrid C + Termscript companion modules) ─
add_subdirectory(termscript/stdlib)
if(TARGET termscript_stdlib)
  # Every DT_TermVM carries the stdlib natively (registered in
  # dt_termscript_create), so the driver and transpiled programs get
  # G:import out of the box.
  target_link_libraries(termscript PUBLIC termscript_stdlib)
endif()

# The standalone embedding library still exposes the stdlib hooks through
# dt_termscript.c.  Keep a useful minimal build when optional vendored
# readline/TOML sources are unavailable.
if(NOT TARGET termscript_stdlib)
  add_library(termscript_stdlib_stub STATIC termscript/stdlib/stub.c)
  target_include_directories(termscript_stdlib_stub PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
  target_link_libraries(termscript_stdlib_stub PUBLIC termscript_standalone)
  target_link_libraries(termscript PUBLIC termscript_stdlib_stub)
endif()

# The generated parser carries benign unused-variable warnings from the
# aurocks runtime scaffolding; keep hand-written sources warning-clean
# without drowning the build log.
set_source_files_properties(${DOMTERM_PARSER_C} PROPERTIES COMPILE_OPTIONS "-w")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(domterm PRIVATE -Wall -Wextra)
  target_compile_options(termscript PRIVATE -Wall -Wextra)
  target_compile_options(termscript_standalone PRIVATE -Wall -Wextra)
endif()

# ── termscript driver ────────────────────────────────────────────────
# NOTE: the binary cannot land next to the target (domlibs/domterm/)
# because OUTPUT_NAME "termscript" would collide with the termscript/
# source directory; it goes to the build-tree bin/ instead.
add_executable(termscript-driver termscript/driver.c)
set_target_properties(termscript-driver PROPERTIES
  OUTPUT_NAME termscript
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
target_link_libraries(termscript-driver PRIVATE termscript)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termscript-driver PRIVATE -Wall -Wextra)
endif()

# ── install ──────────────────────────────────────────────────────────
include(GNUInstallDirs)

install(TARGETS domterm termscript termscript_standalone
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(TARGETS termscript-driver
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES domterm.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/domterm
)
install(FILES termscript/termscript.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript
)
install(FILES termscript/Termscript.g
  DESTINATION ${CMAKE_INSTALL_DATADIR}/domterm
)
