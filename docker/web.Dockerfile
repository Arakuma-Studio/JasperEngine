# Emscripten build image for the engine (web target).
FROM emscripten/emsdk:6.0.10

RUN apt-get update && apt-get install -y --no-install-recommends ninja-build ccache \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*'

# emsdk lives in a root-owned directory; the build runs as the host user and
# needs to write Emscripten's cache.
ENV EM_CACHE=/tmp/emcache

# eMudhra intermediate CA compromise (October 2026): ca-certificates 20260816 distrusts
# "emSign ECC Root CA - G3". The upgrade leaves the root in the Java keystore, so it is
# removed there too. The build fails while either still trusts it.
ARG EMSIGN_G3_ALIAS="debian:emsign_ecc_root_ca_-_g3.pem"
RUN apt-get update && apt-get install -y --no-install-recommends --only-upgrade ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ! test -e /etc/ssl/certs/emSign_ECC_Root_CA_-_G3.pem \
    && if keytool -list -cacerts -storepass changeit -alias "$EMSIGN_G3_ALIAS" >/dev/null 2>&1; then \
        keytool -delete -cacerts -storepass changeit -alias "$EMSIGN_G3_ALIAS"; \
    fi \
    && ! keytool -list -cacerts -storepass changeit | grep -qi 'emsign_ecc_root_ca_-_g3'

# Chrome for Testing's headless shell, for tools/web/browser_gate.py (tools/build.sh web-gate, the
# CI web job): the web build runs in it on SwiftShader WebGL2. Pinned by version and sha256 and
# bumped by hand (docs/testing.md); the runtime libraries it links come from the distribution, and
# the build fails if one is missing. A layer of its own, after every other one.
ARG CHROME_HEADLESS_SHELL_VERSION=156.0.8078.47
ARG CHROME_HEADLESS_SHELL_SHA256=0d5acc62c3e213f8c87d3c298d66ffcf9a236d12ea4b9f85318bf85dbc279daa
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl unzip fonts-liberation \
        libnss3 libnspr4 libatk1.0-0t64 libatk-bridge2.0-0t64 libatspi2.0-0t64 libcups2t64 libdbus-1-3 \
        libdrm2 libgbm1 libxkbcommon0 libx11-6 libxcb1 libxcomposite1 libxdamage1 libxext6 libxfixes3 \
        libxrandr2 libpango-1.0-0 libcairo2 libasound2t64 libexpat1 libglib2.0-0t64 libudev1 \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL -o /tmp/chrome-headless-shell.zip \
        "https://storage.googleapis.com/chrome-for-testing-public/${CHROME_HEADLESS_SHELL_VERSION}/linux64/chrome-headless-shell-linux64.zip" \
    && echo "${CHROME_HEADLESS_SHELL_SHA256}  /tmp/chrome-headless-shell.zip" | sha256sum -c - \
    && unzip -q /tmp/chrome-headless-shell.zip -d /opt \
    && mv /opt/chrome-headless-shell-linux64 /opt/chrome-headless-shell \
    && rm /tmp/chrome-headless-shell.zip \
    && ln -s /opt/chrome-headless-shell/chrome-headless-shell /usr/local/bin/chrome-headless-shell \
    && ! ldd /opt/chrome-headless-shell/chrome-headless-shell | grep 'not found'
