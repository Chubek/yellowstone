# Termscript standard library — hybrid C native modules + .tsc companions.
#
# Layout:
#   <name>/<name>.c     native TS_Module (storage, parsing, syscalls)
#   <name>/<name>.h     module declaration (installed)
#   <name>/<name>.tsc   Termscript companion, loaded via G:import (installed)
#   common/             shared sbuf/args/list-store/DOM helpers (private)
#   gen/                Q-generated vectors (committed; optionally renewed)
#   q/tsvec.q           domqlib Q template behind gen/
#   ts_stdlib.[hc]      registry + default search dir (installed)
#
# Third-party code compiled in:
#   third_party/tomlc99/toml.c      TOML parser behind std.toml
#   third_party/isocline/src/isocline.c  line editing behind std.readline
#   third_party/stb/stb_ds.h        (header-only, used by nothing at
#                                   present — reserved for future maps;
#                                   containers come from Q-generated gen/)

option(DOMWEAVE_BUILD_TERMSCRIPT_STDLIB "Build the Termscript stdlib" ON)

if(NOT DOMWEAVE_BUILD_TERMSCRIPT_STDLIB)
  return ()
endif()

set(TS_STDLIB_MODULES
  array autocomp buffer codec color csv draw edit_history env exec expect fsm glob http io
  ipc json key list log map menu notify panel pipe pty prompt readline regex rules
  sched screen signal socket stream style syntax table termcap terminfo test text_search theme
  toml verbatim vterm widget word_motion yaml
)

set(TS_STDLIB_SOURCES
  common/sbuf.c
  common/values.c
  common/list_store.c
  common/dom.c
  ts_stdlib.c
)
foreach (m IN LISTS TS_STDLIB_MODULES)
  list (APPEND TS_STDLIB_SOURCES ${m}/${m}.c)
endforeach ()

# Vendored third-party sources (warning-cleanliness of our own files
# is enforced below; vendored files compile with -w).
set(TS_STDLIB_TOML_C ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/tomlc99/toml.c)
set(TS_STDLIB_ISOCLINE_C ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/isocline/src/isocline.c)
set(TS_STDLIB_TOML_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/tomlc99)
set(TS_STDLIB_ISOCLINE_INC ${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/isocline/include)

add_library(termscript_stdlib STATIC
  ${TS_STDLIB_SOURCES}
  ${TS_STDLIB_TOML_C}
  ${TS_STDLIB_ISOCLINE_C}
)
add_library(termscript::stdlib ALIAS termscript_stdlib)

target_include_directories(termscript_stdlib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../..>
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(termscript_stdlib PRIVATE
  ${TS_STDLIB_TOML_INC}
  ${TS_STDLIB_ISOCLINE_INC}
)
target_link_libraries(termscript_stdlib PUBLIC termscript_standalone)
target_link_libraries(termscript_stdlib PRIVATE Threads::Threads)

# Installed companions live under <datadir>/termscript/stdlib; bake
# that path in so G:import works out of the box after install.
# _GNU_SOURCE like libdomterm's dt_internal.h (pty, signals, getline).
target_compile_definitions(termscript_stdlib PRIVATE
  TERMSCRIPT_STDLIB_DIR="${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/termscript/stdlib"
  TERMSCRIPT_STDLIB_SRC="${CMAKE_CURRENT_SOURCE_DIR}"
  TS_STD_HAVE_ISOCLINE=1
  _GNU_SOURCE
)

set_source_files_properties(${TS_STDLIB_TOML_C} ${TS_STDLIB_ISOCLINE_C}
  PROPERTIES COMPILE_OPTIONS "-w")

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(termscript_stdlib PRIVATE -Wall -Wextra)
endif()

# ── Q regeneration (optional) ─────────────────────────────────────
# Renews gen/ts_valvec.h + gen/ts_strvec.h from q/tsvec.q through
# domqlib's Q (sourceless .pyc driven by q/run_qc.py).  Runs only when
# python3 + tkinter + the Q bytecode cache are all present; otherwise
# the committed gen/ outputs are used as-is.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(TS_Q_CACHE ${CMAKE_CURRENT_SOURCE_DIR}/../../../../domlibs/domqlib/q/__pycache__)
if(Python3_FOUND AND EXISTS "${TS_Q_CACHE}")
  execute_process(
    COMMAND ${Python3_EXECUTABLE} -c "import tkinter"
    RESULT_VARIABLE TS_TK_OK
    OUTPUT_QUIET ERROR_QUIET
  )
  if(TS_TK_OK EQUAL 0)
    add_custom_command(
      OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_valvec.h
             ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_strvec.h
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/q/run_qc.py
              --qcache ${TS_Q_CACHE}
              --q ${CMAKE_CURRENT_SOURCE_DIR}/q/tsvec.q
              --out ${CMAKE_CURRENT_SOURCE_DIR}/gen
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/q/tsvec.q
              ${CMAKE_CURRENT_SOURCE_DIR}/q/run_qc.py
      COMMENT "Regenerating Termscript stdlib vectors (domqlib Q)"
      VERBATIM
    )
    add_custom_target(termscript_stdlib_qgen DEPENDS
      ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_valvec.h
      ${CMAKE_CURRENT_SOURCE_DIR}/gen/ts_strvec.h
    )
    add_dependencies(termscript_stdlib termscript_stdlib_qgen)
  endif()
endif()

# ── install ───────────────────────────────────────────────────────
install(TARGETS termscript_stdlib
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES ts_stdlib.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib
)
install(FILES common/ts_std_common.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/common
)
install(FILES gen/ts_valvec.h gen/ts_strvec.h
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/gen
)
install(FILES q/tsvec.q q/run_qc.py
  DESTINATION ${CMAKE_INSTALL_DATADIR}/domterm/stdlib-q
)
foreach (m IN LISTS TS_STDLIB_MODULES)
  install(FILES ${m}/${m}.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/${m}
  )
  install(FILES ${m}/${m}.tsc
    DESTINATION ${CMAKE_INSTALL_DATADIR}/termscript/stdlib/${m}
  )
endforeach ()

install(FILES buffer/allocator.h buffer/dommemtk_adapter.hpp
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/termscript/stdlib/buffer)
