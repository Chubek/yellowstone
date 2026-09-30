// qobjstrip - remove symbols and sections from an object file.
//
// The rewriting is done by qBFD's qStrip.hpp, which keeps the file layout
// intact and renumbers every index that could name a symbol or a section.
#include "../common/cli.hpp"
#include "../common/options.hpp"

#include <qStrip.hpp>

using namespace qbfd;
using qobj::OptionSpec;

// -R, -K and -N accumulate across the whole command line, so they are read
// once from the Options object and then applied to every file.
static std::vector<std::string> removeSections_, keepSections_, stripSymbols_;

namespace {

constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};

struct Flags {
  strip::Mode mode = strip::Mode::Debug;
  bool verbose = false, wildcards = false;
};

int stripFile(const std::string& path, const std::string& output,
              const Flags& flags) {
  std::vector<uint8_t> bytes;
  try {
    bytes = qobj::readFile(path);
  } catch (const std::exception& e) {
    return qobj::reportError(path, e);
  }
  strip::Options options;
  options.mode = flags.mode;
  options.removeSections = removeSections_;
  options.keepSections = keepSections_;
  options.stripSymbols = stripSymbols_;
  options.wildcards = flags.wildcards;

  auto stripped = strip::strip(bytes, options);
  if (!stripped) {
    std::cerr << path << ": " << stripped.error().message << '\n';
    return 1;
  }
  const std::string& target = output.empty() ? path : output;
  if (!qobj::writeFile(target, stripped->bytes))
    return qobj::reportError(target, std::runtime_error("cannot write " + target));
  if (flags.verbose)
    std::cerr << path << ": removed " << stripped->symbolsRemoved
              << " symbols and " << stripped->sectionsRemoved << " sections\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<OptionSpec> specs = {
      kHelp,
      {'s', "strip-all", false, "", "remove every symbol"},
      {'g', "strip-debug", false, "", "remove debug symbols and sections (the default)"},
      {'d', "strip-unneeded", false, "", "remove debug symbols and unreferenced locals"},
      {'R', "remove-section", true, "name", "remove this section (repeatable)"},
      {'K', "keep-section", true, "name", "keep this section even if it would go"},
      {'N', "strip-symbol", true, "name", "remove this symbol (repeatable)"},
      {'w', "wildcard", false, "", "treat -R/-K/-N patterns as globs"},
      {'v', "verbose", false, "", "report what was removed"},
      {'o', "output", true, "file", "write the result here instead of in place"},
  };
  qobj::Options options("qobjstrip", "file...", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjstrip: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp)) {
    std::cout << options.usage();
    return 0;
  }
  int modes = int(options.has('s')) + int(options.has('g')) + int(options.has('d'));
  if (modes > 1) {
    std::cerr << "qobjstrip: choose one of -s, -g and -d\n";
    return 2;
  }
  if (options.positional().empty()) {
    std::cerr << "qobjstrip: no input files\n\n" << options.usage();
    return 2;
  }

  Flags flags;
  flags.mode = options.has('s')  ? strip::Mode::All
               : options.has('d') ? strip::Mode::Unneeded
                                  : strip::Mode::Debug;
  flags.verbose = options.has('v');
  flags.wildcards = options.has('w');
  removeSections_ = options.valueList("remove-section");
  keepSections_ = options.valueList("keep-section");
  stripSymbols_ = options.valueList("strip-symbol");

  const std::string output = options.value("output");
  if (!output.empty() && options.positional().size() > 1) {
    std::cerr << "qobjstrip: -o takes one file, but several were given\n";
    return 2;
  }
  int status = 0;
  for (const auto& path : options.positional())
    status |= stripFile(path, output, flags);
  return status;
}
