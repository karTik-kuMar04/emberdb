# syntax=docker/dockerfile:1

# ---------- build stage: the compiler lives here and never reaches the final image ----------
FROM ubuntu:24.04 AS build

RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ninja-build \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY include include
COPY src src

RUN cmake --preset release -DEMBERDB_BUILD_TESTS=OFF \
 && cmake --build --preset release --target emberdb \
 && strip build/release/emberdb

# ---------- runtime stage: a slim base, the binary, and nothing else ----------
FROM ubuntu:24.04 AS runtime

ARG VERSION=dev
LABEL org.opencontainers.image.title="emberdb" \
      org.opencontainers.image.description="Redis-compatible in-memory database written in C++20" \
      org.opencontainers.image.version="${VERSION}"

# The same Ubuntu release as the build stage, so the C++ runtime library matches.
RUN apt-get update \
 && apt-get install -y --no-install-recommends libstdc++6 \
 && rm -rf /var/lib/apt/lists/* \
 && groupadd --gid 10001 emberdb \
 && useradd --uid 10001 --gid emberdb --no-create-home --shell /usr/sbin/nologin emberdb \
 && mkdir /data \
 && chown emberdb:emberdb /data

COPY --from=build /src/build/release/emberdb /usr/local/bin/emberdb

USER emberdb:emberdb
WORKDIR /data
VOLUME /data
EXPOSE 6380 9390
STOPSIGNAL SIGTERM

# Liveness: asks the metrics endpoint's /healthz. bash's /dev/tcp means the image needs no curl.
HEALTHCHECK --interval=5s --timeout=3s --start-period=5s --retries=3 \
  CMD ["bash", "-c", "exec 3<>/dev/tcp/127.0.0.1/9390 && printf 'GET /healthz HTTP/1.0\\r\\n\\r\\n' >&3 && head -n1 <&3 | grep -q ' 200 '"]

# Fixed container settings live here. Anything given to `docker run IMAGE ...` is appended, and
# the last occurrence of a flag wins.
ENTRYPOINT ["emberdb", "--bind", "0.0.0.0", "--metrics-bind", "0.0.0.0", "--metrics-port", "9390", "--aof", "/data/appendonly.aof"]
CMD []
