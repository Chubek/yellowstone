// qobjstr - find printable strings in an object file.
#include "../qobjcommon/cli.hpp"
#include "../qobjcommon/options.hpp"

using namespace qbfd;
using qobj::OptionSpec;

namespace {

constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};
constexpr OptionSpec kMinLength{'n', "bytes", true, "length", "minimum string length"};

struct Flags {
  size_t minimum = 4;
  bool allSections = false;   // -a: search the whole file, not just sections
  bool withFileName = false;  // -f: prefix each string with its file
  bool textOnly = false;      // -t: only initialised sections
  bool withOffset = false;    // -o: print the offset
  bool nulSeparated = false;  // -j: NUL-terminate instead of newline
  bool sectionName = false;   // -s: report which section each came from
  std::vector<std::string> only;  // -j is taken; -S restricts to sections
};

// Collects runs of printable bytes at least `minimum` long.
void scan(std::ostream& out, std::span<const uint8_t> bytes, uint64_t base,
          const Flags& flags, const std::string& label,
          const std::string& section) {
  size_t start = 0;
  bool inString = false;
  for (size_t i = 0; i <= bytes.size(); ++i) {
    bool printable = i < bytes.size() && qobj::isPrintable(bytes[i]);
    if (printable) {
      if (!inString) {
        start = i;
        inString = true;
      }
      continue;
    }
    if (!inString) continue;
    inString = false;
    size_t length = i - start;
    if (length < flags.minimum) continue;
    if (flags.withFileName) out << label << ": ";
    if (flags.sectionName) out << (section.empty() ? "<file>" : section) << ": ";
    if (flags.withOffset)
      out << "0x" << qobj::hex(base + start) << ' ';
    out.write(reinterpret_cast<const char*>(bytes.data() + start),
              static_cast<std::streamsize>(length));
    out.put(flags.nulSeparated ? '\0' : '\n');
  }
}

int report(const qobj::Source& source, const Flags& flags) {
  auto& out = std::cout;
  if (flags.allSections) {
    scan(out, source.bytes, 0, flags, source.label, {});
    return 0;
  }
  for (const auto& s : source.file().sections()) {
    if (!flags.only.empty() &&
        std::find(flags.only.begin(), flags.only.end(), s.name) == flags.only.end())
      continue;
    // -t skips sections with no contents to initialise, i.e. BSS.
    if (flags.textOnly && (s.flags & sec::Bss)) continue;
    auto bytes = source.file().sectionContents(s);
    if (!bytes) continue;
    scan(out, *bytes, s.fileOffset, flags, source.label, s.name);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<OptionSpec> specs = {
      kHelp,
      {'a', "all", false, "", "search the whole file, not just sections"},
      {'f', "print-file-name", false, "", "prefix each string with its file"},
      {'t', "text", false, "", "only search sections with initialised contents"},
      {'o', "offset", false, "", "print the file offset of each string"},
      {'j', "nul-separated", false, "", "NUL-separate instead of newline"},
      kMinLength,
      {'S', "section", true, "name", "search only this section (repeatable)"},
  };
  qobj::Options options("qobjstr", "file...", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjstr: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp)) {
    std::cout << options.usage();
    return 0;
  }
  Flags flags;
  flags.allSections = options.has('a');
  flags.withFileName = options.has('f');
  flags.textOnly = options.has('t');
  flags.withOffset = options.has('o');
  flags.nulSeparated = options.has('j');
  flags.only = options.valueList("section");
  bool ok = false;
  uint64_t length = options.number(kMinLength, 4, &ok);
  if (!ok && options.has(kMinLength)) {
    std::cerr << "qobjstr: -n needs a number\n";
    return 2;
  }
  flags.minimum = size_t(length);
  if (options.positional().empty()) {
    std::cerr << "qobjstr: no input files\n\n" << options.usage();
    return 2;
  }
  return qobj::visit(options.positional(),
                     [&](const qobj::Source& s) { return report(s, flags); });
}
