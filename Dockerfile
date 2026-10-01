# Two stages: a full toolchain to build, and a small runtime image that
# only carries the two simulator binaries.

FROM ubuntu:24.04 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends g++ cmake ninja-build python3 ccache \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The cache mount keeps ccache's data between image builds, so a rebuild
# after a small change only recompiles what actually changed.
ENV CCACHE_DIR=/ccache
RUN --mount=type=cache,target=/ccache \
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DSIGFLOW_BUILD_TESTS=OFF \
    && cmake --build build \
    && cmake --install build --prefix /opt/sigflow \
    && ccache --show-stats

FROM ubuntu:24.04
COPY --from=build /opt/sigflow/bin/ /usr/local/bin/
# Run as an unprivileged user, the simulators do not need root.
RUN useradd --system --no-create-home sigflow
USER sigflow
CMD ["sigflow-enb"]
