// ldscript.cpp - linker-script execution (layout driven by SECTIONS/MEMORY).
//
// Evaluates the AST produced by the parser in ldscript_parser.cpp and assigns
// output VMAs/file offsets plus symbol addresses. Also handles ENTRY,
// SEARCH_DIR and GROUP/INPUT file lists.
#include "internal.hpp"
#include "parser.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace qld::detail {

namespace {

uint64_t alignUp(uint64_t v, uint64_t a) {
  if (a <= 1) return v;
  return ((v + a - 1) / a) * a;
}

// Simple glob: '*' matches any run, '?' matches one char.
bool globMatch(const std::string& pat, const std::string& s) {
  size_t p = 0, t = 0, star = std::string::npos, mark = 0;
  while (t < s.size()) {
    if (p < pat.size() && (pat[p] == '?' || pat[p] == s[t])) {
      ++p;
      ++t;
    } else if (p < pat.size() && pat[p] == '*') {
      star = p++;
      mark = t;
    } else if (star != std::string::npos) {
      p = star + 1;
      t = ++mark;
    } else {
      return false;
    }
  }
  while (p < pat.size() && pat[p] == '*') ++p;
  return p == pat.size();
}

bool fileMatches(const std::string& pattern, const std::string& label) {
  if (pattern == "*" || pattern.empty()) return true;
  // Labels look like "lib.a(member.o)" for archive members; match against
  // the full label, the basename, and the member name.
  std::string base = label;
  auto slash = base.find_last_of("/\\");
  if (slash != std::string::npos) base = base.substr(slash + 1);
  std::string member = label;
  auto lp = label.find('(');
  if (lp != std::string::npos) {
    member = label.substr(lp + 1);
    if (!member.empty() && member.back() == ')') member.pop_back();
  }
  // Archive-qualified patterns like "lib.a:member.o" or "lib.a(member.o)".
  if (globMatch(pattern, label) || globMatch(pattern, base) ||
      globMatch(pattern, member))
    return true;
  // Suffix match for patterns like "*crt*.o".
  return false;
}

struct EvalCtx {
  uint64_t dot = 0;
  const std::map<std::string, uint64_t>* syms = nullptr;
  const std::map<std::string, std::pair<uint64_t, uint64_t>>* mem = nullptr;
  const std::map<std::string, std::pair<uint64_t, uint64_t>>* secs =
      nullptr;  // name -> (addr,size)
  bool* failed = nullptr;
  std::string* error = nullptr;
};

uint64_t evalExpr(const script::Expr* e, EvalCtx& ctx);

uint64_t evalCall(const script::CallExpr* c, EvalCtx& ctx) {
  auto arg = [&](size_t i) -> uint64_t {
    if (i >= c->args.size()) {
      if (ctx.failed) *ctx.failed = true;
      return 0;
    }
    return evalExpr(c->args[i].get(), ctx);
  };
  const std::string& n = c->name;
  if (n == "ALIGN") {
    if (c->args.size() == 1) {
      uint64_t a = arg(0);
      ctx.dot = alignUp(ctx.dot, a ? a : 1);
      return ctx.dot;
    }
    if (c->args.size() == 2) {
      uint64_t v = arg(0), a = arg(1);
      return alignUp(v, a ? a : 1);
    }
    return ctx.dot;
  }
  if (n == "ABSOLUTE") return c->args.empty() ? 0 : arg(0);
  if (n == "ADDR") {
    if (c->args.empty() || !ctx.secs) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    if (!se) return arg(0);
    auto it = ctx.secs->find(se->name);
    return it == ctx.secs->end() ? 0 : it->second.first;
  }
  if (n == "SIZEOF") {
    if (c->args.empty() || !ctx.secs) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    if (!se) return 0;
    auto it = ctx.secs->find(se->name);
    return it == ctx.secs->end() ? 0 : it->second.second;
  }
  if (n == "LOADADDR") {
    if (c->args.empty() || !ctx.secs) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    if (!se) return arg(0);
    auto it = ctx.secs->find(se->name);
    return it == ctx.secs->end() ? 0 : it->second.first;
  }
  if (n == "DEFINED") {
    if (c->args.empty() || !ctx.syms) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    if (!se) return 0;
    return ctx.syms->count(se->name) ? 1 : 0;
  }
  if (n == "CONSTANT") {
    if (c->args.empty()) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    std::string k = se ? se->name : "";
    if (k == "MAXPAGESIZE") return 0x1000;
    if (k == "COMMONPAGESIZE") return 0x1000;
    return arg(0);
  }
  if (n == "ORIGIN") {
    if (c->args.empty() || !ctx.mem) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    if (!se) return 0;
    auto it = ctx.mem->find(se->name);
    return it == ctx.mem->end() ? 0 : it->second.first;
  }
  if (n == "LENGTH") {
    if (c->args.empty() || !ctx.mem) return 0;
    auto* se = dynamic_cast<const script::SymbolExpr*>(c->args[0].get());
    if (!se) return 0;
    auto it = ctx.mem->find(se->name);
    return it == ctx.mem->end() ? 0 : it->second.second;
  }
  if (n == "SEGMENT_START") return ctx.dot;
  if (n == "MAX" || n == "MIN") {
    if (c->args.empty()) return 0;
    uint64_t v = arg(0);
    for (size_t i = 1; i < c->args.size(); ++i) {
      uint64_t w = arg(i);
      v = (n == "MAX") ? std::max(v, w) : std::min(v, w);
    }
    return v;
  }
  if (n == "DATA_SEGMENT_ALIGN" || n == "DATA_SEGMENT_RELRO_END" ||
      n == "DATA_SEGMENT_END" || n == "SEGMENT_START") {
    if (!c->args.empty()) return arg(0);
    return ctx.dot;
  }
  if (n == "ALIGNOF") {
    if (c->args.empty() || !ctx.secs) return 1;
    return 1;
  }
  // Unknown function: evaluate first arg.
  if (!c->args.empty()) return arg(0);
  return 0;
}

uint64_t evalExpr(const script::Expr* e, EvalCtx& ctx) {
  if (!e) return 0;
  if (auto* n = dynamic_cast<const script::NumberExpr*>(e)) return n->value;
  if (dynamic_cast<const script::DotExpr*>(e)) return ctx.dot;
  if (auto* s = dynamic_cast<const script::SymbolExpr*>(e)) {
    if (ctx.syms) {
      auto it = ctx.syms->find(s->name);
      if (it != ctx.syms->end()) return it->second;
    }
    return 0;  // forward refs read as 0 in the single-pass layout
  }
  if (auto* u = dynamic_cast<const script::UnaryExpr*>(e)) {
    uint64_t a = evalExpr(u->arg.get(), ctx);
    if (u->op == "-") return uint64_t(-int64_t(a));
    if (u->op == "~") return ~a;
    if (u->op == "!") return a ? 0 : 1;
    return a;
  }
  if (auto* b = dynamic_cast<const script::BinaryExpr*>(e)) {
    uint64_t l = evalExpr(b->lhs.get(), ctx);
    uint64_t r = evalExpr(b->rhs.get(), ctx);
    const std::string& o = b->op;
    if (o == "+") return l + r;
    if (o == "-") return l - r;
    if (o == "*") return l * r;
    if (o == "/") return r ? l / r : 0;
    if (o == "%") return r ? l % r : 0;
    if (o == "<<") return l << (r & 63);
    if (o == ">>") return l >> (r & 63);
    if (o == "&") return l & r;
    if (o == "|") return l | r;
    if (o == "^") return l ^ r;
    if (o == "==") return l == r;
    if (o == "!=") return l != r;
    if (o == "<") return l < r;
    if (o == ">") return l > r;
    if (o == "<=") return l <= r;
    if (o == ">=") return l >= r;
    if (o == "&&") return (l && r) ? 1 : 0;
    if (o == "||") return (l || r) ? 1 : 0;
    return 0;
  }
  if (auto* t = dynamic_cast<const script::TernaryExpr*>(e)) {
    return evalExpr(t->cond.get(), ctx) ? evalExpr(t->yes.get(), ctx)
                                        : evalExpr(t->no.get(), ctx);
  }
  if (auto* c = dynamic_cast<const script::CallExpr*>(e))
    return evalCall(c, ctx);
  return 0;
}

bool loadScriptFile(const std::string& path, std::string& text,
                    std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open script " + path;
    return false;
  }
  text.assign((std::istreambuf_iterator<char>(in)),
              std::istreambuf_iterator<char>());
  return true;
}

std::string readScriptSource(const LinkState& state, std::string& error) {
  if (state.options.scriptIsText) return state.options.scriptText;
  if (!state.options.script.empty()) {
    std::string text;
    if (!loadScriptFile(state.options.script, text, error)) return {};
    if (!error.empty()) return {};
    return text;
  }
  // Scripts passed as INPUT files are already in objects[] with isScript.
  std::string combined;
  for (const auto& o : state.objects) {
    if (!o.isScript) continue;
    combined += std::string(o.bytes.begin(), o.bytes.end());
    combined += "\n";
  }
  return combined;
}

}  // namespace

bool hasScript(const LinkState& state) {
  if (state.options.scriptIsText && !state.options.scriptText.empty())
    return true;
  if (!state.options.script.empty()) return true;
  for (const auto& o : state.objects)
    if (o.isScript) return true;
  return false;
}

// Loads GROUP/INPUT/SEARCH_DIR extras. Called by the driver before discovery.
bool loadScriptExtras(LinkState& state, std::string& error) {
  std::string src = readScriptSource(state, error);
  if (!error.empty()) return false;
  if (src.empty()) return true;
  script::ScriptFile sf;
  if (!script::parseText(src, sf, error)) return false;
  for (const auto& d : sf.searchDirs) state.options.libPaths.push_back(d);
  if (sf.entry && !state.options.hasBase) {
    // ENTRY from script overrides the default only when the user did not
    // pass -e explicitly. Mark via hasEntry check in driver: the driver's
    // options.entry already defaults to _start; override when the user did
    // not set hasEntry... internal options always carry entry; check the
    // public flag through state? internal LinkOptions has no hasEntry flag,
    // so override only when entry is still the default and script differs.
    if (state.options.entry == "_start") state.options.entry = *sf.entry;
  } else if (sf.entry) {
    state.options.entry = *sf.entry;
  }
  // GROUP files join the input queue.
  for (const auto& g : sf.group) {
    std::string name = g;
    // Handle -lfoo inside scripts.
    if (name.starts_with("-l")) {
      state.options.libs.push_back(name.substr(2));
      continue;
    }
    // AS_NEEDED(...) already unwrapped by the parser.
    std::ifstream probe(name, std::ios::binary);
    std::string resolved = name;
    if (!probe) {
      // Search libPaths.
      bool found = false;
      for (const auto& dir : state.options.libPaths) {
        std::string cand = (std::filesystem::path(dir) / name).string();
        std::ifstream p2(cand, std::ios::binary);
        if (p2) {
          resolved = cand;
          found = true;
          break;
        }
      }
      if (!found) {
        error = "script: cannot find input " + name;
        return false;
      }
    }
    // Avoid double-adding command-line inputs.
    bool dup = false;
    for (const auto& o : state.objects)
      if (o.label == resolved) {
        dup = true;
        break;
      }
    if (dup) continue;
    std::ifstream in(resolved, std::ios::binary);
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    if (qbfd::isArchive(bytes)) {
      auto opened = qbfd::openArchive(resolved);
      if (!opened) {
        error = resolved + ": " + opened.error().message;
        return false;
      }
      LoadedArchive la;
      la.path = resolved;
      auto buf = qbfd::Buffer::fromFile(resolved);
      if (buf) {
        la.buffer = std::move(*buf);
        la.bytes.assign(la.buffer->span().begin(), la.buffer->span().end());
      } else {
        la.bytes = std::move(bytes);
      }
      la.archive = std::move(opened->archive);
      state.archives.push_back(std::move(la));
    } else {
      auto opened = qbfd::open(bytes, resolved);
      if (!opened) {
        // Maybe a nested script.
        LoadedObject lo;
        lo.label = resolved;
        lo.bytes = std::move(bytes);
        lo.isScript = true;
        state.objects.push_back(std::move(lo));
        continue;
      }
      LoadedObject lo;
      lo.label = resolved;
      lo.bytes = std::move(bytes);
      lo.buffer = std::move(opened->buffer);
      lo.object = std::move(opened->object);
      state.objects.push_back(std::move(lo));
    }
  }
  return true;
}

bool runScript(LinkState& state, std::string& error) {
  std::string src = readScriptSource(state, error);
  if (!error.empty()) return false;
  if (src.empty()) {
    error = "internal error: script requested but no source found";
    return false;
  }
  script::ScriptFile sf;
  if (!script::parseText(src, sf, error)) return false;
  if (sf.entry) state.options.entry = *sf.entry;

  // Collect input sections exactly like the default layout.
  state.inputSections.clear();
  for (size_t oi = 0; oi < state.objects.size(); ++oi) {
    const auto& obj = state.objects[oi];
    if (obj.isScript || !obj.object) continue;
    for (const auto& s : obj.object->sections()) {
      if (s.index == 0 && s.name.empty() && s.size == 0) continue;
      if (s.name == ".symtab" || s.name == ".strtab" ||
          s.name == ".shstrtab")
        continue;
      if (s.flags & qbfd::sec::Reloc) continue;
      if (s.name.starts_with(".rel.") || s.name.starts_with(".rela."))
        continue;
      if (state.options.stripDebug && (s.flags & qbfd::sec::Debug))
        continue;
      InputSection in;
      in.objectIndex = oi;
      in.sectionIndex = s.index;
      in.section = s;
      if (s.flags & qbfd::sec::Bss) {
        in.contents.clear();
      } else {
        auto c = obj.object->sectionContents(s);
        if (!c) {
          error = obj.label + ": cannot read section " + s.name;
          return false;
        }
        in.contents.assign(c->begin(), c->end());
      }
      auto r = obj.object->relocations(s);
      if (r) in.relocs = std::move(*r);
      state.inputSections.push_back(std::move(in));
    }
  }

  // MEMORY regions.
  std::map<std::string, std::pair<uint64_t, uint64_t>> mem;  // name->(org,len)
  std::map<std::string, uint64_t> memCursor;
  {
    EvalCtx ctx;
    ctx.syms = &state.symAddr;
    ctx.mem = &mem;
    for (const auto& m : sf.memory) {
      uint64_t org = m.origin ? evalExpr(m.origin.get(), ctx) : 0;
      uint64_t len = m.length ? evalExpr(m.length.get(), ctx) : 0;
      mem[m.name] = {org, len};
      memCursor[m.name] = org;
    }
  }

  bool relocatable = state.options.mode == Mode::Relocatable;
  uint64_t base = state.options.hasBase
                      ? state.options.base
                      : (state.options.mode == Mode::Exec && !state.options.pie
                             ? 0x400000
                             : 0);
  uint64_t ehsize = state.wide ? 64 : 52;
  uint64_t phentsize = state.wide ? 56 : 32;
  uint64_t phnum = relocatable ? 0 : 3;
  uint64_t headerEnd = relocatable ? ehsize : ehsize + phnum * phentsize;
  uint64_t dot = (base == 0) ? headerEnd : alignUp(base + headerEnd, 0x1000);

  // Seed the symbol table with already-known defs at 0 for expression use.
  for (const auto& [n, d] : state.defs) state.symAddr[n] = 0;

  std::map<std::string, std::pair<uint64_t, uint64_t>> secAddr;  // for ADDR()
  state.outSections.clear();
  std::vector<bool> consumed(state.inputSections.size(), false);

  auto takeMatches = [&](const script::SectionPattern& pat) {
    std::vector<size_t> hits;
    for (size_t i = 0; i < state.inputSections.size(); ++i) {
      if (consumed[i]) continue;
      const auto& in = state.inputSections[i];
      if (!fileMatches(pat.file, state.objects[in.objectIndex].label))
        continue;
      if (!globMatch(pat.section, in.section.name)) continue;
      hits.push_back(i);
    }
    return hits;
  };

  // No SECTIONS: only MEMORY/ENTRY mattered; fall back to default order.
  if (!sf.hasSections) {
    // Reuse default layout by clearing script state and delegating. To
    // avoid recursion, inline a minimal default here.
    error = "script has no SECTIONS command";
    return false;
  }

  EvalCtx ctx;
  ctx.syms = &state.symAddr;
  ctx.mem = &mem;
  ctx.secs = &secAddr;
  ctx.dot = dot;

  uint32_t nextIndex = 1;
  for (const auto& st : sf.sectionsBody) {
    if (auto* a = dynamic_cast<script::AssignStmt*>(st.get())) {
      ctx.dot = dot;
      uint64_t v = evalExpr(a->value.get(), ctx);
      if (a->name == ".") {
        if (a->op == "=") dot = v;
        else if (a->op == "+=") dot += v;
        else if (a->op == "-=") dot -= v;
        else if (a->op == "*=") dot *= v;
        else if (a->op == "/=") dot = v ? dot / v : dot;
        else if (a->op == "&=") dot &= v;
        else if (a->op == "|=") dot |= v;
      } else {
        bool exists = state.symAddr.count(a->name) != 0;
        if (a->provide && exists) continue;  // PROVIDE: only when undefined
        uint64_t cur = exists ? state.symAddr[a->name] : 0;
        uint64_t nv = cur;
        if (a->op == "=") nv = v;
        else if (a->op == "+=") nv = cur + v;
        else if (a->op == "-=") nv = cur - v;
        else if (a->op == "*=") nv = cur * v;
        else if (a->op == "/=") nv = v ? cur / v : cur;
        else if (a->op == "&=") nv = cur & v;
        else if (a->op == "|=") nv = cur | v;
        state.symAddr[a->name] = nv;
        // A script assignment also defines the symbol for the final table
        // when no object defined it.
        if (!state.defs.count(a->name)) {
          Def d;
          d.objectIndex = 0;
          d.symbolIndex = 0;
          d.symbol.name = a->name;
          d.symbol.value = nv;
          d.symbol.binding = qbfd::SymbolBinding::Global;
          d.symbol.flags = qbfd::sym::Absolute;
          state.defs.emplace(a->name, std::move(d));
        } else {
          // Update absolute script symbols to the assigned value.
          auto& dd = state.defs[a->name];
          if (dd.symbol.flags & qbfd::sym::Absolute) dd.symbol.value = nv;
        }
      }
      ctx.dot = dot;
      continue;
    }
    if (auto* as = dynamic_cast<script::AssertStmt*>(st.get())) {
      ctx.dot = dot;
      if (!evalExpr(as->cond.get(), ctx)) {
        error = "ASSERT failed" +
                (as->message.empty() ? "" : std::string(": ") + as->message);
        return false;
      }
      continue;
    }
    auto* os = dynamic_cast<script::OutputSectionStmt*>(st.get());
    if (!os) continue;
    const auto& sec = os->section;
    if (sec.name == "/DISCARD/") {
      for (const auto& pat : sec.inputs) {
        for (size_t hit : takeMatches(pat)) {
          consumed[hit] = true;
          state.inputSections[hit].kept = false;
        }
      }
      continue;
    }
    // Address: explicit expression, region cursor, or current dot.
    uint64_t vma = dot;
    ctx.dot = dot;
    if (sec.address) vma = evalExpr(sec.address.get(), ctx);
    if (!sec.region.empty()) {
      auto it = memCursor.find(sec.region);
      if (it == memCursor.end()) {
        error = "unknown memory region `" + sec.region + "'";
        return false;
      }
      if (!sec.address) vma = it->second;
    }
    if (sec.alignExpr) {
      uint64_t a = evalExpr(sec.alignExpr.get(), ctx);
      vma = alignUp(vma, a ? a : 1);
    }
    // Gather members.
    OutputSection out;
    out.name = sec.name;
    out.index = nextIndex++;
    out.alignment = 1;
    // Type NOLOAD: BSS-like, occupies VMA but no file bytes.
    bool noload = (sec.type == "NOLOAD");
    for (const auto& pat : sec.inputs) {
      if (pat.file == "=assign") {
        // In-section symbol assignment at current dot.
        uint64_t base2 = vma;
        // Compute running size so far.
        uint64_t run = 0;
        for (size_t idx : out.inputs)
          run = alignUp(run, state.inputSections[idx].section.alignment
                                 ? state.inputSections[idx].section.alignment
                                 : 1) +
                std::max<uint64_t>(state.inputSections[idx].section.size,
                                   state.inputSections[idx].contents.size());
        // Reconstruct expression from dump (number or symbol ops may have
        // been stringified; re-parse it).
        script::ExprPtr e;
        {
          script::Parser pp(pat.section);
          // pat.section holds the dumped expression; parse via a synthetic
          // script `x = <expr>;` is overkill — parse directly.
          (void)pp;
        }
        // Evaluate simple cases: number, symbol, or '.'-relative. The dump
        // is re-parsed through parseText for correctness.
        script::ScriptFile tmp;
        std::string err2;
        uint64_t v = base2 + run;
        if (script::parseText("SECTIONS { . = " + pat.section + "; }", tmp,
                              err2) &&
            !tmp.sectionsBody.empty()) {
          if (auto* aa =
                  dynamic_cast<script::AssignStmt*>(tmp.sectionsBody[0].get())) {
            EvalCtx c2 = ctx;
            c2.dot = base2 + run;
            // Expose section size context minimally.
            v = evalExpr(aa->value.get(), c2);
          }
        }
        bool exists = state.symAddr.count(pat.sort) != 0;
        state.symAddr[pat.sort] = v;
        if (!state.defs.count(pat.sort)) {
          Def d;
          d.symbol.name = pat.sort;
          d.symbol.binding = qbfd::SymbolBinding::Global;
          d.symbol.flags = qbfd::sym::Absolute;
          d.symbol.value = v;
          state.defs.emplace(pat.sort, std::move(d));
        }
        continue;
      }
      for (size_t hit : takeMatches(pat)) {
        consumed[hit] = true;
        auto& in = state.inputSections[hit];
        in.kept = true;
        uint64_t a = in.section.alignment ? in.section.alignment : 1;
        out.alignment = std::max(out.alignment, a);
        out.flags |= in.section.flags;
        out.inputs.push_back(hit);
      }
    }
    // Lay out members within the section.
    uint64_t off = 0;
    for (size_t idx : out.inputs) {
      auto& in = state.inputSections[idx];
      uint64_t a = in.section.alignment ? in.section.alignment : 1;
      off = alignUp(off, a);
      in.outOffset = off;
      off += std::max<uint64_t>(in.section.size, in.contents.size());
    }
    out.size = off;
    bool anyFile = false;
    for (size_t idx : out.inputs) {
      if (!(state.inputSections[idx].section.flags & qbfd::sec::Bss)) {
        anyFile = true;
        break;
      }
    }
    out.bss = noload || (!out.inputs.empty() && !anyFile);
    // Empty script sections (no inputs) still exist with size 0 at dot.
    out.vma = vma;
    out.alloc = true;  // script sections are alloc by default
    // Heuristic flags when empty.
    if (out.inputs.empty()) {
      if (out.name == ".bss" || out.name.starts_with(".bss"))
        out.flags |= qbfd::sec::Bss | qbfd::sec::Alloc | qbfd::sec::Writable;
      else if (out.name == ".text" || out.name.starts_with(".text"))
        out.flags |= qbfd::sec::Alloc | qbfd::sec::Code | qbfd::sec::Exec;
      else if (out.name == ".data" || out.name.starts_with(".data"))
        out.flags |=
            qbfd::sec::Alloc | qbfd::sec::Data | qbfd::sec::Writable;
      else
        out.flags |= qbfd::sec::Alloc;
      out.alloc = true;
    }
    out.executable = (out.flags & (qbfd::sec::Code | qbfd::sec::Exec)) != 0;
    out.writable = (out.flags & qbfd::sec::Writable) != 0 ||
                   out.name == ".data" || out.name == ".bss";
    // File offsets: BSS/NOLOAD occupy no file space; others follow dot
    // linearly like the default layout.
    out.fileSize = out.bss ? 0 : out.size;
    secAddr[out.name] = {out.vma, out.size};
    dot = vma + out.size;
    if (!sec.region.empty()) memCursor[sec.region] = dot;
    state.outSections.push_back(std::move(out));
  }

  // Orphan sections (not consumed by the script) go after, like GNU ld.
  {
    std::map<std::string, std::vector<size_t>> orphans;
    std::vector<std::string> oorder;
    for (size_t i = 0; i < state.inputSections.size(); ++i) {
      if (consumed[i]) continue;
      auto& in = state.inputSections[i];
      in.kept = true;
      std::string name = in.section.name.empty() ? ".orphan" : in.section.name;
      if (!orphans.count(name)) oorder.push_back(name);
      orphans[name].push_back(i);
    }
    for (const auto& name : oorder) {
      OutputSection out;
      out.name = name;
      out.index = nextIndex++;
      out.alignment = 1;
      for (size_t idx : orphans[name]) {
        auto& in = state.inputSections[idx];
        out.alignment =
            std::max(out.alignment, in.section.alignment
                                        ? in.section.alignment
                                        : uint64_t(1));
        out.flags |= in.section.flags;
        out.inputs.push_back(idx);
      }
      uint64_t off = 0;
      for (size_t idx : out.inputs) {
        auto& in = state.inputSections[idx];
        off = alignUp(off, in.section.alignment ? in.section.alignment : 1);
        in.outOffset = off;
        off += std::max<uint64_t>(in.section.size, in.contents.size());
      }
      out.size = off;
      bool anyFile = false;
      for (size_t idx : out.inputs) {
        if (!(state.inputSections[idx].section.flags & qbfd::sec::Bss)) {
          anyFile = true;
          break;
        }
      }
      out.bss = !anyFile && !out.inputs.empty();
      out.vma = alignUp(dot, out.alignment);
      dot = out.vma + out.size;
      out.fileSize = out.bss ? 0 : out.size;
      out.alloc = (out.flags & qbfd::sec::Alloc) != 0;
      if (out.inputs.empty()) out.alloc = false;
      else if (name.starts_with(".debug") || name == ".comment")
        out.alloc = false;
      out.executable = (out.flags & (qbfd::sec::Code | qbfd::sec::Exec)) != 0;
      out.writable = (out.flags & qbfd::sec::Writable) != 0;
      secAddr[out.name] = {out.vma, out.size};
      state.outSections.push_back(std::move(out));
    }
  }

  // File offsets for alloc sections: VMA - base (first LOAD) style.
  // For a fixed base the file offset must stay congruent to the VMA modulo
  // the page size, otherwise the kernel maps the entry to the wrong bytes.
  // Setting fileOffset = VMA - base for every file-backed section keeps
  // that congruence exactly (gaps in VMA and file stay identical).
  {
    uint64_t f = headerEnd;
    if (base != 0) {
      for (auto& o : state.outSections) {
        if (!o.alloc) continue;
        if (o.bss) {
          o.fileOffset = o.vma - base;
          continue;
        }
        o.fileOffset = o.vma - base;
      }
      // Non-alloc sections follow the last alloc file end.
      uint64_t end = headerEnd;
      for (const auto& o : state.outSections) {
        if (!o.alloc || o.bss) continue;
        end = std::max(end, o.fileOffset + o.fileSize);
      }
      for (auto& o : state.outSections) {
        if (o.alloc) continue;
        end = alignUp(end, std::max<uint64_t>(o.alignment, 1));
        o.fileOffset = end;
        end += o.fileSize;
      }
    } else {
      for (auto& o : state.outSections) {
        if (!o.alloc) continue;
        if (o.bss) {
          o.fileOffset = f;
          continue;
        }
        f = alignUp(f, std::max<uint64_t>(o.alignment, 1));
        o.fileOffset = f;
        f += o.fileSize;
      }
      // Non-alloc follow.
      for (auto& o : state.outSections) {
        if (o.alloc) continue;
        f = alignUp(f, std::max<uint64_t>(o.alignment, 1));
        o.fileOffset = f;
        f += o.fileSize;
      }
    }
  }

  // Symbol addresses (same rules as the default layout).
  for (const auto& [name, d] : state.defs) {
    if (d.common) continue;
    if (state.symAddr.count(name) &&
        (d.symbol.flags & qbfd::sym::Absolute))
      continue;  // script-assigned absolute already set
    const auto& sym = d.symbol;
    if (sym.flags & qbfd::sym::Absolute) {
      if (!state.symAddr.count(name)) state.symAddr[name] = sym.value;
      continue;
    }
    if (!sym.section) {
      if (!state.symAddr.count(name) || state.symAddr[name] == 0)
        state.symAddr[name] = sym.value;
      continue;
    }
    bool found = false;
    for (const auto& o : state.outSections) {
      for (size_t idx : o.inputs) {
        const auto& in = state.inputSections[idx];
        if (in.objectIndex == d.objectIndex &&
            in.sectionIndex == *sym.section) {
          state.symAddr[name] = o.vma + in.outOffset + sym.value;
          found = true;
          break;
        }
      }
      if (found) break;
    }
    if (!found && !state.symAddr.count(name)) state.symAddr[name] = sym.value;
  }
  // COMMONs to .bss tail (create .bss if missing).
  {
    bool need = false;
    for (const auto& [n, d] : state.defs)
      if (d.common) need = true;
    if (need && !secAddr.count(".bss")) {
      OutputSection b;
      b.name = ".bss";
      b.index = nextIndex++;
      b.flags =
          qbfd::sec::Alloc | qbfd::sec::Writable | qbfd::sec::Bss |
          qbfd::sec::Data;
      b.alignment = 8;
      b.alloc = true;
      b.bss = true;
      b.vma = dot;
      b.fileSize = 0;
      b.size = 0;
      state.outSections.push_back(std::move(b));
      secAddr[".bss"] = {dot, 0};
    }
    for (auto& o : state.outSections) {
      if (o.name != ".bss") continue;
      uint64_t tail = o.size;
      for (auto& [name, d] : state.defs) {
        if (!d.common) continue;
        tail = alignUp(tail, d.commonAlign ? d.commonAlign : 1);
        state.symAddr[name] = o.vma + tail;
        tail += d.commonSize;
      }
      o.size = tail;
      secAddr[".bss"] = {o.vma, o.size};
      break;
    }
  }

  // Entry.
  {
    auto it = state.symAddr.find(state.options.entry);
    state.entryAddr = it == state.symAddr.end() ? 0 : it->second;
  }
  return true;
}

}  // namespace qld::detail
