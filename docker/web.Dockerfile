# Emscripten build image for the engine (web target).
FROM emscripten/emsdk:6.0.10

RUN apt-get update && apt-get install -y --no-install-recommends ninja-build ccache \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*'

# emsdk lives in a root-owned directory; the build runs as the host user and
# needs to write Emscripten's cache.
ENV EM_CACHE=/tmp/emcache
