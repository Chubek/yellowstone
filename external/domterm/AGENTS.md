# AGENTS.md -- DomTERM

## Project Scope

This project provides C APIs for PTY and TTY management, Terminfo parsing and expansion, terminal session orchestration, recording/replay, and local or remote connections.

Prioritize predictable behavior, explicit ownership, portability, and robust error handling.

You can use the `/mnt/warble/domweave/domlibs/domterm/.info` directory as a source of truth.

## Core Behavior

Implement the following foundational features:

- Support nonblocking I/O.
- Expose file descriptors for integration with `poll`, `select`, `epoll`, ` file descriptors for integration with `poll`, `select`, `epoll`, `kqueue`, may transfer fewer bytes than requested.
- Define how interrupted system calls are handled:
  - Specify whether `EINTR` is retried internally.
  - Report `EAGAIN` and `EWOULDBLOCK` distinctly when appropriate.
- Distinguish normal conditions from failures:
  - EOF
  - Timeout
  - Would-block
  - Child process exited
  - System error
- Define PTY lifecycle behavior:
  - Create and configure the master and slave descriptors.
  - Close descriptors correctly after spawning.
  - Support process groups and session creation.
  - Report normal exits, signal termination, and stopped processes separately.
  - Clean up all resources when spawning fails.
- Support terminal size queries and resizing.
- Support propagation of terminal resize events, including `SIGWINCH` where appropriate.
- Provide TTY state saving and restoration.
- Restore the original `termios` state when a TTY session closes, unless explicitly disabled.

## Terminfo

The Terminfo implementation should provide:

- Typed capability lookup for boolean, numeric, and string capabilities.
- Capability metadata and introspection.
- Parameter expansion for the supported Terminfo language.
- Limits for:
  - Stack depth
  - Output size
  - Number of execution steps
- Loading entries by terminal name.
- Alias support.
- Explicit database search-path behavior.
- Explicit handling of relevant environment variables.
- Validation of all offsets, lengths, counts, and string-table references.
- Documentation of supported compiled Terminfo formats.
- Clear behavior for unsupported extensions and malformed entries.

Terminfo parsing must reject invalid or truncated data without reading outside the input buffer.

## Recording and Replay

The recording format should be:

- Versioned.
- Explicit about byte order and integer widths.
- Capable of storing:
  - Input data
  - Output data
  - Timestamps
 - Input data
  - Output data
  - Timestamps
  -  - Session metadata
- Detect corruption and truncated records.
- Define behavior for unknown record types.
- Support bounded record and payload sizes.
- Make timestamp units and clock source explicit.
- Provide deterministic replay controls, including:
  - Real-time replay
  - Accelerated replay
  - Step-by-step replay
  - Paused replay

Ownership of replayed event data must be documented. Callers must know whether event buffers remain valid until the next call or require explicit release.

## Connections

Local and remote connections should share a transport-neutral interface.

Define and enforce:

- Message framing.
- Maximum message size.
- Partial send and receive behavior.
- Backpressure handling.
- Read and write timeouts.
- Connection cancellation.
- EOF and orderly shutdown behavior.
- Protocol version negotiation.
- Unsupported message handling.
- Keepalive or heartbeat behavior where required.
- Connection state transitions.

Remote connections must define their security model. For non-local or untrusted networks, support or require:

- Authentication.
- Encryption.
- Peer- Credential.
- Credential or token handling.
- Protection against oversized, malformed, or replayed messages.

Do not treat a raw TCP connection as secure by default.

## API Quality

All public APIs must follow these rules:

- Document ownership of every returned pointer.
- Document ownership of every file descriptor.
- State whether each function borrows, transfers, or copies data.
- Define whether cleanup functions accept `NULL`.
- Make success and failure behavior consistent across subsystems.
- Avoid using generic `DT_ERR_IO` for normal states such as EOF, timeout, or would-block.
- Provide version and feature-query functions.
- Keep implementation structures opaque where possible.
- Provide initialization functions.
- Keep implementation structures opaque where possible.
- Provide initialization functions for public option_init(DT_PTYOptions *options);

This allows new fields to be added without requiring callers to manually initialize every member.

- Reserve fields or include a structure-size member when ABI evolution is expected.
- Define the thread-safety policy for every subsystem.
- Define whether objects may be used concurrently.
- Define cancellation behavior for blocking operations.
- Avoid hidden global mutable state.
- Keep platform-specific types out of the portable public API where possible.

## Error Handling

Use a consistent error-reporting policy:

- Return `DT_Status` for operations that can fail.
- Use `DT_Error` for detailed diagnostic information.
- Preserve the relevant system error value.
- Set the error offset for parser failures.
- Ensure error messages are NUL-terminated.
- Do not overwrite an existing error unless documented.
- Make errors safe to inspect after the failed operation returns.
- Do not expose pointers into temporary or freed storage through error objects.

Consider adding dedicated status values for:

```c
DT_ERR_EOF
DT_ERR_TIMEOUT
DT_ERR_WOULD_BLOCK
DT_ERR_CHILD_EXITED
DT_ERR_CANCELLED
DT_ERR_PROTOCOL
DT_ERR_LIMIT
DT_ERR_AUTH
```

## Resource Management

Every resource-owning type must have a matching cleanup function.

Resource cleanup must be:

- Deterministic.
- Safe after partial initialization.
- Safe during error unwinding.
- Idempotent where practical.
- Explicit about descriptor ownership.
- Explicit about child-process ownership.
- Explicit about whether closing a session terminates its child process.

Avoid leaking file descriptors, allocated buffers, child processes, parser state, or partially decoded messages.

## Termscript

Termscript is a substrate of DomTERM that provides the user with a Turing-complete domain-specific language for manipulating and handling the terminal, and all the features of DomTERM is available in Termscript.

Termscript also exposes an *native extension ABI/API* which allows for implementation of modules in C. Several modules are provided, as standard library for Termscript.

Termscript is parsed by compiling `termscript/Termscript.g` with `/mnt/warble/domweave/scripts/aurocks.pl`.

It is possible to recompile a Terminfo profile into Termscript leveraging the `std.terminfo.compile` module:

```termscript
const TI = G:load "std.terminfo";

const wezterm_ti = TI:load "wezterm" or G:die "Terminal not found";
const wezterm_ts = TI:compile_to_ti &wezterm_ti or G:die "Compilation failed";

G:puts &wezterm_ts;
```

Termscript has two backends: the tree-walking VM and ahead-of-time
compilation to C. The C backend (`ts_vm_compile_to_c` /
`dt_termscript_compile_to_c`) emits real C — statements become C
control flow over the `ts_rt_*` / `dt_rt_*` compiled-code runtime, so
nothing is parsed at run time and behavior (output, errors, step
budget) matches the VM exactly. Termscript-side libraries (`.tsc`
companions loaded with constant-string `G:import`) are resolved at
compile time and inlined as compiled C; native modules are never
embedded — they link statically and resolve through the VM registry
at run time. When transpiled to C, code needs to be linked against
`libtermscript` (plus `libtermscript_stdlib` for the `std.*`
natives). The Termscript driver does this by default. However, you
can stop linking by passing `-c`. It will stop at emitting the C
file. `--static` will link the libraries statically.

## Testing Requirements

Add tests for:

- Invalid and truncated Terminfo entries.
- Invalid capability offsets.
- Missing and unsupported capabilities.
- Parameter expansion edge cases.
- Oversized expansion output.
- PTY spawn failures.
- Child exits and signal termination.
- TTY state restoration.
- Terminal resize behavior.
- Partial reads and writes.
- `EINTR`, `EAGAIN`, and `EWOULDBLOCK`.
- EOF and timeout behavior.
- Recording corruption and truncation.
- Unknown record types.
- Message size limits.
- Connection cancellation.
- Remote authentication failures.
- Double-close and partial-cleanup paths.
- Concurrent access according to the documented thread-safety policy.

Tests must not depend on a particular local Terminfo installation unless the required database is supplied by the test environment.

## Implementation Order

Implement features in this order:

1. Define ownership, lifetime, thread, thread-safety, cancellation, and error semantics.
2. Complete PTY and TTY.
3. Add terminal sizing, resize propagation, and TTY state restoration.
4. Add nonblocking I/O and readiness integration.
5. Harden Terminfo parsing and parameter expansion.
6. Define and implement the versioned recording format.
7. Add replay controls and corruption handling.
8. Add the transport-neutral connection interface.
9. Add local and remote transports.
10. Add authentication, encryption, protocol negotiation, and resource limits.

Do not add higher-level orchestration APIs until the lower-level ownership and error contracts are stable.
