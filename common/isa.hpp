// ISA description loading for qobjfile tools.
#pragma once

#include "qdsl.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qisa {

struct Field {
  std::string name;
  std::string value;
};

struct Register {
  std::string name;
  unsigned width = 0;
  unsigned number = 0;
};

struct RegisterClass {
  std::string name;
  std::vector<Register> registers;
};

struct Encoding {
  std::string name;
  std::map<std::string, std::string> fields;
};

struct Operation {
  std::string name;
  std::map<std::string, std::string> fields;
  std::string syntax;
  std::string semantics;
};

struct Document {
  std::string source;
  std::string arch;
  std::map<std::string, std::string> archFields;
  std::map<std::string, std::string> profile;
  std::vector<RegisterClass> registerClasses;
  std::vector<Encoding> encodings;
  std::vector<Operation> operations;
  std::vector<std::string> aliases;
};

struct Error { std::size_t offset = 0; std::string message; };

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}
  std::optional<Document> parse(Error& error, std::string source = {}) const;

 private:
  std::string_view text_;
};

class Database {
 public:
  bool loadFile(const std::filesystem::path& path, std::string& error);
  std::size_t loadDirectory(const std::filesystem::path& dir,
                            std::vector<std::string>& errors);
  const Document* find(std::string_view arch) const;
  const std::vector<Document>& documents() const { return docs_; }

 private:
  std::vector<Document> docs_;
};

namespace detail {
struct Cursor {
  dsl::ParsecInput input;
  explicit Cursor(std::string_view s) : input{s, 0} {}
  void ws();
  bool eat(char c);
  bool word(std::string_view w);
  bool ident(std::string& out);
  bool value(std::string& out);
  bool quoted(std::string& out);
  bool number(std::string& out);
  bool field(std::map<std::string, std::string>& out);
  bool skipBalanced(char open, char close = '}');
};
}  // namespace detail

}  // namespace qisa

// Implementation is header-only so dump and embedders can use it without a
// separate library target.
namespace qisa::detail {
inline void Cursor::ws() {
  for (;;) {
    while (std::isspace(static_cast<unsigned char>(input.peek()))) input.consume();
    if (input.peek() == '#') {
      while (!input.eof() && input.consume() != '\n') {}
      continue;
    }
    break;
  }
}
inline bool Cursor::eat(char c) { ws(); if (input.peek() != c) return false; input.consume(); return true; }
inline bool Cursor::word(std::string_view w) {
  ws(); auto save = input.pos;
  for (char c : w) if (input.peek() != c) { input.pos = save; return false; } else input.consume();
  char p = input.peek();
  if (std::isalnum(static_cast<unsigned char>(p)) || p == '_' || p == '.') { input.pos = save; return false; }
  return true;
}
inline bool Cursor::ident(std::string& out) {
  ws();
  // Use qDSL's parser combinator for the lexical identifier token. The AST
  // parser below composes these token recognizers with its block rules.
  auto token = dsl::parser([](dsl::ParsecInput& in) -> dsl::ExpectedResult<std::string> {
    if (!(std::isalpha(static_cast<unsigned char>(in.peek())) || in.peek() == '_'))
      return dsl::fail_expected<std::string>(in, "identifier");
    std::size_t b = in.pos++;
    while (std::isalnum(static_cast<unsigned char>(in.peek())) || in.peek() == '_' || in.peek() == '.' || in.peek() == '-') in.consume();
    return std::string(in.source.substr(b, in.pos - b));
  });
  auto r = token(input); if (!r) return false; out = *r; return true;
}
inline bool Cursor::quoted(std::string& out) {
  ws(); if (input.peek() != '"') return false; input.consume(); out.clear();
  while (!input.eof() && input.peek() != '"') { if (input.peek() == '\\') input.consume(); out += input.consume(); }
  return eat('"');
}
inline bool Cursor::number(std::string& out) {
  ws(); std::size_t b = input.pos; if (input.peek() == '-') input.consume();
  bool any = false; while (std::isxdigit(static_cast<unsigned char>(input.peek())) || input.peek() == 'x' || input.peek() == 'X') { any = true; input.consume(); }
  if (!any) { input.pos = b; return false; } out = std::string(input.source.substr(b, input.pos - b)); return true;
}
inline bool Cursor::value(std::string& out) {
  ws();
  std::size_t b = input.pos;
  int square = 0, curly = 0;
  bool quotedValue = false, escaped = false;
  while (!input.eof()) {
    char c = input.peek();
    if (quotedValue) {
      input.consume();
      if (escaped) escaped = false;
      else if (c == '\\') escaped = true;
      else if (c == '"') quotedValue = false;
      continue;
    }
    if (c == '"') { quotedValue = true; input.consume(); continue; }
    if (c == '[') ++square;
    else if (c == ']') --square;
    else if (c == '{') ++curly;
    else if (c == '}') { if (curly == 0 && square == 0) break; --curly; }
    if (c == ';' && square == 0 && curly == 0) break;
    input.consume();
  }
  std::size_t e = input.pos;
  while (e > b && std::isspace(static_cast<unsigned char>(input.source[e-1]))) --e;
  if (e == b) { input.pos = b; return false; }
  out = std::string(input.source.substr(b, e - b));
  return true;
}
inline bool Cursor::field(std::map<std::string, std::string>& out) {
  std::string k, v; if (!ident(k) || !eat('=') || !value(v) || !eat(';')) return false; out[k] = std::move(v); return true;
}
inline bool Cursor::skipBalanced(char open, char close) {
  if (!eat(open)) return false;
  int d = 1;
  while (!input.eof() && d) {
    char c = input.consume();
    if (c == open) ++d;
    else if (c == close) --d;
  }
  return d == 0;
}
}  // namespace qisa::detail

namespace qisa {
inline std::optional<Document> Parser::parse(Error& error, std::string source) const {
  detail::Cursor c(text_); Document d; d.source = std::move(source);
  while (!c.input.eof()) {
    c.ws(); if (c.input.eof()) break;
    if (c.word("arch")) {
      if (!c.ident(d.arch) || !c.eat('{')) { error = {c.input.pos, "expected arch declaration"}; return std::nullopt; }
      while (!c.eat('}')) { if (!c.field(d.archFields)) { error = {c.input.pos, "invalid arch field"}; return std::nullopt; } }
    } else if (c.word("profile")) {
      if (!c.eat('{')) { error = {c.input.pos, "invalid profile"}; return std::nullopt; }
      while (!c.eat('}')) {
        if (!c.field(d.profile)) { while (!c.input.eof() && c.input.peek() != ';' && c.input.peek() != '}') c.input.consume(); c.eat(';'); }
      }
    } else if (c.word("regclass")) {
      RegisterClass rc; if (!c.ident(rc.name) || !c.eat('{')) { error = {c.input.pos, "invalid register class"}; return std::nullopt; }
      while (!c.eat('}')) { std::string n, w, v; if (!c.ident(n) || !c.eat('(') || !c.number(w) || !c.eat(')') || !c.eat('=') || !c.number(v) || !c.eat(',') ) { error = {c.input.pos, "invalid register entry"}; return std::nullopt; } rc.registers.push_back({n, unsigned(std::stoul(w)), unsigned(std::stoul(v))}); }
      d.registerClasses.push_back(std::move(rc));
    } else if (c.word("encoding")) {
      Encoding e; if (!c.ident(e.name) || !c.eat('{')) { error = {c.input.pos, "invalid encoding"}; return std::nullopt; }
      while (!c.eat('}')) { if (!c.field(e.fields)) { while (!c.input.eof() && c.input.peek() != ';' && c.input.peek() != '}') c.input.consume(); c.eat(';'); } } d.encodings.push_back(std::move(e));
    } else if (c.word("op")) {
      Operation o; if (!c.ident(o.name) || !c.eat('{')) { error = {c.input.pos, "invalid operation"}; return std::nullopt; }
      while (!c.eat('}')) { std::string k, v; if (!c.ident(k) || !c.eat('=') || !c.value(v) || !c.eat(';')) { while (!c.input.eof() && c.input.peek() != ';' && c.input.peek() != '}') c.input.consume(); c.eat(';'); continue; } if (k == "syntax") o.syntax = v; else if (k == "semantics") o.semantics = v; else o.fields[k] = std::move(v); } d.operations.push_back(std::move(o));
    } else if (c.word("alias")) {
      std::string a, b; if (!c.ident(a) || !c.eat('=') || !c.ident(b) || !c.eat(';')) { error = {c.input.pos, "invalid alias"}; return std::nullopt; } d.aliases.push_back(a + "=" + b);
    } else {
      std::string ignored; if (!c.ident(ignored)) { error = {c.input.pos, "unexpected token"}; return std::nullopt; } c.ws(); if (c.input.peek() == '{') c.skipBalanced('{'); else { while (!c.input.eof() && c.input.consume() != ';') {} }
    }
  }
  if (d.arch.empty()) { error = {0, "ISA description has no arch declaration"}; return std::nullopt; }
  return d;
}
inline bool Database::loadFile(const std::filesystem::path& path, std::string& error) {
  std::ifstream in(path); std::string text((std::istreambuf_iterator<char>(in)), {}); if (!in && text.empty()) { error = "cannot read " + path.string(); return false; }
  Error e; auto d = Parser(text).parse(e, path.string()); if (!d) { error = path.string() + ": " + e.message; return false; } docs_.push_back(std::move(*d)); return true;
}
inline std::size_t Database::loadDirectory(const std::filesystem::path& dir, std::vector<std::string>& errors) { std::size_t n = 0; if (!std::filesystem::exists(dir)) { errors.push_back("ISA directory does not exist: " + dir.string()); return 0; } for (const auto& e : std::filesystem::directory_iterator(dir)) if (e.path().extension() == ".isa") { std::string err; if (loadFile(e.path(), err)) ++n; else errors.push_back(std::move(err)); } return n; }
inline const Document* Database::find(std::string_view arch) const { for (const auto& d : docs_) if (d.arch == arch) return &d; return nullptr; }
}  // namespace qisa
