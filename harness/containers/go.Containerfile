# NullTorch runtime — Go cell
# Pin by digest at release time; tag pin here is the v1 placeholder.
FROM docker.io/library/golang:1.24-bookworm

# Enforcement: no Python (and no pip-installable escape hatch) in the runtime
# image. Grading happens in the harness sidecar (see ../PROTOCOL.md).
RUN apt-get purge -y 'python3*' 'python*' 2>/dev/null; \
    apt-get autoremove -y; \
    rm -rf /usr/bin/python* /usr/local/bin/python*; \
    ! command -v python3 && ! command -v python

WORKDIR /work
# /fixtures/public, /docs/openbook (open-book cells), /task are bind-mounted
# read-only by the harness at cell start.
