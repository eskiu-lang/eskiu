# Eskiu playground image.
#
# The build context is this directory (playground/). The image downloads the
# Linux x86-64 release of eskiuc, checks it against the release's SHA256SUMS and
# unpacks it under /opt/eskiu (bin/eskiuc, lib/deps, lib/eskiu/stdlib).
#
#   docker build -t eskiu-playground .
#   docker build --build-arg ESKIU_VERSION=0.12.0 -t eskiu-playground .
#
# The release binary statically links LLVM, so runtime only needs a few shared
# libs plus a C linker driver (gcc): `eskiuc run` links the emitted object into a
# temporary executable with $CC.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl \
        python3 python3-pip \
        gcc libc6-dev \
        libstdc++6 zlib1g libz3-4 libzstd1 libgcc-s1 \
    && rm -rf /var/lib/apt/lists/*

ARG ESKIU_VERSION=0.12.0
RUN set -eu; \
    base="https://github.com/eskiu-lang/eskiu/releases/download/v${ESKIU_VERSION}"; \
    asset="eskiuc-linux-x86_64.tar.gz"; \
    cd /tmp; \
    curl -fsSLO "$base/$asset"; \
    curl -fsSL "$base/SHA256SUMS" | grep " $asset\$" | sha256sum -c -; \
    mkdir -p /opt/eskiu; \
    tar -xzf "$asset" -C /opt/eskiu; \
    rm -f "$asset"; \
    /opt/eskiu/bin/eskiuc --version

ENV ESKIU_ROOT=/opt/eskiu/lib/eskiu \
    ESKIUC=/opt/eskiu/bin/eskiuc \
    CC=gcc \
    PATH=/opt/eskiu/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app
COPY requirements.txt .
RUN pip3 install --no-cache-dir --break-system-packages -r requirements.txt
COPY app.py .

# Run unprivileged. User code inherits this UID under the per-request rlimits.
RUN useradd --create-home --uid 10001 runner
USER runner

EXPOSE 8080
CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8080", "--workers", "2"]
