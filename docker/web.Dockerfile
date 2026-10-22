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
