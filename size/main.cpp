// qobjsize - report section sizes and totals for an object file.
#include "../qobjcommon/cli.hpp"
#include "../qobjcommon/options.hpp"

using namespace qbfd;
using qobj::OptionSpec;

namespace {

constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};

struct Totals {
  uint64_t text = 0, data = 0, bss = 0, rodata = 0, debug = 0, total = 0;
  uint64_t fileSize = 0;
  size_t sections = 0;
};

Totals measure(const ObjectFile& file) {
  Totals t;
  t.fileSize = file.data().size();
  for (const auto& s : file.sections()) {
    if (s.index == 0 && s.name.empty()) continue;
    ++t.sections;
    // A section is counted once: debug sections are not also code or data.
    if (s.flags & sec::Debug) {
      t.debug += s.size;
    } else if (s.flags & sec::Bss) {
      t.bss += s.size;
    } else if (s.flags & sec::Code) {
      t.text += s.size;
    } else if (s.flags & sec::Strings || s.flags & sec::ReadOnly) {
      t.rodata += s.size;
    } else if (s.flags & sec::Data) {
      t.data += s.size;
    } else {
      t.data += s.size;
    }
    t.total += s.size;
  }
  return t;
}

void printTotals(const Totals& t, const std::string& label) {
  std::cout << std::setw(10) << t.text << std::setw(10) << t.data
            << std::setw(10) << t.bss << std::setw(10) << t.rodata
            << std::setw(10) << t.debug << std::setw(10) << t.total << std::setw(10)
            << t.fileSize << std::setw(8) << t.sections << ' ' << label << '\n';
}

int report(const qobj::Source& source, bool totalsOnly, bool perSection) {
  const auto& file = source.file();
  Totals t = measure(file);
  if (!totalsOnly) printTotals(t, source.label);
  if (perSection) {
    for (const auto& s : file.sections()) {
      if (s.index == 0 && s.name.empty()) continue;
      std::cout << std::left << std::setw(24) << s.name << std::right
                << std::setw(10) << s.size << std::setw(10) << s.fileSize
                << "  0x" << qobj::hex(s.vma) << ' ' << qobj::sectionFlags(s) << '\n';
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<OptionSpec> specs = {
      kHelp,
      {'A', "format=sysv", false, "", "print in the SysV format (the default)"},
      {'B', "format=berkeley", false, "", "print in the Berkeley format"},
      {'t', "totals", false, "", "print only a grand total"},
      {'d', "radix", false, "", "print sizes in decimal (the default)"},
      {'x', "hex", false, "", "print sizes in hex"},
      {0, "sections", false, "", "list every section with its sizes"},
  };
  qobj::Options options("qobjsize", "file...", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjsize: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp)) {
    std::cout << options.usage();
    return 0;
  }
  if (options.has('A') && options.has('B')) {
    std::cerr << "qobjsize: choose one of -A and -B\n";
    return 2;
  }
  if (options.has('d') && options.has('x')) {
    std::cerr << "qobjsize: choose one of -d and -x\n";
    return 2;
  }
  if (options.positional().empty()) {
    std::cerr << "qobjsize: no input files\n\n" << options.usage();
    return 2;
  }

  if (!options.has('t'))
    std::cout << std::right << std::setw(10) << "text" << std::setw(10) << "data"
              << std::setw(10) << "bss" << std::setw(10) << "rodata"
              << std::setw(10) << "debug" << std::setw(10) << "dec"
              << std::setw(10) << "hex" << std::setw(8) << "sect" << " filename\n";

  Totals grand;
  bool any = false;
  int status = qobj::visit(options.positional(), [&](const qobj::Source& s) {
    Totals t = measure(s.file());
    grand.text += t.text;
    grand.data += t.data;
    grand.bss += t.bss;
    grand.rodata += t.rodata;
    grand.debug += t.debug;
    grand.total += t.total;
    grand.fileSize += t.fileSize;
    grand.sections += t.sections;
    any = true;
    return report(s, options.has('t'), options.has("sections"));
  });
  if (options.has('t') && any) printTotals(grand, " (TOTALS)");
  return status;
}
