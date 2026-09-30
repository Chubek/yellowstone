// Header-only umbrella. All builtin formats are available without macros.
#pragma once
#include "qArchive.hpp"
#include "qBFDCore.hpp"
#include "qCOFF.hpp"
#include "qELF.hpp"
#include "qMachO.hpp"
#include "qPE.hpp"

namespace qbfd {

inline void registerBuiltinTargets() {
  static const bool initialized = [] {
    auto& r = Registry::instance();
    elf::registerTargets(r);
    pe::registerTargets(r);
    coff::registerTargets(r);
    macho::registerTargets(r);
    return true;
  }();
  (void)initialized;
}

// Format detection without a full parse
inline Format detectFormat(std::span<const uint8_t> d) {
  if (d.size() >= 4 && d[0] == 0x7f && d[1] == 'E' && d[2] == 'L' &&
      d[3] == 'F')
    return Format::ELF;
  if (d.size() >= 4) {
    uint32_t m = d[0] | d[1] << 8 | d[2] << 16 | uint32_t(d[3]) << 24;
    if (m == 0xFEEDFACE || m == 0xFEEDFACF || m == 0xCEFAEDFE ||
        m == 0xCFFAEDFE || m == 0xCAFEBABE || m == 0xBEBAFECA ||
        m == 0xCAFEBABF || m == 0xBFBAFECA)
      return Format::MachO;
  }
  if (d.size() >= 2 && d[0] == 'M' && d[1] == 'Z') return Format::PE;
  if (d.size() >= 2) {
    uint16_t m = d[0] | d[1] << 8;
    if (coff::knownMachine(m) ||
        coff::COFFFile::isBigObjHeader(coff::Reader{d}))
      return Format::COFF;
  }
  return Format::Unknown;
}
inline Expected<Opened> openImpl(std::shared_ptr<Buffer> buf,
                                 const std::filesystem::path& label,
                                 const Target* forced) {
  registerBuiltinTargets();
  auto data = buf->span();

  const Target* t = forced;
  if (!t) {
    auto matches = Registry::instance().match(data);
    if (matches.empty())
      return Error{Error::Code::UnknownFormat,
                   "no target recognises " + label.string()};
    if (matches.size() > 1)
      return Error{Error::Code::Ambiguous,
                   "multiple targets match " + label.string()};
    t = matches[0];
  }

  auto obj = t->open(data, label);
  if (!obj) return obj.error();
  if (!*obj)
    return Error{Error::Code::Malformed, "backend returned a null object"};
  (*obj)->retainBuffer(buf);
  return Opened{std::move(buf), std::move(*obj)};
}

inline Expected<Opened> open(const std::filesystem::path& path) {
  auto buf = Buffer::fromFile(path);
  if (!buf) return buf.error();
  return openImpl(*buf, path, nullptr);
}

inline Expected<Opened> open(std::vector<uint8_t> bytes,
                             const std::filesystem::path& label) {
  return openImpl(Buffer::fromBytes(std::move(bytes)), label, nullptr);
}

inline Expected<Opened> openAs(const std::filesystem::path& path,
                               std::string_view targetName) {
  registerBuiltinTargets();
  const Target* t = Registry::instance().find(targetName);
  if (!t)
    return Error{Error::Code::Unsupported,
                 "unknown target " + std::string(targetName)};
  auto buf = Buffer::fromFile(path);
  if (!buf) return buf.error();
  return openImpl(*buf, path, t);
}

}  // namespace qbfd

namespace qbfd {
inline Expected<Opened> openAs(std::vector<uint8_t> bytes,
                               std::string_view targetName,
                               const std::filesystem::path& label = {}) {
  registerBuiltinTargets();
  auto target = Registry::instance().find(targetName);
  if (!target)
    return Error{Error::Code::Unsupported,
                 "unknown target " + std::string(targetName)};
  return openImpl(Buffer::fromBytes(std::move(bytes)), label, target);
}
namespace query {
inline auto read() {
  return ::dsl::pipe([](auto source) { return qbfd::open(std::move(source)); });
}
inline auto as(std::string target) {
  return ::dsl::pipe([target = std::move(target)](auto source) {
    return qbfd::openAs(std::move(source), target);
  });
}
inline auto requireFormat(Format format) {
  return ::dsl::pipe([format](Expected<Opened> input) -> Expected<Opened> {
    if (!input) return input.error();
    if (input->object->format() != format)
      return Error{Error::Code::UnknownFormat, "unexpected object format"};
    return input;
  });
}
// Lift a query into the error-aware open pipeline; never dereference a failed
// open.
template <class Stage>
auto inspect(Stage stage) {
  return ::dsl::pipe([stage = std::move(stage)](Expected<Opened> input) {
    using Value = std::decay_t<decltype(*input->object | stage)>;
    if (!input) return Expected<Value>(input.error());
    return Expected<Value>(*input->object | stage);
  });
}
}  // namespace query
}  // namespace qbfd
