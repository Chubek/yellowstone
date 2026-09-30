# Termscript Standard Library

Forty-nine hybrid libraries. Each one is two halves:

- **C native module** (`<name>/<name>.c`, module `std.<name>`) — owns
  storage, parsing, syscalls and handles, written against
  `termscript.h` plus `common/ts_std_common.h`.
- **Termscript companion** (`<name>/<name>.tsc`) — loaded with
  `G:import "<name>"`, binds the module once (`<name>_m`) and spells
  shared idioms as plain Termscript.

```termscript
G:import "json";
const J = json_m;
const doc = J:parse "{\"a\": [1, 2]}" or G:die "bad json";
const second = J:get doc "a.1";
G:puts second;
```

## Module index

| Companion | Native module | Contents |
|---|---|---|
| `array` | `std.array` | dense int64 vectors: new/push/get/set/len/sum/fill |
| `buffer` | `std.buffer` | editable byte buffers with gap storage and read-only mapped files |
| `edit_history` | `std.edit_history` | bounded snapshot undo/redo for editor documents |
| `termcap` | `std.termcap` | terminfo/termcap capability aliases and simple entry encoding |
| `text_search` | `std.text_search` | forward/backward literal search by byte offset |
| `word_motion` | `std.word_motion` | next/previous ASCII word boundaries |
| `autocomp` | `std.autocomp` | prefix completion: complete/common/rank over joined strings |
| `codec` | `std.codec` | base64/hex/url codecs, crc32, case/trim/replace, predicates, split |
| `color` | `std.color` | named ANSI colors, rgb, attributes, wrap |
| `csv` | `std.csv` | double-quote dialect parse/parse_line/field/join |
| `draw` | `std.draw` | hline/box/bar/progress/center string builders |
| `env` | `std.env` | getenv/setenv/unset, environ list, home, pid |
| `exec` | `std.exec` | system/capture/ok, shell quoting |
| `expect` | `std.expect` | spawn + deadline substring matching over pipes |
| `fsm` | `std.fsm` | table-driven finite state machines |
| `glob` | `std.glob` | fnmatch matching, directory glob, quoting |
| `http` | `std.http` | minimal HTTP/1.0 GET/POST/status/headers/url_parse (no TLS by policy) |
| `io` | `std.io` | whole-file read/write/append, exists/size/remove/rename/mkdir/lines |
| `ipc` | `std.ipc` | flock file locks, FIFOs |
| `json` | `std.json` | strict JSON documents as handles |
| `key` | `std.key` | input sequence names (arrows, F-keys, C0 controls) + encode |
| `list` | `std.list` | ordered value sequences over the shared list store |
| `log` | `std.log` | leveled file/stderr logging with timestamps |
| `map` | `std.map` | insertion-ordered string tables with defaults |
| `menu` | `std.menu` | numbered menus: format/pick/confirm/select |
| `notify` | `std.notify` | bell/title/flash strings, notify-send toast |
| `panel` | `std.panel` | titled boxes, side-by-side columns |
| `pipe` | `std.pipe` | popen streams as handles |
| `pty` | `std.pty` | standalone minipty: spawn/read/write/wait/close |
| `prompt` | `std.prompt` | reusable primary/continuation prompts and visible width |
| `readline` | `std.readline` | line input (isocline when built in, getline fallback) + history |
| `regex` | `std.regex` | POSIX ERE: search/full/find/count/replace with `\1`..`\9` and `&` |
| `rules` | `std.rules` | horizontal and vertical text rules |
| `sched` | `std.sched` | monotonic/wall clocks, sleep_ms, elapsed |
| `screen` | `std.screen` | in-memory character grids |
| `signal` | `std.signal` | signal numbers/names/delivery (no handlers: no callbacks by design) |
| `socket` | `std.socket` | blocking TCP connect/send/recv/listen/accept/close |
| `stream` | `std.stream` | append-only buffers with consuming getline |
| `style` | `std.style` | SGR composition, escape stripping, visible width, padding |
| `syntax` | `std.syntax` | naive keyword highlighting for c/termscript/sh |
| `table` | `std.table` | ruled separator-delimited text tables |
| `terminfo` | `std.tinfo` | curated xterm strings + cup + live winsize (the full database is DomTERM's `std.terminfo`) |
| `test` | `std.test` | counting checks: new/check/eq/summary/done |
| `theme` | `std.theme` | named palettes over role names |
| `toml` | `std.toml` | TOML via vendored tomlc99, same surface as json |
| `verbatim` | `std.verbatim` | line-numbered source listings, optionally syntax-coloured |
| `vterm` | `std.vterm` | small VT parser (CSI CUP/ED/EL/SGR/IL/DL…) over a grid |
| `widget` | `std.widget` | aligned tables, progress/gauge meters, spinner frames |
| `yaml` | `std.yaml` | block-subset YAML (indent maps/lists, scalars, comments) |

Lookups miss as `nil` (so `or` fallbacks work); arity/type errors
are `TS_ERR_INVAL`; malformed documents parse to `nil`.

`std.env` provides `get(key, fallback?)`, `set(key, value)`,
`unset(key)`, `has(key)`, `list(separator?)`, `home()`, and `pid()`.
Set/unset return booleans; missing values return nil or the supplied
fallback. Returned strings are copies owned by Termscript. Environment
changes affect the entire host process: callers must serialize access
with other threads that read or modify the process environment.
The `env/` source directory is explicitly exempt from the repository's
Python virtual-environment ignore rule so it is included in checkouts.

## Language mechanism (no grammar change)

`G:import` / `G:import_path` are ordinary C builtins in the `G`
module (`ts_runtime.c`); `ts_vm_import` / `ts_vm_add_search_path`
are new public API in `termscript.h`. Imports execute in the
caller's scope (bindings persist — that is the export mechanism),
share output and the step budget, resolve through VM search paths
plus `TERMSCRIPT_PATH`, and reject cycles. The aurocks grammar
(`Termscript.g`) and `scripts/aurocks.pl` are untouched.

## Built with

- **domqlib Q** (`domlibs/domqlib`, `q/tsvec.q`): the growable
  vectors behind the list store (`gen/ts_valvec.h`) and string
  helpers (`gen/ts_strvec.h`). Regenerate with `q/run_qc.py`
  (needs the Q bytecode + tkinter); the CMake build renews them
  only when all three are present, otherwise the committed
  outputs are used. One genuine Tcl lesson is documented in
  `q/tsvec.q`'s history: always write `${VAR}(` — bare `$VAR(`
  parses as a Tcl array index where `[...]` still evaluates.
- **third_party**: `tomlc99/toml.c` (compiled in, behind
  `std.toml`), `isocline/src/isocline.c` (umbrella, behind
  `std.readline`), `stb/stb_ds.h` reserved for future maps.
- **DomTERM core**: untouched except `dt_termscript.c`, which
  auto-registers the stdlib on every `DT_TermVM` and reserves the
  `std.` prefix for it.

## Embedding

```c
#include <termscript/stdlib/ts_stdlib.h>
ts_stdlib_register_all (vm, &err);   /* native halves */
ts_stdlib_search_path (vm, &err);    /* install + build-tree .tsc */
ts_vm_run_string (vm, "G:import \"json\"; ...", &out, &err);
```

`pkg-config --libs termscript_stdlib` links it;
`TERMSCRIPT_STDLIB_DIR` / `TERMSCRIPT_STDLIB_SRC` bake the search
paths at build time.

## Adding a module

1. Create `stdlib/<name>/` with `<name>.c` (a `TS_Module`
   named `std.<name>`), `<name>.h` (`extern const TS_Module
   ts_std_<name>_module;`), and `<name>.tsc` (bind `<name>_m`,
   atomic-args calls only — nested calls do not parse).
2. Add `<name>` to `TS_STDLIB_MODULES` in `stdlib/CMakeLists.txt`,
   the registry table in `ts_stdlib.c`, the includes in
   `ts_stdlib.h`, and the companion list in
   `tests/domterm-test-suite/test_stdlib.c`.
3. Handles need a file-static tag object, a destructor, value
   copy on push/set, and `nil` (not errors) for misses.

## Text editing modules

`std.buffer` provides `new(kind, text?)`, `map_file(path)`, `insert(buffer,
byte_offset, text)`, `replace(buffer, byte_offset, byte_count, text)`,
`delete(buffer, byte_offset, byte_count)`, `slice(buffer, byte_offset,
byte_count)`, `line(buffer, zero_based_line)`, `len(buffer)`, and
`contents(buffer)`. The six editable kinds use distinct backends:

| Kind | Storage |
|---|---|
| `rope` | linked chunks of at most 1024 bytes |
| `gap` | contiguous bytes with a movable gap |
| `piece_table` | immutable original, append-only additions, linked piece descriptors |
| `linked_lines` | linked nodes of lines including their newline |
| `array_lines` | array of individually allocated lines |
| `indexed_gap` | movable gap and line-start index |
| `mmap` (`map_file`) | read-only file mapping |

Rope and line-based kinds rebuild their backing structure on edits;
this is correct but can take linear time per edit. The piece table
preserves its original and append-only addition storage across edits.
`mmap` is read-only. Offsets and string lengths count bytes, not Unicode
graphemes; embedded NUL bytes are not supported by Termscript strings.
A missing line returns nil.

`std.edit_history` stores complete snapshots (`new`, `record`/`add`,
`undo`/`previous`, `redo`/`next`, `current`, `position`, `len`, `at`,
`limit`, `clear`). Recording after undo discards the redo branch; `limit`
retains the newest snapshots.
Snapshots use ordinary heap allocation and are freed with the Termscript
handle. `std.prompt:new(primary, continuation?)` creates a reusable prompt;
`render(prompt, continuation?)` selects its primary or continuation form and
`width(text)` ignores SGR escapes plus Readline `\001`/`\002` non-printing
markers. `std.verbatim:render(source, language?, first_line?)` is the standard
code-listing surface and delegates known languages to `std.syntax`.
`std.table:render(headers, rows, column_separator?, row_separator?)` renders
one header row and separator-delimited data with borders supplied by
`std.rules`; it does not parse CSV quoting. `std.text_search` uses `find`/`rfind` (optional byte
start for `find`, inclusive upper bound for `rfind`); `std.word_motion`
uses `next`/`previous` with ASCII letters, digits, and `_` as word
characters.

`std.termcap` maps common capability names (`to_termcap_name`,
`to_terminfo_name`) and translates string entries
(`to_termcap_entry`, `to_terminfo_entry`). Its parameter translator
handles the shared `%i`, decimal/character and two/three-digit fields,
parameter reversal (`%r`), and literal `%%`. It returns an error for
parameter expressions that cannot be represented in the destination
language, including terminfo conditionals and arithmetic. As described
in the ncurses `infocmp(1m)` documentation, arbitrary terminfo strings
cannot always be converted to equivalent termcap programs.

**DomMEMTk integration:** Buffer allocations use a C allocator hook in
`buffer/allocator.h`. A C++ host can construct
`termscript::DomMEMTkBufferArena` from `buffer/dommemtk_adapter.hpp`,
install its callbacks with `ts_std_buffer_set_allocator`, and retain
the arena until every VM using those buffers has been freed. The adapter
uses DomMEMTk's nonmoving free-list allocator; Termscript handle
payloads remain owned by their handle destructors. Install or reset
the process-wide allocator before creating VMs, while no other thread
is creating buffers. The normal Termscript library has no C++ runtime
dependency.
