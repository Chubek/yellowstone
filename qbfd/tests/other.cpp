// Include each backend before the umbrella to catch include-order/ODR
// regressions.
// clang-format off
#include "qPE.hpp"
#include "qMachO.hpp"
#include "qCOFF.hpp"
#include "qELF.hpp"
#include "qBFD.hpp"
// clang-format on
size_t otherTranslationUnit() {
  qbfd::registerBuiltinTargets();
  return qbfd::Registry::instance().targets().size();
}
