# syntax=docker/dockerfile:1
# ---------------------------------------------------------------------------
# Stage 1: build ubersdr_ndb
# ---------------------------------------------------------------------------
FROM ubuntu:24.04 AS ndb-builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        git \
        libcurl4-openssl-dev \
        libfftw3-dev \
        libssl-dev \
        pkg-config \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# Clone IXWebSocket (no system package available on Ubuntu 24.04)
RUN git clone --depth 1 https://github.com/machinezone/IXWebSocket.git /ixwebsocket

# Beacon database: the OurAirports navaid list, fetched at build time so each
# image carries a current copy. Its own layer, ahead of the sources, so a code
# change does not re-download it — rebuild with --no-cache to refresh it.
RUN curl -fsSL -o /navaids.csv https://davidmegginson.github.io/ourairports-data/navaids.csv \
    && head -1 /navaids.csv | grep -q frequency_khz

WORKDIR /src
COPY CMakeLists.txt .
COPY third_party/ ./third_party/
COPY src/ ./src/
COPY tools/ ./tools/

RUN cmake -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DIXWS_ROOT=/ixwebsocket \
    && cmake --build build --parallel "$(nproc)" --target ubersdr_ndb

# ---------------------------------------------------------------------------
# Stage 2: runtime image
# ---------------------------------------------------------------------------
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        libcurl4 \
        libfftw3-single3 \
        libssl3 \
        zlib1g \
        ca-certificates \
        wget \
    && rm -rf /var/lib/apt/lists/* \
    && useradd -r -u 1999 -s /bin/false ndb \
    && mkdir -p /data && chown ndb /data

COPY --from=ndb-builder /src/build/ubersdr_ndb /usr/local/bin/ubersdr_ndb
COPY --from=ndb-builder /navaids.csv /usr/local/share/ubersdr_ndb/navaids.csv
COPY static/ /usr/local/share/ubersdr_ndb/static/

# Copy entrypoint script (translates env vars to ubersdr_ndb flags)
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

USER ndb

# Expose the web UI port (default; override with WEB_PORT env var)
EXPOSE 6100

HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3 \
    CMD ["/usr/bin/wget", "-q", "-O", "/dev/null", "http://localhost:6100/api/status"]

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
