# ld - qobjld linker (Unix-style static/shared/relocatable link + script).
#
# The core is a static library (qobjld) built from the per-area translation
# units; the CLI (qobjld) links it. ld.c exposes the C API and is compiled
# as C++ (LANGUAGE CXX) so it can use the header-only readers in ../qbfd/.
# ld.hpp is the C++ wrapper over that C API.

add_library(qld STATIC
  archive.cpp
  binarizer.cpp
  discover_symbols.cpp
  driver.cpp
  ld.c
  ld.cpp
  ldscript.cpp
  ldscript_parser.cpp
  loader.cpp
  position_dependenc.cpp
  relocate.cpp
  shared.cpp
  static.cpp
  symbol.cpp
)
add_library(qobjld::qobjld ALIAS qld)

set_source_files_properties(ld.c PROPERTIES LANGUAGE CXX)

target_compile_features(qld PUBLIC cxx_std_20)
target_include_directories(qld PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../qbfd
)
target_link_libraries(qld PUBLIC qbfd::qbfd)

target_compile_options(qld PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
if(QOBJFILE_WERROR)
  target_compile_options(qld PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

add_executable(qobjld main.cpp)
target_link_libraries(qobjld PRIVATE qobjld::qobjld qobjcommon)
target_include_directories(qobjld PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(qobjld PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
if(QOBJFILE_WERROR)
  target_compile_options(qobjld PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
endif()

if(BUILD_TESTING)
  add_test(NAME qobjld_help COMMAND qobjld --help)
endif()
