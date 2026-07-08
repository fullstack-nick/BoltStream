FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    clang-format \
    clang-tidy \
    cmake \
    curl \
    git \
    ninja-build \
    pkg-config \
    python3 \
    tar \
    xz-utils \
    zip \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake --preset linux-gcc-debug \
  && cmake --build --preset build-linux-gcc-debug \
  && ctest --preset test-linux-gcc-debug \
  && cmake --preset linux-gcc-release \
  && cmake --build --preset build-linux-gcc-release

FROM ubuntu:24.04 AS runtime

RUN useradd --system --home /var/lib/boltstream --shell /usr/sbin/nologin boltstream \
  && mkdir -p /var/lib/boltstream /opt/boltstream \
  && chown -R boltstream:boltstream /var/lib/boltstream

COPY --from=builder /src/build/linux-gcc-release/boltstream-server /usr/local/bin/boltstream-server
COPY --from=builder /src/build/linux-gcc-release/boltstream-producer /usr/local/bin/boltstream-producer
COPY --from=builder /src/build/linux-gcc-release/boltstream-consumer /usr/local/bin/boltstream-consumer
COPY --from=builder /src/build/linux-gcc-release/boltstream-bench /usr/local/bin/boltstream-bench
COPY --from=builder /src/build/linux-gcc-release/boltstream-logtool /usr/local/bin/boltstream-logtool

EXPOSE 9000
USER boltstream
ENTRYPOINT ["boltstream-server"]
CMD ["--listen", "0.0.0.0:9000", "--admin-listen", "127.0.0.1:9100", "--data", "/var/lib/boltstream"]
