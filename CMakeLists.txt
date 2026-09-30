cmake_minimum_required(VERSION 3.20)
project(qobjfile LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(QobjDSL)

option(QOBJFILE_WERROR "Treat warnings as errors" OFF)

# The inspection tools share argument parsing, formatting and archive-aware
# input, so that lives in one interface target rather than a header each tool
# includes by relative path.
add_library(qobjcommon INTERFACE)
add_library(qobjfile::qobjcommon ALIAS qobjcommon)
# Backwards-compatible alias: older tool sources included
# "../qobjcommon/..."; the directory is now common/.
add_library(qobjfile_qobjcommon ALIAS qobjcommon)
target_compile_features(qobjcommon INTERFACE cxx_std_20)
target_link_libraries(qobjcommon INTERFACE qbfd::qbfd)
target_include_directories(qobjcommon INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/common
  ${CMAKE_CURRENT_SOURCE_DIR}/qbfd
)

function(qobjfile_tool tool dir)
  add_executable(${tool} ${dir}/main.cpp)
  target_link_libraries(${tool} PRIVATE qobjcommon qbfd::qbfd)
  target_compile_options(${tool} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
  if(QOBJFILE_WERROR)
    target_compile_options(${tool} PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
  endif()
  target_include_directories(${tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

# The header-only reader library lives in qbfd/ (target qbfd::qbfd).
add_subdirectory(qbfd)

# Directory names differ from tool names (dump -> qobjdump, ...), so the
# mapping is explicit rather than assumed.
qobjfile_tool(qobjdump dump)
qobjfile_tool(qobjsize size)
qobjfile_tool(qobjcp copy)
qobjfile_tool(qobjstr string)
qobjfile_tool(qobjstrip strip)
qobjfile_tool(qobjnm nm)
qobjfile_tool(qobjar ar)

# The Unix-style linker (qobjld library + CLI) in ld/.
add_subdirectory(ld)

# The command-line tools are only meaningful once the library's own tests pass,
# so they are registered as tests too rather than merely built.
include(CTest)
if(BUILD_TESTING)
  add_test(NAME qobjfile_tools_help
           COMMAND qobjdump --help)
  add_test(NAME qobjstr_help COMMAND qobjstr --help)
  add_test(NAME qobjnm_help COMMAND qobjnm --help)
  add_test(NAME qobjsize_help COMMAND qobjsize --help)
  add_test(NAME qobjcp_help COMMAND qobjcp --help)
  add_test(NAME qobjar_help COMMAND qobjar --help)
  add_test(NAME qobjstrip_help COMMAND qobjstrip --help)
  add_test(NAME qobjld_help COMMAND qobjld --help)
  add_test(NAME qobjfile_cli
           COMMAND ${CMAKE_COMMAND}
                   -DTOOLS_DIR=$<TARGET_FILE_DIR:qobjdump>
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-cli-tests.cmake)
  add_test(NAME qobjld_cli
           COMMAND ${CMAKE_COMMAND}
                   -DTOOLS_DIR=$<TARGET_FILE_DIR:qobjld>
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-ld-tests.cmake)
endif()

install(TARGETS qobjdump qobjsize qobjcp qobjstr qobjstrip qobjnm qobjar qobjld
        RUNTIME DESTINATION bin)
install(TARGETS qld
        ARCHIVE DESTINATION lib
        LIBRARY DESTINATION lib)
install(DIRECTORY qbfd/ DESTINATION include/qbfd
        FILES_MATCHING PATTERN "*.hpp")
install(FILES common/qdsl.hpp common/isa.hpp
        DESTINATION include/qobjfile)
install(FILES ld/ld.h ld/ld.hpp
        DESTINATION include/qobjld)
