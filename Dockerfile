# syntax=docker/dockerfile:1.6
FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential cmake ca-certificates git libpcap-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
      libstdc++6 libpcap0.8 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY --from=build /src/build/mdfeed_itch /app/mdfeed_itch
COPY --from=build /src/build/handler_bench /app/handler_bench
COPY --from=build /src/build/pcap_replay /app/pcap_replay
ENTRYPOINT ["/app/mdfeed_itch"]
