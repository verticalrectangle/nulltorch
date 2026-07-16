# NullTorch runtime — C++ cell (C++20)
# Pin by digest at release time; tag pin here is the v1 placeholder.
FROM docker.io/library/gcc:14-bookworm

# Enforcement: no Python in the runtime image (see ../PROTOCOL.md).
RUN apt-get purge -y 'python3*' 'python*' 2>/dev/null; \
    apt-get autoremove -y; \
    rm -rf /usr/bin/python* /usr/local/bin/python*; \
    ! command -v python3 && ! command -v python

# Stdlib-only: no third-party dev packages are installed; the harness build
# phase compiles with `g++ -std=c++20` and no -I/-l beyond the toolchain.
WORKDIR /work
