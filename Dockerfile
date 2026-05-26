FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y gcc make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN make clean && make

FROM debian:bookworm-slim
COPY --from=build /src/schema-init /sbin/schema-init
COPY --from=build /src/services /etc/schema-init/services
# test services need these binaries
RUN apt-get update && apt-get install -y bsdutils coreutils && rm -rf /var/lib/apt/lists/*
ENTRYPOINT ["/sbin/schema-init"]
