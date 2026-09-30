// qArchive.hpp - `ar` container reader and writer for qBFD.
//
// The three `ar` conventions described on qbfd::Archive are parsed here. The
// writer emits SVR4/GNU, BSD, thin and plain, and can regenerate a symbol
// index from the members' own symbol tables.
//
// Nothing is reinterpreted from a host struct: every field is read byte by
// byte through qbfd::detail::Reader, and every derived value is range-checked
// against the container length.
#pragma once

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <unordered_set>

#include "qBFDCore.hpp"

namespace qbfd::ar {

namespace k {
// Container magics. Both are exactly 8 bytes, the first including its '\n'.
inline constexpr std::string_view Magic = "!<arch>\n";
inline constexpr std::string_view ThinMagic = "!<thin>\n";

// The per-member header is a fixed 60 ASCII bytes ending in "`\n".
inline constexpr size_t HeaderSize = 60;
inline constexpr size_t NameField = 16;
inline constexpr size_t MTimeField = 12;
inline constexpr size_t UidField = 6;
inline constexpr size_t GidField = 6;
inline constexpr size_t ModeField = 8;
inline constexpr size_t SizeField = 10;

// Field offsets within the 60-byte header.
inline constexpr size_t OffName = 0;
inline constexpr size_t OffMTime = 16;
inline constexpr size_t OffUid = 28;
inline constexpr size_t OffGid = 34;
inline constexpr size_t OffMode = 40;
inline constexpr size_t OffSize = 48;
inline constexpr size_t OffTerminator = 58;

// Special SVR4/GNU member names.
inline constexpr std::string_view ArmapName = "/";
inline constexpr std::string_view Armap64Name = "/SYM64/";
inline constexpr std::string_view LongNamesName = "//";
// Special BSD member names.
inline constexpr std::string_view BsdSymdef = "__.SYMDEF";
inline constexpr std::string_view BsdSymdef64 = "__.SYMDEF_64";
inline constexpr std::string_view SortedSuffix = " SORTED";
// The BSD extended-name marker, e.g. "#1/17".
inline constexpr std::string_view BsdNameMarker = "#1/";
}  // namespace k

// ---------------------------------------------------------------------------
// Header field decoding
// ---------------------------------------------------------------------------

// Trims the trailing padding GNU and BSD both use inside the numeric fields.
inline std::string_view trim(std::string_view v) {
  while (!v.empty() && (v.back() == ' ' || v.back() == '\0')) v.remove_suffix(1);
  return v;
}

// Parses a right-padded ASCII number. `base` is 10 for every field except the
// mode, which is octal. Non-numeric noise (a BSD "sec.usec" timestamp, a
// stray NUL) is tolerated by reading the leading run of valid digits, which is
// what every `ar` implementation in the wild does.
inline uint64_t parseNumber(std::string_view v, int base = 10) {
  v = trim(v);
  size_t i = 0;
  while (i < v.size() && v[i] >= '0' && v[i] <= '9') ++i;
  if (!i) return 0;
  uint64_t value = 0;
  auto [ptr, ec] = std::from_chars(v.data(), v.data() + i, value, base);
  (void)ptr;
  if (ec != std::errc{}) return 0;
  return value;
}

// A view over the container with a small amount of `ar`-specific decoding.
// Header fields are ASCII and byte order only matters for the armap tables.
class ArReader {
 public:
  explicit ArReader(std::span<const uint8_t> data) : r_{data} {}

  std::span<const uint8_t> bytes() const { return r_.d; }
  size_t size() const { return r_.d.size(); }
  bool has(uint64_t off, uint64_t len) const { return r_.has(off, len); }
  void require(uint64_t off, uint64_t len) const { r_.require(off, len); }
  void setOrder(Endian order) { r_.order = order; }
  std::string_view text(uint64_t off, uint64_t len) const {
    r_.require(off, len);
    return {reinterpret_cast<const char*>(r_.d.data() + off), size_t(len)};
  }
  // A header field, with the right padding already removed.
  std::string_view field(uint64_t header, size_t offset, size_t width) const {
    return trim(text(header + offset, width));
  }
  // Reads `width` bytes at `off`; the caller has already set the byte order.
  uint64_t integer(uint64_t off, unsigned width) const {
    return r_.integer(off, width);
  }
  std::string cstr(uint64_t off, uint64_t limit) const {
    return r_.cstr(off, limit);
  }
  // A string terminated by `delim` rather than NUL. The BSD `__.SYMDEF` string
  // table is '\n'-separated; the SVR4/GNU one is NUL-separated.
  std::string delimited(uint64_t off, uint64_t limit, char delim) const {
    r_.require(off, limit);
    const auto* p = reinterpret_cast<const char*>(r_.d.data() + off);
    uint64_t n = 0;
    while (n < limit && p[n] && p[n] != delim) ++n;
    return {p, size_t(n)};
  }

 private:
  mutable detail::Reader r_;
};

// ---------------------------------------------------------------------------
// ArFile
// ---------------------------------------------------------------------------
class ArFile final : public Archive {
 public:
  ArFile(std::span<const uint8_t> bytes, std::filesystem::path path,
         std::shared_ptr<const Buffer> owner)
      : data_(bytes), path_(std::move(path)), owner_(std::move(owner)) {}

  const std::vector<Member>& members() const override { return members_; }
  const std::vector<IndexEntry>& symbolIndex() const override {
    return index_;
  }
  ArchiveFlavor flavor() const { return flavor_; }
  bool isThin() const { return flavor_ == ArchiveFlavor::Thin; }

  // A thin container stores no payload, so this is always empty for one.
  std::span<const uint8_t> memberData(const Member& m) const override {
    if (isThin() || m.special || m.size == 0) return {};
    if (m.offset > data_.size() || m.size > data_.size() - m.offset) return {};
    return data_.subspan(m.offset, m.size);
  }

  // Materialises a member, loading a thin member's payload from disk on first
  // use and caching it for the lifetime of this object.
  Expected<std::span<const uint8_t>> readMember(
      const Member& m) const override {
    if (!isThin()) return memberData(m);
    for (size_t i = 0; i < members_.size(); ++i)
      if (members_[i].headerOffset == m.headerOffset) {
        if (thin_[i].has_value()) return std::span<const uint8_t>(*thin_[i]);
        auto file = Buffer::fromFile(resolveThinPath(m));
        if (!file) return file.error();
        thin_[i] = std::vector<uint8_t>((*file)->span().begin(),
                                        (*file)->span().end());
        return std::span<const uint8_t>(*thin_[i]);
      }
    return Error{Error::Code::OutOfRange, "unknown archive member"};
  }

  Expected<std::unique_ptr<ObjectFile>> openMember(
      const Member& m) const override {
    if (m.special)
      return Error{Error::Code::Unsupported,
                   "archive member is metadata, not an object: " + m.name};
    auto payload = readMember(m);
    if (!payload) return payload.error();
    if (payload->empty())
      return Error{Error::Code::UnknownFormat, "empty archive member " + m.name};
    auto opened = open(std::vector<uint8_t>(payload->begin(), payload->end()),
                       m.name);
    if (!opened) return opened.error();
    return Expected<std::unique_ptr<ObjectFile>>(std::move(opened->object));
  }

  // The full 60-byte header of a member, for `qobjar` and round-tripping.
  std::span<const uint8_t> memberHeader(const Member& m) const {
    if (m.headerOffset > data_.size() || k::HeaderSize > data_.size() - m.headerOffset)
      return {};
    return data_.subspan(m.headerOffset, k::HeaderSize);
  }

  // Parses `bytes`. Any structural problem is reported as an Error; no
  // partially-populated archive is ever returned.
  static Expected<std::unique_ptr<ArFile>> parse(
      std::span<const uint8_t> bytes, std::filesystem::path path,
      std::shared_ptr<const Buffer> owner) {
    try {
      auto file = std::make_unique<ArFile>(bytes, std::move(path),
                                           std::move(owner));
      if (Error e = file->scan()) return e;
      return std::unique_ptr<ArFile>(std::move(file));
    } catch (const detail::ParseFailure& e) {
      return e.error;
    }
  }

 private:
  // A header exactly as it appears on disk, before name decoding.
  struct RawHeader {
    uint64_t offset = 0;   // of the header itself
    uint64_t data = 0;     // of the payload
    uint64_t size = 0;     // payload length as declared, name prefix included
    uint64_t mtime = 0;
    uint32_t uid = 0, gid = 0, mode = 0;
    std::string_view name;  // trimmed name field
  };

  std::filesystem::path resolveThinPath(const Member& m) const {
    std::filesystem::path p(m.name);
    return p.is_absolute() ? p : path_.parent_path() / p;
  }

  Error scan() {
    ArReader r(data_);
    if (!r.has(0, 8)) return {Error::Code::Malformed, "archive too short"};
    if (r.text(0, 8) == k::ThinMagic) {
      flavor_ = ArchiveFlavor::Thin;
      thin_.resize(0);
    } else if (r.text(0, 8) == k::Magic) {
      flavor_ = ArchiveFlavor::Unknown;
    } else {
      return {Error::Code::UnknownFormat, "not an ar archive"};
    }

    // Pass 1: walk the raw headers. Names need the leading special members to
    // be known first, so they are resolved in pass 2.
    std::vector<RawHeader> raw;
    uint64_t p = 8;
    while (p + k::HeaderSize <= r.size()) {
      if (r.text(p + k::OffTerminator, 2) != "`\n")
        return {Error::Code::Malformed,
                "bad archive member header at offset " + std::to_string(p)};
      RawHeader h;
      h.offset = p;
      h.name = r.field(p, k::OffName, k::NameField);
      h.mtime = parseNumber(r.field(p, k::OffMTime, k::MTimeField));
      h.uid = uint32_t(parseNumber(r.field(p, k::OffUid, k::UidField)));
      h.gid = uint32_t(parseNumber(r.field(p, k::OffGid, k::GidField)));
      h.mode = uint32_t(parseNumber(r.field(p, k::OffMode, k::ModeField), 8));
      h.size = parseNumber(r.field(p, k::OffSize, k::SizeField));
      h.data = p + k::HeaderSize;
      // A thin archive keeps its own index and long-name table in the file;
      // only the object payloads are external, and those still record their
      // true size in the header.
      const bool metadata = h.name == k::ArmapName ||
                            h.name == k::Armap64Name ||
                            h.name == k::LongNamesName;
      if (isThin() && !metadata) {
        p = h.data;
        raw.push_back(h);
        continue;
      }
      if (h.size > r.size() - h.data)
        return {Error::Code::Malformed,
                "archive member overruns container at offset " +
                    std::to_string(p)};
      raw.push_back(h);
      // Members start on an even offset; an odd payload gets one pad byte.
      uint64_t next = h.data + h.size;
      if (h.size & 1) {
        if (next >= r.size() || data_[next] != '\n')
          return {Error::Code::Malformed, "missing archive member pad byte"};
        ++next;
      }
      p = next;
    }
    if (p != r.size() && p + k::HeaderSize > r.size()) {
      // Trailing bytes that cannot hold a header: only an even run of padding
      // is tolerable, and the loop above already consumed any pad byte.
      return {Error::Code::Malformed, "trailing garbage after last member"};
    }
    if (flavor_ == ArchiveFlavor::Unknown) flavor_ = detectFlavor(raw);
    if (flavor_ == ArchiveFlavor::Unknown)
      return {Error::Code::Unsupported, "unrecognised ar flavour"};

    // Pass 2: resolve the long-name table and the symbol index, which later
    // member names may depend on.
    if (flavor_ != ArchiveFlavor::Bsd) {
      for (const auto& h : raw) {
        if (h.name == k::LongNamesName) {
          r.require(h.data, h.size);
          longNames_ = data_.subspan(h.data, h.size);
          break;
        }
      }
    }
    members_.reserve(raw.size());
    for (const auto& h : raw) {
      Member m;
      m.headerOffset = h.offset;
      m.mtime = h.mtime;
      m.uid = h.uid;
      m.gid = h.gid;
      m.mode = h.mode;
      m.offset = h.data;
      m.size = h.size;
      if (Error e = resolveName(r, h, m)) return e;
      members_.push_back(std::move(m));
    }
    if (flavor_ == ArchiveFlavor::Thin) thin_.resize(members_.size());
    if (Error e = parseIndex(r, raw)) return e;
    return {};
  }

  // Decides SVR4 vs BSD from the leading member headers, and recognises the
  // Windows import-library shape (two members both named "/").
  static ArchiveFlavor detectFlavor(const std::vector<RawHeader>& raw) {
    auto isArmap = [](const RawHeader& h) {
      return h.name == k::ArmapName || h.name == k::Armap64Name;
    };
    if (raw.size() >= 2 && raw[0].name == k::ArmapName &&
        raw[1].name == k::ArmapName)
      return ArchiveFlavor::Windows;
    if (!raw.empty() && isArmap(raw[0])) return ArchiveFlavor::Svr4;
    if (raw.size() >= 2 && isArmap(raw[1])) return ArchiveFlavor::Svr4;
    if (!raw.empty() && raw[0].name == k::LongNamesName)
      return ArchiveFlavor::Svr4;
    if (!raw.empty() && raw[0].name.starts_with(k::BsdSymdef))
      return ArchiveFlavor::Bsd;
    // A first member already using "#1/<len>" settles it.
    for (const auto& h : raw)
      if (h.name.starts_with(k::BsdNameMarker)) return ArchiveFlavor::Bsd;
    // SVR4 terminates a short name with '/', BSD pads it with spaces. Fall
    // back on that distinction for archives with no special members at all.
    for (const auto& h : raw) {
      if (h.name.empty()) continue;
      if (h.name.back() == '/') return ArchiveFlavor::Svr4;
      break;
    }
    return raw.empty() ? ArchiveFlavor::Svr4 : ArchiveFlavor::Bsd;
  }

  Error resolveName(ArReader& r, const RawHeader& h, Member& m) {
    if (flavor_ == ArchiveFlavor::Bsd) {
      if (h.name.starts_with(k::BsdSymdef)) {
        m.special = true;
        m.name = std::string(h.name);
        return {};
      }
      if (h.name.starts_with(k::BsdNameMarker)) {
        uint64_t length = parseNumber(h.name.substr(k::BsdNameMarker.size()));
        if (!length || length > h.size)
          return {Error::Code::Malformed, "bad BSD extended name length"};
        r.require(h.data, length);
        m.name = std::string(r.text(h.data, size_t(length)));
        // The name occupies the head of the payload, so shift the payload.
        m.offset = h.data + length;
        m.size = h.size - length;
        return {};
      }
      m.name = std::string(h.name);
      return {};
    }

    // SVR4 / Windows / thin.
    if (h.name == k::ArmapName || h.name == k::Armap64Name ||
        h.name == k::LongNamesName) {
      m.special = true;
      m.name = std::string(h.name);
      return {};
    }
    if (!h.name.empty() && h.name.front() == '/') {
      uint64_t offset = parseNumber(h.name.substr(1));
      // Offset 0 is a valid table position, so test the table bounds rather
      // than the offset's truthiness.
      if (longNames_.empty() || offset >= longNames_.size())
        return {Error::Code::Malformed, "long name outside long-name table"};
      // The table is a "/\n"-separated list; read to the next separator.
      uint64_t end = offset;
      while (end < longNames_.size() && longNames_[end] != '/') ++end;
      // Drop the trailing '\n' that terminates the entry.
      while (end > offset && longNames_[end - 1] == '\n') --end;
      m.name.assign(reinterpret_cast<const char*>(longNames_.data() + offset),
                    size_t(end - offset));
      return {};
    }
    m.name = std::string(h.name);
    if (!m.name.empty() && m.name.back() == '/') m.name.pop_back();
    return {};
  }

  // BSD's `__.SYMDEF` symbol table is recognised but deliberately not decoded.
  // The variants disagree about whether ran_l[0..ran_cnt) holds member header
  // offsets or string-table offsets, and a wrong reading silently produces an
  // index that points at the wrong member -- worse than no index at all, since
  // a resolver would then pick the wrong definition. Members are still parsed
  // and readable; only symbolIndex() stays empty. Linkers fall back to
  // scanning every member when an archive carries no index.
  Error parseIndex(ArReader& r, const std::vector<RawHeader>& raw) {
    if (flavor_ == ArchiveFlavor::Bsd) return {};
    if (flavor_ == ArchiveFlavor::Windows) return {};
    for (const auto& h : raw) {
      if (h.name == k::Armap64Name) return parseArmap(r, h, 8);
      if (h.name == k::ArmapName) return parseArmap(r, h, 4);
    }
    return {};
  }

  // SVR4/GNU armap: a big-endian count, that many big-endian header offsets,
  // then that many NUL-terminated names, all in one blob.
  Error parseArmap(ArReader& r, const RawHeader& h, unsigned width) {
    r.setOrder(Endian::Big);
    r.require(h.data, h.size);
    if (h.size < width) return {Error::Code::Malformed, "short archive armap"};
    uint64_t count = r.integer(h.data, width);
    uint64_t need = width + count * width;
    if (count > h.size || need > h.size)
      return {Error::Code::Malformed, "archive armap overruns member"};
    index_.reserve(count);
    uint64_t names = h.data + need;
    for (uint64_t i = 0; i < count; ++i) {
      IndexEntry e;
      e.headerOffset = r.integer(h.data + width + i * width, width);
      e.symbol = r.cstr(names, h.data + h.size - names);
      names += e.symbol.size() + 1;
      if (names > h.data + h.size)
        return {Error::Code::Malformed, "archive armap name overruns member"};
      index_.push_back(std::move(e));
    }
    return {};
  }

  std::span<const uint8_t> data_;
  std::filesystem::path path_;
  std::shared_ptr<const Buffer> owner_;
  ArchiveFlavor flavor_ = ArchiveFlavor::Unknown;
  std::span<const uint8_t> longNames_;
  std::vector<Member> members_;
  std::vector<IndexEntry> index_;
  // Lazily materialised payloads for thin archives, parallel to members_.
  mutable std::vector<std::optional<std::vector<uint8_t>>> thin_;
};
// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------
namespace detail_ar {

// Right-pads a header field. Every `ar` field is a fixed width; a value that
// does not fit is reported rather than silently truncated into a corrupt
// archive.
inline bool padField(std::string& out, const std::string& value, size_t width) {
  if (value.size() > width) return false;
  out += value;
  out.append(width - value.size(), ' ');
  return true;
}

// Appends one 60-byte member header. The header is always even, so it never
// needs a pad byte; only the payload that follows may.
inline bool appendHeader(std::string& out, std::string_view name,
                         uint64_t mtime, uint32_t uid, uint32_t gid,
                         uint32_t mode, uint64_t size) {
  char modeText[16];
  std::snprintf(modeText, sizeof modeText, "%o", mode);
  return padField(out, std::string(name), k::NameField) &&
         padField(out, std::to_string(mtime), k::MTimeField) &&
         padField(out, std::to_string(uid), k::UidField) &&
         padField(out, std::to_string(gid), k::GidField) &&
         padField(out, modeText, k::ModeField) &&
         padField(out, std::to_string(size), k::SizeField) &&
         (out += "`\n", true);
}

inline void padToEven(std::string& out) {
  if (out.size() & 1) out += '\n';
}

// Big-endian patch into an already-sized blob.
inline void patchBE(std::string& blob, size_t at, uint64_t value,
                    unsigned width) {
  for (unsigned i = 0; i < width; ++i)
    blob[at + i] = char(uint8_t(value >> (8 * (width - i - 1))));
}

}  // namespace detail_ar

// SVR4 stores a member's name as "name/" when it fits, or "/<offset>" into the
// long-name table when it does not. `offset` is the name's own position in that
// table -- a "/\n"-separated run of the names that did not fit -- so it has to
// be the running position, not the table's final length.
inline std::string svr4NameField(const std::string& name, size_t offset) {
  if (name.size() <= 15) return name + "/";
  return "/" + std::to_string(offset);
}

// One fully-emitted member: header, optional BSD name prefix, payload, and the
// pad byte that realigns the next header. A member's bytes never depend on
// where it sits in the archive, so these can all be built up front.
inline std::string emitMember(const ArchiveEntry& e,
                              const std::string& nameField, uint64_t mtime,
                              uint32_t uid, uint32_t gid, uint32_t mode,
                              bool bsd, bool thin) {
  const bool bsdExtended = bsd && nameField.starts_with("#1/");
  uint64_t size = e.data.size() + (bsdExtended ? e.name.size() : 0);
  std::string blob;
  detail_ar::appendHeader(blob, nameField, mtime, uid, gid, mode,
                          thin ? 0 : size);
  if (bsdExtended) blob += e.name;
  if (!thin)
    blob.append(reinterpret_cast<const char*>(e.data.data()), e.data.size());
  if ((thin ? 0 : size) & 1) blob += '\n';
  return blob;
}

}  // namespace qbfd::ar

// ---------------------------------------------------------------------------
// Free functions declared in qBFDCore.hpp
// ---------------------------------------------------------------------------
namespace qbfd {

inline std::string_view toString(ArchiveFlavor flavor) {
  switch (flavor) {
    case ArchiveFlavor::Svr4:
      return "svr4";
    case ArchiveFlavor::Bsd:
      return "bsd";
    case ArchiveFlavor::Thin:
      return "thin";
    case ArchiveFlavor::Windows:
      return "windows";
    default:
      return "unknown";
  }
}

inline bool isArchive(std::span<const uint8_t> data) {
  if (data.size() < 8) return false;
  return std::memcmp(data.data(), "!<arch>\n", 8) == 0 ||
         std::memcmp(data.data(), "!<thin>\n", 8) == 0;
}

inline Expected<OpenedArchive> openArchive(const std::filesystem::path& path) {
  auto buffer = Buffer::fromFile(path);
  if (!buffer) return buffer.error();
  auto parsed = ar::ArFile::parse((*buffer)->span(), path, *buffer);
  if (!parsed) return parsed.error();
  return OpenedArchive{std::move(*buffer),
                       std::unique_ptr<Archive>(std::move(*parsed))};
}

inline Expected<OpenedArchive> openArchive(std::vector<uint8_t> bytes,
                                           const std::filesystem::path& label) {
  auto buffer = Buffer::fromBytes(std::move(bytes));
  auto parsed = ar::ArFile::parse(buffer->span(), label, buffer);
  if (!parsed) return parsed.error();
  return OpenedArchive{std::move(buffer),
                       std::unique_ptr<Archive>(std::move(*parsed))};
}

inline Expected<ArchiveEntry> archiveEntryFromFile(
    const std::filesystem::path& path) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec))
    return Error{Error::Code::FileNotFound,
                 "not a regular file: " + path.string()};
  auto buffer = Buffer::fromFile(path);
  if (!buffer) return buffer.error();
  ArchiveEntry e;
  e.name = path.filename().string();
  e.data.assign((*buffer)->span().begin(), (*buffer)->span().end());
  auto status = std::filesystem::status(path, ec);
  if (!ec) {
    e.mode = 0100000u | (uint32_t(status.permissions()) & 07777u);
    auto written = std::filesystem::last_write_time(path, ec);
    if (!ec)
      e.mtime = std::chrono::duration_cast<std::chrono::seconds>(
                    written.time_since_epoch())
                    .count();
  }
  return e;
}

// Global symbols an object defines, as a linker would index them in an armap:
// externally visible, actually defined, and not debug or synthetic. A payload
// that is not an object contributes nothing rather than failing the build.
inline Expected<std::vector<std::string>> archiveExportedSymbols(
    const ArchiveEntry& entry) {
  std::vector<std::string> out;
  auto opened = open(entry.data, entry.name);
  if (!opened) return out;
  for (const auto& s : opened->object->symbols()) {
    if (s.name.empty() || s.flags & sym::Undefined) continue;
    if (s.flags & (sym::Debug | sym::Synthetic)) continue;
    if (s.kind == SymbolKind::File || s.kind == SymbolKind::Section) continue;
    if (s.binding != SymbolBinding::Global &&
        s.binding != SymbolBinding::Weak &&
        s.binding != SymbolBinding::Unique)
      continue;
    out.push_back(s.name);
  }
  return out;
}

namespace ar {

// The (symbol, member) pairs a linker would search, first definition winning
// and members in archive order.
inline std::vector<std::pair<std::string, size_t>> planSymbolIndex(
    const std::vector<ArchiveEntry>& entries) {
  std::vector<std::pair<std::string, size_t>> plan;
  std::unordered_set<std::string> seen;
  for (size_t i = 0; i < entries.size(); ++i) {
    auto symbols = archiveExportedSymbols(entries[i]);
    if (!symbols) continue;
    for (auto& name : *symbols)
      if (seen.insert(name).second) plan.emplace_back(std::move(name), i);
  }
  return plan;
}

}  // namespace ar

inline Expected<std::vector<uint8_t>> buildArchive(
    const std::vector<ArchiveEntry>& entries, const ArchiveOptions& options) {
  using namespace ar;
  const bool bsd = options.flavor == ArchiveFlavor::Bsd;
  const bool thin = options.flavor == ArchiveFlavor::Thin;
  if (options.flavor == ArchiveFlavor::Windows ||
      options.flavor == ArchiveFlavor::Unknown)
    return Error{Error::Code::Unsupported,
                 std::string("cannot write a ") +
                     std::string(toString(options.flavor)) + " archive"};

  // Deterministic output zeroes every variable field, matching `ar D`.
  auto mtimeOf = [&](const ArchiveEntry& e) {
    return options.deterministic ? 0 : e.mtime;
  };
  auto uidOf = [&](const ArchiveEntry& e) {
    return options.deterministic ? 0u : e.uid;
  };
  auto gidOf = [&](const ArchiveEntry& e) {
    return options.deterministic ? 0u : e.gid;
  };
  // GNU ar writes the permission bits alone, without the S_IF* type bits, and
  // matching that exactly is what makes a qobjar archive byte-comparable with
  // one from the system tool. Readers here treat the field as an opaque number,
  // so either convention parses.
  auto modeOf = [&](const ArchiveEntry& e) {
    if (options.deterministic) return 0644u;
    uint32_t bits = e.mode & 07777u;
    return bits ? bits : 0644u;
  };

  // The long-name table must precede any member that references it, and each
  // reference is the name's own offset into it.
  std::string longNames;
  std::vector<std::string> nameFields(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& name = entries[i].name;
    if (!bsd && !thin && name.size() > 15) {
      nameFields[i] = svr4NameField(name, longNames.size());
      longNames += name;
      longNames += "/\n";
      continue;
    }
    if (thin) {
      // A thin member's name is a path and its size field is always zero.
      if (name.size() > k::NameField - 1)
        return Error{Error::Code::Unsupported,
                     "thin archive member name is too long: " + name};
      nameFields[i] = name + "/";
    } else if (bsd) {
      // BSD keeps names of at most 16 bytes that contain no space inline.
      bool fits = name.size() <= 16 && name.find(' ') == std::string::npos;
      nameFields[i] = fits ? name : "#1/" + std::to_string(name.size());
    } else {
      nameFields[i] = name + "/";
    }
  }

  // Member bytes do not depend on their position, so build them all now.
  std::vector<std::string> memberBlobs(entries.size());
  for (size_t i = 0; i < entries.size(); ++i)
    memberBlobs[i] = emitMember(entries[i], nameFields[i], mtimeOf(entries[i]),
                                uidOf(entries[i]), gidOf(entries[i]),
                                modeOf(entries[i]), bsd, thin);

  // The armap's *size* depends only on the symbol names, never on the offsets
  // it records, so laying the members out needs no fixed-point iteration.
  //
  // A BSD `__.SYMDEF` is never generated. The reader does not decode one (see
  // ArFile::parseIndex), and no BSD `ar` is available here to check a written
  // one against, so emitting it would be a guess. BSD archives are still
  // written correctly -- just without an index, which is always safe because a
  // resolver with no index falls back to scanning every member.
  const std::vector<std::pair<std::string, size_t>> plan =
      (options.index && !thin && !bsd) ? ar::planSymbolIndex(entries)
                                       : std::vector<std::pair<std::string, size_t>>{};
  std::string namesBlob;
  for (const auto& entry : plan) {
    namesBlob += entry.first;
    namesBlob += '\0';
  }
  const unsigned width = options.wideIndex ? 8 : 4;
  const size_t indexSize = width + plan.size() * width + namesBlob.size();

  // Offsets: armap, then long names, then members in order.
  uint64_t cursor = 8;
  const bool haveIndex = !plan.empty();
  auto skipMember = [&](uint64_t size) {
    cursor += k::HeaderSize + size;
    if (size & 1) ++cursor;
  };
  if (haveIndex) skipMember(indexSize);
  if (!longNames.empty()) skipMember(longNames.size());
  std::vector<uint64_t> headerOffsets(entries.size());
  for (size_t i = 0; i < entries.size(); ++i) {
    headerOffsets[i] = cursor;
    cursor += memberBlobs[i].size();
  }

  std::string out(thin ? k::ThinMagic : k::Magic);
  if (haveIndex) {
    std::string blob(width, '\0');
    blob.resize(width + plan.size() * width, '\0');
    detail_ar::patchBE(blob, 0, plan.size(), width);
    for (size_t i = 0; i < plan.size(); ++i)
      detail_ar::patchBE(blob, width + i * width, headerOffsets[plan[i].second],
                         width);
    blob += namesBlob;
    std::string_view name = options.wideIndex ? k::Armap64Name : k::ArmapName;
    if (!detail_ar::appendHeader(out, name, 0, 0, 0, 0, blob.size()))
      return Error{Error::Code::Unsupported, "symbol index name too long"};
    out += blob;
    detail_ar::padToEven(out);
  }  if (!longNames.empty()) {
    detail_ar::appendHeader(out, k::LongNamesName, 0, 0, 0, 0, longNames.size());
    out += longNames;
    detail_ar::padToEven(out);
  }
  for (auto& blob : memberBlobs) out += blob;
  return Expected<std::vector<uint8_t>>(
      std::vector<uint8_t>(out.begin(), out.end()));
}

}  // namespace qbfd
