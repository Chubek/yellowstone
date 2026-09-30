# Chapter 10: Query DSL

[`common/qdsl.hpp`](../common/qdsl.hpp) supplies composable predicates, pipeline stages, and parser combinators. qbfd query helpers select sections, symbols, segments, imports, and exports.

## ISA parsing

The ISA loader uses `dsl::Parser<T>`, `dsl::ParsecInput`, parser sequencing, failure reporting, and `run_parser`-compatible input semantics. Lexical identifiers are parsed through qDSL rather than a second ad hoc grammar. Higher-level ISA blocks then assemble those tokens into the qisa AST.

This keeps ISA parsing embeddable and makes the same parser infrastructure available to future architecture-sensitive components in qbfd and qld.

## Query example

```cpp
#include "common/isa.hpp"

qisa::Database db;
std::vector<std::string> errors;
db.loadDirectory("infobank/isa", errors);
if (const auto* amd64 = db.find("amd64")) {
  for (const auto& op : amd64->operations)
    std::cout << op.name << " " << op.syntax << "\n";
}
```

## Composition rules

Predicates compose with `&`, `|`, and `!`. Pipeline stages compose with `|`. Parser failures retain an input position and expected-token labels so callers can report useful source diagnostics.
