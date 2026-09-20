# src/relay/Dockerfile -- the UDP relay image (tracker dist:RP4, plan D15(b)).
#
# ASSUMPTION, STATED BECAUSE R1 IS IN FLIGHT IN PARALLEL: this Dockerfile builds the workspace
# member `src/relay` as it stands at base commit 8f9814aa -- a PLACEHOLDER binary named `mh_relay`
# (see src/relay/Cargo.toml `[[bin]] name = "mh_relay"`, src/relay/README.md) whose only real code
# is the wire codec (`src/wire.rs`, mp:T0) against chacha20/hmac/sha2 -- no tokio, no rustls, no
# socket yet. `dist R1` replaces src/main.rs wholesale with the real tokio UDP forwarder; AS LONG
# AS R1 keeps the package/bin name `mh_relay` and stays a `[[bin]]` target of this same workspace
# member, nothing below needs to change. If R1 renames the crate or the binary, update BIN_NAME
# (one line, marked below) and the two `--bin` flags that reference it.
#
# WHY THE BUILD CONTEXT IS THE REPO ROOT, NOT src/relay. `src/relay/Cargo.toml` reads
# `version.workspace = true` etc. from the root `[workspace.package]` (Cargo.toml at the repo
# root, dist DS1) -- a workspace member cannot be built with its own directory as the context. The
# `.dockerignore` at the repo root allowlists exactly what this build needs (the two Rust workspace
# members' manifests + sources) so `docker build -f src/relay/Dockerfile .` does not tar the ~2.5k
# files of C++/Python/RE-doc tree that make up the rest of this monorepo.
#
# WHY src/launcher IS COPIED TOO, EVEN THOUGH THIS IMAGE NEVER RUNS IT: `[workspace] members =
# ["src/launcher", "src/relay"]` at the repo root means `cargo` parses BOTH members' manifests (and
# stats their declared `[[bin]]` source paths) to resolve the workspace at all, `-p`/`--bin`
# filtering notwithstanding -- omitting it fails cargo's workspace load, not merely wastes a copy.
# `cargo chef prepare --bin mh_relay` (below) is what keeps the RECIPE (and so the dependency
# COMPILE) scoped to just mh_relay's closure -- the launcher's `eframe`/`wgpu`/`rfd` GUI stack (which
# needs X11/Wayland dev headers this musl builder does not carry) is never cooked.
#
# WHY clux/muslrust, NOT a plain rust: image + `rustup target add …-musl`. musl targets link fully
# static by default with no extra RUSTFLAGS, which is the whole point of shipping into
# distroless/static -- but "distroless/static + rustls" (this row's own scope line) means whichever
# crypto backend rustls picks up (`ring` or `aws-lc-rs`, both R1's call, not this file's) needs a C
# compiler AND a musl-targeted libc to link against, which plain rustup does not provide. muslrust
# bundles musl-gcc + the musl sysroot precisely for that combination; a bare `rust:1-slim` image
# would need `apt-get install musl-tools` at minimum and still lacks the sysroot maintained here.
#
# UNVERIFIED ON THIS MACHINE: Docker is not installed here (2026-09-17) -- see docs/deploy.md
# "What is verified vs. not". `hadolint` ran clean against this file (see that doc for the command).

# syntax=docker/dockerfile:1.7

ARG RELAY_BIN=mh_relay
ARG MUSL_TARGET=x86_64-unknown-linux-musl
# Pinned, not floated -- reproducible builds are the whole reason cargo-chef exists; a version
# nobody revisits is fine for a tool invoked identically on every CI run.
ARG CARGO_CHEF_VERSION=0.1.78

FROM clux/muslrust:stable AS chef
ARG CARGO_CHEF_VERSION
RUN cargo install --locked cargo-chef --version ${CARGO_CHEF_VERSION}
WORKDIR /build

# ---- planner: compute a dependency-only recipe, scoped to mh_relay ----
FROM chef AS planner
ARG RELAY_BIN
COPY Cargo.toml Cargo.lock rust-toolchain.toml ./
COPY src/relay src/relay
COPY src/launcher src/launcher
# --bin scopes the recipe to mh_relay's OWN dependency closure -- cargo-chef "ignores workspace
# members unnecessary for compiling that specific binary" (its own --bin flag help text). This is
# the line that keeps the launcher's GUI dependency tree out of the next stage's build entirely.
RUN cargo chef prepare --bin ${RELAY_BIN} --recipe-path recipe.json

# ---- builder: cook the dependency layer (cacheable), then build the real binary ----
FROM chef AS builder
ARG RELAY_BIN
ARG MUSL_TARGET
COPY --from=planner /build/recipe.json recipe.json
# `--mount=type=cache` keeps the registry + the dependency-only build products warm across CI runs
# (paired with images.yml's per-image `type=gha` buildx cache scope for the LAYERS themselves --
# this cache is for what buildx cannot see: cargo's own incremental/registry state inside the
# layer). `cook` builds ONLY what `prepare --bin` put in the recipe, so this step never touches
# launcher's dependencies.
RUN --mount=type=cache,target=/root/.cargo/registry \
    --mount=type=cache,target=/build/target \
    cargo chef cook --release --target ${MUSL_TARGET} -p ${RELAY_BIN} --bin ${RELAY_BIN} \
        --recipe-path recipe.json
COPY Cargo.toml Cargo.lock rust-toolchain.toml ./
COPY src/relay src/relay
COPY src/launcher src/launcher
# The cache mount on /build/target means the compiled binary does not survive into the next stage
# by itself -- copy it OUT of the cache mount, into a plain layer, in the same RUN.
RUN --mount=type=cache,target=/root/.cargo/registry \
    --mount=type=cache,target=/build/target \
    cargo build --release --target ${MUSL_TARGET} -p ${RELAY_BIN} --bin ${RELAY_BIN} && \
    cp target/${MUSL_TARGET}/release/${RELAY_BIN} /build/${RELAY_BIN}

# ---- runtime: distroless/static, nothing else -- no shell, no package manager, no libc even ----
# (musl-static binaries need none of glibc's dynamic loader either). amd64 only (images.yml builds
# no other platform for this image); "nonroot" runs as uid 65532 with no further USER step needed.
FROM gcr.io/distroless/static-debian12:nonroot AS runtime
ARG RELAY_BIN
COPY --from=builder /build/${RELAY_BIN} /usr/local/bin/relay
# ENTRYPOINT cannot reference an ARG (distroless has no shell to expand it), hence the fixed name
# above -- the one place BIN_NAME really would need a matching edit if R1 renames the binary.
ENTRYPOINT ["/usr/local/bin/relay"]
