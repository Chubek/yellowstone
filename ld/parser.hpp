/**
 * @file DomDSL.hpp
 * @brief Header-only DSL construction toolkit for modern C++ (C++20)
 *
 * ============================================================
 *  OVERVIEW
 * ============================================================
 *
 * DomDSL.hpp provides a set of composable "feature mixins" that you
 * attach to a CRTP base class to build Domain-Specific Languages entirely
 * in headers, with zero runtime overhead where possible.
 *
 * Core idea:
 *
 *   struct MyDSL : dsl::DSL<MyDSL, Feature1, Feature2, ...> { ... };
 *
 * Available features:
 *   - dsl::Pipeline       — operator| chaining (pipe stages)
 *   - dsl::Operators      — composable predicates (&, |, !)
 *   - dsl::PatternMatch   — compile-time pattern matching
 * (match/when/otherwise)
 *   - dsl::AST            — value-type AST nodes (leaf/node/dump)
 *   - dsl::Rewrite        — AST rewrite rules
 *   - dsl::ExprTemplates  — lazy expression trees with fused eval
 *   - dsl::CustomLiterals — static constexpr literal suffix registration
 *
 * ============================================================
 *  QUICK START
 * ============================================================
 *
 * Step 1 — Pick your features:
 *
 *   struct MathDSL : dsl::DSL<MathDSL, dsl::Pipeline, dsl::Operators> {};
 * Step 2 — Use it:
 *
 *   MathDSL m;
 *   auto result = m.eval(5.0);
 *
 * Step 3 — Expose to users via a thin wrapper header (mydsl.hpp):
 *
 *   #include "DomDSL.hpp"
 *   struct MathDSL : dsl::DSL<MathDSL, dsl::Pipeline, dsl::Operators> { ... };
 *
 * Users only include mydsl.hpp — they never see DomDSL.hpp directly.
 *
 * ============================================================
 *  FEATURE REFERENCE
 * ============================================================
 *
 * --- Pipeline ---
 *
 *   Adds operator| so values can flow through a chain of transformations.
 *
 *   Usage:
 *     auto result = value | dsl::pipe(f1) | dsl::pipe(f2);
 *     // equivalent to: f2(f1(value))
 *
 *   Example:
 *     struct Proc : dsl::DSL<Proc, dsl::Pipeline> {};
 *     Proc p;
 *     auto r = p.wrap(10)
 *             | dsl::pipe([](int x){ return x * 2; })
 *             | dsl::pipe([](int x){ return x + 1; });
 *     // r == 21
 *
 * --- Operators ---
 *
 *   Adds &, |, ! for composing predicates/validators.
 *
 *   Usage:
 *     auto combined = pred_a & pred_b;   // both must pass
 *     auto either   = pred_a | pred_b;   // at least one must pass
 *     auto negated  = !pred_a;
 *
 *   Example:
 *     auto is_positive = dsl::predicate([](int x){ return x > 0; });
 *     auto is_even     = dsl::predicate([](int x){ return x % 2 == 0; });
 *     auto rule = is_positive & is_even;
 *     rule(4);   // true
 *     rule(-2);  // false
 *
 * --- PatternMatch ---
 *
 *   Compile-time dispatch table built from when<>/otherwise clauses.
 *
 *   Usage:
 *     auto table = dsl::match(
 *         dsl::when<Key1>(handler1),
 *         dsl::when<Key2>(handler2),
 *         dsl::otherwise(fallback)
 *     );
 *     auto result = table(key, args...);
 *
 *   For string patterns (CTRE-style, limited):
 *     auto table = dsl::match(
 *         dsl::when<dsl::pattern<"[0-9]+">>(handler),
 *         dsl::otherwise(fallback)
 *     );
 *
 *   Example:
 *     enum class Color { Red, Green, Blue };
 *     auto name = dsl::match(
 *         dsl::when<Color::Red>  ([](auto){ return "red";   }),
 *         dsl::when<Color::Green>([](auto){ return "green"; }),
 *         dsl::otherwise         ([](auto){ return "other"; })
 *     );
 *     name(Color::Red, 0);  // "red"
 *
 * --- AST ---
 *
 *   Value-type AST nodes. Leaves hold data; inner nodes hold children.
 *
 *   Usage:
 *     auto leaf_node  = dsl::leaf<"name">(value);
 *     auto inner_node = dsl::node<"op">(child1, child2);
 *     std::cout << node.dump();   // S-expression string
 *
 *   Example:
 *     auto expr = dsl::node<"add">(
 *         dsl::leaf<"num">(1),
 *         dsl::leaf<"num">(2)
 *     );
 *     expr.dump();  // "(add (num 1) (num 2))"
 *
 * --- Rewrite ---
 *
 *   Apply transformation rules to an AST until a fixpoint is reached.
 *
 *   Usage:
 *     auto rules = dsl::rewrite_set(
 *         dsl::rule<"name">(predicate, transformer)
 *     );
 *     auto new_tree = rules.apply(tree);
 *
 *   Example:
 *     auto rules = dsl::rewrite_set(
 *         dsl::rule<"double-neg">(
 *             [](auto& t){ return t.tag() == "not" && t.child(0).tag() ==
 * "not"; },
 *             [](auto& t){ return t.child(0).child(0); }  // remove double
 * negation
 *         )
 *     );
 *
 * --- ExprTemplates ---
 *
 *   Lazy expression trees. Operators build a tree; .eval() fuses it.
 *
 *   Usage:
 *     auto expr = a + b * c;   // no computation yet
 *     auto val  = expr.eval(); // single fused pass
 *
 *   Example:
 *     struct Vec : dsl::DSL<Vec, dsl::ExprTemplates> {
 *         std::vector<float> data;
 *     };
 *     Vec a{{1,2,3}}, b{{4,5,6}}, c{{7,8,9}};
 *     auto expr = a + b * c;
 *     auto result = expr.eval();  // {1+4*7, 2+5*8, 3+6*9} = {29, 42, 57}
 *
 * --- CustomLiterals ---
 *
 *   Register constexpr literal suffixes via a static table.
 *
 *   Usage:
 *     static constexpr auto literals = dsl::literal_set(
 *         dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *         dsl::lit<"_m"> ([](long double v){ return v; })
 *     );
 *     auto d = MyDSL::parse_literal("3.5_km");  // 3500.0
 *
 *   For native C++ UDL syntax (3.5_km), define operator"" in an
 *   inline namespace in your own header — see examples below.
 *
 * --- Memoization ---
 *
 *   Caches expensive pure-function results by hashable key.
 *
 *   Usage:
 *     auto f = dsl::memoize<int, int>([](int x){ return x * x; });
 *
 *   Example:
 *     auto fib = dsl::memoize<int, long long>([](int n){ return n < 2 ? n :
 * 1LL; }); auto v = fib(20);
 *
 * --- Lazy ---
 *
 *   Defers one computation until first get()/force() and caches it.
 *
 *   Usage:
 *     dsl::Lazy<int> v([]{ return 42; });
 *
 *   Example:
 *     dsl::Lazy<std::string> cfg([]{ return std::string("ready"); });
 *     auto s = cfg.get();
 *
 * --- Monadic ---
 *
 *   Optional-aware map/flat_map/filter/or_else for Maybe and std::optional.
 *
 *   Usage:
 *     auto out = dsl::Maybe<int>(5).map([](int x){ return x + 1;
 * }).or_else(0);
 *
 *   Example:
 *     std::optional<int> v = 3;
 *     auto out = dsl::map(v, [](int x){ return x * 2; });
 *
 * --- Result ---
 *
 *   Explicit success/error flow modeled as Result<T,E>.
 *
 *   Usage:
 *     auto r = dsl::Result<int,std::string>::from_ok(7);
 *
 *   Example:
 *     auto r = dsl::Result<int,std::string>::from_ok(4).map([](int x){ return
 * x+1; });
 *
 * --- CombinatorParser ---
 *
 *   Functional parser core + combinators (`|`, `&`, `*`, optional()).
 *
 *   Usage:
 *     auto p = dsl::parser([](dsl::ParsecInput&
 * in)->dsl::ExpectedResult<char>{...});
 *
 *   Example:
 *     auto seq = p1 & p2;
 *
 * --- TaskPipeline ---
 *
 *   Composes tasks with `|` and executes with shared TaskState.
 *
 *   Usage:
 *     auto chain = t1 | t2; dsl::run(state, chain);
 *
 *   Example:
 *     dsl::TaskState st; dsl::run(st, chain);
 *
 * ============================================================
 *  FULL EXAMPLES
 * ============================================================
 *
 * Example A — UnitsDSL (CustomLiterals):
 *
 *   struct UnitsDSL : dsl::DSL<UnitsDSL, dsl::CustomLiterals> {
 *       static constexpr auto literals = dsl::literal_set(
 *           dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *           dsl::lit<"_m"> ([](long double v){ return v; }),
 *           dsl::lit<"_cm">([](long double v){ return v / 100.0L; })
 *       );
 *   };
 *   inline namespace unit_literals {
 *       constexpr long double operator""_km(long double v){ return v *
 * 1000.0L; } constexpr long double operator""_m (long double v){ return v; }
 *   }
 *   // Usage:
 *   using namespace unit_literals;
 *   auto d = 3.5_km;  // 3500.0
 *
 * Example B — StateMachineDSL (Pipeline + PatternMatch):
 *
 *   struct SM : dsl::DSL<SM, dsl::Pipeline, dsl::PatternMatch> {
 *       enum class S { Idle, Running, Done };
 *       static constexpr auto transitions = dsl::match(
 *           dsl::when<S::Idle>   ([](auto e){ return
 * e=="start"?S::Running:S::Idle; }), dsl::when<S::Running>([](auto e){ return
 * e=="stop" ?S::Done   :S::Running; }), dsl::otherwise       ([](auto  ){
 * return S::Done; })
 *       );
 *       S state = S::Idle;
 *       void send(std::string_view e){ state = transitions(state, e); }
 *   };
 *   // Usage:
 *   SM sm;
 *   sm.send("start");  // state == Running
 *   sm.send("stop");   // state == Done
 *
 * Example C — VecDSL (ExprTemplates):
 *
 *   struct VecDSL : dsl::DSL<VecDSL, dsl::ExprTemplates> {
 *       std::vector<float> data;
 *   };
 *   // Usage:
 *   VecDSL a{{1,2,3}}, b{{4,5,6}};
 *   auto expr   = a + b;          // lazy
 *   auto result = expr.eval();    // {5, 7, 9}
 *
 * Example D — QueryDSL (AST + Rewrite):
 *
 *   struct QueryDSL : dsl::DSL<QueryDSL, dsl::AST, dsl::Rewrite> {
 *       auto select(auto... cols) { return
 * dsl::node<"select">(dsl::leaf<"col">(cols)...); } auto from  (auto table)  {
 * return dsl::leaf<"from">(table); }
 *   };
 *   // Usage:
 *   QueryDSL q;
 *   auto tree = q.select("name","age") | q.from("users");
 *   std::cout << tree.dump();
 *   // "(select (col name) (col age) (from users))"
 *
 * Example E — Memoization:
 *   auto sq = dsl::memoize<int,int>([](int x){ return x*x; });
 *   auto v = sq(12);
 *
 * Example F — Lazy + Monadic:
 *   dsl::Lazy<dsl::Maybe<int>> lazy([]{ return dsl::Maybe<int>(8); });
 *   auto out = lazy.get().map([](int x){ return x+1; }).or_else(0);
 *
 * Example G — Result:
 *   auto r = dsl::Result<int,std::string>::from_ok(9).map([](int x){ return
 * x*2; });
 *
 * Example H — Parser + TaskPipeline:
 *   dsl::ParsecInput in{"abc",0};
 *   auto p = dsl::parser([](dsl::ParsecInput& i)->dsl::ExpectedResult<char>{
 * return i.consume(); }); dsl::Task t{"noop", [](dsl::TaskState&){ return
 * dsl::Result<std::string,std::string>::from_ok("ok"); }};
 *
 * ============================================================
 *  DESIGN NOTES
 * ============================================================
 *
 * 1. CRTP avoids virtual dispatch — all polymorphism is compile-time.
 * 2. Features are orthogonal: adding Pipeline does not affect PatternMatch.
 * 3. NTTPs (non-type template parameters) are used for string tags and
 *    pattern strings, enabling constexpr matching with zero overhead.
 * 4. AST nodes are value types (copyable, movable) — no heap allocation
 *    for small trees (uses std::variant + std::vector internally).
 * 5. ExprTemplates use recursive template structs; .eval() is the only
 *    point where actual computation happens.
 * 6. CustomLiterals::parse_literal does a linear scan of the literal_set
 *    at runtime; for native UDL syntax prefer operator"" in your header.
 *
 * ============================================================
 *  REQUIREMENTS
 * ============================================================
 *
 *   - C++20 or later (uses concepts, NTTPs, constexpr lambdas)
 *   - No external dependencies
 *   - Header-only: just #include "DomDSL.hpp"
 * ============================================================
 */

#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#if __cplusplus < 202002L
#error "DomDSL.hpp requires C++20 or later, compile it with -std=c++20"
#endif

namespace dsl
{

// ============================================================
//  Utility: fixed-length string for use as NTTP
// ============================================================

/**
 * @brief Compile-time fixed-length string, usable as a non-type template
 * parameter.
 *
 * Allows string literals to be used as template arguments:
 *   dsl::leaf<"myTag">(value)
 *   dsl::pattern<"[0-9]+">()
 *
 * @tparam N Length of the string including null terminator.
 */
template <std::size_t N> struct FixedString
{
  char data[N]{};
  constexpr FixedString (const char (&s)[N]) { std::copy_n (s, N, data); }
  constexpr std::string_view
  view () const
  {
    return { data, N - 1 };
  }
  constexpr bool operator== (const FixedString &) const = default;
};

// ============================================================
//  Forward declarations
// ============================================================

struct Pipeline;
struct Operators;
struct PatternMatch;
struct AST;
struct Rewrite;
struct ExprTemplates;
struct CustomLiterals;
struct Memoization;
struct LazyFeature;
struct Monadic;
struct ResultFeature;
struct CombinatorParser;
struct TaskPipeline;

// ============================================================
//  CRTP base: DSL<Derived, Features...>
// ============================================================

/**
 * @brief CRTP base class for all user-defined DSLs.
 *
 * Inherit from this to compose features:
 *
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::Pipeline, dsl::Operators> { ... };
 *
 * The Derived type gets access to all methods provided by each Feature.
 * Features are mixed in via multiple inheritance through FeatureMixin<>.
 *
 * @tparam Derived  The user's DSL class (CRTP pattern).
 * @tparam Features Zero or more feature tags (Pipeline, Operators, etc.).
 */
template <typename Derived, typename... Features>
struct DSL : Features::template Mixin<Derived>...
{
protected:
  constexpr Derived &
  self () noexcept
  {
    return static_cast<Derived &> (*this);
  }
  constexpr const Derived &
  self () const noexcept
  {
    return static_cast<const Derived &> (*this);
  }
};

// ============================================================
//  Predicate wrapper (used by Operators)
// ============================================================

/**
 * @brief Wraps a callable as a composable predicate.
 *
 * Supports &, |, ! composition.
 *
 * @tparam F Callable type: F(auto) -> bool
 *
 * Example:
 *   auto pos  = dsl::predicate([](int x){ return x > 0; });
 *   auto even = dsl::predicate([](int x){ return x % 2 == 0; });
 *   auto rule = pos & even;
 *   rule(4);   // true
 *   rule(3);   // false (odd)
 *   rule(-2);  // false (negative)
 */
template <typename F> struct Predicate
{
  F fn;
  explicit constexpr Predicate (F f) : fn (std::move (f)) {}

  template <typename T>
  constexpr bool
  operator() (T &&v) const
  {
    return fn (std::forward<T> (v));
  }

  /// Conjunction: both predicates must pass.
  template <typename G>
  constexpr auto
  operator& (Predicate<G> other) const
  {
    return Predicate{ [a = fn, b = other.fn] (auto &&v)
                        { return a (v) && b (v); } };
  }

  /// Disjunction: at least one predicate must pass.
  template <typename G>
  constexpr auto
  operator| (Predicate<G> other) const
  {
    return Predicate{ [a = fn, b = other.fn] (auto &&v)
                        { return a (v) || b (v); } };
  }

  /// Negation.
  constexpr auto
  operator!() const
  {
    return Predicate{ [a = fn] (auto &&v) { return !a (v); } };
  }
};

/**
 * @brief Factory for Predicate<F>.
 *
 * Example:
 *   auto is_even = dsl::predicate([](int x){ return x % 2 == 0; });
 */
template <typename F>
constexpr auto
predicate (F &&f)
{
  return Predicate<std::decay_t<F>>{ std::forward<F> (f) };
}

// ============================================================
//  Pipeline stage wrapper
// ============================================================

/**
 * @brief Wraps a callable as a pipeline stage for use with operator|.
 *
 * @tparam F Callable: F(T) -> U
 *
 * Example:
 *   auto double_it = dsl::pipe([](int x){ return x * 2; });
 *   auto result = 5 | double_it;  // 10
 */
template <typename F> struct PipeStage
{
  F fn;
  explicit constexpr PipeStage (F f) : fn (std::move (f)) {}
};

/**
 * @brief Factory for PipeStage<F>.
 *
 * Example:
 *   auto stage = dsl::pipe([](int x){ return x + 1; });
 *   auto r = 4 | stage;  // 5
 */
template <typename F>
constexpr auto
pipe (F &&f)
{
  return PipeStage<std::decay_t<F>>{ std::forward<F> (f) };
}

/// operator| for any value and a PipeStage.
template <typename T, typename F>
constexpr auto
operator| (T &&val, PipeStage<F> stage)
{
  return stage.fn (std::forward<T> (val));
}

// ============================================================
//  Feature: Pipeline
// ============================================================

/**
 * @brief Feature tag: adds pipeline (operator|) support to a DSL.
 *
 * Provides a wrap() method to lift a value into the pipeline.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::Pipeline> {};
 *   MyDSL m;
 *   auto r = m.wrap(10)
 *            | dsl::pipe([](int x){ return x * 2; })
 *            | dsl::pipe([](int x){ return x - 1; });
 *   // r == 19
 */
struct Pipeline
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Lifts a value into the pipeline.
     * @param v Value to wrap.
     * @return v unchanged; subsequent | stages transform it.
     */
    template <typename T>
    static constexpr T
    wrap (T &&v)
    {
      return std::forward<T> (v);
    }
  };
};

// ============================================================
//  Feature: Operators
// ============================================================

/**
 * @brief Feature tag: adds predicate composition helpers to a DSL.
 *
 * Provides make_pred() as a named factory inside the DSL.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::Operators> {};
 *   auto rule = MyDSL::make_pred([](int x){ return x > 0; })
 *             & MyDSL::make_pred([](int x){ return x < 100; });
 *   rule(42);  // true
 *   rule(200); // false
 */
struct Operators
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Creates a composable Predicate from a callable.
     * @param f Callable: f(T) -> bool
     */
    template <typename F>
    static constexpr auto
    make_pred (F &&f)
    {
      return dsl::predicate (std::forward<F> (f));
    }
  };
};

// ============================================================
//  PatternMatch: when / otherwise / match
// ============================================================

/**
 * @brief Compile-time string pattern tag for use with when<>.
 *
 * Wraps a FixedString so it can be used as a template argument
 * to distinguish pattern-based dispatch from value-based dispatch.
 *
 * @tparam S The pattern string (e.g., "[0-9]+").
 *
 * Example:
 *   dsl::when<dsl::pattern<"[0-9]+">>(handler)
 */
template <FixedString S> struct pattern
{
  static constexpr std::string_view value = S.view ();

  /**
   * @brief Minimal regex-like match: anchored prefix/suffix, literal chars,
   * [class], *, +.
   *
   * Supported syntax (subset):
   *   .       any character
   *   [abc]   character class
   *   [a-z]   range in class
   *   *       zero or more of previous
   *   +       one or more of previous
   *   ^       (ignored, match is always anchored at start)
   *   $       (ignored, match is always anchored at end)
   *
   * @param input String to test.
   * @return true if the entire input matches the pattern.
   */
  static constexpr bool
  matches (std::string_view input)
  {
    return match_here (S.view (), input);
  }

private:
  static constexpr bool
  match_class (std::string_view cls, char c)
  {
    for (std::size_t i = 0; i < cls.size ();)
      {
        if (i + 2 < cls.size () && cls[i + 1] == '-')
          {
            if (c >= cls[i] && c <= cls[i + 2])
              return true;
            i += 3;
          }
        else
          {
            if (cls[i] == c)
              return true;
            ++i;
          }
      }
    return false;
  }

  static constexpr bool
  match_one (char pat, std::string_view cls, char c)
  {
    if (pat == '[')
      return match_class (cls, c);
    if (pat == '.')
      return true;
    return pat == c;
  }

  static constexpr bool
  match_here (std::string_view pat, std::string_view str)
  {
    if (pat.empty ())
      return str.empty ();
    if (pat[0] == '^')
      return match_here (pat.substr (1), str);

    // Parse class [...]
    std::string_view cls;
    std::size_t pat_advance = 1;
    char first = pat[0];
    if (first == '[')
      {
        auto end = pat.find (']', 1);
        if (end == std::string_view::npos)
          return false;
        cls = pat.substr (1, end - 1);
        pat_advance = end + 1;
      }

    // Check for quantifier
    char quant = 0;
    if (pat_advance < pat.size ()
        && (pat[pat_advance] == '*' || pat[pat_advance] == '+'))
      quant = pat[pat_advance++];

    auto rest = pat.substr (pat_advance);

    if (quant == '*')
      {
        // Try matching zero occurrences first, then one or more
        if (match_here (rest, str))
          return true;
        while (!str.empty () && match_one (first, cls, str[0]))
          {
            str.remove_prefix (1);
            if (match_here (rest, str))
              return true;
          }
        return false;
      }
    if (quant == '+')
      {
        // One or more
        if (str.empty () || !match_one (first, cls, str[0]))
          return false;
        str.remove_prefix (1);
        while (!str.empty () && match_one (first, cls, str[0]))
          str.remove_prefix (1);
        return match_here (rest, str);
      }

    // Exactly one
    if (str.empty () || !match_one (first, cls, str[0]))
      return false;
    return match_here (rest, str.substr (1));
  }
};

// --- when<Key> clause ---

/**
 * @brief A single dispatch clause for dsl::match().
 *
 * Two specializations:
 *   when<EnumOrIntegralValue>(handler)  — value-based dispatch
 *   when<dsl::pattern<"...">>(handler)  — pattern-based dispatch
 *
 * @tparam Key  The value or pattern to match against.
 * @tparam F    Handler callable.
 */
template <auto Key, typename F> struct WhenClause
{
  F handler;
  using handler_type_or_void = F;
  explicit constexpr WhenClause (F f) : handler (std::move (f)) {}

  template <typename K, typename... Args>
  constexpr bool
  try_invoke (K key, std::optional<std::invoke_result_t<F, K, Args...>> &out,
              Args &&...args) const
  {
    using KeyT = std::remove_cvref_t<decltype (Key)>;
    if constexpr (requires { KeyT::matches (std::string_view{}); })
      {
        if constexpr (std::convertible_to<K, std::string_view>)
          {
            if (KeyT::matches (std::string_view (key)))
              {
                out = handler (key, std::forward<Args> (args)...);
                return true;
              }
          }
      }
    else if constexpr (std::equality_comparable_with<K, decltype (Key)>)
      {
        if (key == Key)
          {
            out = handler (key, std::forward<Args> (args)...);
            return true;
          }
      }
    return false;
  }
};

/**
 * @brief Factory for a when<Key> clause.
 *
 * Example (value-based):
 *   dsl::when<Color::Red>([](auto key){ return "red"; })
 *
 * Example (pattern-based):
 *   dsl::when<dsl::pattern<"[0-9]+">>(handler)
 */
template <auto Key, typename F>
constexpr auto
when (F &&f)
{
  return WhenClause<Key, std::decay_t<F>>{ std::forward<F> (f) };
}

// --- otherwise clause ---

/**
 * @brief Fallback clause for dsl::match() when no when<> matches.
 *
 * @tparam F Handler callable.
 *
 * Example:
 *   dsl::otherwise([](auto key){ return "unknown"; })
 */
template <typename F> struct OtherwiseClause
{
  F handler;
  using handler_type_or_void = F;
  explicit constexpr OtherwiseClause (F f) : handler (std::move (f)) {}

  template <typename K, typename... Args>
  constexpr bool
  try_invoke (K, std::optional<std::invoke_result_t<F, K, Args...>> &,
              Args &&...) const
  {
    return false;
  }
};

/**
 * @brief Factory for an otherwise clause.
 */
template <typename F>
constexpr auto
otherwise (F &&f)
{
  return OtherwiseClause<std::decay_t<F>>{ std::forward<F> (f) };
}

// --- MatchTable ---

/**
 * @brief Dispatch table built from when<>/otherwise clauses.
 *
 * Created by dsl::match(). Call operator()(key, args...) to dispatch.
 *
 * @tparam Clauses  Tuple of WhenClause and OtherwiseClause types.
 *
 * Example:
 *   auto table = dsl::match(
 *       dsl::when<1>([](auto){ return "one"; }),
 *       dsl::when<2>([](auto){ return "two"; }),
 *       dsl::otherwise([](auto){ return "other"; })
 *   );
 *   table(1, 0);  // "one"
 *   table(99, 0); // "other"
 */
template <typename... Clauses> struct MatchTable
{
  std::tuple<Clauses...> clauses;
  explicit constexpr MatchTable (Clauses... cs) : clauses (std::move (cs)...)
  {
  }

  template <typename Key, typename... Args>
  constexpr auto
  operator() (Key key, Args &&...args) const
  {
    using RetT = std::common_type_t<std::invoke_result_t<
        typename Clauses::handler_type_or_void, Key, Args...>...>;
    // Try each when<> clause in order
    std::optional<RetT> result;
    bool has_otherwise = false;
    bool matched = std::apply (
        [&] (const auto &...c)
          {
            return (c.try_invoke (key, result, std::forward<Args> (args)...)
                    || ...);
          },
        clauses);
    if (!matched)
      {
        // Find and invoke the otherwise clause
        std::apply (
            [&] (const auto &...c)
              {
                ((
                     [&]
                       {
                         using Cls = std::remove_cvref_t<decltype (c)>;
                         if constexpr (std::is_same_v<
                                           Cls, OtherwiseClause<
                                                    typename Cls::
                                                        handler_type_or_void>>)
                           {
                             has_otherwise = true;
                             if (!result)
                               result = c.handler (
                                   key, std::forward<Args> (args)...);
                           }
                       }()),
                 ...);
              },
            clauses);
      }
    if (!result && !has_otherwise)
      throw std::runtime_error (
          "dsl::match: no clause matched and no otherwise");
    return *result;
  }
};

// Deduction guide
template <typename... Cs> MatchTable (Cs...) -> MatchTable<Cs...>;

/**
 * @brief Builds a MatchTable from when<>/otherwise clauses.
 *
 * Example:
 *   auto t = dsl::match(
 *       dsl::when<0>([](auto){ return "zero"; }),
 *       dsl::otherwise([](auto){ return "nonzero"; })
 *   );
 *   t(0, 0);  // "zero"
 *   t(5, 0);  // "nonzero"
 */
template <typename... Clauses>
constexpr auto
match (Clauses &&...cs)
{
  return MatchTable<std::decay_t<Clauses>...>{ std::forward<Clauses> (cs)... };
}

// ============================================================
//  Feature: PatternMatch
// ============================================================

/**
 * @brief Feature tag: adds pattern matching helpers to a DSL.
 *
 * Exposes dsl::match / dsl::when / dsl::otherwise as static methods
 * on the derived class for convenience.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::PatternMatch> {
 *       static constexpr auto table = dsl::match(
 *           dsl::when<1>([](auto){ return "one"; }),
 *           dsl::otherwise([](auto){ return "?"; })
 *       );
 *   };
 */
struct PatternMatch
{
  template <typename Derived> struct Mixin
  {
    // Feature presence marker; no additional methods needed since
    // dsl::match/when/otherwise are free functions in the dsl:: namespace.
  };
};

// ============================================================
//  AST: leaf / node / ASTNode
// ============================================================

/**
 * @brief A value-type AST node.
 *
 * Each node has a tag (compile-time string) and either:
 *   - A leaf value (string representation), or
 *   - A list of child nodes.
 *
 * Nodes are copyable, movable, and support dump() for S-expression output.
 */
class ASTNode
{
public:
  ASTNode () : tag_ (""), is_leaf_ (true) {}

  /// Construct a leaf node.
  ASTNode (std::string_view tag, std::string value)
      : tag_ (tag), value_ (std::move (value)), is_leaf_ (true)
  {
  }

  /// Construct an inner node with children.
  ASTNode (std::string_view tag, std::vector<ASTNode> children)
      : tag_ (tag), children_ (std::move (children)), is_leaf_ (false)
  {
  }

  /// @return The tag string of this node.
  std::string_view
  tag () const
  {
    return tag_;
  }

  /// @return true if this is a leaf (no children).
  bool
  is_leaf () const
  {
    return is_leaf_;
  }

  /// @return The leaf value (empty string if not a leaf).
  const std::string &
  value () const
  {
    return value_;
  }

  /// @return The children of this node (empty if leaf).
  const std::vector<ASTNode> &
  children () const
  {
    return children_;
  }

  /// @return Mutable reference to children.
  std::vector<ASTNode> &
  children ()
  {
    return children_;
  }

  /// @return The i-th child. Throws if out of range.
  const ASTNode &
  child (std::size_t i) const
  {
    return children_.at (i);
  }
  ASTNode &
  child (std::size_t i)
  {
    return children_.at (i);
  }

  /// @return Number of children.
  std::size_t
  size () const
  {
    return children_.size ();
  }

  /**
   * @brief Produces an S-expression string representation.
   *
   * Leaf:  "(tag value)"
   * Inner: "(tag child1 child2 ...)"
   *
   * Example:
   *   node<"add">(leaf<"num">(1), leaf<"num">(2)).dump()
   *   // => "(add (num 1) (num 2))"
   */
  std::string
  dump () const
  {
    std::ostringstream os;
    dump_impl (os);
    return os.str ();
  }

  bool
  operator== (const ASTNode &other) const
  {
    if (tag_ != other.tag_ || is_leaf_ != other.is_leaf_)
      return false;
    if (is_leaf_)
      return value_ == other.value_;
    return children_ == other.children_;
  }

private:
  void
  dump_impl (std::ostringstream &os) const
  {
    os << '(' << tag_;
    if (is_leaf_)
      {
        if (!value_.empty ())
          os << ' ' << value_;
      }
    else
      {
        for (auto &c : children_)
          {
            os << ' ';
            c.dump_impl (os);
          }
      }
    os << ')';
  }

  std::string tag_;
  std::string value_;
  std::vector<ASTNode> children_;
  bool is_leaf_ = true;
};

/**
 * @brief Creates a leaf AST node with the given tag and value.
 *
 * @tparam Tag Compile-time string tag.
 * @param val  The value (converted to string via ostringstream).
 * @return ASTNode leaf.
 *
 * Example:
 *   auto n = dsl::leaf<"num">(42);
 *   n.dump();  // "(num 42)"
 */
template <FixedString Tag, typename T>
ASTNode
leaf (T &&val)
{
  std::ostringstream os;
  os << std::forward<T> (val);
  return ASTNode{ Tag.view (), os.str () };
}

/// Overload for string_view values (avoids extra stream).
template <FixedString Tag>
ASTNode
leaf (std::string_view val)
{
  return ASTNode{ Tag.view (), std::string (val) };
}

/// Overload for const char* values.
template <FixedString Tag>
ASTNode
leaf (const char *val)
{
  return ASTNode{ Tag.view (), std::string (val) };
}

/**
 * @brief Creates an inner AST node with the given tag and children.
 *
 * @tparam Tag Compile-time string tag.
 * @param children  Variadic list of ASTNode children.
 * @return ASTNode inner node.
 *
 * Example:
 *   auto n = dsl::node<"add">(dsl::leaf<"x">(1), dsl::leaf<"y">(2));
 *   n.dump();  // "(add (x 1) (y 2))"
 */
template <FixedString Tag, typename... Children>
  requires (std::convertible_to<Children, ASTNode> && ...)
ASTNode
node (Children &&...children)
{
  std::vector<ASTNode> v;
  v.reserve (sizeof...(children));
  (v.emplace_back (std::forward<Children> (children)), ...);
  return ASTNode{ Tag.view (), std::move (v) };
}

// ============================================================
//  Feature: AST
// ============================================================

/**
 * @brief Feature tag: adds AST construction helpers to a DSL.
 *
 * Provides make_leaf() and make_node() as member functions.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::AST> {};
 *   MyDSL m;
 *   auto tree = dsl::node<"root">(dsl::leaf<"val">(42));
 */
struct AST
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Creates a leaf node (convenience wrapper).
     */
    template <FixedString Tag, typename T>
    static ASTNode
    make_leaf (T &&val)
    {
      return dsl::leaf<Tag> (std::forward<T> (val));
    }

    /**
     * @brief Creates an inner node (convenience wrapper).
     */
    template <FixedString Tag, typename... Children>
      requires (std::convertible_to<Children, ASTNode> && ...)
    static ASTNode
    make_node (Children &&...children)
    {
      return dsl::node<Tag> (std::forward<Children> (children)...);
    }
  };
};

// ============================================================
//  Rewrite: rule / rewrite_set
// ============================================================

/**
 * @brief A single rewrite rule: predicate + transformer.
 *
 * @tparam Tag   Name of the rule (for debugging/logging).
 * @tparam Pred  Callable: Pred(const ASTNode&) -> bool
 * @tparam Trans Callable: Trans(const ASTNode&) -> ASTNode
 *
 * Example:
 *   auto r = dsl::rule<"elim-double-neg">(
 *       [](const ASTNode& n){ return n.tag()=="not" &&
 * n.child(0).tag()=="not"; },
 *       [](const ASTNode& n){ return n.child(0).child(0); }
 *   );
 */
template <FixedString Tag, typename Pred, typename Trans> struct RewriteRule
{
  Pred pred;
  Trans trans;
  static constexpr std::string_view name = Tag.view ();

  constexpr RewriteRule (Pred p, Trans t)
      : pred (std::move (p)), trans (std::move (t))
  {
  }

  /**
   * @brief Attempts to apply this rule to a node.
   * @param n The node to test and potentially transform.
   * @return The transformed node if pred matched, or std::nullopt.
   */
  std::optional<ASTNode>
  try_apply (const ASTNode &n) const
  {
    if (pred (n))
      return trans (n);
    return std::nullopt;
  }
};

/**
 * @brief Factory for RewriteRule.
 *
 * @tparam Tag  Rule name (FixedString NTTP).
 * @param pred  Predicate: pred(const ASTNode&) -> bool
 * @param trans Transformer: trans(const ASTNode&) -> ASTNode
 */
template <FixedString Tag, typename Pred, typename Trans>
constexpr auto
rule (Pred &&p, Trans &&t)
{
  return RewriteRule<Tag, std::decay_t<Pred>, std::decay_t<Trans>>{
    std::forward<Pred> (p), std::forward<Trans> (t)
  };
}

/**
 * @brief A set of rewrite rules applied repeatedly until fixpoint.
 *
 * @tparam Rules  Variadic list of RewriteRule types.
 *
 * Example:
 *   auto rules = dsl::rewrite_set(
 *       dsl::rule<"r1">(pred1, trans1),
 *       dsl::rule<"r2">(pred2, trans2)
 *   );
 *   auto optimized = rules.apply(tree);
 */
template <typename... Rules> struct RewriteSet
{
  std::tuple<Rules...> rules;
  std::size_t max_iterations = 100;

  explicit constexpr RewriteSet (Rules... rs) : rules (std::move (rs)...) {}

  /**
   * @brief Applies all rules bottom-up until no rule fires or max iterations
   * reached.
   * @param root The AST to rewrite.
   * @return The rewritten AST.
   */
  ASTNode
  apply (ASTNode root) const
  {
    for (std::size_t i = 0; i < max_iterations; ++i)
      {
        auto [changed, result] = rewrite_once (root);
        if (!changed)
          return result;
        root = std::move (result);
      }
    return root;
  }

private:
  /// Single bottom-up pass: rewrite children first, then try rules on the
  /// node.
  std::pair<bool, ASTNode>
  rewrite_once (const ASTNode &n) const
  {
    bool any_changed = false;

    // Recursively rewrite children
    std::vector<ASTNode> new_children;
    if (!n.is_leaf ())
      {
        new_children.reserve (n.children ().size ());
        for (auto &child : n.children ())
          {
            auto [changed, rewritten] = rewrite_once (child);
            any_changed |= changed;
            new_children.push_back (std::move (rewritten));
          }
      }

    ASTNode current
        = n.is_leaf () ? n : ASTNode{ n.tag (), std::move (new_children) };

    // Try each rule on the current node
    std::apply (
        [&] (const auto &...r)
          {
            (
                [&]
                  {
                    if (auto result = r.try_apply (current))
                      {
                        current = std::move (*result);
                        any_changed = true;
                      }
                  }(),
                ...);
          },
        rules);

    return { any_changed, current };
  }
};

/**
 * @brief Factory for RewriteSet.
 *
 * Example:
 *   auto rs = dsl::rewrite_set(rule1, rule2, rule3);
 *   auto tree2 = rs.apply(tree1);
 */
template <typename... Rules>
constexpr auto
rewrite_set (Rules &&...rs)
{
  return RewriteSet<std::decay_t<Rules>...>{ std::forward<Rules> (rs)... };
}

// ============================================================
//  Feature: Rewrite
// ============================================================

/**
 * @brief Feature tag: adds rewrite capabilities to a DSL.
 *
 * Provides a rewrite() convenience method.
 *
 * Usage:
 *   struct MyDSL : dsl::DSL<MyDSL, dsl::AST, dsl::Rewrite> {
 *       static inline auto rules = dsl::rewrite_set(...);
 *   };
 *   MyDSL m;
 *   auto optimized = m.rewrite(tree);
 */
struct Rewrite
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Applies the Derived::rules rewrite set to the given tree.
     * @param tree The AST to rewrite.
     * @return Rewritten AST.
     *
     * Requires that Derived defines a static `rules` member of type
     * RewriteSet.
     */
    ASTNode
    rewrite (const ASTNode &tree) const
      requires requires { Derived::rules; }
    {
      return Derived::rules.apply (tree);
    }
  };
};

// ============================================================
//  ExprTemplates: lazy expression trees
// ============================================================

/**
 * @brief Tag type indicating a terminal (leaf) in an expression template tree.
 */
struct ExprTerminal
{
};

/**
 * @brief Binary expression node in an expression template tree.
 *
 * @tparam Op  Operation tag (e.g., std::plus<>, std::multiplies<>).
 * @tparam L   Left operand type.
 * @tparam R   Right operand type.
 *
 * Evaluation is deferred until .eval() is called.
 */
template <typename Op, typename L, typename R> struct BinExpr
{
  L lhs;
  R rhs;

  constexpr BinExpr (L l, R r) : lhs (std::move (l)), rhs (std::move (r)) {}

  /**
   * @brief Evaluates the expression tree recursively.
   *
   * For terminal nodes, returns the stored value.
   * For binary nodes, evaluates both sides and applies Op.
   */
  constexpr auto
  eval () const
  {
    auto l = eval_operand (lhs);
    auto r = eval_operand (rhs);
    return Op{}(l, r);
  }

private:
  template <typename T>
  static constexpr auto
  eval_operand (const T &operand)
  {
    if constexpr (requires { operand.eval (); })
      {
        return operand.eval ();
      }
    else if constexpr (requires { operand.expr_value (); })
      {
        return operand.expr_value ();
      }
    else
      {
        return operand;
      }
  }
};

/**
 * @brief Unary expression node.
 *
 * @tparam Op  Operation tag (e.g., std::negate<>).
 * @tparam E   Operand type.
 */
template <typename Op, typename E> struct UnaryExpr
{
  E operand;

  explicit constexpr UnaryExpr (E e) : operand (std::move (e)) {}

  constexpr auto
  eval () const
  {
    if constexpr (requires { operand.eval (); })
      {
        return Op{}(operand.eval ());
      }
    else if constexpr (requires { operand.expr_value (); })
      {
        return Op{}(operand.expr_value ());
      }
    else
      {
        return Op{}(operand);
      }
  }
};

// ============================================================
//  Feature: ExprTemplates
// ============================================================

/**
 * @brief Feature tag: adds expression template operators to a DSL.
 *
 * The derived class must provide an expr_value() method that returns
 * the underlying value for evaluation.
 *
 * Provides: operator+, operator-, operator*, operator/ (binary)
 *           operator- (unary negation)
 *
 * Usage:
 *   struct Scalar : dsl::DSL<Scalar, dsl::ExprTemplates> {
 *       double val;
 *       double expr_value() const { return val; }
 *   };
 *   Scalar a{3.0}, b{4.0};
 *   auto expr = a + b * a;   // lazy: no computation
 *   double r = expr.eval();  // 3.0 + 4.0*3.0 = 15.0
 */
struct ExprTemplates
{
  template <typename Derived> struct Mixin
  {
    /// operator+ (binary)
    template <typename R>
    friend constexpr auto
    operator+ (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::plus<>, Derived, R>{ lhs, rhs };
    }

    /// operator- (binary)
    template <typename R>
    friend constexpr auto
    operator- (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::minus<>, Derived, R>{ lhs, rhs };
    }

    /// operator* (binary)
    template <typename R>
    friend constexpr auto
    operator* (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::multiplies<>, Derived, R>{ lhs, rhs };
    }

    /// operator/ (binary)
    template <typename R>
    friend constexpr auto
    operator/ (const Derived &lhs, const R &rhs)
    {
      return BinExpr<std::divides<>, Derived, R>{ lhs, rhs };
    }

    /// Unary negation
    friend constexpr auto
    operator- (const Derived &e)
    {
      return UnaryExpr<std::negate<>, Derived>{ e };
    }
  };
};

// Also allow BinExpr on the left side of further operations:

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator+ (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::plus<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator- (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::minus<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator* (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::multiplies<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

template <typename Op1, typename L1, typename R1, typename R>
constexpr auto
operator/ (const BinExpr<Op1, L1, R1> &lhs, const R &rhs)
{
  return BinExpr<std::divides<>, BinExpr<Op1, L1, R1>, R>{ lhs, rhs };
}

// ============================================================
//  CustomLiterals: literal suffix registration
// ============================================================

/**
 * @brief A single literal suffix entry.
 *
 * @tparam Suffix  The suffix string (e.g., "_km").
 * @tparam F       Callable: F(long double) -> T
 */
template <FixedString Suffix, typename F> struct LiteralEntry
{
  F handler;
  static constexpr std::string_view suffix = Suffix.view ();

  explicit constexpr LiteralEntry (F f) : handler (std::move (f)) {}
};

/**
 * @brief Factory for LiteralEntry.
 *
 * @tparam Suffix  The suffix string (FixedString NTTP).
 * @param f        Handler: f(long double) -> T
 *
 * Example:
 *   dsl::lit<"_km">([](long double v){ return v * 1000.0L; })
 */
template <FixedString Suffix, typename F>
constexpr auto
lit (F &&f)
{
  return LiteralEntry<Suffix, std::decay_t<F>>{ std::forward<F> (f) };
}

/**
 * @brief A set of literal suffix entries for runtime parsing.
 *
 * @tparam Entries  Variadic list of LiteralEntry types.
 *
 * Example:
 *   constexpr auto lits = dsl::literal_set(
 *       dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *       dsl::lit<"_m"> ([](long double v){ return v; })
 *   );
 *   auto val = lits.parse("3.5_km");  // 3500.0L
 */
template <typename... Entries> struct LiteralSet
{
  std::tuple<Entries...> entries;

  explicit constexpr LiteralSet (Entries... es) : entries (std::move (es)...)
  {
  }

  /**
   * @brief Parses a string like "3.5_km" by finding the matching suffix.
   *
   * @param input  The full literal string (e.g., "3.5_km").
   * @return The result of applying the matched handler to the numeric part.
   * @throws std::runtime_error if no suffix matches.
   */
  long double
  parse (std::string_view input) const
  {
    long double result = 0.0L;
    bool found = false;

    std::apply (
        [&] (const auto &...e)
          {
            (
                [&]
                  {
                    if (found)
                      return;
                    auto suf = e.suffix;
                    if (input.size () > suf.size ()
                        && input.substr (input.size () - suf.size ()) == suf)
                      {
                        auto num_str
                            = input.substr (0, input.size () - suf.size ());
                        long double num = std::stold (std::string (num_str));
                        result = e.handler (num);
                        found = true;
                      }
                  }(),
                ...);
          },
        entries);

    if (!found)
      {
        throw std::runtime_error (
            std::string ("dsl::LiteralSet::parse: no suffix matched for '")
            + std::string (input) + "'");
      }
    return result;
  }
};

/**
 * @brief Factory for LiteralSet.
 *
 * Example:
 *   auto ls = dsl::literal_set(
 *       dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *       dsl::lit<"_cm">([](long double v){ return v / 100.0L; })
 *   );
 */
template <typename... Entries>
constexpr auto
literal_set (Entries &&...es)
{
  return LiteralSet<std::decay_t<Entries>...>{ std::forward<Entries> (es)... };
}

// ============================================================
//  Feature: CustomLiterals
// ============================================================

/**
 * @brief Feature tag: adds literal parsing support to a DSL.
 *
 * The derived class must define a static `literals` member of type LiteralSet.
 *
 * Usage:
 *   struct UnitsDSL : dsl::DSL<UnitsDSL, dsl::CustomLiterals> {
 *       static constexpr auto literals = dsl::literal_set(
 *           dsl::lit<"_km">([](long double v){ return v * 1000.0L; }),
 *           dsl::lit<"_m"> ([](long double v){ return v; })
 *       );
 *   };
 *   auto val = UnitsDSL::parse_literal("5.0_km");  // 5000.0
 */
struct CustomLiterals
{
  template <typename Derived> struct Mixin
  {
    /**
     * @brief Parses a literal string using the Derived::literals set.
     * @param input  String like "3.5_km".
     * @return Parsed value.
     */
    static long double
    parse_literal (std::string_view input)
      requires requires { Derived::literals; }
    {
      return Derived::literals.parse (input);
    }
  };
};

/**
 * @brief Feature tag: adds cache invalidation helper for memoized DSL state.
 *
 * The derived DSL is expected to define `memo_cache` (typically an
 * unordered_map). clear_cache() erases all entries. Thread-safety is
 * intentionally not provided.
 */
struct Memoization
{
  template <typename Derived> struct Mixin
  {
    /// Clears all cached entries stored in `Derived::memo_cache`.
    void
    clear_cache ()
    {
      static_cast<Derived &> (*this).memo_cache.clear ();
    }
  };
};
/** @brief Feature tag for lazy-evaluation related DSL integrations. */
struct LazyFeature
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for monadic combinator integrations. */
struct Monadic
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for Result-based error-flow integrations. */
struct ResultFeature
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for combinatory parser integrations. */
struct CombinatorParser
{
  template <typename Derived> struct Mixin
  {
  };
};
/** @brief Feature tag for task-pipeline integrations. */
struct TaskPipeline
{
  template <typename Derived> struct Mixin
  {
  };
};

/**
 * @brief Wraps a callable with a memoization cache keyed by the first
 * argument.
 *
 * This helper is suitable for expensive pure functions where repeated calls
 * with the same key should return cached results. The key type must be
 * hashable by `std::hash<Key>`. This helper is intentionally not thread-safe.
 *
 * @tparam Key Cache key type.
 * @tparam Value Cached value type.
 * @tparam Fn Callable type with signature compatible with `Value(const Key&)`.
 * @param fn Callable to memoize.
 * @return Callable object that performs memoized lookup.
 *
 * Example:
 *   auto fib = dsl::memoize<int, long long>([](int n){ return n < 2 ? n : 1LL;
 * });
 */
template <typename Key, typename Value, typename Fn> class MemoizedCallable
{
public:
  /// Constructs a memoized callable wrapper.
  explicit MemoizedCallable (Fn fn) : fn_ (std::move (fn)) {}
  /// Evaluates or returns cached value.
  Value
  operator() (const Key &key)
  {
    if (auto it = cache_.find (key); it != cache_.end ())
      return it->second;
    auto value = fn_ (key);
    cache_.emplace (key, value);
    return value;
  }
  /// Clears the memoization cache.
  void
  clear_cache ()
  {
    cache_.clear ();
  }

private:
  Fn fn_;
  std::unordered_map<Key, Value> cache_{};
};
template <typename Key, typename Value, typename Fn>
auto
memoize (Fn &&fn)
{
  return MemoizedCallable<Key, Value, std::decay_t<Fn>>{ std::forward<Fn> (
      fn) };
}

/**
 * @brief Deferred single-shot computation wrapper.
 *
 * Lazy<T> stores a callable and evaluates it only on first access through
 * get()/force(). The computed value is cached for subsequent reads.
 * Unlike ExprTemplates, this models one deferred computation, not an
 * expression tree. This type is not thread-safe by default.
 *
 * @tparam T Value produced by the deferred computation.
 *
 * Example:
 *   dsl::Lazy<std::string> cfg([]{ return std::string("loaded"); });
 *   auto& v = cfg.get();
 */
template <typename T> class Lazy
{
public:
  /// Creates lazy value from producer callable.
  explicit Lazy (std::function<T ()> producer)
      : producer_ (std::move (producer))
  {
  }
  /// Computes the value on first access and returns cached reference.
  T &
  get ()
  {
    if (!value_)
      value_ = producer_ ();
    return *value_;
  }
  /// Alias of get().
  T &
  force ()
  {
    return get ();
  }
  /// Indicates whether value has already been computed.
  bool
  is_computed () const noexcept
  {
    return value_.has_value ();
  }

private:
  std::function<T ()> producer_;
  std::optional<T> value_{};
};

/**
 * @brief Optional-like wrapper with monadic method chaining.
 *
 * This type wraps std::optional semantics and adds fluent
 * map/flat_map/filter/or_else. Monadic laws (left identity/right
 * identity/associativity) apply when map/flat_map functions are pure and
 * side-effect free.
 *
 * @tparam T Contained value type.
 *
 * Example:
 *   auto out = dsl::Maybe<int>(4).map([](int x){ return x + 1; }).or_else(0);
 */
template <typename T> class Maybe
{
public:
  /// Constructs empty Maybe.
  Maybe () = default;
  /// Constructs Maybe with a value.
  Maybe (T value) : value_ (std::move (value)) {}
  /// Constructs Maybe from std::optional.
  Maybe (std::optional<T> value) : value_ (std::move (value)) {}
  /// Checks whether a value is present.
  bool
  has_value () const noexcept
  {
    return value_.has_value ();
  }
  /// Returns underlying std::optional.
  const std::optional<T> &
  as_optional () const
  {
    return value_;
  }
  /// Applies function to contained value, if present.
  /// Applies function returning Maybe-like type, if value is present.
  template <typename Fn>
  auto
  map (Fn &&fn) const -> Maybe<decltype (fn (std::declval<T> ()))>
  {
    using U = decltype (fn (*value_));
    if (!value_)
      return Maybe<U>{};
    return Maybe<U>{ fn (*value_) };
  }
  template <typename Fn>
  auto
  flat_map (Fn &&fn) const -> decltype (fn (std::declval<T> ()))
  {
    using R = decltype (fn (*value_));
    if (!value_)
      return R{};
    return fn (*value_);
  }
  /// Retains value only if predicate passes.
  template <typename Pred>
  Maybe
  filter (Pred &&pred) const
  {
    if (value_ && pred (*value_))
      return *this;
    return Maybe{};
  }
  /// Returns contained value or fallback.
  T
  or_else (T fallback) const
  {
    return value_.value_or (std::move (fallback));
  }

private:
  std::optional<T> value_{};
};
template <typename Opt, typename Fn>
auto
map (const Opt &opt, Fn &&fn) -> std::optional<decltype (fn (*opt))>
{
  if (!opt)
    return std::nullopt;
  return fn (*opt);
}
template <typename Opt, typename Fn>
auto
flat_map (const Opt &opt, Fn &&fn) -> decltype (fn (*opt))
{
  using R = decltype (fn (*opt));
  if (!opt)
    return R{};
  return fn (*opt);
}
template <typename Opt, typename Pred>
auto
filter (const Opt &opt, Pred &&pred) -> Opt
{
  if (opt && pred (*opt))
    return opt;
  return std::nullopt;
}
template <typename Opt, typename T>
T
or_else (const Opt &opt, T fallback)
{
  return opt.value_or (std::move (fallback));
}

template <typename T, typename E> class Result
{
public:
  /// Constructs an Ok result.
  Result (T value) : data_ (std::in_place_index<0>, std::move (value)) {}
  /// Constructs an Err result (disabled when T == E to avoid ambiguity).
  Result (E error)
    requires (!std::same_as<T, E>)
      : data_ (std::in_place_index<1>, std::move (error))
  {
  }
  /// True when value branch is active.
  bool
  is_ok () const
  {
    return std::holds_alternative<T> (data_);
  }
  /// True when error branch is active.
  bool
  is_err () const
  {
    return std::holds_alternative<E> (data_);
  }
  /// Returns value or throws if Err.
  T
  unwrap () const
  {
    if (!is_ok ())
      throw std::runtime_error ("dsl::Result::unwrap called on Err");
    return std::get<T> (data_);
  }
  /// Returns value or fallback when Err.
  T
  unwrap_or (T fallback) const
  {
    return is_ok () ? std::get<T> (data_) : std::move (fallback);
  }
  /// Maps Ok value with function.
  template <typename Fn>
  auto
  map (Fn &&fn) const -> Result<decltype (fn (std::declval<T> ())), E>
  {
    using U = decltype (fn (std::declval<T> ()));
    if (is_ok ())
      return Result<U, E>{ fn (std::get<T> (data_)) };
    return Result<U, E>::from_err (std::get<E> (data_));
  }
  /// Chains operation producing another Result.
  template <typename Fn>
  auto
  and_then (Fn &&fn) const -> decltype (fn (std::declval<T> ()))
  {
    using R = decltype (fn (std::declval<T> ()));
    if (is_ok ())
      return fn (std::get<T> (data_));
    return R::from_err (std::get<E> (data_));
  }
  /**
   * @brief Transforms the error payload while keeping success values
   * unchanged.
   *
   * @tparam Fn Error transformer callable.
   * @param fn Function applied only when this is an Err.
   * @return Result<T, E2> with transformed error type.
   */
  template <typename Fn>
  auto
  map_err (Fn &&fn) const -> Result<T, decltype (fn (std::declval<E> ()))>
  {
    using E2 = decltype (fn (std::declval<E> ()));
    if (is_ok ())
      return Result<T, E2>::from_ok (std::get<T> (data_));
    return Result<T, E2>::from_err (fn (std::get<E> (data_)));
  }
  /// Factory for Ok value.
  static Result
  from_ok (T value)
  {
    return Result{ std::move (value) };
  }
  /// Factory for Err value.
  static Result
  from_err (E error)
  {
    return Result (std::in_place_index<1>, std::move (error));
  }

private:
  Result (std::in_place_index_t<1>, E error)
      : data_ (std::in_place_index<1>, std::move (error))
  {
  }
  std::variant<T, E> data_;
};
template <typename T> struct OkWrap
{
  T value;
};
template <typename E> struct ErrWrap
{
  E error;
};
template <typename T>
OkWrap<T>
Ok (T value)
{
  return { std::move (value) };
}
template <typename E>
ErrWrap<E>
Err (E error)
{
  return { std::move (error) };
}

/**
 * @brief Mutable parser input stream for combinatory parsers.
 *
 * ParsecInput tracks source text and cursor position and provides primitive
 * operations used by low-level parser callables.
 */
struct ParsecInput
{
  std::string_view source{};
  std::size_t pos{ 0 };
  char
  peek () const
  {
    return pos < source.size () ? source[pos] : '\0';
  }
  bool
  eof () const
  {
    return pos >= source.size ();
  }
  char
  consume ()
  {
    return eof () ? '\0' : source[pos++];
  }
  std::string_view
  get_span (std::size_t start) const
  {
    return source.substr (start, pos - start);
  }
};

inline Result<std::string, std::string>
load_parse_input_file (const std::string &path)
{
  std::ifstream in (path, std::ios::in | std::ios::binary);
  if (!in)
    return Result<std::string, std::string>::from_err (
        "dsl::load_parse_input_file: cannot open '" + path + "'");
  std::ostringstream ss;
  ss << in.rdbuf ();
  if (!in.good () && !in.eof ())
    return Result<std::string, std::string>::from_err (
        "dsl::load_parse_input_file: read failed for '" + path + "'");
  return Result<std::string, std::string>::from_ok (ss.str ());
}
enum class ParseFailureKind
{
  Soft,
  Committed
};

struct ParseError
{
  std::size_t pos{ 0 };
  ParseFailureKind kind{ ParseFailureKind::Soft };
  std::vector<std::string> expected{};
};

template <typename T> struct ExpectedResult
{
  using value_type = T;

  std::optional<T> value{};
  ParseError error{};

  ExpectedResult () = default;
  ExpectedResult (std::nullopt_t) : value (std::nullopt) {}
  ExpectedResult (const T &v) : value (v) {}
  ExpectedResult (T &&v) : value (std::move (v)) {}

  static ExpectedResult
  failure (std::size_t pos, ParseFailureKind kind = ParseFailureKind::Soft,
           std::vector<std::string> expected = {})
  {
    ExpectedResult out{};
    out.error = ParseError{ pos, kind, std::move (expected) };
    return out;
  }

  explicit
  operator bool () const
  {
    return value.has_value ();
  }
  T &
  operator* ()
  {
    return *value;
  }
  const T &
  operator* () const
  {
    return *value;
  }
};

/**
 * @brief Functional parser wrapper around `ExpectedResult<T>(ParsecInput&)`.
 *
 * @tparam T Parse output type.
 */
template <typename T> struct Parser
{
  std::function<ExpectedResult<T> (ParsecInput &)> parse;
  ExpectedResult<T>
  operator() (ParsecInput &input) const
  {
    return parse (input);
  }
};

inline void
merge_error (ParseError &dst, const ParseError &src)
{
  if (src.pos > dst.pos)
    {
      dst = src;
      return;
    }
  if (src.pos < dst.pos)
    return;
  if (src.kind == ParseFailureKind::Committed)
    dst.kind = ParseFailureKind::Committed;
  for (const auto &label : src.expected)
    {
      if (std::find (dst.expected.begin (), dst.expected.end (), label)
          == dst.expected.end ())
        dst.expected.push_back (label);
    }
}

template <typename T>
ExpectedResult<T>
fail_expected (const ParsecInput &in, std::string label,
               ParseFailureKind kind = ParseFailureKind::Soft)
{
  return ExpectedResult<T>::failure (in.pos, kind, { std::move (label) });
}

template <typename Fn>
auto
parser (Fn &&fn)
{
  using Ret = decltype (fn (std::declval<ParsecInput &> ()));
  using T = typename Ret::value_type;
  return Parser<T>{ std::forward<Fn> (fn) };
}

template <typename T>
Parser<std::vector<T>>
operator* (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<std::vector<T>>
        {
          std::vector<T> out;
          ParseError best_err{};
          while (true)
            {
              std::size_t save = in.pos;
              auto r = p (in);
              if (!r)
                {
                  merge_error (best_err, r.error);
                  if (r.error.kind == ParseFailureKind::Committed)
                    return ExpectedResult<std::vector<T>>::failure (
                        best_err.pos, best_err.kind, best_err.expected);
                  in.pos = save;
                  break;
                }
              out.push_back (*r);
            }
          return out;
        });
}
template <typename A, typename B>
Parser<std::pair<A, B>>
operator& (const Parser<A> &a, const Parser<B> &b)
{
  return parser (
      [a, b] (ParsecInput &in) -> ExpectedResult<std::pair<A, B>>
        {
          auto save = in.pos;
          auto ra = a (in);
          if (!ra)
            return ExpectedResult<std::pair<A, B>>::failure (
                ra.error.pos, ra.error.kind, ra.error.expected);
          auto rb = b (in);
          if (!rb)
            {
              bool consumed = in.pos != save;
              in.pos = save;
              auto kind = rb.error.kind;
              if (consumed)
                kind = ParseFailureKind::Committed;
              return ExpectedResult<std::pair<A, B>>::failure (
                  rb.error.pos, kind, rb.error.expected);
            }
          return std::make_pair (*ra, *rb);
        });
}
template <typename T>
Parser<T>
operator| (const Parser<T> &a, const Parser<T> &b)
{
  return parser (
      [a, b] (ParsecInput &in) -> ExpectedResult<T>
        {
          auto save = in.pos;
          auto ra = a (in);
          if (ra)
            return ra;
          if (ra.error.kind == ParseFailureKind::Committed)
            return ra;
          in.pos = save;
          auto rb = b (in);
          if (!rb)
            merge_error (rb.error, ra.error);
          return rb;
        });
}
template <typename T>
Parser<std::optional<T>>
optional (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<std::optional<T>>
        {
          auto save = in.pos;
          auto r = p (in);
          if (!r)
            {
              if (r.error.kind == ParseFailureKind::Committed)
                return ExpectedResult<std::optional<T>>::failure (
                    r.error.pos, r.error.kind, r.error.expected);
              in.pos = save;
              return std::optional<T>{};
            }
          return std::optional<T>{ *r };
        });
}

template <typename T>
Parser<T>
try_parse (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<T>
        {
          auto save = in.pos;
          auto r = p (in);
          if (!r)
            {
              in.pos = save;
              r.error.kind = ParseFailureKind::Soft;
            }
          return r;
        });
}

template <typename T>
Parser<T>
labeled (const Parser<T> &p, std::string expected)
{
  return parser (
      [p,
       expected = std::move (expected)] (ParsecInput &in) -> ExpectedResult<T>
        {
          auto r = p (in);
          if (!r)
            r.error.expected = { expected };
          return r;
        });
}

template <typename T> struct ParseOutcome
{
  std::optional<T> value{};
  ParseError error{};
};

template <typename T>
ParseOutcome<T>
run_parser (const Parser<T> &p, std::string_view source)
{
  ParsecInput in{ source, 0 };
  auto r = p (in);
  if (!r)
    return { std::nullopt, r.error };
  if (!in.eof ())
    {
      ParseError trailing{ in.pos,
                           ParseFailureKind::Committed,
                           { "<end-of-input>" } };
      return { std::nullopt, trailing };
    }
  return { std::move (r.value), {} };
}

inline Parser<char>
ch (char c)
{
  return labeled (parser (
                      [c] (ParsecInput &in) -> ExpectedResult<char>
                        {
                          if (in.peek () == c)
                            return in.consume ();
                          return fail_expected<char> (in, std::string (1, c));
                        }),
                  std::string (1, c));
}

inline Parser<char>
satisfy (std::function<bool (char)> pred, std::string label)
{
  return labeled (parser (
                      [pred = std::move (pred),
                       label] (ParsecInput &in) -> ExpectedResult<char>
                        {
                          if (!in.eof () && pred (in.peek ()))
                            return in.consume ();
                          return fail_expected<char> (in, label);
                        }),
                  std::move (label));
}

enum class TaskPolicy
{
  StopOnError,
  ContinueOnError
};
/**
 * @brief Shared mutable state for task-pipeline execution.
 */
struct TaskState
{
  std::unordered_map<std::string, Result<std::string, std::string>> results{};
  bool suspended{ false };
};
/**
 * @brief Pipeline task unit, either internal callable or external command
 * adapter.
 */
struct Task
{
  std::string name{};
  std::function<Result<std::string, std::string> (TaskState &)> run_fn{};
  Result<std::string, std::string>
  run (TaskState &state) const
  {
    return run_fn (state);
  }
};
/**
 * @brief Sequence of tasks composed by operator|.
 */
struct TaskChain
{
  std::vector<Task> tasks{};
  TaskPolicy policy{ TaskPolicy::StopOnError };
};
inline TaskChain
operator| (Task lhs, Task rhs)
{
  return TaskChain{ { std::move (lhs), std::move (rhs) },
                    TaskPolicy::StopOnError };
}
inline TaskChain
operator| (TaskChain lhs, Task rhs)
{
  lhs.tasks.push_back (std::move (rhs));
  return lhs;
}
template <typename... Args>
TaskState &
run (TaskState &state, const TaskChain &chain, Args &&...)
{
  for (const auto &task : chain.tasks)
    {
      auto result = task.run (state);
      state.results.insert_or_assign (task.name, result);
      if (result.is_err () && chain.policy == TaskPolicy::StopOnError)
        break;
      if (state.suspended)
        break;
    }
  return state;
}
inline void
suspend (TaskState &state)
{
  state.suspended = true;
}
template <typename EventFn>
void
upon (TaskState &state, EventFn &&event, Task task)
{
  if (event ())
    state.results[task.name] = task.run (state);
}

/**
 * @brief Parser stage metadata passed to semantic hooks.
 */
struct ParserStage
{
  std::size_t begin{};
  std::size_t end{};
  std::string_view span{};
};

/**
 * @brief Runs a parser and invokes semantic action with matched span.
 *
 * @tparam T Parser output type.
 * @tparam ActionFn Action callable type.
 * @param p Parser to execute.
 * @param input Parse input stream.
 * @param semantics Map of production id to matched content.
 * @param action Semantic action callback.
 * @return Optional action result; empty when parse fails.
 */
template <typename T, typename ActionFn>
auto
parse_with_action (
    const Parser<T> &p, ParsecInput &input,
    const std::unordered_map<std::string, std::string> &semantics,
    ActionFn &&action)
    -> std::optional<decltype (action (semantics, ParserStage{}))>
{
  std::size_t begin = input.pos;
  auto r = p (input);
  if (!r)
    return std::nullopt;
  ParserStage stage{ begin, input.pos, input.get_span (begin) };
  return action (semantics, stage);
}

/**
 * @brief Production registration entry for parser table-pass semantics.
 *
 * @tparam Id Compile-time production identifier.
 * @tparam Fn Handler callable.
 */
template <FixedString Id, typename Fn> struct ProductionHandler
{
  Fn fn;
};

/**
 * @brief Registers a production handler keyed by compile-time string ID.
 *
 * @tparam Id Compile-time production identifier.
 * @tparam Fn Handler callable type.
 * @param fn Production handler.
 * @return ProductionHandler object.
 */
template <FixedString Id, typename Fn>
auto
production (Fn &&fn)
{
  return ProductionHandler<Id, std::decay_t<Fn>>{ std::forward<Fn> (fn) };
}

// ============================================================
//  PEG: definitions, rules, channels, matching, combinators
// ============================================================
//
// This section is a purely additive extension: it layers a PEG
// (Parsing Expression Grammar) subsystem on top of the existing
// pattern-matching (`dsl::pattern`) and parser-combinator
// (`dsl::Parser` / `ParsecInput`) machinery above. No existing type,
// function, or layout is modified.

struct PEGDefinition;
struct PEGRule;
struct PEGMatch;
struct PEGParseResult;
struct PEGMatcher;

/**
 * @brief A named logical stream that PEG matches are routed into.
 *
 * Channels suppress or categorize matches without changing the grammar:
 * the built-in @IGNORE channel is used for whitespace/comments, while
 * user-defined channels (created with dsl::new_peg_channel) group matches
 * by category and enable filtering in the PEG matcher.
 */
struct PEGChannel
{
  std::string name{ "@DEFAULT" };

  constexpr bool operator== (const PEGChannel &) const = default;

  /// @return true if this is the built-in @IGNORE channel.
  bool
  is_ignore () const noexcept
  {
    return name == "@IGNORE";
  }
};

/// Built-in channel for ignorable input (whitespace, comments, ...).
inline const PEGChannel PEGIgnoreChannel{ "@IGNORE" };

/// Built-in channel for ordinary (semantic) input.
inline const PEGChannel PEGDefaultChannel{ "@DEFAULT" };

/**
 * @brief Creates a custom PEG channel.
 *
 * Custom channel names must begin with '@'.
 *
 * @param name Channel name, e.g. "@COMMENTS".
 * @throws std::invalid_argument if the name is empty or lacks the '@'
 * prefix.
 */
inline PEGChannel
new_peg_channel (std::string_view name)
{
  if (name.empty () || name.front () != '@')
    throw std::invalid_argument (
        "dsl::new_peg_channel: channel names must begin with '@'");
  return PEGChannel{ std::string (name) };
}

/**
 * @brief Match record handed to PEG semantic actions.
 *
 * `value` is a view into the parsed source text and is only valid for the
 * duration of the parse that produced it.
 */
struct PEGMatch
{
  std::string_view value{};   ///< matched text
  std::size_t begin{ 0 };     ///< start offset in source
  std::size_t end{ 0 };       ///< one-past-end offset in source
  PEGRule *rule{ nullptr };   ///< rule that produced this match
  PEGChannel channel{};       ///< resolved channel of the rule
  void *user_data{ nullptr }; ///< optional extension point

  std::size_t line{ 1 };   ///< 1-based line of `begin`
  std::size_t column{ 1 }; ///< 1-based column of `begin`
  bool ignored{ false };   ///< true if routed to @IGNORE

  /// @return length of the matched text (end - begin).
  std::size_t
  length () const noexcept
  {
    return end - begin;
  }
};

/// Signature of a PEG semantic action.
using PEGSemanticAction = std::function<void (PEGMatch &)>;

namespace peg_detail
{

/// Runtime-compiled matcher for the dsl::pattern regex subset:
/// literals, '.', [class] (with ranges and ^ negation), and */+/?/?!
/// quantifiers. Matching is an unanchored greedy prefix match.
struct CompiledPattern
{
  struct Atom
  {
    bool any{ false };      ///< '.' wildcard
    bool is_class{ false }; ///< [...]
    bool negate{ false };   ///< [^...]
    char ch{ 0 };           ///< literal character
    std::string cls{};      ///< class body, e.g. "a-z0-9"
    char quant{ 0 };        ///< 0, '*', '+', '?', '!'
  };

  std::vector<Atom> atoms{};
  bool valid{ true };
  std::string error{};

  static bool
  class_test (std::string_view cls, bool negate, char c)
  {
    bool hit = false;
    for (std::size_t i = 0; i < cls.size ();)
      {
        if (i + 2 < cls.size () && cls[i + 1] == '-')
          {
            if (c >= cls[i] && c <= cls[i + 2])
              hit = true;
            i += 3;
          }
        else
          {
            if (cls[i] == c)
              hit = true;
            ++i;
          }
      }
    return negate ? !hit : hit;
  }

  static bool
  atom_test (const Atom &a, char c)
  {
    if (a.any)
      return true;
    if (a.is_class)
      return class_test (a.cls, a.negate, c);
    return a.ch == c;
  }

  static CompiledPattern
  compile (std::string_view pat)
  {
    CompiledPattern out;
    std::size_t i = 0;
    while (i < pat.size ())
      {
        // '^'/'$' anchors are accepted and ignored: PEG matching is
        // implicitly anchored at the current position.
        if (pat[i] == '^' || pat[i] == '$')
          {
            ++i;
            continue;
          }
        Atom a;
        if (pat[i] == '.')
          {
            a.any = true;
            ++i;
          }
        else if (pat[i] == '[')
          {
            auto close = pat.find (']', i + 1);
            if (close == std::string_view::npos)
              {
                out.valid = false;
                out.error = "unterminated character class";
                return out;
              }
            a.is_class = true;
            std::string body{ pat.substr (i + 1, close - i - 1) };
            if (!body.empty () && body.front () == '^')
              {
                a.negate = true;
                body.erase (body.begin ());
              }
            a.cls = std::move (body);
            i = close + 1;
          }
        else
          {
            a.ch = pat[i];
            ++i;
          }
        if (i < pat.size ()
            && (pat[i] == '*' || pat[i] == '+' || pat[i] == '?'
                || pat[i] == '!'))
          a.quant = pat[i++];
        out.atoms.push_back (std::move (a));
      }
    return out;
  }

  /// Greedy prefix match starting at `start`; returns match end offset.
  std::optional<std::size_t>
  match_at (std::string_view text, std::size_t start) const
  {
    if (!valid)
      return std::nullopt;
    std::size_t pos = start;
    for (const auto &a : atoms)
      {
        auto avail = [&] { return pos < text.size (); };
        switch (a.quant)
          {
          case '*':
            while (avail () && atom_test (a, text[pos]))
              ++pos;
            break;
          case '+':
            {
              std::size_t n = 0;
              while (avail () && atom_test (a, text[pos]))
                {
                  ++pos;
                  ++n;
                }
              if (n == 0)
                return std::nullopt;
              break;
            }
          case '?':
            if (avail () && atom_test (a, text[pos]))
              ++pos;
            break;
          case '!':
            // Must-fail atom: zero-width negative lookahead.
            if (avail () && atom_test (a, text[pos]))
              return std::nullopt;
            break;
          default:
            if (!avail () || !atom_test (a, text[pos]))
              return std::nullopt;
            ++pos;
            break;
          }
      }
    return pos;
  }
};

inline void
line_col_at (std::string_view text, std::size_t pos, std::size_t &line,
             std::size_t &col)
{
  line = 1;
  col = 1;
  for (std::size_t i = 0; i < pos && i < text.size (); ++i)
    {
      if (text[i] == '\n')
        {
          ++line;
          col = 1;
        }
      else
        ++col;
    }
}

/// A lightweight match-arm consumed by PEGMatcher::operator<<.
struct PEGArm
{
  PEGRule *rule{ nullptr }; ///< nullptr => wildcard
  PEGSemanticAction action{};
  bool is_wildcard{ false };
  bool is_channel{ false }; ///< match any rule on `channel`
  PEGChannel channel{};
};

} // namespace peg_detail

/**
 * @brief A single PEG rule: a compiled pattern plus mutable behavior.
 *
 * Rules are created via PEGDefinition::add_rule<"...">() and remain
 * mutable afterwards: the semantic action, channel, and the guarded
 * (must_fail / if_succeed) behavior may be adjusted at any time.
 *
 * The rule is invocable with a callback to build a matcher arm:
 *   matcher << rule([](dsl::PEGMatch& m){ ... });
 */
struct PEGRule
{
  std::string pattern_text{};
  peg_detail::CompiledPattern compiled{};
  PEGSemanticAction semantic_action{};
  PEGChannel channel{ "@DEFAULT" };
  bool must_fail{ false };        ///< succeed only when the pattern fails
  bool fire_on_neg{ false };      ///< run the action even on negated success
  PEGRule *if_succeed{ nullptr }; ///< rule to attempt when pattern matches
  std::size_t order{ 0 };         ///< declaration order (PEG precedence)

  /// Attempts this rule at `pos`. Returns the match end offset, or
  /// std::nullopt on failure. For must_fail rules, success consumes one
  /// character so tokenizing parses always make progress.
  std::optional<std::size_t>
  try_match (std::string_view text, std::size_t pos) const
  {
    if (must_fail)
      {
        if (compiled.match_at (text, pos))
          return std::nullopt;
        return pos < text.size () ? std::optional<std::size_t> (pos + 1)
                                  : std::nullopt;
      }
    return compiled.match_at (text, pos);
  }

  /// Builds a match-arm binding this rule to a callback.
  template <typename Fn>
  peg_detail::PEGArm
  operator() (Fn &&fn) const
  {
    peg_detail::PEGArm arm;
    arm.rule = const_cast<PEGRule *> (this);
    arm.action = std::forward<Fn> (fn);
    return arm;
  }
};

/**
 * @brief Diagnostics produced by PEGDefinition::parse().
 */
struct PEGParseResult
{
  bool success{ true };
  std::size_t offset{ 0 };
  std::size_t line{ 1 };
  std::size_t column{ 1 };
  std::string message{};

  bool
  ok () const noexcept
  {
    return success;
  }
  bool
  failed () const noexcept
  {
    return !success;
  }
  std::size_t
  error_offset () const noexcept
  {
    return offset;
  }
  std::size_t
  error_line () const noexcept
  {
    return line;
  }
  std::size_t
  error_column () const noexcept
  {
    return column;
  }
  /// @return human-readable failure description (empty on success).
  std::string_view
  error_message () const noexcept
  {
    return message;
  }
};

/**
 * @brief A PEG grammar: an ordered set of rules plus channels and
 * optional inheritance metadata.
 *
 * Create a root grammar with dsl::create_peg_definition(), or extend an
 * existing one with dsl::derive_peg_definition(parent). Derived grammars
 * see parent rules first (parent rules precede child rules at any
 * decision point), and may add new rules or shadow parent behavior
 * without mutating the parent.
 */
struct PEGDefinition
{
  const PEGDefinition *parent{ nullptr };

  /// Adds a rule from a compile-time pattern string, with an optional
  /// semantic action. Returns a reference to the stored rule; the
  /// reference stays valid as long as rules are only appended (and this
  /// definition is alive).
  template <FixedString S, typename Fn>
  PEGRule &
  add_rule (Fn &&action)
  {
    return emplace_rule (S.view (),
                         PEGSemanticAction (std::forward<Fn> (action)));
  }

  template <FixedString S>
  PEGRule &
  add_rule ()
  {
    return emplace_rule (S.view (), PEGSemanticAction{});
  }

  /// @return all rules visible at this level: parent rules first, then
  /// local rules, preserving PEG ordered-choice precedence.
  std::vector<const PEGRule *>
  rules () const
  {
    std::vector<const PEGRule *> out;
    if (parent)
      out = parent->rules ();
    for (const auto &r : local_rules_)
      out.push_back (&r);
    return out;
  }

  /**
   * @brief Parses `text` token by token using ordered choice over all
   * visible rules, invoking semantic actions as matches occur.
   *
   * Matches routed to @IGNORE are recognized and skipped silently.
   * Parsing succeeds when the whole input is consumed.
   */
  PEGParseResult
  parse (std::string_view text) const
  {
    PEGParseResult result;
    auto all = rules ();
    std::size_t pos = 0;
    while (pos < text.size ())
      {
        bool matched = false;
        for (const PEGRule *r : all)
          {
            auto end = r->try_match (text, pos);
            if (!end)
              continue;
            const bool negated = r->must_fail;
            if (!negated && *end == pos)
              {
                // Zero-width positive match would loop forever.
                result.success = false;
                result.offset = pos;
                peg_detail::line_col_at (text, pos, result.line,
                                         result.column);
                result.message
                    = "PEG rule matched empty input; refusing to loop";
                return result;
              }
            matched = true;
            if (!negated || r->fire_on_neg)
              {
                PEGMatch m;
                m.value = text.substr (pos, *end - pos);
                m.begin = pos;
                m.end = *end;
                m.rule = const_cast<PEGRule *> (r);
                m.channel = r->channel;
                m.ignored = r->channel.is_ignore ();
                peg_detail::line_col_at (text, pos, m.line, m.column);
                if (r->semantic_action)
                  r->semantic_action (m);
              }
            if (r->if_succeed)
              {
                if (auto e2 = r->if_succeed->try_match (text, pos))
                  {
                    PEGMatch m;
                    m.value = text.substr (pos, *e2 - pos);
                    m.begin = pos;
                    m.end = *e2;
                    m.rule = r->if_succeed;
                    m.channel = r->if_succeed->channel;
                    m.ignored = r->if_succeed->channel.is_ignore ();
                    peg_detail::line_col_at (text, pos, m.line, m.column);
                    if (r->if_succeed->semantic_action)
                      r->if_succeed->semantic_action (m);
                  }
              }
            pos = *end;
            break; // ordered choice: first success wins
          }
        if (!matched)
          {
            result.success = false;
            result.offset = pos;
            peg_detail::line_col_at (text, pos, result.line, result.column);
            result.message
                = "no PEG rule matched at offset " + std::to_string (pos);
            return result;
          }
      }
    result.offset = pos;
    return result;
  }

private:
  PEGRule &
  emplace_rule (std::string_view pat, PEGSemanticAction action)
  {
    PEGRule r;
    r.pattern_text = std::string (pat);
    r.compiled = peg_detail::CompiledPattern::compile (pat);
    if (!r.compiled.valid)
      throw std::invalid_argument ("dsl::PEGDefinition::add_rule: "
                                   + r.compiled.error + " in pattern '"
                                   + r.pattern_text + "'");
    r.semantic_action = std::move (action);
    r.order = next_order_++;
    local_rules_.push_back (std::move (r));
    return local_rules_.back ();
  }

  std::vector<PEGRule> local_rules_{};
  std::size_t next_order_{ 0 };
};

/// Creates a root PEG definition (parent == nullptr).
inline PEGDefinition
create_peg_definition ()
{
  return PEGDefinition{};
}

/// Derives a PEG definition from `parent`; parent rules stay visible.
inline PEGDefinition
derive_peg_definition (const PEGDefinition &parent)
{
  PEGDefinition d;
  d.parent = &parent;
  return d;
}

/**
 * @brief Fluent, stream-style PEG pattern matcher.
 *
 * Created with dsl::peg_new_matcher(peg, text). Arms are consumed with
 * operator<<; the first arm whose rule matches at the current position
 * wins (PEG ordered choice). The wildcard arm fires when no preceding
 * arm matches, consuming one character by default.
 *
 * Channel filters (only/include/exclude) restrict which rules the rule
 * arms will consider; ignored-channel matches are skipped automatically
 * before any arm is consulted.
 *
 *   auto matcher = dsl::peg_new_matcher(peg, text);
 *   while (!matcher.is_spent())
 *       matcher << ident(cb) << number(cb) << matcher.wildcard(cb);
 *   matcher.close();
 */
struct PEGMatcher
{
  const PEGDefinition *def{ nullptr };
  std::string_view text{};
  std::size_t pos{ 0 };

  bool
  is_spent () const noexcept
  {
    return pos >= text.size ();
  }
  std::size_t
  position () const noexcept
  {
    return pos;
  }
  /// No-op for API symmetry; releases nothing (nothing is held).
  void
  close () noexcept
  {
  }

  /// Restricts matching to a single channel.
  PEGMatcher &
  only (std::string_view channel)
  {
    mode_ = FilterMode::Only;
    channels_.clear ();
    channels_.push_back (std::string (channel));
    return *this;
  }
  /// Adds a channel to the accepted set.
  PEGMatcher &
  include (std::string_view channel)
  {
    if (mode_ == FilterMode::Exclude)
      mode_ = FilterMode::Only;
    channels_.push_back (std::string (channel));
    return *this;
  }
  /// Rejects rules on the given channel.
  PEGMatcher &
  exclude (std::string_view channel)
  {
    if (mode_ != FilterMode::Exclude)
      {
        mode_ = FilterMode::Exclude;
        channels_.clear ();
      }
    channels_.push_back (std::string (channel));
    return *this;
  }
  bool
  channel_allowed (std::string_view name) const
  {
    if (mode_ == FilterMode::All)
      return true;
    bool found = std::find (channels_.begin (), channels_.end (), name)
                 != channels_.end ();
    return mode_ == FilterMode::Only ? found : !found;
  }

  /// Wildcard arm: fires when no rule arm matches at the position.
  template <typename Fn>
  peg_detail::PEGArm
  wildcard (Fn &&fn)
  {
    peg_detail::PEGArm arm;
    arm.is_wildcard = true;
    arm.action = std::forward<Fn> (fn);
    return arm;
  }

  /// Enqueues one match arm. Arms accumulate across chained << calls
  /// until one of them fires at the current position; the first arm
  /// whose rule matches wins (ordered choice). @IGNORE matches are
  /// skipped automatically before arms are consulted.
  PEGMatcher &
  operator<< (const peg_detail::PEGArm &arm)
  {
    arms_.push_back (arm);
    step ();
    return *this;
  }

private:
  /// Tries the accumulated arms once at the current position.
  void
  step ()
  {
    if (arms_.empty () || is_spent ())
      return;
    skip_ignored ();
    if (is_spent ())
      {
        arms_.clear ();
        return;
      }
    // Non-wildcard arms in order: first match wins.
    for (std::size_t i = 0; i < arms_.size (); ++i)
      {
        const auto &arm = arms_[i];
        if (arm.is_wildcard)
          continue;
        if (arm.is_channel)
          {
            for (const PEGRule *r : def->rules ())
              {
                if (r->channel.name != arm.channel.name)
                  continue;
                if (auto end = r->try_match (text, pos))
                  {
                    fire (arm.action, r, pos, *end);
                    arms_.clear ();
                    return;
                  }
              }
            continue;
          }
        if (!arm.rule || !channel_allowed (arm.rule->channel.name))
          continue;
        if (auto end = arm.rule->try_match (text, pos))
          {
            fire (arm.action, arm.rule, pos, *end);
            arms_.clear ();
            return;
          }
      }
    // No rule arm matched: fire the first wildcard arm, if any.
    for (std::size_t i = 0; i < arms_.size (); ++i)
      {
        if (!arms_[i].is_wildcard)
          continue;
        PEGMatch m;
        m.value = text.substr (pos, 1);
        m.begin = pos;
        m.end = pos + 1;
        m.rule = nullptr;
        peg_detail::line_col_at (text, pos, m.line, m.column);
        if (arms_[i].action)
          arms_[i].action (m);
        ++pos; // wildcard consumes one character
        arms_.clear ();
        return;
      }
    // Nothing fired yet: keep the accumulated arms; the next << may
    // provide one that matches at this position.
  }

  void
  skip_ignored ()
  {
    bool progressed = true;
    while (progressed && !is_spent ())
      {
        progressed = false;
        for (const PEGRule *r : def->rules ())
          {
            if (!r->channel.is_ignore ())
              continue;
            if (auto end = r->try_match (text, pos))
              {
                if (*end == pos)
                  continue;
                pos = *end;
                progressed = true;
                break;
              }
          }
      }
  }

  void
  fire (const PEGSemanticAction &action, const PEGRule *rule,
        std::size_t begin, std::size_t end)
  {
    PEGMatch m;
    m.value = text.substr (begin, end - begin);
    m.begin = begin;
    m.end = end;
    m.rule = const_cast<PEGRule *> (rule);
    m.channel = rule->channel;
    m.ignored = rule->channel.is_ignore ();
    peg_detail::line_col_at (text, begin, m.line, m.column);
    pos = end;
    if (action)
      action (m);
    else if (rule->semantic_action)
      rule->semantic_action (m);
  }

  enum class FilterMode
  {
    All,
    Only,
    Exclude
  };
  FilterMode mode_{ FilterMode::All };
  std::vector<std::string> channels_{};
  std::vector<peg_detail::PEGArm> arms_{};
  static constexpr std::size_t npos = static_cast<std::size_t> (-1);
};

/// Creates a fluent PEG matcher over `text` for the grammar `peg`.
inline PEGMatcher
peg_new_matcher (const PEGDefinition &peg, std::string_view text)
{
  PEGMatcher m;
  m.def = &peg;
  m.text = text;
  return m;
}

// ============================================================
//  PEG parser combinators
// ============================================================
//
// Thin, additive wrappers over the existing dsl::Parser machinery with
// strict PEG semantics: ordered choice (operator| already behaves
// ordered), repetition that never commits on partial input, and
// lookahead operators.

namespace peg_detail
{

/// Converts any dsl::Parser<T> into a Parser<PEGMatch> that reports the
/// consumed span. The output value is discarded; the span is kept.
template <typename T>
Parser<PEGMatch>
as_span_parser (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto r = p (in);
          if (!r)
            return ExpectedResult<PEGMatch>::failure (
                r.error.pos, r.error.kind, r.error.expected);
          PEGMatch m;
          m.begin = begin;
          m.end = in.pos;
          m.value = in.get_span (begin);
          return m;
        });
}

} // namespace peg_detail

/// PEG sequence: parse a, then b, then c... Spans concatenate.
template <typename A, typename B, typename... Rest>
auto
peg_seq (const Parser<A> &a, const Parser<B> &b, const Rest &...rest)
{
  auto ab = parser (
      [a, b] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto ra = a (in);
          if (!ra)
            return ExpectedResult<PEGMatch>::failure (
                ra.error.pos, ra.error.kind, ra.error.expected);
          auto rb = b (in);
          if (!rb)
            {
              bool consumed = in.pos != begin;
              in.pos = begin;
              auto kind = rb.error.kind;
              if (consumed)
                kind = ParseFailureKind::Committed;
              return ExpectedResult<PEGMatch>::failure (rb.error.pos, kind,
                                                        rb.error.expected);
            }
          PEGMatch m;
          m.begin = begin;
          m.end = in.pos;
          m.value = in.get_span (begin);
          return m;
        });
  if constexpr (sizeof...(rest) == 0)
    return ab;
  else
    return peg_seq (ab, rest...);
}

/// PEG ordered choice: first successful alternative wins; later
/// alternatives are not tried at that decision point.
template <typename A, typename B, typename... Rest>
auto
peg_choice (const Parser<A> &a, const Parser<B> &b, const Rest &...rest)
{
  auto ab = try_parse (a) | try_parse (b);
  if constexpr (sizeof...(rest) == 0)
    return ab;
  else
    return peg_choice (ab, rest...);
}

/// PEG optional: a? (never fails, never commits).
template <typename T>
auto
peg_opt (const Parser<T> &p)
{
  return optional (try_parse (p));
}

/// PEG zero-or-more: a* (greedy; partial failures backtrack fully).
template <typename T>
auto
peg_many (const Parser<T> &p)
{
  return *try_parse (p);
}

/// PEG one-or-more: a+ (greedy; fails if the first iteration fails).
template <typename T>
auto
peg_many1 (const Parser<T> &p)
{
  return p & *try_parse (p);
}

/// PEG positive lookahead: &a — succeeds if a matches, consumes nothing.
template <typename T>
auto
peg_and (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<bool>
        {
          auto save = in.pos;
          auto r = p (in);
          in.pos = save;
          if (!r)
            return ExpectedResult<bool>::failure (
                r.error.pos, ParseFailureKind::Soft, r.error.expected);
          return true;
        });
}

/// PEG negative lookahead: !a — succeeds if a fails, consumes nothing.
template <typename T>
auto
peg_not (const Parser<T> &p)
{
  return parser (
      [p] (ParsecInput &in) -> ExpectedResult<bool>
        {
          auto save = in.pos;
          auto r = p (in);
          in.pos = save;
          if (r)
            return fail_expected<bool> (in, "negative lookahead");
          return true;
        });
}

/// Adapts a PEGRule into a Parser<PEGMatch> honoring must_fail.
inline Parser<PEGMatch>
peg_rule (const PEGRule &rule)
{
  return parser (
      [&rule] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto end = rule.try_match (in.source, begin);
          if (!end)
            return fail_expected<PEGMatch> (in, rule.pattern_text);
          in.pos = *end;
          PEGMatch m;
          m.begin = begin;
          m.end = *end;
          m.value = in.source.substr (begin, *end - begin);
          m.rule = const_cast<PEGRule *> (&rule);
          m.channel = rule.channel;
          m.ignored = rule.channel.is_ignore ();
          peg_detail::line_col_at (in.source, begin, m.line, m.column);
          return m;
        });
}

/// Assigns a channel to a parser's PEGMatch result.
template <typename T>
auto
peg_channel (PEGChannel channel, const Parser<T> &p)
{
  return parser (
      [channel = std::move (channel),
       p] (ParsecInput &in) -> ExpectedResult<PEGMatch>
        {
          std::size_t begin = in.pos;
          auto r = p (in);
          if (!r)
            return ExpectedResult<PEGMatch>::failure (
                r.error.pos, r.error.kind, r.error.expected);
          PEGMatch m;
          m.begin = begin;
          m.end = in.pos;
          m.value = in.get_span (begin);
          m.channel = channel;
          m.ignored = channel.is_ignore ();
          peg_detail::line_col_at (in.source, begin, m.line, m.column);
          return m;
        });
}

/// Overload accepting the channel name directly (must begin with '@').
template <typename T>
auto
peg_channel (std::string_view name, const Parser<T> &p)
{
  return peg_channel (new_peg_channel (name), p);
}

// ============================================================
//  Utility concepts
// ============================================================

/// Concept: T has a feature F mixed in.
template <typename T, typename F>
concept HasFeature = std::derived_from<T, typename F::template Mixin<T>>;

/// Concept: T is a DSL with Pipeline support.
template <typename T>
concept Pipeable = HasFeature<T, Pipeline>;

/// Concept: T is a DSL with ExprTemplates support.
template <typename T>
concept ExprLike = HasFeature<T, ExprTemplates>
                   && requires (const T &t) { t.expr_value (); };

/// Concept: T is a DSL with CustomLiterals support.
template <typename T>
concept HasLiterals
    = HasFeature<T, CustomLiterals> && requires { T::literals; };

/// Concept: T is a DSL with Rewrite support.
template <typename T>
concept Rewritable = HasFeature<T, Rewrite> && requires { T::rules; };

} // namespace dsl
