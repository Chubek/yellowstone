cmake_minimum_required(VERSION 3.20)
project(qBFD LANGUAGES CXX)

# qBFD builds on its own, so the qDSL lookup lives here as well as in the
# parent project: `cmake -S qobjfile/qobjbfd -B build` is a supported workflow.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake")
include(QobjDSL)

add_library(qbfd INTERFACE)
add_library(qbfd::qbfd ALIAS qbfd)
target_compile_features(qbfd INTERFACE cxx_std_20)
target_include_directories(qbfd INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(qbfd INTERFACE qobjfile::qdsl)
message(STATUS "qBFD: qDSL headers from ${qobjfile_qdsl_SOURCE}")

option(QBFD_BUILD_TESTS "Build qBFD regression tests" ON)
option(QBFD_SANITIZE "Enable address and undefined-behavior sanitizers" OFF)
if(QBFD_BUILD_TESTS)
  enable_testing()
  add_executable(qbfd_tests tests/test.cpp tests/other.cpp)
  target_link_libraries(qbfd_tests PRIVATE qbfd)
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(qbfd_tests PRIVATE -Wall -Wextra -Wpedantic)
    if(QBFD_SANITIZE)
      target_compile_options(qbfd_tests PRIVATE
        -fsanitize=address,undefined -fno-omit-frame-pointer)
      target_link_options(qbfd_tests PRIVATE -fsanitize=address,undefined)
    endif()
  endif()
  add_test(NAME qbfd_regressions COMMAND qbfd_tests)

  # Cross-target fixtures: build objects with Clang, cross-check their metadata
  # against LLVM's own reader, then feed them to the same test binary. Skipped,
  # not failed, when Clang or llvm-readobj is unavailable.
  find_package(Python3 COMPONENTS Interpreter QUIET)
  if(Python3_Interpreter_FOUND)
    add_test(NAME qbfd_fixtures
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tests/run-fixtures.py
              $<TARGET_FILE:qbfd_tests>)
    set_tests_properties(qbfd_fixtures PROPERTIES SKIP_RETURN_CODE 77)
  endif()
endif()
