// parser.hpp - linker-script lexer and parser.
//
// Implements the GNU ld script language subset used by qobjld:
//
//   ENTRY(sym)  OUTPUT_FORMAT(..)  OUTPUT_ARCH(..)  SEARCH_DIR(..)
//   GROUP(..) / INPUT(..)  MEMORY { .. }  PHDRS { .. }  SECTIONS { .. }
//   VERSION { .. }  INSERT ...
//
// SECTIONS bodies support output-section descriptors, symbol assignments,
// `. = expr`, PROVIDE/HIDDEN, ALIGN, ADDR/SIZEOF/LOADADDR, DEFINED, CONSTANT,
// ORIGIN/LENGTH, SEGMENT_START, ABSOLUTE, MAX/MIN, DATA_SEGMENT_* and the
// usual C-like expression operators with K/M/G suffixes.
//
// The parser produces a small AST (ScriptFile) consumed by ldscript.cpp.
// It is a hand-rolled recursive-descent parser over qBFD-independent text;
// it does not depend on fd/ headers.
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace qld::script {

// ---------------------------------------------------------------------------
// Tokens
// ---------------------------------------------------------------------------
enum class TokKind {
  Eof,
  Ident,
  Number,
  String,
  LParen,
  RParen,
  LBrace,
  RBrace,
  Colon,
  Semicolon,
  Comma,
  Assign,       // =
  PlusAssign,   // +=
  MinusAssign,  // -=
  MulAssign,    // *=
  DivAssign,    // /=
  AndAssign,    // &=
  OrAssign,     // |=
  Plus,
  Minus,
  Star,
  Slash,
  Percent,
  Bang,
  Tilde,
  Lt,
  Gt,
  Le,
  Ge,
  Eq,
  Ne,
  And,          // &
  Or,           // |
  AndAnd,       // &&
  OrOr,         // ||
  Caret,        // ^
  Shl,          // <<
  Shr,          // >>
  Question,
  Dot,
  Greater,      // > (region assignment, same spelling as Gt; context decides)
};

struct Token {
  TokKind kind = TokKind::Eof;
  std::string text;
  uint64_t number = 0;
  size_t line = 1;
  size_t col = 1;
};

std::string tokName(TokKind k);

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
class Lexer {
 public:
  explicit Lexer(std::string text);

  Token next();
  Token peek();
  bool eof() const;

 private:
  Token readNext();
  char cur() const;
  char peekChar(size_t ahead = 1) const;
  void advance(size_t n = 1);
  void skipSpaceAndComments();
  Token makeNumber(size_t line, size_t col);
  Token makeIdentOrKeyword(size_t line, size_t col);
  Token makeString(size_t line, size_t col);

  std::string text_;
  size_t pos_ = 0;
  size_t line_ = 1;
  size_t col_ = 1;
  std::optional<Token> pushed_;
};

// ---------------------------------------------------------------------------
// Expression AST
// ---------------------------------------------------------------------------
struct Expr {
  virtual ~Expr() = default;
  virtual std::string dump() const = 0;
};

struct NumberExpr : Expr {
  uint64_t value = 0;
  explicit NumberExpr(uint64_t v) : value(v) {}
  std::string dump() const override;
};

struct SymbolExpr : Expr {
  std::string name;
  explicit SymbolExpr(std::string n) : name(std::move(n)) {}
  std::string dump() const override;
};

struct DotExpr : Expr {
  std::string dump() const override;
};

struct UnaryExpr : Expr {
  std::string op;
  std::unique_ptr<Expr> arg;
  UnaryExpr(std::string o, std::unique_ptr<Expr> a)
      : op(std::move(o)), arg(std::move(a)) {}
  std::string dump() const override;
};

struct BinaryExpr : Expr {
  std::string op;
  std::unique_ptr<Expr> lhs, rhs;
  BinaryExpr(std::string o, std::unique_ptr<Expr> l, std::unique_ptr<Expr> r)
      : op(std::move(o)), lhs(std::move(l)), rhs(std::move(r)) {}
  std::string dump() const override;
};

struct TernaryExpr : Expr {
  std::unique_ptr<Expr> cond, yes, no;
  TernaryExpr(std::unique_ptr<Expr> c, std::unique_ptr<Expr> y,
              std::unique_ptr<Expr> n)
      : cond(std::move(c)), yes(std::move(y)), no(std::move(n)) {}
  std::string dump() const override;
};

struct CallExpr : Expr {
  std::string name;
  std::vector<std::unique_ptr<Expr>> args;
  CallExpr(std::string n, std::vector<std::unique_ptr<Expr>> a)
      : name(std::move(n)), args(std::move(a)) {}
  std::string dump() const override;
};

using ExprPtr = std::unique_ptr<Expr>;

// ---------------------------------------------------------------------------
// Script AST
// ---------------------------------------------------------------------------
struct MemoryRegion {
  std::string name;
  std::string attrs;
  ExprPtr origin;
  ExprPtr length;
  std::string originText;
  std::string lengthText;
};

struct SectionPattern {
  std::string file;     // empty means any file ("*")
  std::string section;  // e.g. ".text", ".text.*"
  bool excludeFile = false;
  bool keep = false;
  std::string sort;     // "", "SORT", "SORT_BY_NAME", ...
};

struct OutputSection {
  std::string name;                 // ".text" or "/DISCARD/"
  ExprPtr address;                  // optional VMA expression
  std::string type;                 // e.g. "NOLOAD", "" for default
  std::vector<SectionPattern> inputs;
  std::vector<std::string> rawLines;  // kept for diagnostics
  std::string region;               // ">region"
  std::string lmaRegion;            // "AT>region"
  ExprPtr lma;                      // "AT(expr)"
  bool hasAlign = false;
  ExprPtr alignExpr;
};

struct Stmt {
  virtual ~Stmt() = default;
};

struct AssignStmt : Stmt {
  std::string name;     // symbol name or "."
  ExprPtr value;
  std::string op = "=";  // =, +=, -=, *=, /=, &=, |=
  bool provide = false;  // PROVIDE(...)
  bool hidden = false;   // HIDDEN(...) / PROVIDE_HIDDEN
};

struct OutputSectionStmt : Stmt {
  OutputSection section;
};

struct AssertStmt : Stmt {
  ExprPtr cond;
  std::string message;
};

struct ScriptFile {
  std::optional<std::string> entry;
  std::optional<std::string> outputFormat;
  std::optional<std::string> outputArch;
  std::optional<std::string> outputName;
  std::vector<std::string> searchDirs;
  std::vector<std::string> group;   // GROUP/INPUT file lists
  std::vector<MemoryRegion> memory;
  std::vector<std::unique_ptr<Stmt>> sectionsBody;  // inside SECTIONS {}
  bool hasSections = false;
  std::string dump() const;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
 public:
  explicit Parser(std::string text);

  // Parses the whole file. On failure returns false and fills *error.
  bool parse(ScriptFile& out, std::string& error);

 private:
  Lexer lex_;
  Token cur_;

  void advance();
  bool at(TokKind k) const;
  bool accept(TokKind k);
  bool expect(TokKind k, std::string& error, const char* what);
  void fail(const std::string& msg);
  std::string error_;

  bool parseTopLevel(ScriptFile& out);
  bool parseMemory(ScriptFile& out);
  bool parseSections(ScriptFile& out);
  bool parsePhdrs(ScriptFile& out);
  bool parseVersion(ScriptFile& out);
  bool parseEntry(ScriptFile& out);
  bool parseOutputFormat(ScriptFile& out);
  bool parseOutputArch(ScriptFile& out);
  bool parseOutput(ScriptFile& out);
  bool parseSearchDir(ScriptFile& out);
  bool parseGroup(ScriptFile& out, bool isGroup);
  bool parseInsert(ScriptFile& out);
  // Inside SECTIONS { ... }
  std::unique_ptr<Stmt> parseSectionsStmt();
  std::unique_ptr<Stmt> parseAssign(const std::string& op);
  bool parseOutputSection(OutputSection& section);
  bool parseInputPatterns(OutputSection& section);
  // Expressions (C precedence, ?: lowest).
  ExprPtr parseExpr();
  ExprPtr parseTernary();
  ExprPtr parseOrOr();
  ExprPtr parseAndAnd();
  ExprPtr parseOr();
  ExprPtr parseXor();
  ExprPtr parseAnd();
  ExprPtr parseEquality();
  ExprPtr parseRelational();
  ExprPtr parseShift();
  ExprPtr parseAdd();
  ExprPtr parseMul();
  ExprPtr parseUnary();
  ExprPtr parsePrimary();
  std::string parseStringLiteral();
};

// Convenience: parse text, returning true on success.
bool parseText(const std::string& text, ScriptFile& out, std::string& error);

// Number with K/M/G (and k/m/g) suffixes, 0x hex, octal.
bool parseScriptNumber(const std::string& text, uint64_t& value);

}  // namespace qld::script
