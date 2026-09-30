// qobjdump - report on the structure of an object file or archive member.
//
// Option letters follow the conventional objdump so the tool is familiar:
//   -f file headers      -h section headers   -p program headers
//   -t symbol table      -T dynamic symbols   -r relocations
//   -s section contents  -x all headers       -a archive index
//   -m architecture      -V version           -b <target>  force a target
//   -j <section>         -? help
#include "../qobjcommon/cli.hpp"
#include "../qobjcommon/options.hpp"

using namespace qbfd;
using qobj::hex;
using qobj::OptionSpec;

namespace {

// The help option is long-only, because -h conventionally means "section
// headers" in objdump and reusing it would be more confusing than the loss.
constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};
constexpr OptionSpec kHelpShort{'?', "", false, "", "show this help"};

struct Flags {
  bool fileHeaders = false, sectionHeaders = false, programHeaders = false;
  bool symbols = false, dynamicSymbols = false, relocations = false;
  bool contents = false, allHeaders = false, archiveIndex = false;
  bool architecture = false, version = false, privateHeaders = false;
  bool fullRelocations = false, needed = false;
  std::string section, target;
  std::optional<uint64_t> startAddress, stopAddress;
};

void dumpContents(std::ostream& out, const ObjectFile& file, const Section& s,
                  std::optional<uint64_t> start, std::optional<uint64_t> stop) {
  auto bytes = file.sectionContents(s);
  if (!bytes) {
    out << "qobjdump: " << s.name << ": " << bytes.error().message << '\n';
    return;
  }
  if (bytes->empty()) return;
  // --start-address/--stop-address select a virtual-address range; sections
  // with no address (a relocatable object's) are only range-limited when the
  // address is inside them at all.
  size_t begin = 0, end = bytes->size();
  if (start || stop) {
    uint64_t low = start.value_or(0);
    uint64_t high = stop.value_or(UINT64_MAX);
    uint64_t lo = std::max(low, s.vma), hi = std::min(high, s.vma + s.size);
    if (hi <= lo) return;
    begin = size_t(lo - s.vma);
    end = size_t(std::min<uint64_t>(hi - s.vma, bytes->size()));
  }
  out << "Contents of section " << s.name << ":\n";
  for (size_t i = begin; i < end; i += 16) {
    size_t n = std::min<size_t>(16, end - i);
    out << ' ' << std::setw(8) << std::setfill('0') << std::hex
        << (s.fileOffset + i) << std::dec << std::setfill(' ') << ' ';
    for (size_t k = 0; k < 16; ++k)
      out << (k < n ? hex((*bytes)[i + k], 2) : "  ") << ' ';
    out << " |" << qobj::printableText(bytes->subspan(i, n)) << "|\n";
  }
}

const char* bindingName(SymbolBinding b) {
  switch (b) {
    case SymbolBinding::Local: return "LOCAL";
    case SymbolBinding::Global: return "GLOBAL";
    case SymbolBinding::Weak: return "WEAK";
    case SymbolBinding::Unique: return "UNIQUE";
  }
  return "?";
}

void showSections(std::ostream& out, const ObjectFile& file, bool details) {
  out << "\nSections:\n"
      << "Idx Name" << std::string(details ? 32 : 20, ' ') << "Size"
      << std::string(12, ' ') << "VMA" << std::string(19, ' ') << "FileOff"
      << std::string(9, ' ') << "Flags\n";
  for (const auto& s : file.sections()) {
    if (s.index == 0 && s.name.empty()) continue;
    out << std::setw(3) << s.index << ' ' << std::left << std::setw(32) << s.name
        << std::right << std::setw(10) << s.size << "  0x" << std::setw(16)
        << std::setfill('0') << std::hex << s.vma << std::dec << std::setfill(' ')
        << "  0x" << std::setw(8) << std::setfill('0') << std::hex << s.fileOffset
        << std::dec << std::setfill(' ') << ' ' << qobj::sectionFlags(s) << '\n';
    if (!details) continue;
    out << "      lma=0x" << hex(s.lma) << " fileSize=" << s.fileSize
        << " align=" << s.alignment << " nativeType=0x" << hex(s.nativeType)
        << " rawFlags=0x" << hex(s.nativeFlags);
    if (file.format() == Format::ELF)
      out << " (" << qobj::elfSectionType(s.nativeType) << ')';
    out << '\n';
  }
}

void showSymbols(std::ostream& out, const std::vector<Symbol>& symbols,
                 const char* title) {
  if (symbols.empty()) return;
  out << "\n" << title << ":\n"
      << "   Num:    Value          Size Type    Bind   Vis      Ndx Name\n";
  for (const auto& s : symbols) {
    out << std::setw(6) << s.index << ": 0x" << std::setw(16) << std::setfill('0')
        << std::hex << s.value << std::dec << std::setfill(' ') << ' '
        << std::setw(8) << s.size << ' ' << std::left << std::setw(7)
        << qobj::symbolTypeName(s) << std::right << ' ' << std::setw(7)
        << bindingName(s.binding) << ' ' << std::setw(7)
        << ((s.flags & sym::Hidden) ? "HIDDEN" : "DEFAULT") << ' ' << std::setw(4)
        << (s.section ? std::to_string(*s.section) : std::string("*UND*"));
    if (s.flags & sym::Absolute) out << "  (ABS)";
    if (s.kind == SymbolKind::Common) out << "  (COM)";
    if (s.flags & sym::Imported) out << "  (IMP)";
    if (s.flags & sym::Exported) out << "  (EXP)";
    out << ' ' << s.name << '\n';
  }
}

void showRelocations(std::ostream& out, const ObjectFile& file, bool full) {
  bool any = false;
  for (const auto& s : file.sections()) {
    auto relocs = file.relocations(s);
    if (!relocs) {
      out << "\nRelocations: " << s.name << ": " << relocs.error().message << '\n';
      any = true;
      continue;
    }
    if (relocs->empty()) continue;
    if (!any) {
      out << "\nRelocations:\n";
      any = true;
    }
    out << s.name << ":\n";
    for (const auto& r : *relocs) {
      out << "  0x" << hex(r.offset) << ' ' << r.typeName;
      if (r.symbol) {
        const auto& table = r.dynamicSymbol ? file.dynamicSymbols() : file.symbols();
        if (*r.symbol < table.size()) {
          const auto& target = table[*r.symbol];
          if (full)
            out << "  [" << target.index << ' ' << target.name << ']';
          else
            out << ' ' << target.name;
        } else {
          out << "  <invalid symbol index " << *r.symbol << '>';
        }
      } else if (r.targetSection) {
        out << "  <section " << *r.targetSection << '>';
      }
      if (r.scattered) out << "  [scattered]";
      if (r.hasAddend) out << (r.addend < 0 ? " -" : " +") << std::abs(r.addend);
      if (full) {
        out << "  (native=" << r.nativeType;
        if (r.nativeValue) out << " value=0x" << hex(r.nativeValue);
        if (r.bitSize) out << " width=" << unsigned(r.bitSize);
        if (r.pcRelative) out << " pcrel";
        if (r.dynamicSymbol) out << " dynamic";
        out << ')';
      }
      out << '\n';
    }
  }
  if (!any) out << "\nRelocations: none\n";
}

void showFileHeader(std::ostream& out, const ObjectFile& file) {
  out << "\nFile header:\n"
      << "  format     " << qobj::formatName(file.format()) << '\n'
      << "  target     " << file.targetName() << '\n'
      << "  arch       " << std::string(toString(file.arch())) << '\n'
      << "  class      " << (file.is64Bit() ? "64-bit" : "32-bit") << '\n'
      << "  endian     "
      << (file.endian() == Endian::Little ? "little" : "big") << '\n'
      << "  type       " << std::string(toString(file.fileType())) << '\n'
      << "  entry      0x" << hex(file.entryPoint()) << '\n'
      << "  image base 0x" << hex(file.imageBase()) << '\n';
  if (const auto* e = dynamic_cast<const elf::ELFFile*>(&file))
    out << "  e_flags    0x" << hex(e->flags()) << '\n';
  if (const auto* p = dynamic_cast<const pe::PEFile*>(&file)) {
    out << "  subsystem  " << p->subsystem() << '\n'
        << "  timestamp  " << p->timestamp() << '\n';
    if (auto name = p->exportName()) out << "  export     " << *name << '\n';
  }
  if (const auto* m = dynamic_cast<const macho::MachOFile*>(&file)) {
    out << "  cpu type   0x" << hex(m->cpuType()) << '\n';
    if (!m->installName().empty())
      out << "  install    " << m->installName() << '\n';
    for (const auto& r : m->runpaths()) out << "  rpath      " << r << '\n';
  }
}

void showProgramHeaders(std::ostream& out, const ObjectFile& file) {
  out << "\nProgram headers / segments:\n";
  if (file.segments().empty()) out << "  (none)\n";
  for (const auto& s : file.segments()) {
    out << "  " << std::setw(2) << std::hex << s.nativeType << std::dec << ' '
        << std::left << std::setw(18) << s.name << std::right << " vaddr=0x"
        << hex(s.vaddr) << " memsz=" << s.memSize << " fileoff=0x"
        << hex(s.fileOffset) << " filesz=" << s.fileSize << " align="
        << s.alignment;
    if (s.flags & seg::Read) out << " R";
    if (s.flags & seg::Write) out << " W";
    if (s.flags & seg::Exec) out << " E";
    if (!s.sections.empty()) {
      out << "  sections:";
      for (uint32_t i : s.sections) out << ' ' << i;
    }
    out << '\n';
  }
}

void showPrivateHeaders(std::ostream& out, const ObjectFile& file) {
  out << "\nRaw section header table:\n";
  const auto* e = dynamic_cast<const elf::ELFFile*>(&file);
  if (!e) {
    out << "  (only ELF exposes its raw section headers)\n";
    return;
  }
  for (const auto& h : e->rawSections())
    out << "  name=0x" << hex(h.name) << " type=" << h.type
        << " flags=0x" << hex(h.flags) << " addr=0x" << hex(h.addr)
        << " off=0x" << hex(h.offset) << " size=" << h.size << " link=" << h.link
        << " info=" << h.info << " align=" << h.align
        << " entsize=" << h.entsize << '\n';
}

void showNeeded(std::ostream& out, const ObjectFile& file) {
  out << "\nNeeded libraries:\n";
  auto needed = file.neededLibraries();
  if (needed.empty()) out << "  (none)\n";
  for (const auto& n : needed) out << "  " << n << '\n';
}

int dump(const qobj::Source& source, const Flags& flags) {
  const auto& file = source.file();
  auto& out = std::cout;

  if (flags.version) {
    out << "qobjdump " << qobj::kVersion << " (qBFD " << qobj::kBFDVersion
        << ")\n";
    return 0;
  }
  if (flags.architecture) {
    out << source.label << ": " << std::string(toString(file.arch())) << '\n';
    return 0;
  }
  if (flags.needed) {
    showNeeded(out, file);
    return 0;
  }
  if (!flags.section.empty()) {
    const Section* s = file.findSection(flags.section);
    if (!s) {
      out << "qobjdump: " << source.label << ": no section named "
          << flags.section << '\n';
      return 1;
    }
    showSections(out, file, false);
    dumpContents(out, file, *s, flags.startAddress, flags.stopAddress);
    return 0;
  }

  out << "\n" << source.label << ":  " << qobj::formatName(file.format()) << ' '
      << file.targetName() << ' ' << std::string(toString(file.fileType()))
      << (flags.allHeaders ? " (all headers)" : "") << '\n';

  if (flags.allHeaders || flags.fileHeaders) showFileHeader(out, file);
  if (flags.allHeaders || flags.programHeaders) showProgramHeaders(out, file);
  if (flags.privateHeaders) showPrivateHeaders(out, file);
  if (flags.allHeaders || flags.sectionHeaders) showSections(out, file, flags.allHeaders);
  if (flags.allHeaders || flags.symbols) showSymbols(out, file.symbols(), "Symbol table");
  if (flags.allHeaders || flags.dynamicSymbols)
    showSymbols(out, file.dynamicSymbols(), "Dynamic symbol table");
  if (flags.allHeaders || flags.relocations || flags.contents)
    showRelocations(out, file, flags.fullRelocations);
  if (flags.allHeaders || flags.contents) {
    for (const auto& s : file.sections())
      dumpContents(out, file, s, flags.startAddress, flags.stopAddress);
  }
  return 0;
}

}  // namespace

// -a reports the archive's symbol index, so it works on the container rather
// than on each member.
int showArchiveIndex(const std::string& path) {
  try {
    auto bytes = qobj::readFile(path);
    if (!qbfd::isArchive(bytes)) {
      std::cerr << "qobjdump: " << path << ": not an archive\n";
      return 1;
    }
    auto opened = qbfd::openArchive(bytes, path);
    if (!opened) return qobj::reportError(path, opened.error());
    std::cout << "\nArchive index (" << opened->archive->members().size()
              << " members):\n";
    const auto& index = opened->archive->symbolIndex();
    if (index.empty()) {
      std::cout << "  (none)\n";
      return 0;
    }
    for (const auto& e : index) {
      const auto* member = opened->archive->memberAt(e.headerOffset);
      std::cout << "  " << e.symbol << " in "
                << (member ? member->name : "<unknown member>") << '\n';
    }
    return 0;
  } catch (const std::exception& e) {
    return qobj::reportError(path, e);
  }
}

int main(int argc, char** argv) {
  std::vector<OptionSpec> specs = {
      kHelp, kHelpShort,
      {'f', "file-headers", false, "", "show the file header"},
      {'h', "section-headers", false, "", "show section headers"},
      {'p', "program-headers", false, "", "show segment/program headers"},
      {'t', "syms", false, "", "show the symbol table"},
      {'T', "dynamic-syms", false, "", "show the dynamic symbol table"},
      {'r', "reloc", false, "", "show relocations"},
      {0, "full-reloc", false, "", "show every relocation field"},
      {'s', "section-contents", false, "", "hex dump section contents"},
      {'x', "all-headers", false, "", "show every header and section"},
      {'a', "archive-index", false, "", "show the archive symbol index"},
      {0, "private-headers", false, "", "dump the raw header tables"},
      {0, "needs", false, "", "show shared-library dependencies"},
      {'m', "arch-name", false, "", "show the architecture only"},
      {'V', "version", false, "", "show the version"},
      {'b', "target", true, "name", "parse with this target instead of sniffing"},
      {'j', "section", true, "name", "operate on this section only"},
      {0, "start-address", true, "addr", "start dumping at this address"},
      {0, "stop-address", true, "addr", "stop dumping at this address"},
  };
  qobj::Options options("qobjdump", "file...", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjdump: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp) || options.has('?')) {
    std::cout << options.usage();
    return 0;
  }

  Flags flags;
  flags.fileHeaders = options.has('f');
  flags.sectionHeaders = options.has('h');
  flags.programHeaders = options.has('p');
  flags.symbols = options.has('t');
  flags.dynamicSymbols = options.has('T');
  flags.relocations = options.has('r');
  flags.fullRelocations = options.has("full-reloc");
  flags.contents = options.has('s');
  flags.allHeaders = options.has('x');
  flags.archiveIndex = options.has('a');
  flags.privateHeaders = options.has("private-headers");
  flags.needed = options.has("needs");
  flags.architecture = options.has('m');
  flags.version = options.has('V');
  flags.target = options.value("target");
  flags.section = options.value("section");

  // Addresses accept decimal and 0x-prefixed hex.
  if (options.has("start-address")) {
    uint64_t address = 0;
    if (!qobj::parseNumber(options.value("start-address"), address)) {
      std::cerr << "qobjdump: --start-address needs a number\n";
      return 2;
    }
    flags.startAddress = address;
  }
  if (options.has("stop-address")) {
    uint64_t address = 0;
    if (!qobj::parseNumber(options.value("stop-address"), address)) {
      std::cerr << "qobjdump: --stop-address needs a number\n";
      return 2;
    }
    flags.stopAddress = address;
  }

  if (flags.version) {
    std::cout << "qobjdump " << qobj::kVersion << " (qBFD " << qobj::kBFDVersion
              << ")\n";
    return 0;
  }
  if (options.positional().empty()) {
    std::cerr << "qobjdump: no input files\n\n" << options.usage();
    return 2;
  }
  if (flags.archiveIndex) {
    int status = 0;
    for (const auto& path : options.positional()) status |= showArchiveIndex(path);
    return status;
  }
  if (flags.target.empty())
    return qobj::visit(options.positional(),
                       [&](const qobj::Source& s) { return dump(s, flags); });

  // -b forces a target, so the archive/member sniffing in visit() does not
  // apply: read the bytes and open them with that target.
  int status = 0;
  for (const auto& path : options.positional()) {
    try {
      auto bytes = qobj::readFile(path);
      auto opened = qbfd::openAs(bytes, flags.target, path);
      if (!opened) {
        status |= qobj::reportError(path, opened.error());
        continue;
      }
      qobj::Source s;
      s.label = path;
      s.object = std::move(opened->object);
      status |= dump(s, flags);
    } catch (const std::exception& e) {
      status |= qobj::reportError(path, e);
    }
  }
  return status;
}
