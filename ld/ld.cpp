// ld.cpp - C++ core implementation behind the ld.c / ld.hpp APIs.
//
// Implements the one-shot link used by the C layer: translates the C++
// LinkOptions (mirrored in internal.hpp) through the driver and returns
// byte vectors. The file also hosts the version string in one place.
#include "internal.hpp"

namespace qld::detail {

const char* kVersionString = "1.0";

}  // namespace qld::detail
