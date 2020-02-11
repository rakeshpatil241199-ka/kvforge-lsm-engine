# Stage 1: Build C++17 engine & run GoogleTest suite
FROM gcc:9.3 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    ninja-build \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include/ ./include/
COPY src/ ./src/
COPY tests/ ./tests/

RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DKVFORGE_BUILD_TESTS=ON \
    && cmake --build build -j"$(nproc)" \
    && ctest --test-dir build --output-on-failure

# Stage 2: Minimal runtime with kvforge_cli + Python systems tooling
FROM python:3.8-slim

WORKDIR /app
COPY --from=builder /src/build/kvforge_cli /usr/local/bin/kvforge_cli
COPY tools/ ./tools/

RUN python3 -m unittest discover -s /app/tools -p "test_*.py" -v

ENTRYPOINT ["kvforge_cli"]
CMD ["--db", "/data/kvforge"]
