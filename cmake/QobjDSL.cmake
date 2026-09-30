# Locates the qDSL header-only toolkit and exposes it as `qobjfile::qdsl`.
#
# qBFD's query DSL is built on qdsl/qDSL.hpp's CRTP `DSL`, `Pipeline`,
# `Operators`, `PipeStage` and `Predicate` facilities. qobjfile used to carry a
# 14-line hand-rolled stand-in for that header; it now consumes the real one so
# the two can never drift.
#
# Resolution order:
#   1. `-DQOBJFILE_QDSL_DIR=<dir>` when the caller knows where qDSL lives.
#   2. The sibling `qdsl/` directory in the surrounding repository.
#   3. An installed `qDSL` CMake package.
#   4. A `qDSL.hpp` already on the include path.
#
# Defines the imported interface target `qobjfile::qdsl` and sets
# `qobjfile_qdsl_SOURCE` in the caller's scope for diagnostics.

include_guard(GLOBAL)

if(TARGET qobjfile::qdsl)
  return()
endif()

set(_qobjfile_qdsl_hints "")
if(DEFINED QOBJFILE_QDSL_DIR)
  list(APPEND _qobjfile_qdsl_hints "${QOBJFILE_QDSL_DIR}")
endif()
# .../qobjfile/qobjbfd -> .../qobjfile -> .../<repo>
get_filename_component(_qobjfile_qdsl_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
list(APPEND _qobjfile_qdsl_hints "${_qobjfile_qdsl_repo}/qdsl")

find_path(qobjfile_qdsl_include_dir
  NAMES qDSL.hpp
  HINTS ${_qobjfile_qdsl_hints}
  PATH_SUFFIXES ""
  DOC "Directory containing the qDSL.hpp header-only toolkit")

find_package(qDSL CONFIG QUIET)

add_library(qobjfile_qdsl INTERFACE)
if(qobjfile_qdsl_include_dir)
  target_include_directories(qobjfile_qdsl SYSTEM INTERFACE "${qobjfile_qdsl_include_dir}")
  set(qobjfile_qdsl_SOURCE "source tree (${qobjfile_qdsl_include_dir})")
elseif(TARGET qDSL::qDSL)
  target_link_libraries(qobjfile_qdsl INTERFACE qDSL::qDSL)
  set(qobjfile_qdsl_SOURCE "installed package")
elseif(TARGET qDSL)
  target_link_libraries(qobjfile_qdsl INTERFACE qDSL)
  set(qobjfile_qdsl_SOURCE "installed package")
else()
  # A bare include path still satisfies a header-only dependency.
  message(WARNING
    "qobjfile: could not locate qDSL.hpp; set -DQOBJFILE_QDSL_DIR=<dir> "
    "or install the qDSL package. Compilation of qBFD will likely fail.")
  set(qobjfile_qdsl_SOURCE "not found")
endif()
add_library(qobjfile::qdsl ALIAS qobjfile_qdsl)
