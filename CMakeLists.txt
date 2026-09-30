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
target_compile_features(qobjcommon INTERFACE cxx_std_20)
target_link_libraries(qobjcommon INTERFACE qbfd::qbfd)

function(qobjfile_tool name)
  add_executable(${name} ${name}/main.cpp)
  target_link_libraries(${name} PRIVATE qobjcommon)
  target_compile_options(${name} PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic>)
  if(QOBJFILE_WERROR)
    target_compile_options(${name} PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Werror>)
  endif()
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endfunction()

add_subdirectory(qobjbfd)

foreach(tool qobjdump qobjsize qobjcp qobjstr qobjstrip qobjnm qobjar)
  qobjfile_tool(${tool})
endforeach()

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
  add_test(NAME qobjfile_cli
           COMMAND ${CMAKE_COMMAND}
                   -DTOOLS_DIR=$<TARGET_FILE_DIR:qobjdump>
                   -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/run-cli-tests.cmake)
endif()

install(TARGETS qobjdump qobjsize qobjcp qobjstr qobjstrip qobjnm qobjar
        RUNTIME DESTINATION bin)
