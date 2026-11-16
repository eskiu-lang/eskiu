# ghcr.io/eskiu-lang/agent: a machine ready for a coding agent to build, run, drive and test
# Eskiu programs and ui apps. It holds eskiuc and the ui tool (from their releases),
# emscripten for web builds, Google Chrome for `ui agent web` and `ui test web`, and a C
# compiler for native builds. Sans-serif and emoji fonts make Chrome's screenshots look as
# they do on a desktop, and tini reaps the processes `ui agent --stop` ends.
#
#   docker run --rm -it ghcr.io/eskiu-lang/agent
#   ui new myapp && cd myapp && ui agent web --json tree
FROM ubuntu:24.04

ARG ESKIU_VERSION=latest
ARG UI_VERSION=latest
ARG EMSDK_VERSION=5.0.2
ARG INSTALL_SH=https://eskiu-lang.org/install.sh

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends ca-certificates curl git xz-utils python3 clang lld \
      make pkg-config unzip fonts-dejavu-core fonts-liberation fonts-noto-core fonts-noto-color-emoji tini \
 && curl -fsSL -o /tmp/chrome.deb https://dl.google.com/linux/direct/google-chrome-stable_current_amd64.deb \
 && apt-get install -y --no-install-recommends /tmp/chrome.deb \
 && rm -rf /tmp/chrome.deb /var/lib/apt/lists/*

RUN git clone --depth 1 https://github.com/emscripten-core/emsdk /opt/emsdk \
 && /opt/emsdk/emsdk install "$EMSDK_VERSION" \
 && /opt/emsdk/emsdk activate "$EMSDK_VERSION" \
 && rm -rf /opt/emsdk/downloads

RUN curl -fsSL "$INSTALL_SH" | ESKIU_PREFIX=/opt/eskiu ESKIU_VERSION="$ESKIU_VERSION" UI_VERSION="$UI_VERSION" sh -s -- --with-ui

ENV EMSDK=/opt/emsdk \
    PATH=/opt/eskiu/bin:/opt/emsdk:/opt/emsdk/upstream/emscripten:$PATH \
    UI_CHROME=google-chrome \
    UI_CHROME_ARGS=--no-sandbox

WORKDIR /work
ENTRYPOINT ["/usr/bin/tini", "--"]
CMD ["bash"]
