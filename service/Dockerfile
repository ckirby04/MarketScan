# The research job service: ms_research built for Linux, behind FastAPI.
#
# Build context is the repository root, not service/ -- the engine's sources,
# the contracts and the research layer's validator all come from there:
#
#   docker build -f service/Dockerfile -t marketscan-research .
#
# or, with no local Docker, on Cloud Build: see service/cloudbuild.yaml and
# service/deploy.ps1.

# --- Build ---------------------------------------------------------------------
# Debian bookworm on both stages, so the engine is built against the same C and
# C++ runtime it runs on. Its CMake is 3.25.1, the project's minimum.
FROM debian:bookworm AS build

ARG VCPKG_BASELINE=1577f17ee57f42a0ef6d75bbb82cb37d0b76d7e8

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git curl zip unzip tar pkg-config ca-certificates \
 && rm -rf /var/lib/apt/lists/*

# vcpkg pinned to vcpkg.json's builtin-baseline, as CI pins it, so the image
# gets the dependency versions everything else was tested with.
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
 && git -C /opt/vcpkg checkout ${VCPKG_BASELINE} \
 && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics
ENV VCPKG_ROOT=/opt/vcpkg

WORKDIR /src
COPY vcpkg.json CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY include include
COPY src src
COPY apps apps

# Only ms_research: no GUI, no tests, no theory plugins. The live seam is a
# separate library ms_research does not link, so the image cannot reach a broker.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_TARGET_TRIPLET=x64-linux \
        -DVCPKG_MANIFEST_FEATURES= \
        -DMARKETSCAN_BUILD_GUI=OFF \
        -DMARKETSCAN_BUILD_TESTS=OFF \
        -DMARKETSCAN_BUILD_THEORIES=OFF \
        -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
 && cmake --build build --target ms_research \
 && strip build/bin/ms_research

# --- Run -----------------------------------------------------------------------
FROM python:3.12-slim-bookworm

WORKDIR /app
COPY service/requirements.txt service/requirements.txt
RUN pip install --no-cache-dir -r service/requirements.txt

COPY --from=build /src/build/bin/ms_research /usr/local/bin/ms_research
COPY contracts contracts
COPY research/__init__.py research/contracts.py research/
COPY data/samples/research data/samples/research
COPY service/app service/app

ENV MS_RESEARCH_BIN=/usr/local/bin/ms_research \
    MS_CONTRACTS=/app/contracts/research \
    MS_RUNS_DIR=/tmp/runs \
    PYTHONUNBUFFERED=1 \
    PORT=8080

# Fails the build, rather than the first request, if the engine does not start.
RUN ms_research --help > /dev/null

RUN useradd --create-home --uid 10001 service
USER service

WORKDIR /app/service
CMD exec uvicorn app.main:create_app --factory --host 0.0.0.0 --port ${PORT}
