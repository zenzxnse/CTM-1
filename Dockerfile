ARG NODE_IMAGE=node:24.15.0-bookworm-slim
FROM ${NODE_IMAGE} AS workbench
WORKDIR /workbench
COPY workbench/package.json workbench/package-lock.json ./
RUN npm ci --ignore-scripts
COPY workbench/ ./
RUN npm run check && npm run build

ARG UBUNTU_IMAGE=ubuntu:24.04
FROM ${UBUNTU_IMAGE} AS build
ARG TARGETARCH
ARG CMAKE_VERSION=4.3.3
ARG CONTEXT_HMI_CONSTRAINED=OFF
ARG CONTEXT_HMI_ENABLE_TLS=OFF
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       ca-certificates curl g++ git ninja-build libjsoncpp-dev libssl-dev \
       uuid-dev zlib1g-dev \
    && case "${TARGETARCH}" in \
         amd64) cmake_arch=x86_64; cmake_sha=927b2368a946c37269c3a66225ab00544e756459cdd0b5d0da438694fb9ff802 ;; \
         arm64) cmake_arch=aarch64; cmake_sha=9ea38356dbd3e32e51029a3e09a0f2f8e117ef4fbcaad7a21ffb36409bbd5cb4 ;; \
         *) echo "unsupported container architecture: ${TARGETARCH}" >&2; exit 2 ;; \
       esac \
    && curl --fail --location --silent --show-error \
       "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-${cmake_arch}.tar.gz" \
       --output /tmp/cmake.tar.gz \
    && echo "${cmake_sha}  /tmp/cmake.tar.gz" | sha256sum --check --strict \
    && mkdir -p /opt/cmake \
    && tar --extract --gzip --file /tmp/cmake.tar.gz --directory /opt/cmake --strip-components=1 \
    && rm /tmp/cmake.tar.gz \
    && rm -rf /var/lib/apt/lists/*
ENV PATH=/opt/cmake/bin:${PATH}
WORKDIR /src
COPY . .
COPY --from=workbench /workbench/build /src/workbench/build
RUN cmake -S . -B build/container -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX=/opt/context-hmi \
      -DBUILD_TESTING=OFF \
      -DCONTEXT_HMI_BUILD_BENCHMARKS=OFF \
      -DCONTEXT_HMI_CONSTRAINED=${CONTEXT_HMI_CONSTRAINED} \
      -DCONTEXT_HMI_ENABLE_TLS=${CONTEXT_HMI_ENABLE_TLS} \
      -DCONTEXT_HMI_WARNINGS_AS_ERRORS=ON \
    && cmake --build build/container --parallel 2 \
    && cmake --install build/container

FROM ${UBUNTU_IMAGE} AS runtime
RUN apt-get update \
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
       ca-certificates curl libjsoncpp25 libssl3t64 libuuid1 zlib1g \
    && rm -rf /var/lib/apt/lists/* \
    && groupadd --gid 65532 context-hmi \
    && useradd --uid 65532 --gid 65532 --no-create-home --shell /usr/sbin/nologin context-hmi \
    && install -d -o 65532 -g 65532 -m 0700 /runtime
COPY --from=build /opt/context-hmi /opt/context-hmi
USER 65532:65532
EXPOSE 8080
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s --retries=3 \
  CMD ["curl", "--fail", "--silent", "http://127.0.0.1:8080/api/v1/health"]
ENTRYPOINT ["/opt/context-hmi/bin/context-hmi"]
CMD ["--config", "/opt/context-hmi/share/context-hmi/config/context-hmi.json", "--host", "0.0.0.0", "--allow-remote", "--model", "/opt/context-hmi/share/context-hmi/examples/machines/assembly-line.json", "--models", "/opt/context-hmi/share/context-hmi/examples/machines", "--web-dir", "/opt/context-hmi/share/context-hmi/workbench", "--store-file", "/runtime/operations.chj", "--log-file", "/runtime/diagnostics.jsonl"]
