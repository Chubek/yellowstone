# Chapter 13: C ABI

ld/ld.h is the stable FFI boundary. Options are caller-owned for a call, diagnostics are malloc-owned, and qld_free helpers release returned memory.

## Scope

This chapter is part of the qobjfile manual and documents the public behavior that callers can rely on. Examples should be checked against the current headers and test suite.
