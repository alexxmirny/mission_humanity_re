"""mh report collector (tracker dist:RP2).

A small FastAPI service that accepts crash/bug report zips from the launcher
(RP1, Rust, not this package), authenticates them with a shared-secret HMAC,
stores them under a ULID-keyed path, and indexes them in SQLite with
sha256-based de-duplication. Designed to sit behind Caddy (see ../Caddyfile)
which terminates TLS (self-signed until a domain exists -- plan D14 / section
6 answer 4) and enforces the same 64 MB body cap at the edge.

See ../README.md for how to run it and for the design decisions this implements
(the MP refinement plan's D13 and D14: HMAC-authenticated zip uploads, ULID storage, dedupe).
"""

__version__ = "1.0.0"
