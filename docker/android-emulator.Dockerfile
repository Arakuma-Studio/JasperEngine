# Android emulator image for tools/android-emulator-test.sh: the Android build image (SDK,
# platform-tools, aapt2) plus the emulator and an x86_64 system image. Needs KVM at run time
# (docker run --device /dev/kvm); the emulator runs headless on SwiftShader (GLES 3).
ARG BASE=jasperengine/android-build
FROM ${BASE}

ARG DEBIAN_FRONTEND=noninteractive
# the system image: an Automated Test Device (no Play services, no launcher apps: boots fast)
ARG EMULATOR_API=34
ARG EMULATOR_TAG=aosp_atd

# what the emulator binary links even with -no-window
RUN apt-get update && apt-get install -y --no-install-recommends \
        libpulse0 libgl1 libegl1 libnss3 libxcomposite1 libxcursor1 libxdamage1 libxi6 libxrandr2 \
        libxtst6 libxkbfile1 libxkbcommon0 libx11-xcb1 libasound2 libdbus-1-3 libfontconfig1 libtcmalloc-minimal4 \
    && rm -rf /var/lib/apt/lists/*

RUN yes | sdkmanager --licenses >/dev/null \
    && sdkmanager "emulator" "system-images;android-${EMULATOR_API};${EMULATOR_TAG};x86_64" >/dev/null \
    && chmod -R a+rwX $ANDROID_HOME

ENV EMULATOR_API=${EMULATOR_API}
ENV EMULATOR_TAG=${EMULATOR_TAG}
ENV PATH=$PATH:/opt/android-sdk/emulator:/opt/android-sdk/platform-tools
