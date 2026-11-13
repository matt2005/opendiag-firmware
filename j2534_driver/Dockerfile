# Two stages. The host tests run on Ubuntu 24.04 with GCC 14 and gcovr 7.0, the toolchain the CI
# coverage gate uses. The Windows package stays on Debian Bookworm's MSVCRT MinGW toolchain, so the
# DLLs are built with the same compiler and SDK as before. Keep the pins in both stages.

FROM ubuntu:24.04@sha256:2198e4c65076e9cb0b3d695219df667c0e3b0072da64ca3c2648aca17dcecaf5 AS test

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    python3 \
    python3-protobuf \
    python3-venv \
    protobuf-compiler \
    gcc-14 \
    g++-14 \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc-14 140 \
    && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++-14 140 \
    && update-alternatives --install /usr/bin/gcov gcov /usr/bin/gcov-14 140 \
    && python3 -m venv --system-site-packages /opt/gcovr \
    && /opt/gcovr/bin/pip install --no-cache-dir gcovr==7.0

ENV PATH=/opt/gcovr/bin:$PATH

WORKDIR /project
COPY . .
RUN make -C j2534_driver -j2 test && touch /opendiag-host-tests.passed

FROM debian:bookworm-slim@sha256:5ae3c39ebd15e229dcedd5cee596b2497182493d41ff162e824ba13fc1b2b867 AS package

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    python3 \
    python3-protobuf \
    protobuf-compiler \
    gcc-mingw-w64-i686-win32=12.2.0-14+deb12u1+25.2+b1 \
    g++-mingw-w64-i686-win32=12.2.0-14+deb12u1+25.2+b1 \
    gcc-mingw-w64-i686-win32-runtime=12.2.0-14+deb12u1+25.2+b1 \
    mingw-w64-i686-dev=10.0.0-3 \
    mingw-w64-common=10.0.0-3 \
    binutils-mingw-w64-i686=2.40-2+10.4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /project
# The package stage starts only after the host tests in the test stage have passed.
COPY --from=test /opendiag-host-tests.passed /opendiag-host-tests.passed
COPY . .
