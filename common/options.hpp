// options.hpp - a small POSIX/GNU-style command-line parser.
//
// Every tool in this package needs the same handful of things: clustered short
// flags (`-Ss`), long options (`--strip-debug`), long options with an inline
// value (`-n=4`, `--width=4`), a separate value (`-n 4`, `--width 4`),
// `--` to end options, and a usage string. Six independent hand-rolled loops
// got that subtly different; this is the one place it is defined.
#pragma once

#include <algorithm>
#include <cstdint>
#include <exception>
#include <map>
#include <stdexcept>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace qobj {

// One recognised option. `shortLetter` is '\0' for a long-only option.
struct OptionSpec {
  char shortLetter = 0;
  std::string_view longName;  // without the leading "--"
  bool takesValue = false;
  std::string_view valueHint;  // shown in the usage line, e.g. "length"
  std::string_view help;
};

class Options {
 public:
  Options(std::string_view program, std::string_view synopsis,
          std::vector<OptionSpec> specs)
      : program_(program), synopsis_(synopsis), specs_(std::move(specs)) {}

  // A group of flags that may be given more than once and collect their values.
  class List {
   public:
    void add(std::string value) { values_.push_back(std::move(value)); }
    const std::vector<std::string>& values() const { return values_; }
    bool empty() const { return values_.empty(); }
    size_t size() const { return values_.size(); }
    bool contains(std::string_view v) const {
      return std::find(values_.begin(), values_.end(), v) != values_.end();
    }

   private:
    std::vector<std::string> values_;
  };

  // Parses argv[1..argc). Options land in `flags` (count) or `values` (last
  // wins). Everything else, and every value taken, goes to `positional()`.
  //
  // Returns false and fills `error()` on an unknown option or a missing value.
  // `--` stops option processing; a bare `-` is a positional (standard input).
  bool parse(int argc, char** argv) {
    // A spec is reachable by both spellings, and key() resolves to the short
    // letter, so registering only one of them would make the other unlookupable.
    for (const auto& s : specs_) {
      if (s.shortLetter) index_[std::string(1, s.shortLetter)] = &s;
      if (!s.longName.empty()) index_["--" + std::string(s.longName)] = &s;
    }
    for (int i = 1; i < argc; ++i) {
      std::string arg = argv[i];
      if (endOfOptions_) {
        positional_.push_back(std::move(arg));
        continue;
      }
      if (arg == "--") {
        endOfOptions_ = true;
        continue;
      }
      // takeValue() consumes the next argv entry for options given as two
      // arguments, so it needs the vector and the index it is looking at.
      current_ = i;
      argc_ = argc;
      argv_ = argv;
      if (arg.size() > 2 && arg[0] == '-' && arg[1] == '-') {
        if (!longOption(arg)) return false;
        // The option may have eaten the next entry as its value.
        i = current_;
        continue;
      }
      if (arg.size() > 1 && arg[0] == '-') {
        if (!shortCluster(arg)) return false;
        i = current_;
        continue;
      }
      positional_.push_back(std::move(arg));
    }
    return true;
  }

  // Number of times a flag was given.
  int count(const OptionSpec& spec) const { return countOf(key(spec)); }
  bool has(const OptionSpec& spec) const { return count(spec) > 0; }
  bool has(char shortLetter) const { return countOf(std::string(1, shortLetter)) > 0; }
  bool has(std::string_view longName) const {
    const OptionSpec* s = find("--" + std::string(longName));
    return s && countOf(key(*s)) > 0;
  }

  // The most recent value, or `fallback` when the option was absent.
  const std::string& value(const OptionSpec& spec,
                           const std::string& fallback = empty()) const {
    auto it = values_.find(key(spec));
    return it == values_.end() ? fallback : it->second;
  }
  // The value as a number, with `ok` reporting whether it parsed.
  uint64_t number(const OptionSpec& spec, uint64_t fallback, bool* ok) const {
    auto it = values_.find(key(spec));
    if (it == values_.end()) return fallback;
    try {
      size_t used = 0;
      uint64_t v = std::stoull(it->second, &used, 0);
      if (used != it->second.size()) throw std::invalid_argument("trailing");
      *ok = true;
      return v;
    } catch (const std::exception&) {
      *ok = false;
      return fallback;
    }
  }
  // The value of a long-only option, looked up by name.
  const std::string& value(std::string_view longName,
                           const std::string& fallback = empty()) const {
    const OptionSpec* s = find("--" + std::string(longName));
    if (!s) return fallback;
    auto it = values_.find(key(*s));
    return it == values_.end() ? fallback : it->second;
  }

  // Every value given, in order. Useful for options that may repeat.
  const std::vector<std::string>& valueList(const OptionSpec& spec) const {
    static const std::vector<std::string> none;
    auto it = lists_.find(key(spec));
    return it == lists_.end() ? none : it->second;
  }
  const std::vector<std::string>& valueList(std::string_view longName) const {
    static const std::vector<std::string> none;
    const OptionSpec* s = find("--" + std::string(longName));
    if (!s) return none;
    auto it = lists_.find(key(*s));
    return it == lists_.end() ? none : it->second;
  }

  const std::vector<std::string>& positional() const { return positional_; }
  const std::string& error() const { return error_; }

  // A usage line, generated from the specs so it cannot drift from the parser.
  std::string usage() const {
    std::string out = "usage: ";
    out += program_;
    out += ' ';
    out += synopsis_;
    for (const auto& s : specs_) {
      out += " [";
      if (s.shortLetter) {
        out += '-';
        out += s.shortLetter;
      } else {
        out += "--";
        out += s.longName;
      }
      if (s.takesValue) {
        out += s.shortLetter ? " " : "=";
        if (!s.valueHint.empty()) out += s.valueHint;
      }
      out += ']';
    }
    out += '\n';
    if (!specs_.empty()) out += '\n';
    for (const auto& s : specs_) {
      std::string left = "  ";
      if (s.shortLetter) {
        left += '-';
        left += s.shortLetter;
      } else {
        left += "    ";
      }
      if (!s.longName.empty()) {
        if (s.shortLetter) left += ", ";
        left += "--";
        left += s.longName;
      }
      if (s.takesValue) {
        left += s.shortLetter ? " " : "=";
        left += s.valueHint;
      }
      while (left.size() < 30) left += ' ';
      out += left;
      out += s.help;
      out += '\n';
    }
    return out;
  }

 private:
  static const std::string& empty() {
    static const std::string none;
    return none;
  }

  static std::string key(const OptionSpec& s) {
    return s.shortLetter ? std::string(1, s.shortLetter)
                         : "--" + std::string(s.longName);
  }
  const OptionSpec* find(const std::string& k) const {
    auto it = index_.find(k);
    return it == index_.end() ? nullptr : it->second;
  }
  int countOf(const std::string& k) const {
    auto it = counts_.find(k);
    return it == counts_.end() ? 0 : it->second;
  }

  void record(const OptionSpec& s, const std::string* v) {
    counts_[key(s)]++;
    if (v) {
      values_[key(s)] = *v;
      lists_[key(s)].push_back(*v);
    }
  }

  bool takeValue(const OptionSpec& s, const std::string& inlineValue,
                 bool hasInline) {
    if (hasInline) {
      record(s, &inlineValue);
      return true;
    }
    if (current_ + 1 >= argc_) {
      error_ = std::string("option requires a value: -") + s.shortLetter;
      return false;
    }
    std::string v = argv_[++current_];
    record(s, &v);
    return true;
  }

  bool longOption(const std::string& arg) {
    std::string body = arg.substr(2);
    std::string name = body, inlineValue;
    bool hasInline = false;
    if (auto eq = body.find('='); eq != std::string::npos) {
      name = body.substr(0, eq);
      inlineValue = body.substr(eq + 1);
      hasInline = true;
    }
    const OptionSpec* s = find("--" + name);
    if (!s) {
      error_ = "unknown option: --" + name;
      return false;
    }
    if (s->takesValue) return takeValue(*s, inlineValue, hasInline);
    if (hasInline) {
      error_ = "option does not take a value: --" + name;
      return false;
    }
    record(*s, nullptr);
    return true;
  }

  bool shortCluster(const std::string& arg) {
    for (size_t i = 1; i < arg.size(); ++i) {
      const OptionSpec* s = find(std::string(1, arg[i]));
      if (!s) {
        error_ = std::string("unknown option: -") + arg[i];
        return false;
      }
      if (s->takesValue) {
        // The rest of the cluster is the value: -n4 and -n 4 both work.
        std::string rest = arg.substr(i + 1);
        if (!rest.empty() && rest[0] == '=') rest.erase(0, 1);
        return takeValue(*s, rest, !rest.empty());
      }
      record(*s, nullptr);
    }
    return true;
  }

  std::string program_, synopsis_, error_;
  std::vector<OptionSpec> specs_;
  std::map<std::string, const OptionSpec*> index_;
  std::map<std::string, int> counts_;
  std::map<std::string, std::string> values_;
  std::map<std::string, std::vector<std::string>> lists_;
  std::vector<std::string> positional_;
  std::vector<std::string> emptyStrings_;
  bool endOfOptions_ = false;
  int current_ = 0, argc_ = 0;
  char** argv_ = nullptr;
};

}  // namespace qobj
