# ==============================================================================
# Stage 1: Build C++20 binaries and generate baseline benchmark/sweep datasets
# ==============================================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    cmake \
    ninja-build \
    ca-certificates \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY benchmarks ./benchmarks

RUN cmake -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DMARTINGALE_BUILD_TESTS=OFF \
    -DMARTINGALE_BUILD_BENCHMARKS=ON \
    && cmake --build build --target martingale_experiments martingale_benchmarks

RUN mkdir -p /app/bin /app/data/processed \
    && cp build/martingale_experiments /app/bin/martingale_experiments \
    && /app/bin/martingale_experiments --export-dir /app/data/processed \
    && ./build/benchmarks/martingale_benchmarks \
        --benchmark_min_time=0.1s \
        --benchmark_out=/app/data/processed/benchmarks.json \
        --benchmark_out_format=json

# ==============================================================================
# Stage 2: Lightweight Python 3.12 + Streamlit Runtime
# ==============================================================================
FROM python:3.12-slim AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY python/requirements.txt ./python/requirements.txt
RUN pip install --no-cache-dir -r python/requirements.txt

COPY --from=builder /app/bin/martingale_experiments /app/bin/martingale_experiments
COPY --from=builder /app/data/processed /app/data/processed
COPY python ./python

ENV MARTINGALE_EXPERIMENTS_BIN=/app/bin/martingale_experiments \
    MARTINGALE_DATA_DIR=/app/data/processed \
    PYTHONUNBUFFERED=1

EXPOSE 8501

CMD ["sh", "-c", "streamlit run python/app.py --server.port=${PORT:-8501} --server.address=0.0.0.0 --server.headless=true"]
