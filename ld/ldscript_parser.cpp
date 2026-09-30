// ldscript.cpp - linker-script lexer, parser and execution.
//
// The lexer/parser implement the subset declared in parser.hpp. Execution
// (runScript) drives address assignment from SECTIONS/MEMORY when a script
// is present, otherwise position_dependenc.cpp uses the default layout.
#include "internal.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>

namespace qld::script {

std::string tokName(TokKind k) {
  switch (k) {
    case TokKind::Eof: return "end of file";
    case TokKind::Ident: return "identifier";
    case TokKind::Number: return "number";
    case TokKind::String: return "string";
    case TokKind::LParen: return "'('";
    case TokKind::RParen: return "')'";
    case TokKind::LBrace: return "'{'";
    case TokKind::RBrace: return "'}'";
    case TokKind::Colon: return "':'";
    case TokKind::Semicolon: return "';'";
    case TokKind::Comma: return "','";
    case TokKind::Assign: return "'='";
    case TokKind::PlusAssign: return "'+='";
    case TokKind::MinusAssign: return "'-='";
    case TokKind::MulAssign: return "'*='";
    case TokKind::DivAssign: return "'/='";
    case TokKind::AndAssign: return "'&='";
    case TokKind::OrAssign: return "'|='";
    case TokKind::Plus: return "'+'";
    case TokKind::Minus: return "'-'";
    case TokKind::Star: return "'*'";
    case TokKind::Slash: return "'/'";
    case TokKind::Percent: return "'%'";
    case TokKind::Bang: return "'!'";
    case TokKind::Tilde: return "'~'";
    case TokKind::Lt: return "'<'";
    case TokKind::Gt: return "'>'";
    case TokKind::Le: return "'<='";
    case TokKind::Ge: return "'>='";
    case TokKind::Eq: return "'=='";
    case TokKind::Ne: return "'!='";
    case TokKind::And: return "'&'";
    case TokKind::Or: return "'|'";
    case TokKind::AndAnd: return "'&&'";
    case TokKind::OrOr: return "'||'";
    case TokKind::Caret: return "'^'";
    case TokKind::Shl: return "'<<'";
    case TokKind::Shr: return "'>>'";
    case TokKind::Question: return "'?'";
    case TokKind::Dot: return "'.'";
    case TokKind::Greater: return "'>'";
  }
  return "?";
}

// --- Lexer ---------------------------------------------------------------

Lexer::Lexer(std::string text) : text_(std::move(text)) {}

char Lexer::cur() const { return pos_ < text_.size() ? text_[pos_] : '\0'; }

char Lexer::peekChar(size_t ahead) const {
  return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
}

void Lexer::advance(size_t n) {
  for (size_t i = 0; i < n && pos_ < text_.size(); ++i) {
    if (text_[pos_] == '\n') {
      ++line_;
      col_ = 1;
    } else {
      ++col_;
    }
    ++pos_;
  }
}

void Lexer::skipSpaceAndComments() {
  for (;;) {
    while (pos_ < text_.size() &&
           (text_[pos_] == ' ' || text_[pos_] == '\t' ||
            text_[pos_] == '\r' || text_[pos_] == '\n' ||
            text_[pos_] == '\f' || text_[pos_] == '\v'))
      advance();
    if (pos_ + 1 < text_.size() && text_[pos_] == '/' &&
        text_[pos_ + 1] == '*') {
      advance(2);
      while (pos_ < text_.size() &&
             !(text_[pos_] == '*' && peekChar() == '/'))
        advance();
      if (pos_ < text_.size()) advance(2);
      continue;
    }
    if (cur() == '#') {
      while (pos_ < text_.size() && text_[pos_] != '\n') advance();
      continue;
    }
    // "//" comments are also accepted.
    if (cur() == '/' && peekChar() == '/') {
      while (pos_ < text_.size() && text_[pos_] != '\n') advance();
      continue;
    }
    break;
  }
}

Token Lexer::makeNumber(size_t line, size_t col) {
  size_t start = pos_;
  if (cur() == '0' && (peekChar() == 'x' || peekChar() == 'X')) {
    advance(2);
    while (std::isxdigit((unsigned char)cur())) advance();
  } else {
    while (std::isalnum((unsigned char)cur()) || cur() == '.' ||
           cur() == 'x' || cur() == 'X')
      advance();
    // Stop at the first char that cannot be part of a number suffix.
    while (pos_ > start + 1 && text_[pos_ - 1] == '.') {
      --pos_;
      --col_;
      break;
    }
  }
  // K/M/G suffix.
  if (cur() == 'K' || cur() == 'M' || cur() == 'G' || cur() == 'k' ||
      cur() == 'm' || cur() == 'g')
    advance();
  std::string t = text_.substr(start, pos_ - start);
  Token tok;
  tok.kind = TokKind::Number;
  tok.text = t;
  tok.line = line;
  tok.col = col;
  uint64_t v = 0;
  if (parseScriptNumber(t, v)) tok.number = v;
  return tok;
}

Token Lexer::makeIdentOrKeyword(size_t line, size_t col) {
  size_t start = pos_;
  while (std::isalnum((unsigned char)cur()) || cur() == '_' || cur() == '.' ||
         cur() == '-' || cur() == '/')
    advance();
  // Identifiers may contain '*' '?' '[' ']' for patterns, but those are
  // lexed separately when they appear inside parens. A bare '*' is an
  // operator; only consume word chars here.
  Token tok;
  tok.kind = TokKind::Ident;
  tok.text = text_.substr(start, pos_ - start);
  tok.line = line;
  tok.col = col;
  return tok;
}

Token Lexer::makeString(size_t line, size_t col) {
  char q = cur();
  advance();
  std::string s;
  while (pos_ < text_.size() && text_[pos_] != q) {
    if (text_[pos_] == '\\' && pos_ + 1 < text_.size()) {
      advance();
      s += text_[pos_];
      advance();
    } else {
      s += text_[pos_];
      advance();
    }
  }
  if (pos_ < text_.size()) advance();  // closing quote
  Token tok;
  tok.kind = TokKind::String;
  tok.text = s;
  tok.line = line;
  tok.col = col;
  return tok;
}

Token Lexer::readNext() {
  skipSpaceAndComments();
  size_t line = line_, col = col_;
  char c = cur();
  if (c == '\0') return Token{TokKind::Eof, {}, 0, line, col};
  if (c == '"' || c == '\'') return makeString(line, col);
  if (std::isdigit((unsigned char)c)) return makeNumber(line, col);
  if (c == '.' && !std::isalnum((unsigned char)peekChar()) &&
      peekChar() != '_' && peekChar() != '.') {
    // A lone '.' is the location counter. '...' or '.text' are idents.
    if (peekChar() == '.' || peekChar() == '*') {
      return makeIdentOrKeyword(line, col);
    }
    advance();
    return Token{TokKind::Dot, ".", 0, line, col};
  }
  if (std::isalpha((unsigned char)c) || c == '_' || c == '.' ||
      (c == '*' && std::isalnum((unsigned char)peekChar())))
    return makeIdentOrKeyword(line, col);
  // Multi-char operators.
  char n = peekChar();
  auto two = [&](char a, char b, TokKind k, const char* t) -> Token {
    if (c == a && n == b) {
      advance(2);
      return Token{k, t, 0, line, col};
    }
    return Token{TokKind::Eof, {}, 0, 0, 0};
  };
  // Order matters: <<= not used; check 2-char first.
  if (c == '=' && n == '=') {
    advance(2);
    return Token{TokKind::Eq, "==", 0, line, col};
  }
  if (c == '!' && n == '=') {
    advance(2);
    return Token{TokKind::Ne, "!=", 0, line, col};
  }
  if (c == '<' && n == '=') {
    advance(2);
    return Token{TokKind::Le, "<=", 0, line, col};
  }
  if (c == '>' && n == '=') {
    advance(2);
    return Token{TokKind::Ge, ">=", 0, line, col};
  }
  if (c == '<' && n == '<') {
    advance(2);
    return Token{TokKind::Shl, "<<", 0, line, col};
  }
  if (c == '>' && n == '>') {
    advance(2);
    return Token{TokKind::Shr, ">>", 0, line, col};
  }
  if (c == '&' && n == '&') {
    advance(2);
    return Token{TokKind::AndAnd, "&&", 0, line, col};
  }
  if (c == '|' && n == '|') {
    advance(2);
    return Token{TokKind::OrOr, "||", 0, line, col};
  }
  if (c == '+' && n == '=') {
    advance(2);
    return Token{TokKind::PlusAssign, "+=", 0, line, col};
  }
  if (c == '-' && n == '=') {
    advance(2);
    return Token{TokKind::MinusAssign, "-=", 0, line, col};
  }
  if (c == '*' && n == '=') {
    advance(2);
    return Token{TokKind::MulAssign, "*=", 0, line, col};
  }
  if (c == '/' && n == '=') {
    advance(2);
    return Token{TokKind::DivAssign, "/=", 0, line, col};
  }
  if (c == '&' && n == '=') {
    advance(2);
    return Token{TokKind::AndAssign, "&=", 0, line, col};
  }
  if (c == '|' && n == '=') {
    advance(2);
    return Token{TokKind::OrAssign, "|=", 0, line, col};
  }
  (void)two;
  advance();
  switch (c) {
    case '(': return Token{TokKind::LParen, "(", 0, line, col};
    case ')': return Token{TokKind::RParen, ")", 0, line, col};
    case '{': return Token{TokKind::LBrace, "{", 0, line, col};
    case '}': return Token{TokKind::RBrace, "}", 0, line, col};
    case ':': return Token{TokKind::Colon, ":", 0, line, col};
    case ';': return Token{TokKind::Semicolon, ";", 0, line, col};
    case ',': return Token{TokKind::Comma, ",", 0, line, col};
    case '=': return Token{TokKind::Assign, "=", 0, line, col};
    case '+': return Token{TokKind::Plus, "+", 0, line, col};
    case '-': return Token{TokKind::Minus, "-", 0, line, col};
    case '*': return Token{TokKind::Star, "*", 0, line, col};
    case '/': return Token{TokKind::Slash, "/", 0, line, col};
    case '%': return Token{TokKind::Percent, "%", 0, line, col};
    case '!': return Token{TokKind::Bang, "!", 0, line, col};
    case '~': return Token{TokKind::Tilde, "~", 0, line, col};
    case '<': return Token{TokKind::Lt, "<", 0, line, col};
    case '>': return Token{TokKind::Greater, ">", 0, line, col};
    case '&': return Token{TokKind::And, "&", 0, line, col};
    case '|': return Token{TokKind::Or, "|", 0, line, col};
    case '^': return Token{TokKind::Caret, "^", 0, line, col};
    case '?': return Token{TokKind::Question, "?", 0, line, col};
    default: break;
  }
  Token tok{TokKind::Eof, std::string(1, c), 0, line, col};
  return tok;
}

Token Lexer::next() {
  if (pushed_) {
    Token t = *pushed_;
    pushed_.reset();
    return t;
  }
  return readNext();
}

Token Lexer::peek() {
  if (!pushed_) pushed_ = readNext();
  return *pushed_;
}

bool Lexer::eof() const {
  if (pushed_) return pushed_->kind == TokKind::Eof;
  return pos_ >= text_.size();
}

// --- Expr dumps --------------------------------------------------------

std::string NumberExpr::dump() const { return std::to_string(value); }
std::string SymbolExpr::dump() const { return name; }
std::string DotExpr::dump() const { return "."; }
std::string UnaryExpr::dump() const { return "(" + op + arg->dump() + ")"; }
std::string BinaryExpr::dump() const {
  return "(" + lhs->dump() + " " + op + " " + rhs->dump() + ")";
}
std::string TernaryExpr::dump() const {
  return "(" + cond->dump() + " ? " + yes->dump() + " : " + no->dump() + ")";
}
std::string CallExpr::dump() const {
  std::string s = name + "(";
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) s += ", ";
    s += args[i]->dump();
  }
  return s + ")";
}

std::string ScriptFile::dump() const {
  std::string s;
  if (entry) s += "ENTRY(" + *entry + ")\n";
  if (!memory.empty()) {
    s += "MEMORY {\n";
    for (const auto& m : memory) s += "  " + m.name + "\n";
    s += "}\n";
  }
  if (hasSections) {
    s += "SECTIONS {\n";
    for (const auto& st : sectionsBody) {
      if (auto* a = dynamic_cast<AssignStmt*>(st.get()))
        s += "  " + a->name + " " + a->op + " " + a->value->dump() + "\n";
      else if (auto* o = dynamic_cast<OutputSectionStmt*>(st.get()))
        s += "  " + o->section.name + " : { ... }\n";
    }
    s += "}\n";
  }
  return s;
}

bool parseScriptNumber(const std::string& text, uint64_t& value) {
  if (text.empty()) return false;
  std::string t = text;
  uint64_t mult = 1;
  char last = t.back();
  if (last == 'K' || last == 'k') {
    mult = 1024;
    t.pop_back();
  } else if (last == 'M' || last == 'm') {
    mult = 1024 * 1024;
    t.pop_back();
  } else if (last == 'G' || last == 'g') {
    mult = 1024 * 1024 * 1024;
    t.pop_back();
  }
  uint64_t v = 0;
  try {
    size_t used = 0;
    int base = 10;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X')) base = 16;
    else if (t.size() > 1 && t[0] == '0' &&
             t.find_first_not_of("01234567") == std::string::npos)
      base = 8;
    v = std::stoull(t, &used, base);
    if (used != t.size()) return false;
  } catch (...) {
    return false;
  }
  value = v * mult;
  return true;
}

// --- Parser --------------------------------------------------------------

Parser::Parser(std::string text) : lex_(std::move(text)) { advance(); }

void Parser::advance() { cur_ = lex_.next(); }

bool Parser::at(TokKind k) const { return cur_.kind == k; }

bool Parser::accept(TokKind k) {
  if (cur_.kind == k) {
    advance();
    return true;
  }
  return false;
}

bool Parser::expect(TokKind k, std::string& error, const char* what) {
  if (cur_.kind == k) {
    advance();
    return true;
  }
  error = "line " + std::to_string(cur_.line) + ": expected " + what +
          " but found '" + cur_.text + "'";
  return false;
}

void Parser::fail(const std::string& msg) { error_ = msg; }

bool parseText(const std::string& text, ScriptFile& out, std::string& error) {
  Parser p(text);
  return p.parse(out, error);
}

bool Parser::parse(ScriptFile& out, std::string& error) {
  error_.clear();
  if (!parseTopLevel(out)) {
    error = error_.empty() ? "script parse error" : error_;
    return false;
  }
  if (cur_.kind != TokKind::Eof) {
    error = "line " + std::to_string(cur_.line) +
            ": unexpected trailing '" + cur_.text + "'";
    return false;
  }
  error.clear();
  return true;
}

bool Parser::parseTopLevel(ScriptFile& out) {
  while (cur_.kind != TokKind::Eof) {
    if (cur_.kind == TokKind::Semicolon) {
      advance();
      continue;
    }
    if (cur_.kind != TokKind::Ident && cur_.kind != TokKind::Dot) {
      fail("line " + std::to_string(cur_.line) + ": unexpected '" +
           cur_.text + "' at top level");
      return false;
    }
    std::string kw = cur_.text;
    if (kw == "ENTRY") {
      if (!parseEntry(out)) return false;
    } else if (kw == "OUTPUT_FORMAT") {
      if (!parseOutputFormat(out)) return false;
    } else if (kw == "OUTPUT_ARCH") {
      if (!parseOutputArch(out)) return false;
    } else if (kw == "OUTPUT") {
      if (!parseOutput(out)) return false;
    } else if (kw == "SEARCH_DIR") {
      if (!parseSearchDir(out)) return false;
    } else if (kw == "GROUP") {
      if (!parseGroup(out, true)) return false;
    } else if (kw == "INPUT") {
      if (!parseGroup(out, false)) return false;
    } else if (kw == "MEMORY") {
      if (!parseMemory(out)) return false;
    } else if (kw == "SECTIONS") {
      if (!parseSections(out)) return false;
    } else if (kw == "PHDRS") {
      if (!parsePhdrs(out)) return false;
    } else if (kw == "VERSION") {
      if (!parseVersion(out)) return false;
    } else if (kw == "INSERT") {
      if (!parseInsert(out)) return false;
    } else if (kw == "ASSERT") {
      // ASSERT(expr, msg); — record for execution.
      advance();
      std::string err;
      if (!expect(TokKind::LParen, err, "'('")) {
        fail(err);
        return false;
      }
      ExprPtr e = parseExpr();
      if (!e) {
        fail("bad ASSERT expression");
        return false;
      }
      std::string msg;
      if (accept(TokKind::Comma) && cur_.kind == TokKind::String) {
        msg = cur_.text;
        advance();
      }
      if (!expect(TokKind::RParen, err, "')'")) {
        fail(err);
        return false;
      }
      accept(TokKind::Semicolon);
      if (!out.hasSections) out.hasSections = true;
      auto st = std::make_unique<AssertStmt>();
      st->cond = std::move(e);
      st->message = msg;
      out.sectionsBody.push_back(std::move(st));
    } else if (kw == "PROVIDE" || kw == "HIDDEN" ||
               kw == "PROVIDE_HIDDEN") {
      // PROVIDE(sym = expr); at top level defines a symbol when undefined.
      std::string kind = kw;
      advance();
      std::string err;
      if (!expect(TokKind::LParen, err, "'('")) {
        fail(err);
        return false;
      }
      if (cur_.kind != TokKind::Ident && cur_.kind != TokKind::Dot) {
        fail(kind + " expects a symbol");
        return false;
      }
      std::string name = cur_.text;
      advance();
      if (!expect(TokKind::Assign, err, "'='")) {
        fail(err);
        return false;
      }
      ExprPtr e = parseExpr();
      if (!e) {
        fail("bad expression in " + kind);
        return false;
      }
      if (!expect(TokKind::RParen, err, "')'")) {
        fail(err);
        return false;
      }
      accept(TokKind::Semicolon);
      if (!out.hasSections) out.hasSections = true;
      auto st = std::make_unique<AssignStmt>();
      st->name = name;
      st->value = std::move(e);
      st->provide = true;
      st->hidden = (kind != "PROVIDE");
      out.sectionsBody.push_back(std::move(st));
    } else if (kw == "KEEP" || kw == "NOCROSSREFS" || kw == "REGION_ALIAS" ||
               kw == "INCLUDE") {
      // Tolerate miscellaneous top-level commands by skipping their
      // parenthesised/braced payload.
      advance();
      if (accept(TokKind::LParen)) {
        int depth = 1;
        while (depth && cur_.kind != TokKind::Eof) {
          if (cur_.kind == TokKind::LParen) ++depth;
          if (cur_.kind == TokKind::RParen) --depth;
          advance();
        }
      } else if (accept(TokKind::LBrace)) {
        int depth = 1;
        while (depth && cur_.kind != TokKind::Eof) {
          if (cur_.kind == TokKind::LBrace) ++depth;
          if (cur_.kind == TokKind::RBrace) --depth;
          advance();
        }
      }
      accept(TokKind::Semicolon);
    } else {
      // A bare assignment at top level (e.g. `foo = 0x1000;`).
      std::string name = cur_.text;
      advance();
      std::string op = "=";
      if (cur_.kind == TokKind::Assign) advance();
      else if (cur_.kind == TokKind::PlusAssign) {
        op = "+=";
        advance();
      } else if (cur_.kind == TokKind::MinusAssign) {
        op = "-=";
        advance();
      } else if (cur_.kind == TokKind::MulAssign) {
        op = "*=";
        advance();
      } else if (cur_.kind == TokKind::DivAssign) {
        op = "/=";
        advance();
      } else if (cur_.kind == TokKind::AndAssign) {
        op = "&=";
        advance();
      } else if (cur_.kind == TokKind::OrAssign) {
        op = "|=";
        advance();
      } else {
        fail("line " + std::to_string(cur_.line) + ": expected '=' after '" +
             name + "'");
        return false;
      }
      ExprPtr e = parseExpr();
      if (!e) {
        fail("line " + std::to_string(cur_.line) + ": bad expression");
        return false;
      }
      accept(TokKind::Semicolon);
      if (!out.hasSections) {
        // Stash as a synthetic sections-body prologue? Keep minimal: store
        // as entry in group? Instead create sectionsBody lazily.
        out.hasSections = true;
      }
      auto st = std::make_unique<AssignStmt>();
      st->name = name;
      st->value = std::move(e);
      st->op = op;
      out.sectionsBody.push_back(std::move(st));
    }
  }
  return true;
}

bool Parser::parseEntry(ScriptFile& out) {
  advance();  // ENTRY
  std::string err;
  if (!expect(TokKind::LParen, err, "'('")) {
    fail(err);
    return false;
  }
  if (cur_.kind != TokKind::Ident) {
    fail("ENTRY expects a symbol");
    return false;
  }
  out.entry = cur_.text;
  advance();
  if (!expect(TokKind::RParen, err, "')'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseOutputFormat(ScriptFile& out) {
  advance();
  std::string err;
  if (!expect(TokKind::LParen, err, "'('")) {
    fail(err);
    return false;
  }
  std::string v;
  if (cur_.kind == TokKind::String || cur_.kind == TokKind::Ident)
    v = cur_.text;
  else {
    fail("OUTPUT_FORMAT expects a name");
    return false;
  }
  advance();
  // OUTPUT_FORMAT(a, b, c): keep the first.
  while (accept(TokKind::Comma)) {
    if (cur_.kind == TokKind::String || cur_.kind == TokKind::Ident) advance();
  }
  if (!expect(TokKind::RParen, err, "')'")) {
    fail(err);
    return false;
  }
  out.outputFormat = v;
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseOutputArch(ScriptFile& out) {
  advance();
  std::string err;
  if (!expect(TokKind::LParen, err, "'('")) {
    fail(err);
    return false;
  }
  if (cur_.kind != TokKind::Ident && cur_.kind != TokKind::String) {
    fail("OUTPUT_ARCH expects a name");
    return false;
  }
  out.outputArch = cur_.text;
  advance();
  if (!expect(TokKind::RParen, err, "')'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseOutput(ScriptFile& out) {
  advance();
  std::string err;
  if (!expect(TokKind::LParen, err, "'('")) {
    fail(err);
    return false;
  }
  if (cur_.kind != TokKind::Ident && cur_.kind != TokKind::String) {
    fail("OUTPUT expects a name");
    return false;
  }
  out.outputName = cur_.text;
  advance();
  if (!expect(TokKind::RParen, err, "')'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseSearchDir(ScriptFile& out) {
  advance();
  std::string err;
  if (!expect(TokKind::LParen, err, "'('")) {
    fail(err);
    return false;
  }
  std::string v = parseStringLiteral();
  if (v.empty() && cur_.kind == TokKind::Ident) {
    v = cur_.text;
    advance();
  }
  if (!expect(TokKind::RParen, err, "')'")) {
    fail(err);
    return false;
  }
  out.searchDirs.push_back(v);
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseGroup(ScriptFile& out, bool isGroup) {
  (void)isGroup;
  advance();
  std::string err;
  if (!expect(TokKind::LParen, err, "'('")) {
    fail(err);
    return false;
  }
  while (cur_.kind != TokKind::RParen && cur_.kind != TokKind::Eof) {
    if (cur_.kind == TokKind::Comma) {
      advance();
      continue;
    }
    std::string f;
    if (cur_.kind == TokKind::String) {
      f = cur_.text;
      advance();
    } else if (cur_.kind == TokKind::Ident) {
      f = cur_.text;
      advance();
      // Handle -lfoo inside GROUP.
      if (f == "-l" && cur_.kind == TokKind::Ident) {
        f += cur_.text;
        advance();
      }
    } else {
      fail("GROUP/INPUT expects file names");
      return false;
    }
    // Skip "AS_NEEDED(...)" wrappers: keep the inner file.
    if ((f == "AS_NEEDED" || f == "GROUP" || f == "INPUT") &&
        cur_.kind == TokKind::LParen) {
      advance();
      while (cur_.kind != TokKind::RParen && cur_.kind != TokKind::Eof) {
        if (cur_.kind == TokKind::String || cur_.kind == TokKind::Ident) {
          out.group.push_back(cur_.text);
          advance();
        } else if (cur_.kind == TokKind::Comma) {
          advance();
        } else {
          advance();
        }
      }
      accept(TokKind::RParen);
      continue;
    }
    out.group.push_back(f);
    // Optional "(lib)" suffix after a file: `file.a(sym.o)`.
    if (cur_.kind == TokKind::LParen) {
      advance();
      while (cur_.kind != TokKind::RParen && cur_.kind != TokKind::Eof)
        advance();
      accept(TokKind::RParen);
    }
  }
  if (!expect(TokKind::RParen, err, "')'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseInsert(ScriptFile& out) {
  (void)out;
  advance();
  // INSERT [AFTER|BEFORE] ... ; — accepted and ignored.
  while (cur_.kind != TokKind::Semicolon && cur_.kind != TokKind::Eof)
    advance();
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseMemory(ScriptFile& out) {
  advance();  // MEMORY
  std::string err;
  if (!expect(TokKind::LBrace, err, "'{'")) {
    fail(err);
    return false;
  }
  while (cur_.kind != TokKind::RBrace && cur_.kind != TokKind::Eof) {
    if (cur_.kind == TokKind::Semicolon) {
      advance();
      continue;
    }
    if (cur_.kind != TokKind::Ident) {
      fail("MEMORY expects region names");
      return false;
    }
    MemoryRegion r;
    r.name = cur_.text;
    advance();
    // Optional attributes "(...)" e.g. (rwx).
    if (accept(TokKind::LParen)) {
      std::string attrs;
      while (cur_.kind != TokKind::RParen && cur_.kind != TokKind::Eof) {
        attrs += cur_.text;
        advance();
      }
      r.attrs = attrs;
      if (!expect(TokKind::RParen, err, "')'")) {
        fail(err);
        return false;
      }
    }
    if (!expect(TokKind::Colon, err, "':'")) {
      fail(err);
      return false;
    }
    // ORIGIN = expr, LENGTH = expr in either order, comma-separated.
    for (int i = 0; i < 2; ++i) {
      if (cur_.kind != TokKind::Ident) {
        fail("MEMORY region expects ORIGIN/LENGTH");
        return false;
      }
      std::string key = cur_.text;
      advance();
      if (!expect(TokKind::Assign, err, "'='")) {
        fail(err);
        return false;
      }
      ExprPtr e = parseExpr();
      if (!e) {
        fail("bad MEMORY expression");
        return false;
      }
      if (key == "ORIGIN" || key == "org" || key == "o")
        r.origin = std::move(e);
      else if (key == "LENGTH" || key == "len" || key == "l")
        r.length = std::move(e);
      else {
        fail("MEMORY region expects ORIGIN/LENGTH, got '" + key + "'");
        return false;
      }
      if (accept(TokKind::Comma)) continue;
      break;
    }
    out.memory.push_back(std::move(r));
  }
  if (!expect(TokKind::RBrace, err, "'}'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseSections(ScriptFile& out) {
  advance();  // SECTIONS
  std::string err;
  if (!expect(TokKind::LBrace, err, "'{'")) {
    fail(err);
    return false;
  }
  out.hasSections = true;
  while (cur_.kind != TokKind::RBrace && cur_.kind != TokKind::Eof) {
    if (cur_.kind == TokKind::Semicolon) {
      advance();
      continue;
    }
    auto st = parseSectionsStmt();
    if (!st) return false;
    if (st) out.sectionsBody.push_back(std::move(st));
  }
  if (!expect(TokKind::RBrace, err, "'}'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parsePhdrs(ScriptFile& out) {
  (void)out;
  advance();
  std::string err;
  if (!expect(TokKind::LBrace, err, "'{'")) {
    fail(err);
    return false;
  }
  int depth = 1;
  while (depth && cur_.kind != TokKind::Eof) {
    if (cur_.kind == TokKind::LBrace) ++depth;
    if (cur_.kind == TokKind::RBrace) --depth;
    if (depth) advance();
    else break;
  }
  if (!expect(TokKind::RBrace, err, "'}'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseVersion(ScriptFile& out) {
  (void)out;
  advance();
  std::string err;
  if (!expect(TokKind::LBrace, err, "'{'")) {
    fail(err);
    return false;
  }
  int depth = 1;
  while (depth && cur_.kind != TokKind::Eof) {
    if (cur_.kind == TokKind::LBrace) ++depth;
    if (cur_.kind == TokKind::RBrace) --depth;
    if (depth) advance();
    else break;
  }
  if (!expect(TokKind::RBrace, err, "'}'")) {
    fail(err);
    return false;
  }
  accept(TokKind::Semicolon);
  return true;
}

std::unique_ptr<Stmt> Parser::parseSectionsStmt() {
  // ASSERT(...) ;
  if (cur_.kind == TokKind::Ident && cur_.text == "ASSERT") {
    advance();
    std::string err;
    if (!expect(TokKind::LParen, err, "'('")) {
      fail(err);
      return nullptr;
    }
    ExprPtr e = parseExpr();
    std::string msg;
    if (accept(TokKind::Comma) && cur_.kind == TokKind::String) {
      msg = cur_.text;
      advance();
    }
    if (!expect(TokKind::RParen, err, "')'")) {
      fail(err);
      return nullptr;
    }
    accept(TokKind::Semicolon);
    auto st = std::make_unique<AssertStmt>();
    st->cond = std::move(e);
    st->message = msg;
    return st;
  }
  // PROVIDE(sym = expr) / HIDDEN(sym = expr) as a statement.
  if (cur_.kind == TokKind::Ident &&
      (cur_.text == "PROVIDE" || cur_.text == "HIDDEN" ||
       cur_.text == "PROVIDE_HIDDEN")) {
    std::string kind = cur_.text;
    advance();
    std::string err;
    if (!expect(TokKind::LParen, err, "'('")) {
      fail(err);
      return nullptr;
    }
    if (cur_.kind != TokKind::Ident && cur_.kind != TokKind::Dot) {
      fail("PROVIDE expects a symbol");
      return nullptr;
    }
    std::string name = cur_.text;
    advance();
    if (!expect(TokKind::Assign, err, "'='")) {
      fail(err);
      return nullptr;
    }
    ExprPtr e = parseExpr();
    if (!expect(TokKind::RParen, err, "')'")) {
      fail(err);
      return nullptr;
    }
    accept(TokKind::Semicolon);
    auto st = std::make_unique<AssignStmt>();
    st->name = name;
    st->value = std::move(e);
    st->provide = true;
    st->hidden = (kind != "PROVIDE");
    return st;
  }
  // `. = expr;` location-counter assignment.
  if (cur_.kind == TokKind::Dot) {
    return parseAssign("=");
  }
  if (cur_.kind != TokKind::Ident) {
    fail("line " + std::to_string(cur_.line) + ": unexpected '" +
         cur_.text + "' in SECTIONS");
    return nullptr;
  }
  // Lookahead: `name = ...` (symbol assignment) vs `name : ...` (output
  // section) vs `name (...) : ...` (typed section).
  std::string name = cur_.text;
  Token saved = cur_;
  advance();
  if (cur_.kind == TokKind::Assign || cur_.kind == TokKind::PlusAssign ||
      cur_.kind == TokKind::MinusAssign || cur_.kind == TokKind::MulAssign ||
      cur_.kind == TokKind::DivAssign || cur_.kind == TokKind::AndAssign ||
      cur_.kind == TokKind::OrAssign) {
    std::string op = cur_.text;
    advance();
    ExprPtr e = parseExpr();
    if (!e) {
      fail("bad expression in assignment");
      return nullptr;
    }
    accept(TokKind::Semicolon);
    auto st = std::make_unique<AssignStmt>();
    st->name = name;
    st->value = std::move(e);
    st->op = op;
    return st;
  }
  // PROVIDE-style `name PROVIDE(...)`? No: output section or HIDDEN alias.
  if (name == "HIDDEN" && cur_.kind == TokKind::LParen) {
    // HIDDEN(sym) bare (no assignment): treat as no-op assignment of sym.
    advance();
    if (cur_.kind != TokKind::Ident) {
      fail("HIDDEN expects a symbol");
      return nullptr;
    }
    std::string inner = cur_.text;
    advance();
    std::string err;
    if (!expect(TokKind::RParen, err, "')'")) {
      fail(err);
      return nullptr;
    }
    accept(TokKind::Semicolon);
    auto st = std::make_unique<AssignStmt>();
    st->name = inner;
    st->value = std::make_unique<SymbolExpr>(inner);
    st->hidden = true;
    return st;
  }
  // Otherwise it must be an output-section descriptor. The name token was
  // already consumed; reconstruct: optional address/type already? Handle:
  //   name [addr] [(type)] : [AT(...)] { ... } [>region] [AT>region] [:phdr]
  OutputSection sec;
  sec.name = (name == "OVERLAY") ? name : name;
  // Optional address expression before the colon: `name addr :`.
  if (cur_.kind != TokKind::Colon) {
    // Could be `(type)` or an address. Try parsing an expression; if the
    // next token after it is a colon, it was the address.
    if (cur_.kind == TokKind::LParen) {
      // Section type `(NOLOAD)` etc.
      advance();
      std::string ty;
      if (cur_.kind == TokKind::Ident) {
        ty = cur_.text;
        advance();
      }
      std::string err;
      if (!expect(TokKind::RParen, err, "')'")) {
        fail(err);
        return nullptr;
      }
      sec.type = ty;
    } else if (cur_.kind == TokKind::Number || cur_.kind == TokKind::Ident ||
               cur_.kind == TokKind::Dot || cur_.kind == TokKind::LParen) {
      // Attempt address parse with lexer pushback on failure: parse an
      // expression, then require a colon.
      Lexer backup = lex_;
      Token curBackup = cur_;
      ExprPtr addr = parseExpr();
      if (addr && cur_.kind == TokKind::Colon) {
        sec.address = std::move(addr);
      } else {
        // Not an address; restore and continue (e.g. OVERLAY).
        lex_ = backup;
        cur_ = curBackup;
      }
    }
  }
  if (!parseOutputSection(sec)) return nullptr;
  auto st = std::make_unique<OutputSectionStmt>();
  st->section = std::move(sec);
  return st;
}

std::unique_ptr<Stmt> Parser::parseAssign(const std::string& op) {
  (void)op;
  advance();  // '.'
  std::string oper = "=";
  if (cur_.kind == TokKind::Assign) advance();
  else if (cur_.kind == TokKind::PlusAssign) {
    oper = "+=";
    advance();
  } else if (cur_.kind == TokKind::MinusAssign) {
    oper = "-=";
    advance();
  } else if (cur_.kind == TokKind::MulAssign) {
    oper = "*=";
    advance();
  } else if (cur_.kind == TokKind::DivAssign) {
    oper = "/=";
    advance();
  } else if (cur_.kind == TokKind::AndAssign) {
    oper = "&=";
    advance();
  } else if (cur_.kind == TokKind::OrAssign) {
    oper = "|=";
    advance();
  } else {
    fail("expected '=' after '.'");
    return nullptr;
  }
  ExprPtr e = parseExpr();
  if (!e) {
    fail("bad expression after '.'");
    return nullptr;
  }
  accept(TokKind::Semicolon);
  auto st = std::make_unique<AssignStmt>();
  st->name = ".";
  st->value = std::move(e);
  st->op = oper;
  return st;
}

bool Parser::parseOutputSection(OutputSection& section) {
  std::string err;
  if (!expect(TokKind::Colon, err, "':'")) {
    fail(err);
    return false;
  }
  // Optional AT(expr) LMA before the brace.
  if (cur_.kind == TokKind::Ident && cur_.text == "AT") {
    advance();
    if (!expect(TokKind::LParen, err, "'('")) {
      fail(err);
      return false;
    }
    section.lma = parseExpr();
    if (!expect(TokKind::RParen, err, "')'")) {
      fail(err);
      return false;
    }
  }
  if (!expect(TokKind::LBrace, err, "'{'")) {
    fail(err);
    return false;
  }
  if (!parseInputPatterns(section)) return false;
  if (!expect(TokKind::RBrace, err, "'}'")) {
    fail(err);
    return false;
  }
  // Trailing `>region`, `AT>region`, `:phdr`, `=fillexp`.
  while (true) {
    if (cur_.kind == TokKind::Greater) {
      advance();
      if (cur_.kind == TokKind::Ident) {
        section.region = cur_.text;
        advance();
      }
    } else if (cur_.kind == TokKind::Ident && cur_.text == "AT") {
      advance();
      if (cur_.kind == TokKind::Greater) {
        advance();
        if (cur_.kind == TokKind::Ident) {
          section.lmaRegion = cur_.text;
          advance();
        }
      } else if (cur_.kind == TokKind::LParen) {
        advance();
        section.lma = parseExpr();
        if (!expect(TokKind::RParen, err, "')'")) {
          fail(err);
          return false;
        }
      }
    } else if (cur_.kind == TokKind::Colon) {
      advance();  // :phdr — accepted and ignored
      if (cur_.kind == TokKind::Ident) advance();
    } else if (cur_.kind == TokKind::Assign) {
      advance();  // =fillexp — accepted and ignored
      ExprPtr fill = parseExpr();
      (void)fill;
    } else {
      break;
    }
  }
  accept(TokKind::Semicolon);
  return true;
}

bool Parser::parseInputPatterns(OutputSection& section) {
  // Contents: a sequence of `*(sections)`, `file(sections)`,
  // `EXCLUDE_FILE(...) *(...)`, `KEEP(...)`, assignments and `. = .` etc.
  while (cur_.kind != TokKind::RBrace && cur_.kind != TokKind::Eof) {
    if (cur_.kind == TokKind::Semicolon) {
      advance();
      continue;
    }
    // Nested assignment inside the section (e.g. `_etext = .;`).
    if (cur_.kind == TokKind::Dot) {
      auto st = parseAssign("=");
      if (!st) return false;
      // Stash as raw line for the executor: assignments inside output
      // sections define symbols at the current dot. Represent by appending
      // a synthetic pattern? Instead record in rawLines and handle in
      // ldscript execution via re-parse? Simpler: the executor re-walks
      // the section body? For now, record the assignment text so the
      // executor can evaluate it. We stash into inputs as a marker with
      // an empty file/section and keep the statement in a side list.
      // To avoid changing the AST shape, encode as a SectionPattern with
      // file=="=assign" and section==dump. The executor recognises it.
      SectionPattern p;
      p.file = "=assign";
      p.section = dynamic_cast<AssignStmt*>(st.get())->value->dump();
      // Also keep the symbol name in sort field.
      p.sort = dynamic_cast<AssignStmt*>(st.get())->name;
      section.inputs.push_back(std::move(p));
      continue;
    }
    std::string word =
        cur_.kind == TokKind::Ident ? cur_.text : std::string();
    if (word == "KEEP" || word == "EXCLUDE_FILE" || word == "INPUT_SECTION_FLAGS" ||
        word == "SORT" || word == "SORT_BY_NAME" || word == "SORT_BY_ALIGNMENT" ||
        word == "SORT_NONE" || word == "SORT_BY_INIT_PRIORITY") {
      // KEEP(*( ... )) — unwrap one level.
      bool isKeep = (word == "KEEP");
      bool isExclude = (word == "EXCLUDE_FILE");
      advance();
      {
        std::string err;
        if (!expect(TokKind::LParen, err, "'('")) {
          fail(err);
          return false;
        }
      }
      if (isExclude) {
        // EXCLUDE_FILE (*a.o ...) *(...)
        while (cur_.kind == TokKind::LParen ||
               (cur_.kind == TokKind::Ident && cur_.text != "*")) {
          if (cur_.kind == TokKind::LParen) {
            advance();
            while (cur_.kind != TokKind::RParen &&
                   cur_.kind != TokKind::Eof)
              advance();
            accept(TokKind::RParen);
          } else {
            advance();
          }
        }
      }
      // Recurse into the inner patterns.
      size_t before = section.inputs.size();
      if (!parseInputPatterns(section)) return false;
      if (isKeep) {
        for (size_t i = before; i < section.inputs.size(); ++i)
          section.inputs[i].keep = true;
      }
      std::string err;
      if (!expect(TokKind::RParen, err, "')'")) {
        fail(err);
        return false;
      }
      continue;
    }
    if (word == "PROVIDE" || word == "HIDDEN" || word == "PROVIDE_HIDDEN" ||
        word == "ASSERT" || word == "FILL" || word == "CREATE_OBJECT_SYMBOLS" ||
        word == "CONSTRUCTORS") {
      // Skip balanced payload.
      advance();
      if (accept(TokKind::LParen)) {
        int depth = 1;
        while (depth && cur_.kind != TokKind::Eof) {
          if (cur_.kind == TokKind::LParen) ++depth;
          if (cur_.kind == TokKind::RParen) --depth;
          if (depth) {
            // Record PROVIDE assignments inside sections too.
            advance();
          } else
            break;
        }
        accept(TokKind::RParen);
      }
      accept(TokKind::Semicolon);
      continue;
    }
    // `file ( sections )` or `*( sections )`.
    if (cur_.kind == TokKind::Ident || cur_.kind == TokKind::Star ||
        cur_.kind == TokKind::LParen) {
      std::string file;
      if (cur_.kind == TokKind::Ident) {
        file = cur_.text;
        advance();
        // Suffix wildcards: `file*.o` lexes as Ident + Star pieces.
        while (cur_.kind == TokKind::Star) {
          file += cur_.text;
          advance();
          if (cur_.kind == TokKind::Ident) {
            file += cur_.text;
            advance();
          }
        }
      } else if (cur_.kind == TokKind::Star) {
        file = "*";
        advance();
        // `*.o` / `*crt.o`: reassemble the file pattern.
        while (cur_.kind == TokKind::Ident) {
          file += cur_.text;
          advance();
          if (cur_.kind == TokKind::Star) {
            file += cur_.text;
            advance();
          }
        }
      } else {
        file = "*";
      }
      // `file` without parens means a bare filename (INPUT-like)? Treat
      // `*(...)` shape only; a bare word followed by ';' is ignored.
      if (cur_.kind != TokKind::LParen) {
        if (cur_.kind == TokKind::Semicolon) advance();
        continue;
      }
      advance();  // '('
      // Section list inside.
      while (cur_.kind != TokKind::RParen && cur_.kind != TokKind::Eof) {
        if (cur_.kind == TokKind::Semicolon ||
            cur_.kind == TokKind::Comma) {
          advance();
          continue;
        }
        // Nested SORT*(...) wrappers.
        if (cur_.kind == TokKind::Ident &&
            (cur_.text == "SORT" || cur_.text == "SORT_BY_NAME" ||
             cur_.text == "SORT_BY_ALIGNMENT" || cur_.text == "SORT_NONE" ||
             cur_.text == "SORT_BY_INIT_PRIORITY" ||
             cur_.text == "EXCLUDE_FILE" || cur_.text == "KEEP" ||
             cur_.text == "INPUT_SECTION_FLAGS")) {
          std::string wrap = cur_.text;
          advance();
          if (accept(TokKind::LParen)) {
            // EXCLUDE_FILE takes file patterns then the real list; skip
            // file patterns until a section-looking token appears.
            if (wrap == "EXCLUDE_FILE") {
              while (cur_.kind != TokKind::RParen &&
                     cur_.kind != TokKind::Eof &&
                     cur_.kind != TokKind::Star &&
                     !(cur_.kind == TokKind::Ident &&
                       !cur_.text.empty() && cur_.text[0] == '.'))
                advance();
            }
            while (cur_.kind != TokKind::RParen &&
                   cur_.kind != TokKind::Eof) {
              if (cur_.kind == TokKind::Ident ||
                  cur_.kind == TokKind::Star) {
                SectionPattern p;
                p.file = file;
                p.section = cur_.text;
                p.sort = wrap;
                if (wrap == "KEEP") p.keep = true;
                section.inputs.push_back(std::move(p));
                advance();
              } else if (cur_.kind == TokKind::LParen) {
                // Nested parens (e.g. SORT_BY_NAME(COMMON)): skip.
                advance();
                while (cur_.kind != TokKind::RParen &&
                       cur_.kind != TokKind::Eof)
                  advance();
                accept(TokKind::RParen);
              } else {
                advance();
              }
            }
            accept(TokKind::RParen);
          }
          continue;
        }
        if (cur_.kind == TokKind::Ident || cur_.kind == TokKind::Star) {
          std::string sec = cur_.text;
          advance();
          // Reassemble split wildcards: `.bss*` lexes as Ident + Star,
          // `*` alone stays as-is.
          for (int k = 0; k < 4; ++k) {
            if (cur_.kind == TokKind::Star) {
              sec += cur_.text;
              advance();
            } else if (cur_.kind == TokKind::Ident &&
                       (sec.ends_with("*") || sec == "*")) {
              sec += cur_.text;
              advance();
            } else {
              break;
            }
          }
          SectionPattern p;
          p.file = file;
          p.section = sec;
          section.inputs.push_back(std::move(p));
          continue;
        }
        // COMMON / section keywords without dots.
        if (cur_.kind == TokKind::Dot) {
          advance();
          continue;
        }
        advance();
      }
      accept(TokKind::RParen);
      continue;
    }
    // Unknown token: skip to avoid infinite loop.
    advance();
  }
  return true;
}

// --- Expressions -----------------------------------------------------------

ExprPtr Parser::parseExpr() { return parseTernary(); }

ExprPtr Parser::parseTernary() {
  ExprPtr c = parseOrOr();
  if (!c) return nullptr;
  if (accept(TokKind::Question)) {
    ExprPtr y = parseExpr();
    if (!y) return nullptr;
    std::string err;
    if (!expect(TokKind::Colon, err, "':'")) {
      fail(err);
      return nullptr;
    }
    ExprPtr n = parseTernary();
    if (!n) return nullptr;
    return std::make_unique<TernaryExpr>(std::move(c), std::move(y),
                                         std::move(n));
  }
  return c;
}

ExprPtr Parser::parseOrOr() {
  ExprPtr l = parseAndAnd();
  while (cur_.kind == TokKind::OrOr) {
    advance();
    ExprPtr r = parseAndAnd();
    l = std::make_unique<BinaryExpr>("||", std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseAndAnd() {
  ExprPtr l = parseOr();
  while (cur_.kind == TokKind::AndAnd) {
    advance();
    ExprPtr r = parseOr();
    l = std::make_unique<BinaryExpr>("&&", std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseOr() {
  ExprPtr l = parseXor();
  while (cur_.kind == TokKind::Or) {
    advance();
    ExprPtr r = parseXor();
    l = std::make_unique<BinaryExpr>("|", std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseXor() {
  ExprPtr l = parseAnd();
  while (cur_.kind == TokKind::Caret) {
    advance();
    ExprPtr r = parseAnd();
    l = std::make_unique<BinaryExpr>("^", std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseAnd() {
  ExprPtr l = parseEquality();
  while (cur_.kind == TokKind::And) {
    advance();
    ExprPtr r = parseEquality();
    l = std::make_unique<BinaryExpr>("&", std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseEquality() {
  ExprPtr l = parseRelational();
  while (cur_.kind == TokKind::Eq || cur_.kind == TokKind::Ne) {
    std::string op = cur_.kind == TokKind::Eq ? "==" : "!=";
    advance();
    ExprPtr r = parseRelational();
    l = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseRelational() {
  ExprPtr l = parseShift();
  while (cur_.kind == TokKind::Lt || cur_.kind == TokKind::Gt ||
         cur_.kind == TokKind::Le || cur_.kind == TokKind::Ge ||
         cur_.kind == TokKind::Greater) {
    std::string op;
    if (cur_.kind == TokKind::Lt) op = "<";
    else if (cur_.kind == TokKind::Greater || cur_.kind == TokKind::Gt) op = ">";
    else if (cur_.kind == TokKind::Le) op = "<=";
    else op = ">=";
    advance();
    ExprPtr r = parseShift();
    l = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseShift() {
  ExprPtr l = parseAdd();
  while (cur_.kind == TokKind::Shl || cur_.kind == TokKind::Shr) {
    std::string op = cur_.kind == TokKind::Shl ? "<<" : ">>";
    advance();
    ExprPtr r = parseAdd();
    l = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseAdd() {
  ExprPtr l = parseMul();
  while (cur_.kind == TokKind::Plus || cur_.kind == TokKind::Minus) {
    std::string op = cur_.kind == TokKind::Plus ? "+" : "-";
    advance();
    ExprPtr r = parseMul();
    l = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseMul() {
  ExprPtr l = parseUnary();
  while (cur_.kind == TokKind::Star || cur_.kind == TokKind::Slash ||
         cur_.kind == TokKind::Percent) {
    std::string op = cur_.kind == TokKind::Star ? "*"
                     : cur_.kind == TokKind::Slash ? "/" : "%";
    advance();
    ExprPtr r = parseUnary();
    l = std::make_unique<BinaryExpr>(op, std::move(l), std::move(r));
  }
  return l;
}

ExprPtr Parser::parseUnary() {
  if (cur_.kind == TokKind::Minus) {
    advance();
    return std::make_unique<UnaryExpr>("-", parseUnary());
  }
  if (cur_.kind == TokKind::Plus) {
    advance();
    return parseUnary();
  }
  if (cur_.kind == TokKind::Bang) {
    advance();
    return std::make_unique<UnaryExpr>("!", parseUnary());
  }
  if (cur_.kind == TokKind::Tilde) {
    advance();
    return std::make_unique<UnaryExpr>("~", parseUnary());
  }
  return parsePrimary();
}

ExprPtr Parser::parsePrimary() {
  if (cur_.kind == TokKind::Number) {
    uint64_t v = cur_.number;
    advance();
    return std::make_unique<NumberExpr>(v);
  }
  if (cur_.kind == TokKind::Dot) {
    advance();
    return std::make_unique<DotExpr>();
  }
  if (cur_.kind == TokKind::LParen) {
    advance();
    ExprPtr e = parseExpr();
    std::string err;
    if (!expect(TokKind::RParen, err, "')'")) {
      fail(err);
      return nullptr;
    }
    return e;
  }
  if (cur_.kind == TokKind::Ident) {
    std::string name = cur_.text;
    advance();
    if (cur_.kind == TokKind::LParen) {
      advance();
      std::vector<ExprPtr> args;
      while (cur_.kind != TokKind::RParen && cur_.kind != TokKind::Eof) {
        if (cur_.kind == TokKind::Comma) {
          advance();
          continue;
        }
        // String args (e.g. CONSTANT (MAXPAGESIZE)): keep as symbols.
        if (cur_.kind == TokKind::String) {
          args.push_back(
              std::make_unique<SymbolExpr>(cur_.text));
          advance();
          continue;
        }
        ExprPtr a = parseExpr();
        if (!a) return nullptr;
        args.push_back(std::move(a));
        if (cur_.kind == TokKind::Comma) advance();
      }
      std::string err;
      if (!expect(TokKind::RParen, err, "')'")) {
        fail(err);
        return nullptr;
      }
      // PROVIDE/HIDDEN as expression-level wrappers.
      if (name == "PROVIDE" || name == "HIDDEN" ||
          name == "PROVIDE_HIDDEN" || name == "ABSOLUTE") {
        if (args.size() == 1) return std::move(args[0]);
        fail(name + " expects one argument");
        return nullptr;
      }
      return std::make_unique<CallExpr>(name, std::move(args));
    }
    return std::make_unique<SymbolExpr>(name);
  }
  if (cur_.kind == TokKind::String) {
    std::string s = cur_.text;
    advance();
    return std::make_unique<SymbolExpr>(s);
  }
  return nullptr;
}

std::string Parser::parseStringLiteral() {
  if (cur_.kind == TokKind::String) {
    std::string s = cur_.text;
    advance();
    return s;
  }
  return {};
}

}  // namespace qld::script
