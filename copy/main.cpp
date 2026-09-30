// qobjcp - copy a whole object file, or one of its sections.
#include "../qobjcommon/cli.hpp"
#include "../qobjcommon/options.hpp"

using namespace qbfd;
using qobj::OptionSpec;

namespace {

constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};

int listSections(const qobj::Source& source) {
  std::cout << source.label << ": " << qobj::formatName(source.file().format())
            << " " << source.file().targetName() << '\n';
  for (const auto& s : source.file().sections()) {
    if (s.index == 0 && s.name.empty()) continue;
    std::cout << std::left << std::setw(24) << s.name << std::right
              << std::setw(10) << s.size << std::setw(10) << s.fileSize
              << "  0x" << qobj::hex(s.fileOffset) << ' ' << qobj::sectionFlags(s)
              << '\n';
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<OptionSpec> specs = {
      kHelp,
      {0, "dump-section", true, "name", "write this section's contents"},
      {0, "list-sections", false, "", "list the sections and their sizes"},
  };
  qobj::Options options("qobjcp", "input [output]", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjcp: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp)) {
    std::cout << options.usage();
    return 0;
  }
  const bool dump = options.has("dump-section");
  const bool list = options.has("list-sections");
  if (!dump && !list && options.positional().size() != 2) {
    std::cerr << "qobjcp: need an input and an output, or an option\n\n"
              << options.usage();
    return 2;
  }
  if (options.positional().empty()) {
    std::cerr << "qobjcp: no input file\n\n" << options.usage();
    return 2;
  }
  const std::string input = options.positional()[0];
  const std::string output =
      options.positional().size() > 1 ? options.positional()[1] : std::string();

  try {
    auto bytes = qobj::readFile(input);
    if (list || dump) {
      auto opened = qbfd::open(bytes, input);
      if (!opened) return qobj::reportError(input, opened.error());
      qobj::Source source;
      source.label = input;
      source.bytes = bytes;
      source.object = std::move(opened->object);
      if (list) return listSections(source);
      const Section* s = source.file().findSection(options.value("dump-section"));
      if (!s) {
        std::cerr << "qobjcp: " << input << ": no section named "
                  << options.value("dump-section") << '\n';
        return 1;
      }
      auto contents = source.file().sectionContents(*s);
      if (!contents) return qobj::reportError(input, contents.error());
      if (output.empty()) {
        std::cout.write(reinterpret_cast<const char*>(contents->data()),
                        static_cast<std::streamsize>(contents->size()));
        return 0;
      }
      if (!qobj::writeFile(output, *contents))
        return qobj::reportError(output,
                                 std::runtime_error("cannot write " + output));
      return 0;
    }
    if (!qobj::writeFile(output, bytes))
      return qobj::reportError(output, std::runtime_error("cannot write " + output));
    return 0;
  } catch (const std::exception& e) {
    return qobj::reportError(input, e);
  }
}
