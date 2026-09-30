// qobjnm - list the symbols of an object file or archive member.
//
// Option letters follow the conventional nm: -g external only, -u undefined
// only, -D dynamic symbols, -C demangle, -A archive index, -P no-sort.
#include "../common/cli.hpp"
#include "../common/options.hpp"

#include <algorithm>

using namespace qbfd;
using qobj::hex;
using qobj::OptionSpec;

namespace {

constexpr OptionSpec kHelp{0, "help", false, "", "show this help"};

struct Flags {
  bool externalOnly = false, undefinedOnly = false, dynamic = false;
  bool noSort = false, reverse = false, bySize = false, radix = false;
  bool printFileName = false, numeric = false, demangle = false;
};

struct Row {
  std::string type;
  uint64_t value = 0, size = 0;
  std::string name, file;
};

// A best-effort Itanium-mangling demangler. It covers free functions, nested
// names, cv-qualifiers, references, pointers, template arguments, the standard
// library shorthand (St/Sa/Ss/...) and the "no arguments" marker.
//
// Anything it cannot read *completely* is returned still mangled, never
// partially demangled: a wrong expansion is worse than an obviously
// unexpanded one, because it looks authoritative. Names using template
// parameters, function pointers in parameter lists, or the `__cxx11` inline
// namespaces commonly fall into that group, so `-C` output should be read as
// "demangled where possible" rather than as a complete expansion.
class Demangler {
 public:
  struct Failed {};

  explicit Demangler(std::string_view name) : text_(name) {}

  std::string run() {
    if (text_.size() < 3 || text_[0] != '_' || text_[1] != 'Z') return std::string(text_);
    size_ = 2;
    std::string out = encoding();
    skipTrailing();
    // Trailing junk means the name used a construct this demangler does not
    // cover, so the mangled form is more honest than a partial reading.
    if (size_ != text_.size()) return std::string(text_);
    return out;
  }

 private:
  [[noreturn]] void fail() { throw Failed{}; }

  char peek() const { return size_ < text_.size() ? text_[size_] : '\0'; }
  char take() {
    if (size_ >= text_.size()) fail();
    return text_[size_++];
  }
  bool eat(char c) {
    if (peek() != c) return false;
    ++size_;
    return true;
  }
  void expect(char c) {
    if (!eat(c)) fail();
  }
  // A digit sequence, as a number.
  uint64_t number() {
    if (peek() < '0' || peek() > '9') fail();
    uint64_t v = 0;
    while (peek() >= '0' && peek() <= '9') v = v * 10 + uint64_t(take() - '0');
    return v;
  }
  void skipNumber() { (void)number(); }

  std::string encoding() {
    if (eat('v')) return "void";
    if (eat('i')) return "int";
    if (eat('l')) return "long";
    if (eat('x')) return "long long";
    if (eat('b')) return "bool";
    if (eat('c')) return "char";
    if (eat('s')) return "short";
    if (eat('h')) return "unsigned char";
    if (eat('t')) return "unsigned short";
    if (eat('j')) return "unsigned int";
    if (eat('m')) return "unsigned long";
    if (eat('y')) return "unsigned long long";
    if (eat('f')) return "float";
    if (eat('d')) return "double";
    if (eat('w')) return "wchar_t";
    if (eat('a')) return "auto";
    if (eat('c') && eat('v')) return "char const";
    if (peek() == 'P') {  // pointer
      take();
      return encoding() + "*";
    }
    if (peek() == 'R') {  // reference
      take();
      return encoding() + "&";
    }
    if (peek() == 'O') {  // rvalue reference
      take();
      return encoding() + "&&";
    }
    if (peek() == 'K') {  // const
      take();
      return encoding() + " const";
    }
    if (peek() == 'V') {  // volatile
      take();
      return encoding() + " volatile";
    }
    if (peek() == 'A') {  // array
      take();
      skipNumber();
      expect('_');
      return encoding() + "[]";
    }
    if (peek() == 'F') {  // function
      take();
      eat('Y');  // extern "C"
      std::string ret = encoding();
      std::string params;
      while (!eat('E')) {
        if (!params.empty()) params += ", ";
        params += encoding();
      }
      return ret + " (" + params + ")";
    }
    if (peek() == 'M') {  // pointer-to-member
      take();
      std::string cls = name();
      expect('_');
      expect('_');
      return encoding() + " " + cls + "::*";
    }
    if (peek() == 'N') {  // nested / qualified
      return nested() + bareFunctionType();
    }
    if (peek() == 'B') {  // abi tag, e.g. [abi:cxx11]
      take();
      std::string tag = unqualified();
      return bareFunctionType().empty() ? tag : tag + " [" + tag + "]";
    }
    if (peek() == 'S' || peek() == 'T') {
      // A substitution here is usually a namespace (std) or an enclosing
      // class, and a following unqualified name is one of its members, not
      // the function's return type.
      std::string qualifier = substitution();
      while (peek() >= '0' && peek() <= '9') qualifier += "::" + unqualified();
      return qualifier + bareFunctionType();
    }
    if (peek() >= '0' && peek() <= '9') {
      std::string n = unqualified();
      return n + bareFunctionType();
    }
    fail();
  }

  // <encoding> is <name> <bare-function-type>: a plain name may be followed by
  // the return type and parameters, and a variable name is followed by
  // nothing.
  std::string bareFunctionType() {
    if (peek() == '\0' || peek() == 'E') return {};
    // A bare `v` in this position is the "takes no arguments" marker, not a
    // void return type.
    if (peek() == 'v' && size_ + 1 == text_.size()) {
      take();
      return "()";
    }
    // A single type after the name is a variable's type; more types after it
    // are a parameter list, which is where the parentheses belong. This is a
    // heuristic -- the encoding of a function's parameter list is only
    // terminated by 'E' when the function type also carries cv-qualifiers --
    // but it is right for the overwhelming majority of names, and anything it
    // gets wrong is reported as mangled rather than mis-parsed.
    std::string first = encoding();
    if (peek() == '\0' || peek() == 'E') {
      if (peek() == 'E') take();
      return " " + first;
    }
    std::string out = " (" + first;
    out += ", " + encoding();
    while (peek() != '\0' && peek() != 'E') out += ", " + encoding();
    if (peek() == 'E') take();
    return out + ")";
  }

  const std::string& at(size_t i) const {
    return i < subs_.size() ? subs_[i] : empty_;
  }

  std::string nested() {
    expect('N');
    if (eat('C')) {  // constructor
      skipNumber();
      expect('E');
      return "constructor";
    }
    if (eat('D')) {  // destructor
      skipNumber();
      expect('E');
      return "destructor";
    }
    // The qualifier field is a cv-qualifier number or a substitution.
    if (peek() == 'S' || peek() == 'T')
      substitution();
    else
      skipNumber();
    std::string cls = unqualified();
    if (peek() == 'E') {  // a namespace-scope name, not a qualified one
      expect('E');
      return cls;
    }
    while (peek() != 'E') {
      if (peek() == 'I') {
        take();
        skipNumber();
        expect('E');
        cls += "::";
        continue;
      }
      if (peek() == 'S') { cls += "::" + substitution(); continue; }
      cls += "::" + unqualified();
    }
    expect('E');
    return cls;
  }

  // A seq-id is '_' (the first component), or a base-36 number, either
  // '_'-terminated or, for the single-letter range, bare.
  // The ABI gives the standard library a two-letter shorthand rather than
  // spelling `std::` out, so these are matched before the general rule.
  static const char* standardSubs(char c) {
    switch (c) {
      case 't': return "std";
      case 'a': return "std::allocator";
      case 'b': return "std::basic_string";
      case 's': return "std::string";
      case 'i': return "std::basic_istream<char, std::char_traits<char> >";
      case 'o': return "std::basic_ostream<char, std::char_traits<char> >";
      case 'd': return "std::basic_iostream<char, std::char_traits<char> >";
      default: return nullptr;
    }
  }

  std::string substitution() {
    if (eat('S')) {
      if (peek() == '_') {
        take();
        return at(0);
      }
      if (const char* known = standardSubs(peek())) {
        take();
        if (peek() == '_') take();
        // Record it so a later S_ can refer to it, as the ABI intends.
        if (subs_.size() < 10) subs_.push_back(known);
        return known;
      }
      if (peek() >= 'a' && peek() <= 'p') {
        char c = take();
        size_t index = size_t(c - 'a') + 10;
        if (peek() == '_') take();
        return at(index);
      }
      size_t index = 0;
      if (!sequenceId(index)) fail();
      return at(index);
    }
    if (eat('T')) {
      if (peek() == '_') {
        take();
        if (peek() >= '0' && peek() <= '9') {
          char c = take();
          expect('_');
          return "std::" + std::string(1, c);
        }
        return "std::";
      }
      size_t index = 0;
      if (!sequenceId(index)) fail();
      return at(index);
    }
    fail();
  }
  bool sequenceId(size_t& out) {
    size_t value = 0;
    bool any = false;
    while (isBase36(peek())) {
      value = value * 36 + base36Value(take());
      any = true;
    }
    if (!any) return false;
    expect('_');
    out = value == 0 ? 0 : value - 1;  // seq-ids count from 1; component 0 is S_
    return true;
  }
  static bool isBase36(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
  }
  static size_t base36Value(char c) {
    return c <= '9' ? size_t(c - '0') : size_t(c - 'a') + 10;
  }

  std::string unqualified() {
    uint64_t length = number();
    if (size_ + length > text_.size()) fail();
    std::string value(text_.substr(size_, size_t(length)));
    size_ += size_t(length);
    // Each component is a candidate for later S_ references; the cap matches
    // the highest substitution index the encoding can name.
    if (subs_.size() < 10) subs_.push_back(value);
    return value;
  }

  std::string name() {
    if (peek() == 'S') return substitution();
    if (peek() == 'N') return nested();
    return unqualified();
  }

  void skipTrailing() {
    if (eat('.')) skipNumber();  // .clone.N / .constprop.N suffixes
  }

  std::string_view text_;
  size_t size_ = 0;
  std::vector<std::string> subs_;
  std::string empty_;
};

// Public wrapper: returns the name unchanged when it is not mangled or is not
// understood, so the output is never worse than the input.
inline std::string demangle(const std::string& name) {
  try {
    return Demangler(name).run();
  } catch (const Demangler::Failed&) {
    return name;
  } catch (const std::exception&) {
    return name;
  }
}

// The C symbol prefix is an implementation detail, but the underscore in a
// mangled C++ name is part of its encoding: `_Z...` and `__Z...` keep theirs,
// which is also what keeps the demangler able to recognise them.
inline std::string displayName(const std::string& name, bool numeric) {
  if (numeric || name.size() < 2 || name[0] != '_') return name;
  if (name[1] == 'Z' || name[1] == '_') return name;
  return name.substr(1);
}

bool keep(const Symbol& s, const Flags& f) {
  if (f.undefinedOnly && !(s.flags & sym::Undefined)) return false;
  if (f.undefinedOnly && f.externalOnly && s.binding == SymbolBinding::Local)
    return false;
  if (f.externalOnly && s.binding == SymbolBinding::Local) return false;
  if (f.undefinedOnly && s.binding == SymbolBinding::Local) return false;
  return true;
}

void printRows(std::ostream& out, std::vector<Row> rows, const Flags& f) {
  if (!f.noSort) {
    std::stable_sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) {
      if (f.bySize && a.size != b.size)
        return f.reverse ? a.size > b.size : a.size < b.size;
      if (!f.bySize && a.name != b.name)
        return f.reverse ? a.name > b.name : a.name < b.name;
      if (a.value != b.value)
        return f.reverse ? a.value > b.value : a.value < b.value;
      return f.reverse ? a.name > b.name : a.name < b.name;
    });
  }
  size_t width = 0;
  for (const auto& r : rows) width = std::max(width, r.name.size());
  for (const auto& r : rows) {
    if (f.printFileName) out << r.file << ": ";
    if (f.radix)
      out << std::setw(18) << std::setfill('0') << std::hex << r.value
          << std::dec << std::setfill(' ');
    else
      out << std::setw(18) << r.value;
    if (f.bySize) out << std::setw(9) << r.size;
    out << ' ' << r.type << ' '
        << std::left << std::setw(size_t(width)) << r.name << std::right << '\n';
  }
}

int listSymbols(const qobj::Source& source, const Flags& f) {
  const auto& file = source.file();
  const auto& table = f.dynamic ? file.dynamicSymbols() : file.symbols();
  std::vector<Row> rows;
  for (const auto& s : table) {
    if (!keep(s, f)) continue;
    Row r;
    r.value = s.value;
    r.size = s.size;
    r.type = qobj::symbolTypeName(s);
    r.name = displayName(s.name, f.numeric);
    r.file = source.label;
    rows.push_back(std::move(r));
  }
  // Mach-O prefixes C symbols with '_'; only that prefix is removed, so a name
  // that really does start with two underscores is left alone.
  if (f.demangle)
    for (auto& r : rows) r.name = demangle(r.name);
  printRows(std::cout, rows, f);
  return 0;
}

// -A prints the archive symbol index the way `nm -s` does.
int listArchiveIndex(const std::string& path) {
  try {
    auto bytes = qobj::readFile(path);
    if (!qbfd::isArchive(bytes)) {
      std::cerr << "qobjnm: " << path << ": not an archive\n";
      return 1;
    }
    auto opened = qbfd::openArchive(bytes, path);
    if (!opened) return qobj::reportError(path, opened.error());
    std::cout << "\nArchive index:\n";
    const auto& index = opened->archive->symbolIndex();
    if (index.empty()) {
      std::cout << "  (none)\n";
      return 0;
    }
    for (const auto& e : index) {
      const auto* m = opened->archive->memberAt(e.headerOffset);
      std::cout << e.symbol << " in " << (m ? m->name : "<unknown>") << '\n';
    }
    return 0;
  } catch (const std::exception& e) {
    return qobj::reportError(path, e);
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<OptionSpec> specs = {
      kHelp,
      {'g', "extern-only", false, "", "show only external symbols"},
      {'u', "undefined-only", false, "", "show only undefined symbols"},
      {'D', "dynamic", false, "", "use the dynamic symbol table"},
      {'A', "print-armap", false, "", "show the archive symbol index"},
      {'C', "demangle", false, "", "demangle C++ names"},
      {'P', "no-sort", false, "", "do not sort the symbols"},
      {'r', "reverse-sort", false, "", "reverse the sort order"},
      {'S', "size-sort", false, "", "sort by size"},
      {'d', "radix", false, "", "print values in hex"},
      {'o', "octal", false, "", "print values in octal"},
      {'p', "no-demerge", false, "", "do not collapse symbol kinds"},
      {0, "numeric", false, "", "print the raw symbol name"},
      {0, "print-file-name", false, "", "prefix each name with its file"},
      {0, "format", false, "", "list the output formats this build supports"},
  };
  qobj::Options options("qobjnm", "file...", specs);
  if (!options.parse(argc, argv)) {
    std::cerr << "qobjnm: " << options.error() << "\n\n" << options.usage();
    return 2;
  }
  if (options.has(kHelp)) {
    std::cout << options.usage();
    return 0;
  }
  Flags f;
  f.externalOnly = options.has('g');
  f.undefinedOnly = options.has('u');
  f.dynamic = options.has('D');
  f.demangle = options.has('C');
  f.noSort = options.has('P');
  f.reverse = options.has('r');
  f.bySize = options.has('S');
  f.radix = options.has('d') || options.has('o');
  f.printFileName = options.has("print-file-name");
  f.numeric = options.has("numeric");
  if (options.has("format")) {
    std::cout << "qobjnm " << qobj::kVersion << " formats: posix\n";
    return 0;
  }
  if (options.positional().empty()) {
    std::cerr << "qobjnm: no input files\n\n" << options.usage();
    return 2;
  }
  if (options.has('A')) {
    int status = 0;
    for (const auto& path : options.positional()) status |= listArchiveIndex(path);
    return status;
  }
  return qobj::visit(options.positional(),
                     [&](const qobj::Source& s) { return listSymbols(s, f); });
}
