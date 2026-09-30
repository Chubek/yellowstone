/* domterm.h -- DomTERM public C API.
 *
 * DomTERM provides PTY/TTY management, Terminfo parsing and expansion,
 * terminal session orchestration, session recording/replay, local and
 * remote terminal connections, and the Termscript automation language.
 *
 * ============ Ownership, lifetime and thread-safety policy ============
 *
 * - Every opaque handle (DT_TIDB, DT_TIProf, DT_TIParser, DT_TTYSession,
 *   DT_PTYSession, DT_Puppeteer, DT_Recorder, DT_Replayer, DT_LocalConn,
 *   DT_RemoteConn, DT_Envelope, DT_Message, DT_TermVM) is heap allocated
 *   and must be released with its matching dt_*_close/free function.
 *   All cleanup functions accept NULL (no-op) and are safe to call after
 *   partial initialization or a failed creation call that returned NULL.
 * - Functions that return pointers document whether the caller receives a
 *   borrowed reference (valid while the parent object lives, must NOT be
 *   freed) or a transferred reference (caller owns it, must free it).
 * - File descriptors: dt_tty_open/dt_localconn_open only take ownership
 *   of the wrapped fd when take_ownership is true.  dt_pty_spawn always
 *   owns the PTY master fd it creates.  After dt_*_close the fd is closed
 *   iff ownership was taken; the fd value must not be used afterwards.
 * - Buffer ownership: input buffers passed IN are always borrowed (copied
 *   if retained).  Output/event buffers returned in structs owned by a
 *   parent (DT_Replayer events, DT_Message payloads) remain valid until
 *   the next call on that object or its destruction, unless documented
 *   otherwise.
 * - Thread safety: no object may be used concurrently from multiple
 *   threads unless documented.  The only thread-safe operations are the
 *   dt_*_cancel functions and dt_puppeteer_request_resize (designed to be
 *   called from signal handlers or watchdog threads).  Distinct objects
 *   may be used on distinct threads.  There is no hidden global mutable
 *   state; all tables are read-only after process start.
 * - Cancellation: blocking reads/writes/wait calls retry on EINTR
 *   internally.  dt_localconn_cancel/dt_remoteconn_cancel unblock a
 *   thread stuck in the matching receive/send on socket transports.
 * - Errors: fallible operations return DT_Status and optionally fill a
 *   caller-provided DT_Error (which may be NULL if the caller does not
 *   need details).  DT_Error storage belongs to the caller and stays
 *   valid after the call returns.
 */

#ifndef DOMTERM_H
#define DOMTERM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* ------------------------------------------------------------------ */
/* Version and feature queries.                                        */
/* ------------------------------------------------------------------ */

#define DT_VERSION_MAJOR 0
#define DT_VERSION_MINOR 1
#define DT_VERSION_PATCH 0

/* Opaque implementation handles. */
typedef struct DT_TIDB DT_TIDB;
typedef struct DT_TIProf DT_TIProf;
typedef struct DT_TIParser DT_TIParser;
typedef struct DT_TTYSession DT_TTYSession;
typedef struct DT_PTYSession DT_PTYSession;
typedef struct DT_Puppeteer DT_Puppeteer;
typedef struct DT_Recorder DT_Recorder;
typedef struct DT_Replayer DT_Replayer;
typedef struct DT_LocalConn DT_LocalConn;
typedef struct DT_RemoteConn DT_RemoteConn;
typedef struct DT_Envelope DT_Envelope;
typedef struct DT_Message DT_Message;
typedef struct DT_TermVM DT_TermVM;

/* Portable limits. */
#define DT_TI_MAX_EXPAND (4096u)        /* Max bytes of one expansion. */
#define DT_TI_MAX_STEPS (10000u)        /* Max Terminfo opcodes per expansion. */
#define DT_TI_MAX_STACK (32u)           /* Max Terminfo parameter stack depth. */
#define DT_TI_MAX_ENTRY (1u << 20)      /* Max compiled Terminfo entry bytes. */
#define DT_RECORD_MAX_PAYLOAD (16u << 20) /* Max recording payload bytes. */
#define DT_CONN_MAX_MESSAGE (4u << 20)  /* Max framed message payload bytes. */
#define DT_ERROR_MSG_SIZE 256

/* Common status and error reporting. */
typedef enum
{
  DT_OK = 0,
  DT_ERR_INVALID_ARGUMENT = 1,
  DT_ERR_NO_MEMORY = 2,
  DT_ERR_IO = 3,
  DT_ERR_NOT_FOUND = 4,
  DT_ERR_PARSE = 5,
  DT_ERR_UNSUPPORTED = 6,
  DT_ERR_SYSTEM = 7,
  /* Normal conditions that are NOT generic I/O failures.  Callers must
     use these instead of DT_ERR_IO for the matching condition. */
  DT_ERR_EOF = 8,          /* End of file / orderly peer shutdown. */
  DT_ERR_TIMEOUT = 9,      /* Operation timed out. */
  DT_ERR_WOULD_BLOCK = 10, /* Nonblocking fd has no data / no space. */
  DT_ERR_CHILD_EXITED = 11,/* Child process already exited. */
  DT_ERR_CANCELLED = 12,   /* Operation cancelled via dt_*_cancel. */
  DT_ERR_PROTOCOL = 13,    /* Framing, handshake or version mismatch. */
  DT_ERR_LIMIT = 14,       /* Size/step/depth/quota limit exceeded. */
  DT_ERR_AUTH = 15         /* Authentication or credential failure. */
} DT_Status;

typedef struct
{
  DT_Status code;
  int system_errno;         /* Preserved errno, 0 when not applicable. */
  size_t offset;            /* Byte offset for parser failures, SIZE_MAX if n/a. */
  char message[DT_ERROR_MSG_SIZE]; /* Always NUL-terminated. */
} DT_Error;

/* Human readable status name. Never returns NULL. */
const char *dt_status_string (DT_Status status);
/* Fill/clear a DT_Error. Accepts NULL error (no-op). Message is printf-style. */
void dt_error_set (DT_Error *error, DT_Status code, int system_errno,
                   size_t offset, const char *fmt, ...);
void dt_error_clear (DT_Error *error);

/* Packed version: (major << 16) | (minor << 8) | patch. */
unsigned int dt_version_number (void);
/* NUL-terminated "major.minor.patch" string in static storage. */
const char *dt_version_string (void);
/* Feature names: "pty", "tty", "terminfo", "record", "replay",
   "local-conn", "remote-conn", "termscript", "puppeteer".  Returns true
   when the feature is compiled in.  "tls" returns false: raw remote TCP
   connections are never encrypted (see security notes below). */
bool dt_feature_query (const char *feature);

/* ------------------------------------------------------------------ */
/* Terminfo values, database and profiles.                             */
/* ------------------------------------------------------------------ */

/* Terminfo values. Strings returned by lookup are owned by the profile
   (borrowed): valid while the profile lives, must NOT be freed. */
typedef enum
{
  DT_TI_BOOL,
  DT_TI_NUMBER,
  DT_TI_STRING
} DT_TIValueType;

typedef struct
{
  DT_TIValueType type;
  union
  {
    bool boolean;
    int32_t number;
    const char *string; /* Borrowed from the profile. */
  } as;
} DT_TIValue;

/* Capability metadata for introspection. */
typedef struct
{
  const char *name;     /* Borrowed short name, e.g. "cup". */
  DT_TIValueType type;
  unsigned int index;   /* Index within its type section. */
  bool present;         /* False when absent in this profile. */
} DT_TICapInfo;

/* Database search-path behavior: explicit paths win; when NULL/empty,
   dt_tidb_open_default consults $TERMINFO, then $TERMINFO_DIRS
   (colon-separated), then $HOME/.terminfo, then the compiled-in fallback
   "/usr/share/terminfo:/lib/terminfo:/usr/lib/terminfo".  $TERM selects
   the entry for dt_tidb_load_default. */
/* Returned pointer is transferred: free with dt_tidb_close (NULL-safe). */
DT_TIDB *dt_tidb_open (const char *const *search_paths,
                       size_t search_path_count, DT_Error *error);
/* Same but resolves the search path from the environment (see above). */
DT_TIDB *dt_tidb_open_default (DT_Error *error);
void dt_tidb_close (DT_TIDB *db);

/* Load the entry for `terminal_name` ("xterm-256color", ...).  Matches the
   primary name or any '|'-separated alias stored in the entry.
   Transferred: free with dt_tiprof_free (NULL-safe). */
DT_TIProf *dt_tiprof_load (DT_TIDB *db, const char *terminal_name,
                           DT_Error *error);
/* Load the entry named by $TERM. */
DT_TIProf *dt_tidb_load_default (DT_TIDB *db, DT_Error *error);
void dt_tiprof_free (DT_TIProf *profile);
/* Borrowed primary name (first entry of the names field). */
const char *dt_tiprof_name (const DT_TIProf *profile);
/* Number of '|' aliases after the primary name. */
size_t dt_tiprof_alias_count (const DT_TIProf *profile);
/* Borrowed alias string by index, NULL when out of range. */
const char *dt_tiprof_alias (const DT_TIProf *profile, size_t index);

/* Look up a capability by its Terminfo short name, e.g. "cup" or "cols".
   Returns true and fills *value on success.  Strings are borrowed from
   the profile.  Returns false (no error details) when the capability is
   unknown or absent in this profile. */
bool dt_tiprof_get (const DT_TIProf *profile, const char *capability,
                    DT_TIValue *value);
/* True when the capability exists AND is present in this profile. */
bool dt_tiprof_has (const DT_TIProf *profile, const char *capability);
/* Metadata/introspection for any known capability name.  Returns false
   for unknown names. */
bool dt_tiprof_cap_info (const DT_TIProf *profile, const char *capability,
                         DT_TICapInfo *info);
/* Number of known capabilities (bool + numeric + string). */
size_t dt_tiprof_cap_count (void);
/* Metadata for the i-th known capability in canonical order. */
bool dt_tiprof_cap_by_index (size_t index, DT_TICapInfo *info);

/* Expand a parameterized Terminfo string into caller-provided storage.
   Supported language: %p1..%p9 %P[a-zA-Z] %g[a-zA-Z] %'c' %{n} %l
   %[[:]flags][width[.precision]][doxXs] (printf-style, flags [-+# space 0])
   %c %s %i %+ %- %* %/ %m %& %| %^ %= %> %< %A %O %! %~ %? %t %e %; %%
   (`%+`/`%-` always lex as binary operators; use the `%:` introducer
   for `-`/`+` flags, e.g. `%:-3d`, `%:+d`.)
   Missing parameters read as 0.  Div/mod by zero, stack underflow or
   overflow, type errors (%s/%l on numbers), stray conditionals and
   unknown directives (%S/%E/%F/%D/%[...] etc.) fail with DT_ERR_PARSE
   or DT_ERR_UNSUPPORTED.  Output beyond DT_TI_MAX_EXPAND or steps
   beyond DT_TI_MAX_STEPS fail with DT_ERR_LIMIT.  Unknown capability
   names fail with DT_ERR_NOT_FOUND; known non-string capabilities
   fail with DT_ERR_INVALID_ARGUMENT.
   Returns the required output size (excluding NUL).  On success the
   output is NUL-terminated when output_size > 0 (truncated to fit when
   the buffer is small).  Returns SIZE_MAX on error with *error filled. */
size_t dt_tiprof_expand (const DT_TIProf *profile, const char *capability,
                         const int32_t *params, size_t param_count,
                         char *output, size_t output_size, DT_Error *error);

/* Tagged parameter for dt_tiprof_expand_params. */
typedef struct
{
  bool is_string;
  int32_t number;
  const char *string; /* Borrowed when is_string (NULL = empty). */
} DT_TIParam;

size_t dt_tiprof_expand_params (const DT_TIProf *profile,
                                const char *capability,
                                const DT_TIParam *params, size_t param_count,
                                char *output, size_t output_size,
                                DT_Error *error);

/* Incremental parser for compiled Terminfo entries.
   Supported formats: classic 16-bit numbers (magic 0432 octal / 282)
   and 32-bit numbers (magic 0542 octal / 346), little-endian, plus the
   optional extended capability section.  All offsets, counts, lengths
   and string-table references are validated; truncated or malformed
   input fails with DT_ERR_PARSE (or DT_ERR_LIMIT past DT_TI_MAX_ENTRY)
   without ever reading outside the input buffer.  Trailing garbage
   after a valid entry fails.  Unknown future magic numbers fail with
   DT_ERR_UNSUPPORTED. */
DT_TIParser *dt_tiparser_create (DT_Error *error);
void dt_tiparser_free (DT_TIParser *parser); /* NULL-safe. */
/* Discard buffered input and reset to the initial state. */
void dt_tiparser_reset (DT_TIParser *parser);

/* Feed bytes; set final_chunk when no more input will arrive.  Because
   the compiled format carries no total length, bytes are buffered until
   final_chunk.  On success, consumed receives the number of input bytes
   accepted (always data_size unless a limit/error occurs). */
DT_Status dt_tiparser_feed (DT_TIParser *parser, const uint8_t *data,
                            size_t data_size, bool final_chunk,
                            size_t *consumed, DT_Error *error);
/* Parse buffered input (requires a final chunk) and transfer ownership
   of the profile to the caller (free with dt_tiprof_free).  Returns NULL
   on error. */
DT_TIProf *dt_tiparser_take_profile (DT_TIParser *parser);

/* ------------------------------------------------------------------ */
/* TTY sessions.                                                       */
/* ------------------------------------------------------------------ */

/* TTY session wraps an already-open terminal file descriptor. It borrows
   the descriptor unless take_ownership is true.  On open the current
   termios state is saved when the fd refers to a TTY; on close it is
   restored unless no_restore is true.  Not thread-safe; use one thread
   per session or external locking. */
typedef struct
{
  uint32_t struct_size; /* Set by dt_tty_options_init. */
  int fd;
  bool take_ownership;
  bool no_restore;   /* Skip termios restore on close. */
  bool nonblocking;  /* Set O_NONBLOCK on open. */
  uint32_t reserved;
} DT_TTYOptions;

void dt_tty_options_init (DT_TTYOptions *options);

DT_TTYSession *dt_tty_open (const DT_TTYOptions *options, DT_Error *error);
void dt_tty_close (DT_TTYSession *session); /* NULL-safe. */
int dt_tty_fd (const DT_TTYSession *session); /* -1 for NULL. */

typedef struct
{
  bool canonical;
  bool echo;
  bool signals;
  bool input_processing;
  bool output_processing;
  unsigned int read_timeout_ds; /* VTIME in deciseconds, 0 = blocking. */
} DT_TTYMode;

typedef struct
{
  unsigned short rows;
  unsigned short columns;
  unsigned short xpixel;
  unsigned short ypixel;
} DT_Winsize;

DT_Status dt_tty_get_mode (DT_TTYSession *session, DT_TTYMode *mode,
                           DT_Error *error);
DT_Status dt_tty_set_mode (DT_TTYSession *session, const DT_TTYMode *mode,
                           DT_Error *error);
/* Convenience: raw (no processing, no echo, byte-at-a-time) / cooked. */
DT_Status dt_tty_set_raw (DT_TTYSession *session, DT_Error *error);
DT_Status dt_tty_set_cooked (DT_TTYSession *session, DT_Error *error);
/* Explicit termios save/restore (close restores automatically). */
DT_Status dt_tty_save (DT_TTYSession *session, DT_Error *error);
DT_Status dt_tty_restore (DT_TTYSession *session, DT_Error *error);

DT_Status dt_tty_get_winsize (DT_TTYSession *session, DT_Winsize *size,
                              DT_Error *error);
DT_Status dt_tty_set_winsize (DT_TTYSession *session, const DT_Winsize *size,
                              DT_Error *error);
/* Toggle O_NONBLOCK on the wrapped fd. */
DT_Status dt_tty_set_nonblocking (DT_TTYSession *session, bool nonblocking,
                                  DT_Error *error);
/* Fd for poll/select/epoll/kqueue integration (borrowed, still owned
   according to take_ownership). */
int dt_tty_pollfd (const DT_TTYSession *session);

/* Partial transfers are normal: OK may report fewer bytes than
   requested; the caller loops.  EINTR is retried internally.  A
   nonblocking fd with no progress reports DT_ERR_WOULD_BLOCK.
   A VTIME timeout read reports DT_ERR_TIMEOUT.  EOF reports
   DT_ERR_EOF (never DT_ERR_IO). */
DT_Status dt_tty_read (DT_TTYSession *session, void *buffer, size_t capacity,
                       size_t *bytes_read, DT_Error *error);
DT_Status dt_tty_write (DT_TTYSession *session, const void *buffer,
                        size_t size, size_t *bytes_written, DT_Error *error);

/* ------------------------------------------------------------------ */
/* PTY creation and child process control.                             */
/* ------------------------------------------------------------------ */

/* PTY lifecycle: dt_pty_spawn creates master+slave, forks, makes the
   child a session leader with the slave as controlling terminal, dups
   the slave onto 0/1/2, applies winsize/workdir/TERM=terminal_name, and
   execs argv.  The parent keeps only the master (owned by the session)
   and the child pid.  Any failure before exec cleans up all fds and
   reaps the child; exec failure is reported to the parent as
   DT_ERR_SYSTEM with the child errno preserved.  dt_pty_close
   terminates a still-running child (kill_signal, default SIGKILL) and
   reaps it, then closes the master.  NULL-safe. */
typedef struct
{
  uint32_t struct_size; /* Set by dt_pty_options_init. */
  const char *terminal_name;   /* Borrowed; sets TERM in the child. */
  const char *working_directory; /* Borrowed; NULL = inherit. */
  char *const *argv;           /* Borrowed; argv[0] required. */
  char *const *envp;           /* Borrowed; NULL = inherit environ. */
  unsigned short rows;
  unsigned short columns;
  bool nonblocking;            /* O_NONBLOCK on the master. */
  bool kill_on_close;          /* Terminate a live child in dt_pty_close. */
  int kill_signal;             /* Signal used when kill_on_close. */
  uint32_t reserved;
} DT_PTYOptions;

void dt_pty_options_init (DT_PTYOptions *options);

typedef struct
{
  int master_fd;  /* Borrowed: owned by the session, do not close. */
  int child_pid;
} DT_PTYInfo;

DT_PTYSession *dt_pty_spawn (const DT_PTYOptions *options, DT_Error *error);
void dt_pty_close (DT_PTYSession *session); /* NULL-safe, reaps child. */
DT_Status dt_pty_get_info (const DT_PTYSession *session, DT_PTYInfo *info,
                           DT_Error *error);
/* Same partial-transfer / EINTR / WOULD_BLOCK contract as dt_tty_read.
   EOF (slave side closed / child gone) reports DT_ERR_EOF. */
DT_Status dt_pty_read (DT_PTYSession *session, void *buffer, size_t capacity,
                       size_t *bytes_read, DT_Error *error);
DT_Status dt_pty_write (DT_PTYSession *session, const void *buffer,
                        size_t size, size_t *bytes_written, DT_Error *error);
DT_Status dt_pty_resize (DT_PTYSession *session, unsigned short rows,
                         unsigned short columns, DT_Error *error);
DT_Status dt_pty_get_winsize (const DT_PTYSession *session, DT_Winsize *size,
                              DT_Error *error);
DT_Status dt_pty_set_nonblocking (DT_PTYSession *session, bool nonblocking,
                                  DT_Error *error);
/* Borrowed master fd for poll/select/epoll/kqueue. */
int dt_pty_pollfd (const DT_PTYSession *session);
/* Blocking reap: *exited_normally distinguishes exit() (true) from
   signal death (false, *exit_code = 128 + signo).  Fails with
   DT_ERR_CHILD_EXITED if no child is attached. */
DT_Status dt_pty_wait (DT_PTYSession *session, int *exit_code,
                       bool *exited_normally, DT_Error *error);
/* Nonblocking reap poll: DT_OK + *exited=true when the child was
   reaped (fields as in dt_pty_wait); DT_ERR_WOULD_BLOCK while the
   child still runs. */
DT_Status dt_pty_poll (DT_PTYSession *session, bool *exited, int *exit_code,
                       bool *exited_normally, DT_Error *error);
DT_Status dt_pty_terminate (DT_PTYSession *session, int signal_number,
                            DT_Error *error);
/* Raw waitpid status helpers. */
DT_Status dt_pty_wait_status (DT_PTYSession *session, int *raw_status,
                              bool wait_blocking, DT_Error *error);
bool dt_pty_status_exited (int raw_status, int *exit_code);
bool dt_pty_status_signaled (int raw_status, int *signal_number);
bool dt_pty_status_stopped (int raw_status, int *stop_signal);

/* ------------------------------------------------------------------ */
/* Terminal orchestration (puppeteer).                                 */
/* ------------------------------------------------------------------ */

typedef struct
{
  uint32_t struct_size; /* Set by dt_puppeteer_options_init. */
  DT_PTYOptions pty;
  const char *terminal_name; /* Borrowed; terminfo profile to load (may be NULL). */
  bool record;
  DT_Recorder *recorder;     /* Borrowed when record is true (may be NULL). */
  uint32_t reserved;
} DT_PuppeteerOptions;

void dt_puppeteer_options_init (DT_PuppeteerOptions *options);

/* Higher-level orchestration; requires the lower-level PTY/TTY/record
   contracts above to be stable.  The puppeteer owns its PTY session and
   terminfo profile; a recorder passed in options is borrowed. */
DT_Puppeteer *dt_puppeteer_create (const DT_PuppeteerOptions *options,
                                   DT_Error *error);
void dt_puppeteer_free (DT_Puppeteer *puppeteer); /* NULL-safe. */
/* Borrowed handles, valid while the puppeteer lives. */
DT_PTYSession *dt_puppeteer_pty (DT_Puppeteer *puppeteer);
DT_TIProf *dt_puppeteer_profile (DT_Puppeteer *puppeteer);
DT_Recorder *dt_puppeteer_recorder (DT_Puppeteer *puppeteer);
/* Transfer one chunk of PTY output into the recorder (as OUTPUT event).
   timeout_ms < 0 blocks, 0 polls, > 0 waits.  DT_ERR_TIMEOUT when idle,
   DT_ERR_EOF when the child side closed. */
DT_Status dt_puppeteer_pump_once (DT_Puppeteer *puppeteer, int timeout_ms,
                                  DT_Error *error);
/* Resize the PTY and record a RESIZE event. */
DT_Status dt_puppeteer_resize (DT_Puppeteer *puppeteer, unsigned short rows,
                               unsigned short columns, DT_Error *error);
/* Async-signal-safe: queue a resize applied by the next pump call.
   Thread-safe; may be called from a SIGWINCH handler. */
void dt_puppeteer_request_resize (DT_Puppeteer *puppeteer,
                                  unsigned short rows,
                                  unsigned short columns);
/* Blocking child reap (see dt_pty_wait). */
DT_Status dt_puppeteer_wait (DT_Puppeteer *puppeteer, int *exit_code,
                             bool *exited_normally, DT_Error *error);

/* ------------------------------------------------------------------ */
/* Recording and replay.                                               */
/* ------------------------------------------------------------------ */

/* Versioned binary format (all integers little-endian):
   file header: magic "DMTR" (4 bytes), format version u16 (=1),
     flags u16 (reserved, must be 0).
   record: type u8, flags u8 (reserved), reserved u16,
     timestamp_ns u64 (CLOCK_REALTIME, ns since Unix epoch),
     payload_len u32, payload bytes, crc32 u32 (IEEE, over every record
     byte from `type` through the payload end).
   Payloads: INPUT/OUTPUT = raw bytes; RESIZE = rows u16 + cols u16;
   META = UTF-8 "key=value\n" lines; HEARTBEAT = empty.
   Max payload DT_RECORD_MAX_PAYLOAD.  Corrupt CRC, short reads and
   unknown reserved types (>= 0x80) fail with DT_ERR_PROTOCOL/PARSE;
   unknown informational types (< 0x80) are skipped by the replayer for
   forward compatibility. */
typedef enum
{
  DT_RECORD_INPUT = 1,
  DT_RECORD_OUTPUT = 2,
  DT_RECORD_RESIZE = 3,
  DT_RECORD_META = 4,
  DT_RECORD_EVENT = 4, /* Alias kept for source compatibility. */
  DT_RECORD_HEARTBEAT = 5
} DT_RecordType;

typedef enum
{
  DT_CCTARGET_TERMSCRIPT,
  DT_CCTARGET_JS,
  DT_CCTARGET_WASM,
  DT_CCTARGET_WEBGPU,
  DT_CCTARGET_MANIM,
  DT_CCTARGET_PROCESSING,
} DT_ReplayerTarget;

/* Deterministic replay pacing. */
typedef enum
{
  DT_REPLAY_REALTIME, /* Honor recorded timestamps. */
  DT_REPLAY_FAST,     /* Deliver as fast as possible. */
  DT_REPLAY_STEP,     /* Deliver one event per dt_replayer_step. */
  DT_REPLAY_PAUSED    /* dt_replayer_next reports DT_ERR_WOULD_BLOCK. */
} DT_ReplayMode;

typedef struct
{
  DT_RecordType type;
  uint64_t timestamp_ns; /* CLOCK_REALTIME ns since the Unix epoch. */
  const uint8_t *data;   /* See ownership note on dt_replayer_next. */
  size_t size;
  unsigned short rows;   /* Valid for RESIZE events. */
  unsigned short columns;
} DT_RecordEvent;

const char *dt_record_type_string (DT_RecordType type);

/* The stream is borrowed unless take_ownership is true (then fclose'd
   by free). */
DT_Recorder *dt_recorder_create (FILE *stream, bool take_ownership,
                                 DT_Error *error);
void dt_recorder_free (DT_Recorder *recorder); /* NULL-safe, flushes. */
DT_Status dt_recorder_write (DT_Recorder *recorder,
                             const DT_RecordEvent *event, DT_Error *error);
DT_Status dt_recorder_flush (DT_Recorder *recorder, DT_Error *error);

DT_Replayer *dt_replayer_create (FILE *stream, bool take_ownership,
                                 DT_Error *error);
void dt_replayer_free (DT_Replayer *replayer); /* NULL-safe. */
/* Ownership: event data is owned by the replayer and stays valid until
   the next dt_replayer_next, dt_replayer_release_event or
   dt_replayer_free.  The caller must NOT free event->data. */
DT_Status dt_replayer_next (DT_Replayer *replayer, DT_RecordEvent *event,
                            DT_Error *error);
/* Invalidate the current event buffer early (NULL-safe args). */
void dt_replayer_release_event (DT_Replayer *replayer,
                                DT_RecordEvent *event);
DT_ReplayMode dt_replayer_get_mode (const DT_Replayer *replayer);
void dt_replayer_set_mode (DT_Replayer *replayer, DT_ReplayMode mode);
/* Speed multiplier for REALTIME mode (> 0, 1 = recorded pace). */
void dt_replayer_set_speed (DT_Replayer *replayer, double speed);
/* STEP mode: advance exactly one event (same contract as next). */
DT_Status dt_replayer_step (DT_Replayer *replayer, DT_RecordEvent *event,
                            DT_Error *error);
void dt_replayer_pause (DT_Replayer *replayer);
void dt_replayer_resume (DT_Replayer *replayer);
/* Rewind to the first record (requires a seekable stream). */
DT_Status dt_replayer_rewind (DT_Replayer *replayer, DT_Error *error);
/* Render the recording as a replay program for `target`.  Borrowed
   string, valid until the next compile call or replayer free.  The
   generated program embeds the recorded events. */
const char *dt_replayer_compile (DT_Replayer *replayer,
                                 const char *tmplate,
                                 DT_ReplayerTarget target);

/* ------------------------------------------------------------------ */
/* Messages, envelopes and connections.                                */
/* ------------------------------------------------------------------ */

/* Transport-neutral framing shared by local and remote transports:
   u32 BE total length (msgid + type + payload + crc), u64 BE message
   id, u8 message type, payload bytes, u32 BE CRC32-IEEE over
   msgid+type+payload.  Payloads larger than DT_CONN_MAX_MESSAGE are
   rejected with DT_ERR_LIMIT.  Partial sends/receives are handled
   internally (callers see whole envelopes or a timeout/EOF/cancel).
   Unknown type bytes on receive report DT_ERR_PROTOCOL. */

/* Messages and envelopes. The message owns its payload after creation. */
typedef enum
{
  DT_MESSAGE_DATA = 0,
  DT_MESSAGE_RESIZE = 1,
  DT_MESSAGE_CLOSE = 2,
  DT_MESSAGE_ERROR = 3,
  DT_MESSAGE_HEARTBEAT = 4,
  DT_MESSAGE_AUTH = 5
} DT_MessageType;

const char *dt_message_type_string (DT_MessageType type);

/* Transferred: free with dt_message_free (NULL-safe). */
DT_Message *dt_message_create (DT_MessageType type, const void *data,
                               size_t size, DT_Error *error);
DT_Message *dt_message_create_resize (unsigned short rows,
                                      unsigned short columns,
                                      DT_Error *error);
DT_Message *dt_message_create_heartbeat (DT_Error *error);
DT_Message *dt_message_create_error_msg (int code, const char *text,
                                         DT_Error *error);
void dt_message_free (DT_Message *message); /* NULL-safe. */
DT_MessageType dt_message_type (const DT_Message *message);
/* Borrowed payload, valid while the message lives. */
const uint8_t *dt_message_data (const DT_Message *message, size_t *size);
DT_Status dt_message_resize (const DT_Message *message, unsigned short *rows,
                             unsigned short *columns);

/* Transferred envelope (takes ownership of message); free with
   dt_envelope_free (NULL-safe). */
DT_Envelope *dt_envelope_create (uint64_t message_id, DT_Message *message,
                                 DT_Error *error);
void dt_envelope_free (DT_Envelope *envelope); /* NULL-safe. */
uint64_t dt_envelope_message_id (const DT_Envelope *envelope);
/* Borrowed message, valid while the envelope lives. */
DT_Message *dt_envelope_message (const DT_Envelope *envelope);
/* Transfer the message out (envelope is freed, caller owns message). */
DT_Message *dt_envelope_take_message (DT_Envelope *envelope);

typedef enum
{
  DT_CONN_CLOSED = 0,
  DT_CONN_CONNECTING = 1,
  DT_CONN_OPEN = 2,
  DT_CONN_SHUTDOWN = 3
} DT_ConnState;

const char *dt_conn_state_string (DT_ConnState state);

/* Local connection wraps an existing connected stream descriptor
   (socketpair/pipe/socket).  Borrows the fd unless take_ownership. */
typedef struct
{
  uint32_t struct_size; /* Set by dt_localconn_options_init. */
  int fd;
  bool take_ownership;
  bool nonblocking;
  int read_timeout_ms;  /* < 0 blocks forever, 0 polls, > 0 waits. */
  int write_timeout_ms;
  uint32_t reserved;
} DT_LocalConnOptions;

void dt_localconn_options_init (DT_LocalConnOptions *options);

DT_LocalConn *dt_localconn_open (const DT_LocalConnOptions *options,
                                 DT_Error *error);
void dt_localconn_close (DT_LocalConn *connection); /* NULL-safe. */
DT_Status dt_localconn_send (DT_LocalConn *connection,
                             const DT_Envelope *envelope, DT_Error *error);
DT_Status dt_localconn_receive (DT_LocalConn *connection,
                                DT_Envelope **envelope, DT_Error *error);
int dt_localconn_fd (const DT_LocalConn *connection);
DT_ConnState dt_localconn_state (const DT_LocalConn *connection);
void dt_localconn_cancel (DT_LocalConn *connection); /* Thread-safe. */
DT_Status dt_localconn_ping (DT_LocalConn *connection, uint64_t message_id,
                             DT_Error *error);
/* EOF: orderly peer shutdown (read side closed) reports DT_ERR_EOF. */

/* Remote connection: TCP client with a version/auth handshake.
   Wire handshake (ASCII lines, client first):
     C: "DT/1.0 HELLO proto=1 [token=<secret>]\n"
     S: "DT/1.0 OK\n" | "DT/1.0 DENIED\n" | "DT/1.0 VERSION-MISMATCH\n"
   DENIED maps to DT_ERR_AUTH, version problems to DT_ERR_PROTOCOL.
   SECURITY MODEL: the transport is raw TCP with NO encryption,
   NO integrity beyond framing CRCs, NO peer-credential verification
   and NO replay protection.  The token is a bearer secret sent in the
   clear: it only proves knowledge of the secret to a passive-matching
   server and provides zero confidentiality.  Never run this over a
   non-local or untrusted network without an external secure tunnel
   (TLS terminator, SSH forwarding, VPN).  Oversized frames (past
   DT_CONN_MAX_MESSAGE) are rejected before allocation. */
typedef struct
{
  uint32_t struct_size; /* Set by dt_remoteconn_options_init. */
  const char *host;     /* Borrowed, e.g. "127.0.0.1". */
  uint16_t port;
  unsigned int connect_timeout_ms;
  const char *auth_token; /* Borrowed bearer secret, NULL = none. */
  int read_timeout_ms;  /* < 0 blocks forever, 0 polls, > 0 waits. */
  int write_timeout_ms;
  int keepalive_ms;     /* Reserved; application drives pings. */
  uint32_t reserved;
} DT_RemoteConnOptions;

void dt_remoteconn_options_init (DT_RemoteConnOptions *options);

DT_RemoteConn *dt_remoteconn_open (const DT_RemoteConnOptions *options,
                                   DT_Error *error);
void dt_remoteconn_close (DT_RemoteConn *connection); /* NULL-safe. */
DT_Status dt_remoteconn_send (DT_RemoteConn *connection,
                              const DT_Envelope *envelope, DT_Error *error);
DT_Status dt_remoteconn_receive (DT_RemoteConn *connection,
                                 DT_Envelope **envelope, DT_Error *error);
int dt_remoteconn_fd (const DT_RemoteConn *connection);
DT_ConnState dt_remoteconn_state (const DT_RemoteConn *connection);
void dt_remoteconn_cancel (DT_RemoteConn *connection); /* Thread-safe. */
DT_Status dt_remoteconn_ping (DT_RemoteConn *connection, uint64_t message_id,
                              DT_Error *error);

/* ------------------------------------------------------------------ */
/* Termscript: automation language substrate.                          */
/* ------------------------------------------------------------------ */

/* Termscript is a small Turing-complete scripting language exposing all
   DomTERM features (PTY/TTY control, terminfo lookup/expansion,
   recording, connections).  Grammar: termscript/Termscript.g (compiled
   with scripts/aurocks.pl).  Two backends: the tree-walking VM below
   and ahead-of-time compilation to C (dt_termscript_compile_to_c);
   generated C holds compiled statements over the dt_rt_* runtime
   (inlined Termscript-side imports, statically linked native modules)
   and must be linked against libtermscript plus libtermscript_stdlib
   (the driver does this unless -c is passed; --static links
   statically).
   Native extension ABI: embedders implement DT_TermModule tables and
   register them with dt_termscript_register_module.  Modules shipped:
   "G" (load/puts/die) and "std.terminfo" (load/compile_to_ti/get). */

typedef enum
{
  DT_TERM_NIL = 0,
  DT_TERM_BOOL,
  DT_TERM_INT,
  DT_TERM_STRING,
  DT_TERM_HANDLE /* Opaque native handle (e.g. terminfo profile). */
} DT_TermValueType;

typedef struct DT_TermValue DT_TermValue;
struct DT_TermValue
{
  DT_TermValueType type;
  union
  {
    bool boolean;
    int64_t integer;
    char *string; /* Owned when type == STRING (free with the value). */
    void *handle; /* Owned per module contract. */
  } as;
};

typedef struct DT_TermVM DT_TermVM;

typedef DT_Status (*DT_TermFunc) (DT_TermVM *vm, const DT_TermValue *argv,
                                  size_t argc, DT_TermValue *ret,
                                  DT_Error *error);

typedef struct
{
  const char *name;   /* Borrowed static string. */
  DT_TermFunc func;   /* Must be non-NULL. */
} DT_TermFuncDef;

typedef struct
{
  const char *name;           /* Borrowed static string, e.g. "std.gpio". */
  const DT_TermFuncDef *funcs;/* Borrowed NULL-terminated table. */
} DT_TermModule;

/* Register a native module (copies the reference, not the table:
   the table must stay alive).  Thread-safe.  Duplicate names fail. */
DT_Status dt_termscript_register_module (const DT_TermModule *module,
                                         DT_Error *error);

DT_TermVM *dt_termscript_create (DT_Error *error);
/* Borrowed embedding VM; valid until dt_termscript_free. Use to register
 * per-instance native modules and search paths alongside DomTERM bindings. */
struct TS_VM;
struct TS_VM *dt_termscript_inner_vm (DT_TermVM *vm);
void dt_termscript_free (DT_TermVM *vm); /* NULL-safe. */
/* Run source text.  Captured G:puts output is appended to *output when
   non-NULL (caller frees with free()). */
DT_Status dt_termscript_run_string (DT_TermVM *vm, const char *source,
                                    char **output, DT_Error *error);
DT_Status dt_termscript_run_file (DT_TermVM *vm, const char *path,
                                  char **output, DT_Error *error);
/* Transpile to C (AOT backend for the termscript driver).  Borrowed
   string, valid until the next compile call or vm free.  Generated C
   is real, ahead-of-time compiled C: statements become C control flow
   calling the dt_rt_* runtime below (nothing is parsed at run time).
   Termscript-side libraries (`.tsc` companions loaded with
   `G:import`) are resolved at compile time and inlined as compiled C;
   native modules (`G`, `std.*`, custom) link (statically) and resolve
   through the registry at run time. */
const char *dt_termscript_compile_to_c (DT_TermVM *vm, const char *source,
                                        const char *unit_name,
                                        DT_Error *error);
/* Compiled-code runtime for generated C (mirrors ts_rt_*; `line` feeds
 * error offsets).  dt_rt_set adopts *val on success. */
DT_Status dt_rt_call (DT_TermVM *vm, const DT_TermValue *mod,
                      const char *modname, const char *func,
                      const DT_TermValue *argv, size_t argc,
                      DT_TermValue *ret, unsigned line, DT_Error *error);
DT_Status dt_rt_get (DT_TermVM *vm, const char *name, DT_TermValue *out,
                     unsigned line, DT_Error *error);
DT_Status dt_rt_set (DT_TermVM *vm, const char *name, DT_TermValue *val,
                     bool is_const, unsigned line, DT_Error *error);
DT_Status dt_rt_step (DT_TermVM *vm, DT_Error *error);
DT_Status dt_rt_mkstring (DT_TermValue *val, const char *s,
                          DT_Error *error);
bool dt_rt_truthy (const DT_TermValue *val);
/* Borrowed captured G:puts output ("" when empty, never NULL). */
const char *dt_rt_output (const DT_TermVM *vm);
/* Value helpers (DT_TERM_STRING payloads are heap copies). */
void dt_termvalue_free (DT_TermValue *value);
DT_Status dt_termvalue_to_string (const DT_TermValue *value, char **out,
                                  DT_Error *error);

#ifdef __cplusplus
}
#endif

#endif /* DOMTERM_H */
