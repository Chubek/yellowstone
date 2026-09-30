// qobjdump - report on the structure of an object file or archive member.
//
// Option letters follow the conventional objdump so the tool is familiar:
//   -f file headers      -h section headers   -p program headers
//   -t symbol table      -T dynamic symbols   -r relocations
//   -s section contents  -x all headers       -a archive index
//   -m architecture      -V version           -b <target>  force a target
//   -j <section>         -? help
#include "../common/cli.hpp"
#include "../common/isa.hpp"
#include "../common/options.hpp"
#include "../common/domterm_pager.hpp"

#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#ifndef _WIN32
#include <unistd.h>
#endif

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
  bool disassemble = false, hexDump = false, octDump = false;
  bool text = true, tui = false, semantics = false, registerNames = false;
  bool raw = false;
  bool color = true, pager = true;
  std::string theme = "default", pagerCommand;
  std::string search, isaDir = "infobank/isa";
  std::string section, target;
  std::optional<uint64_t> startAddress, stopAddress;
};

struct Config {
  bool pager = true;
  bool semantics = false;
  bool registerNames = false;
  bool color = true;
  std::string theme = "default";
  std::string pagerCommand;
  std::string isaDir = "infobank/isa";
};

std::string configPath() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) return std::string(xdg) + "/yellowstone/Objdump.ini";
  const char* home = std::getenv("HOME");
  return home ? std::string(home) + "/.config/yellowstone/Objdump.ini" : std::string{};
}

bool parseBool(std::string v, bool fallback) {
  for (char& c : v) c = char(std::tolower(static_cast<unsigned char>(c)));
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return fallback;
}

Config loadConfig() {
  Config c; auto path = configPath(); if (path.empty()) return c;
  std::ifstream in(path); if (!in) return c;
  std::string line, section;
  while (std::getline(in, line)) {
    auto trim = [](std::string s) { auto a=s.find_first_not_of(" \t\r"); auto b=s.find_last_not_of(" \t\r"); return a==std::string::npos ? std::string{} : s.substr(a,b-a+1); };
    line = trim(line); if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') { section = line.substr(1, line.size()-2); continue; }
    auto eq = line.find('='); if (eq == std::string::npos) continue;
    auto key = trim(line.substr(0, eq)); auto value = trim(line.substr(eq+1));
    if (section != "display" && section != "qobjdump" && !section.empty()) continue;
    if (key == "pager") c.pager = parseBool(value, c.pager);
    else if (key == "semantics") c.semantics = parseBool(value, c.semantics);
    else if (key == "register_names" || key == "register-names") c.registerNames = parseBool(value, c.registerNames);
    else if (key == "color" || key == "colour") c.color = parseBool(value, c.color);
    else if (key == "theme") c.theme = value;
    else if (key == "pager_command" || key == "pager-command") c.pagerCommand = value;
    else if (key == "isa_dir" || key == "isa-dir") c.isaDir = value;
  }
  return c;
}

int emitOutput(const std::string& text, const Flags& flags) {
  if (!flags.tui || !flags.pager) { std::cout << text; return 0; }
  if (flags.pagerCommand.empty()) {
    qobj::DomtermPager pager;
    if (pager.run(text)) return 0;
  }
  std::string command = flags.pagerCommand;
  if (command.empty()) { const char* p = std::getenv("PAGER"); command = p && *p ? p : "less -R"; }
  FILE* pipe = popen(command.c_str(), "w");
  if (!pipe) { std::cout << text; return 0; }
  std::fwrite(text.data(), 1, text.size(), pipe); int rc = pclose(pipe);
  if (rc != 0) std::cout << text;
  return 0;
}

std::string isaKey(Arch a) {
  switch (a) {
    case Arch::X86: return "x86";
    case Arch::X86_64: return "amd64";
    case Arch::ARM: return "arm32";
    case Arch::AArch64: return "aarch64";
    case Arch::RISCV32: return "riscv32";
    case Arch::RISCV64: return "riscv64";
    case Arch::PowerPC: return "ppc32";
    case Arch::PowerPC64: return "ppc64";
    case Arch::MIPS: return "mips32";
    case Arch::MIPS64: return "mips64";
    case Arch::SPARC64: return "sparc64";
    case Arch::S390X: return "s390x";
    case Arch::LoongArch64: return "loongarch64";
    default: return {};
  }
}

uint64_t fieldNumber(const qisa::Encoding& e, const char* key) {
  auto it = e.fields.find(key); if (it == e.fields.end()) return UINT64_MAX;
  try { return std::stoull(it->second, nullptr, 0); } catch (...) { return UINT64_MAX; }
}

std::string unquote(std::string value) {
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    value = value.substr(1, value.size() - 2);
  std::string out;
  for (std::size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '\\' && i + 1 < value.size()) {
      char next = value[++i];
      if (next == '"' || next == '\\') out += next;
      else { out += '\\'; out += next; }
    } else out += value[i];
  }
  return out;
}

std::string registerName(const qisa::Document& doc, std::string_view cls,
                         unsigned number) {
  std::string className(cls);
  while (!className.empty() && std::isspace(static_cast<unsigned char>(className.back()))) className.pop_back();
  while (!className.empty() && std::isspace(static_cast<unsigned char>(className.front()))) className.erase(className.begin());
  for (const auto& rc : doc.registerClasses) if (rc.name == className)
    for (const auto& r : rc.registers) if (r.number == number) return r.name;
  return className + std::to_string(number);
}

std::string replaceAll(std::string value, std::string_view key,
                       std::string_view replacement) {
  std::string needle = "{" + std::string(key) + "}";
  for (std::size_t p = 0; (p = value.find(needle, p)) != std::string::npos; ) {
    value.replace(p, needle.size(), replacement); p += replacement.size();
  }
  return value;
}

std::string renderSyntax(const qisa::Document& doc, const qisa::Operation& op,
                         const qisa::Encoding& enc, uint64_t raw,
                         std::size_t width, bool names) {
  std::string text = unquote(op.syntax);
  if (!names) return text;
  unsigned low = unsigned(raw & 7u) + (unsigned((raw >> 32) & 1u) * 8u), reg = 0, rm = 0;
  if (width >= 2) {
    unsigned modrm = unsigned((raw >> 8) & 0xffu);
    reg = (modrm >> 3) & 7u; rm = modrm & 7u;
  }
  auto operands = op.fields.find("operands");
  std::string operandText = operands == op.fields.end() ? "" : unquote(operands->second);
  auto clsFor = [&](std::string_view name) -> std::string {
    std::size_t p = operandText.find(std::string(name) + ":");
    if (p == std::string::npos) return "GPR";
    p += name.size() + 1; std::size_t e = operandText.find_first_of(",; ", p);
    while (p < operandText.size() && std::isspace(static_cast<unsigned char>(operandText[p]))) ++p;
    e = operandText.find_first_of(",; ", p);
    return operandText.substr(p, e == std::string::npos ? std::string::npos : e-p);
  };
  if (enc.fields.count("opreg")) text = replaceAll(text, "rd", registerName(doc, clsFor("rd"), low));
  if (enc.fields.count("modrm")) {
    std::string layout = enc.fields.at("modrm");
    auto comma = layout.find(',');
    std::string first = comma == std::string::npos ? layout : layout.substr(0, comma);
    std::string second = comma == std::string::npos ? "" : layout.substr(comma + 1);
    auto trim = [](std::string s) { s.erase(0, s.find_first_not_of(" \t")); s.erase(s.find_last_not_of(" \t") + 1); return s; };
    first = trim(first); second = trim(second);
    auto valueFor = [&](std::string_view name) { return name == first ? reg : name == second ? rm : reg; };
    text = replaceAll(text, "rd", registerName(doc, clsFor("rd"), valueFor("rd")));
    text = replaceAll(text, "rs1", registerName(doc, clsFor("rs1"), valueFor("rs1")));
    text = replaceAll(text, "mem", "[mem]");
  }
  return text;
}

std::string tuiColor(const Flags& flags, std::string_view role) {
  if (!flags.color) return {};
  if (flags.theme == "monochrome") return role == "mnemonic" ? "\033[1m" : "";
  if (flags.theme == "dracula") return role == "mnemonic" ? "\033[1;35m" : "\033[38;5;117m";
  if (flags.theme == "solarized") return role == "mnemonic" ? "\033[1;33m" : "\033[36m";
  return role == "mnemonic" ? "\033[1;36m" : "\033[37m";
}

void showDisassembly(std::ostream& out, const ObjectFile& file,
                     const Flags& flags) {
  qisa::Database db; std::vector<std::string> errors;
  db.loadDirectory(flags.isaDir, errors);
  std::string key = isaKey(file.arch());
  const qisa::Document* doc = db.find(key);
  if (!doc) {
    out << "\nDisassembly: ISA description unavailable for " << key << "\n";
    for (const auto& e : errors) out << "  ISA: " << e << '\n';
    return;
  }
  std::multimap<uint64_t, const qisa::Operation*> ops;
  for (const auto& op : doc->operations) {
    auto it = op.fields.find("encoding"); if (it == op.fields.end()) continue;
    for (const auto& e : doc->encodings) if (e.name == it->second) {
      uint64_t opcode = fieldNumber(e, "opcode"); if (opcode == UINT64_MAX) opcode = fieldNumber(e, "base");
      if (opcode != UINT64_MAX) ops.emplace(opcode, &op);
    }
  }
  out << "\nDisassembly for " << key << " using " << doc->source << ":\n";
  for (const auto& s : file.sections()) {
    if (!(s.flags & sec::Code) || (s.flags & sec::Bss)) continue;
    auto bytes = file.sectionContents(s); if (!bytes || bytes->empty()) continue;
    std::size_t begin = 0, end = bytes->size();
    if (flags.startAddress || flags.stopAddress) {
      uint64_t endAddress = s.vma;
      if (bytes->size() > UINT64_MAX - endAddress) endAddress = UINT64_MAX;
      else endAddress += bytes->size();
      uint64_t lo = flags.startAddress.value_or(s.vma);
      uint64_t hi = flags.stopAddress.value_or(UINT64_MAX);
      if (hi <= s.vma || lo >= endAddress) continue;
      begin = std::size_t(std::max(lo, s.vma) - s.vma);
      end = std::size_t(std::min<uint64_t>(hi, endAddress) - s.vma);
    }
    out << "\n" << s.name << ":\n";
    std::size_t width = key.starts_with("riscv") || key == "aarch64" || key == "arm32" ? 4 : 1;
    for (std::size_t i = begin; i < end; ) {
      std::size_t n = std::min(width, end - i);
      std::size_t opcodeOffset = 0;
      if (key == "amd64" || key == "x86") {
        if (i + 1 < end && (((*bytes)[i] & 0xf0) == 0x40 ||
                            (*bytes)[i] == 0x66 || (*bytes)[i] == 0xf2 ||
                            (*bytes)[i] == 0xf3 || (*bytes)[i] == 0x0f))
          opcodeOffset = 1;
        if (opcodeOffset >= n) opcodeOffset = 0;
        n = std::min<std::size_t>(end - i, opcodeOffset + 1);
      }
      uint64_t raw = 0; for (std::size_t j = 0; j < n; ++j) raw |= uint64_t((*bytes)[i+j]) << (8*j);
      uint64_t opcodeByte = (raw >> (opcodeOffset * 8)) & 0xffu;
      const qisa::Operation* op = nullptr;
      for (const auto& [code, candidate] : ops) {
        auto ei = candidate->fields.find("encoding");
        const qisa::Encoding* ce = nullptr;
        if (ei != candidate->fields.end()) for (const auto& e : doc->encodings)
          if (e.name == ei->second) { ce = &e; break; }
        bool opreg = ce && ce->fields.contains("opreg");
        bool match = key == "riscv32" || key == "riscv64"
            ? ((raw & 0x7fu) == code)
            : (opreg ? ((opcodeByte & 0xf8u) == (code & 0xf8u))
                     : (opcodeByte == (code & 0xffu)));
        if (!match) continue;
        if (ce && ce->fields.contains("prefix")) {
          uint64_t prefix = fieldNumber(*ce, "prefix");
          if (prefix == 0xf) prefix = 0x0f;
          if (opcodeOffset == 0 || (*bytes)[i + opcodeOffset - 1] != prefix) continue;
        }
        if (ce && ce->fields.contains("opreg")) { op = candidate; break; }
        if (!op) op = candidate;
      }
      if (op && (op->fields.contains("encoding"))) {
        auto ei = op->fields.find("encoding");
        for (const auto& e : doc->encodings) if (e.name == ei->second && e.fields.contains("modrm")) {
          n = std::min<std::size_t>(end - i, opcodeOffset + 2);
          break;
        }
        if (op->fields.contains("encoding")) {
          auto ei2 = op->fields.find("encoding");
          for (const auto& e : doc->encodings) if (e.name == ei2->second && e.fields.contains("opreg")) n = std::min<std::size_t>(end - i, opcodeOffset + 1);
        }
      }
      raw = 0; for (std::size_t j = 0; j < n; ++j) raw |= uint64_t((*bytes)[i+j]) << (8*j);
      const qisa::Encoding* matched = nullptr;
      if (op) {
        auto ei = op->fields.find("encoding");
        if (ei != op->fields.end()) for (const auto& e : doc->encodings)
          if (e.name == ei->second) { matched = &e; break; }
      }
      std::string text = op && matched
          ? renderSyntax(*doc, *op, *matched,
                         (key == "amd64" || key == "x86")
                             ? ((raw >> (opcodeOffset * 8)) |
                                ((opcodeOffset && ((*bytes)[i] & 1)) ? (1ull << 32) : 0)) : raw,
                         n - std::min(n, opcodeOffset), flags.registerNames)
          : (n == 1 ? ".byte " + hex((*bytes)[i], 2) : ".word " + hex(raw));
      if (!flags.search.empty() && text.find(flags.search) == std::string::npos) { i += n; continue; }
      if (flags.color) out << "  \033[2;37m";
      else out << "  ";
      out << std::setw(16) << std::setfill('0') << std::hex << (s.vma + i)
          << std::dec << std::setfill(' ');
      if (flags.color) out << "\033[0m  \033[33m";
      else out << "  ";
      for (std::size_t j = 0; j < n; ++j) out << hex((*bytes)[i+j], 2) << ' ';
      if (flags.color) out << "\033[0m";
      if (flags.color) out << "  " << tuiColor(flags, "mnemonic") << text << "\033[0m";
      else out << "  " << text;
      if (flags.semantics && op && !op->semantics.empty())
        out << (flags.color ? "  " + tuiColor(flags, "semantic") + "; " : "  ; ")
            << unquote(op->semantics) << (flags.color ? "\033[0m" : "");
      out << '\n'; i += n;
    }
  }
}

void dumpRaw(std::ostream& out, const std::string& label,
             const std::vector<uint8_t>& bytes, int radix, bool showText) {
  out << "Contents of " << label << ":\n";
  for (std::size_t i = 0; i < bytes.size(); i += 16) {
    std::size_t n = std::min<std::size_t>(16, bytes.size() - i);
    out << ' ' << std::setw(8) << std::setfill('0') << std::hex << i
        << std::dec << std::setfill(' ') << ' ';
    for (std::size_t j = 0; j < 16; ++j) {
      if (j >= n) out << "  ";
      else if (radix == 8) out << std::oct << std::setw(3) << std::setfill('0') << unsigned(bytes[i+j]) << std::dec << std::setfill(' ');
      else out << hex(bytes[i+j], 2);
      out << ' ';
    }
    if (showText) out << " |" << qobj::printableText(std::span<const uint8_t>(bytes.data()+i, n)) << '|';
    out << '\n';
  }
}

void dumpContents(std::ostream& out, const ObjectFile& file, const Section& s,
                  std::optional<uint64_t> start, std::optional<uint64_t> stop,
                  bool showText = true, int radix = 16) {
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
    uint64_t sectionEnd = s.vma;
    if (s.size > UINT64_MAX - sectionEnd) sectionEnd = UINT64_MAX;
    else sectionEnd += s.size;
    uint64_t lo = std::max(low, s.vma), hi = std::min(high, sectionEnd);
    if (hi <= lo) return;
    uint64_t delta = lo - s.vma;
    uint64_t upper = std::min<uint64_t>(hi - s.vma, bytes->size());
    if (delta >= bytes->size()) return;
    begin = size_t(delta);
    end = size_t(upper);
  }
  out << "Contents of section " << s.name << ":\n";
  for (size_t i = begin; i < end; i += 16) {
    size_t n = std::min<size_t>(16, end - i);
    out << ' ' << std::setw(8) << std::setfill('0') << std::hex
        << (s.fileOffset + i) << std::dec << std::setfill(' ') << ' ';
    for (size_t k = 0; k < 16; ++k) {
      if (k >= n) { out << "  "; }
      else if (radix == 8) { out << std::oct << std::setw(3) << std::setfill('0') << unsigned((*bytes)[i+k]) << std::dec << std::setfill(' '); }
      else { out << hex((*bytes)[i + k], 2); }
      out << ' ';
    }
    if (showText) out << " |" << qobj::printableText(bytes->subspan(i, n)) << "|";
    out << '\n';
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
  if (e) {
    for (const auto& h : e->rawSections())
      out << "  ELF name=0x" << hex(h.name) << " type=" << h.type
          << " flags=0x" << hex(h.flags) << " addr=0x" << hex(h.addr)
          << " off=0x" << hex(h.offset) << " size=" << h.size << " link=" << h.link
          << " info=" << h.info << " align=" << h.align
          << " entsize=" << h.entsize << '\n';
    return;
  }
  if (const auto* p = dynamic_cast<const pe::PEFile*>(&file)) {
    out << "  PE machine=0x" << hex(p->machine())
        << " sections=" << p->rawSections().size()
        << " image-base=0x" << hex(p->imageBase())
        << " image-size=0x" << hex(p->sizeOfImage()) << '\n';
    for (size_t i = 0; i < p->rawSections().size(); ++i) {
      const auto& h = p->rawSections()[i];
      out << "  PE[" << i + 1 << "] va=0x" << hex(h.virtualAddress)
          << " vsize=0x" << hex(h.virtualSize)
          << " raw=0x" << hex(h.rawPtr)
          << " rawsize=0x" << hex(h.rawSize) << '\n';
    }
    return;
  }
  out << "  (no raw private-header view for this format)\n";
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
    qisa::Database db; std::vector<std::string> errors; db.loadDirectory(flags.isaDir, errors);
    if (const auto* isa = db.find(isaKey(file.arch()))) {
      auto show = [&](const char* key) { auto it = isa->profile.find(key); if (it != isa->profile.end()) out << "  " << key << " = " << unquote(it->second) << '\n'; };
      show("family"); show("model"); show("version"); show("word_size");
      show("instruction_encoding"); show("instruction_count");
    }
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
    dumpContents(out, file, *s, flags.startAddress, flags.stopAddress, flags.text, flags.octDump ? 8 : 16);
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
      dumpContents(out, file, s, flags.startAddress, flags.stopAddress, flags.text, flags.octDump ? 8 : 16);
  }
  if (flags.disassemble) showDisassembly(out, file, flags);
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
      {'d', "disassemble", false, "", "disassemble code sections using ISA descriptions"},
      {0, "hex-dump", false, "", "dump bytes in hexadecimal"},
      {0, "oct-dump", false, "", "dump bytes in octal"},
      {0, "no-text", false, "", "suppress printable text beside dumps"},
      {0, "isa-dir", true, "dir", "ISA description directory"},
      {0, "search", true, "pattern", "show disassembly lines containing a pattern"},
      {0, "tui", false, "", "highlight disassembly for terminal viewing"},
      {0, "semantics", false, "", "show ISA semantic expressions"},
      {0, "register-names", false, "", "render decoded register names"},
      {0, "no-color", false, "", "disable ANSI highlighting"},
      {0, "no-pager", false, "", "disable the TUI pager"},
      {0, "theme", true, "name", "select a TUI color theme"},
      {0, "pager-command", true, "command", "pager command used by --tui"},
      {0, "raw", false, "", "treat input as raw bytes when object parsing fails"},
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
  Config config = loadConfig();
  flags.fileHeaders = options.has('f');
  flags.sectionHeaders = options.has('h');
  flags.programHeaders = options.has('p');
  flags.symbols = options.has('t');
  flags.dynamicSymbols = options.has('T');
  flags.relocations = options.has('r');
  flags.fullRelocations = options.has("full-reloc");
  flags.contents = options.has('s');
  flags.disassemble = options.has('d');
  flags.hexDump = options.has("hex-dump");
  flags.octDump = options.has("oct-dump");
  flags.text = !options.has("no-text");
  flags.isaDir = options.value("isa-dir").empty() ? config.isaDir : options.value("isa-dir");
  flags.search = options.value("search");
  flags.tui = options.has("tui");
  flags.semantics = options.has("semantics") ? true : config.semantics;
  flags.registerNames = options.has("register-names") ? true : config.registerNames;
  flags.color = options.has("no-color") ? false : config.color;
  flags.pager = options.has("no-pager") ? false : config.pager;
  flags.theme = options.value("theme").empty() ? config.theme : options.value("theme");
  flags.pagerCommand = options.value("pager-command").empty() ? config.pagerCommand : options.value("pager-command");
  flags.raw = options.has("raw");
  if (flags.hexDump || flags.octDump) flags.contents = true;
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
  auto run = [&]() -> int {
    if (flags.archiveIndex) {
      int status = 0;
      for (const auto& path : options.positional()) status |= showArchiveIndex(path);
      return status;
    }
    if (flags.raw) {
      int status = 0;
      for (const auto& path : options.positional()) {
        try { dumpRaw(std::cout, path, qobj::readFile(path), flags.octDump ? 8 : 16, flags.text); }
        catch (const std::exception& e) { status |= qobj::reportError(path, e); }
      }
      return status;
    }
    if (flags.target.empty())
      return qobj::visit(options.positional(),
                         [&](const qobj::Source& s) { return dump(s, flags); });
    int status = 0;
    for (const auto& path : options.positional()) {
      try {
        auto bytes = qobj::readFile(path);
        auto opened = qbfd::openAs(bytes, flags.target, path);
        if (!opened) { status |= qobj::reportError(path, opened.error()); continue; }
        qobj::Source s; s.label = path; s.object = std::move(opened->object);
        status |= dump(s, flags);
      } catch (const std::exception& e) { status |= qobj::reportError(path, e); }
    }
    return status;
  };
  if (!flags.tui) return run();
  std::ostringstream capture;
  auto* old = std::cout.rdbuf(capture.rdbuf());
  int status = run();
  std::cout.rdbuf(old);
  emitOutput(capture.str(), flags);
  return status;
}
