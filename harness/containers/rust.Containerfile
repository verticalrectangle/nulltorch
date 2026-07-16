# NullTorch runtime — Rust cell
# Pin by digest at release time; tag pin here is the v1 placeholder.
FROM docker.io/library/rust:1.83-bookworm

# Enforcement: no Python in the runtime image (see ../PROTOCOL.md).
RUN apt-get purge -y 'python3*' 'python*' 2>/dev/null; \
    apt-get autoremove -y; \
    rm -rf /usr/bin/python* /usr/local/bin/python*; \
    ! command -v python3 && ! command -v python

# Stdlib-only is enforced by the harness: `cargo build --offline` with an
# empty registry; any [dependencies] entry fails the build phase.
WORKDIR /work
