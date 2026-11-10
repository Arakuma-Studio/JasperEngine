# Android build image: JDK 21 plus the SDK pieces the Gradle project needs.
FROM eclipse-temurin:21-jdk-jammy

ARG DEBIAN_FRONTEND=noninteractive
ARG CMDLINE_TOOLS=9862592

# nasm: dav1d's asm for the x86_64 ABI (the NDK assembles the arm .S files itself)
RUN apt-get update && apt-get install -y --no-install-recommends unzip wget git ccache nasm \
    && rm -rf /var/lib/apt/lists/* \
    && git config --system --add safe.directory '*'

ENV ANDROID_HOME=/opt/android-sdk
ENV ANDROID_SDK_ROOT=/opt/android-sdk
ENV PATH=$PATH:/opt/android-sdk/cmdline-tools/latest/bin

RUN mkdir -p $ANDROID_HOME/cmdline-tools \
    && wget -q https://dl.google.com/android/repository/commandlinetools-linux-${CMDLINE_TOOLS}_latest.zip -O /tmp/tools.zip \
    && unzip -q /tmp/tools.zip -d $ANDROID_HOME/cmdline-tools \
    && mv $ANDROID_HOME/cmdline-tools/cmdline-tools $ANDROID_HOME/cmdline-tools/latest \
    && rm /tmp/tools.zip \
    && yes | sdkmanager --licenses >/dev/null \
    && sdkmanager "platform-tools" "platforms;android-36" "build-tools;36.0.0" >/dev/null \
    # The build runs as the host user; AGP installs any missing NDK/CMake here.
    && chmod -R a+rwX $ANDROID_HOME

# eMudhra intermediate CA compromise (October 2026): ca-certificates 20260816 distrusts
# "emSign ECC Root CA - G3". Temurin keeps its own keystore, which still trusts the root
# before 21.0.13, so it is removed there too. The build fails while either still trusts it.
ARG EMSIGN_G3_ALIAS="cn_emsign_ecc_root_ca__g3,o_emudhra_technologies_limited,ou_emsign_pki,c_in [jdk]"
RUN apt-get update && apt-get install -y --no-install-recommends --only-upgrade ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && ! test -e /etc/ssl/certs/emSign_ECC_Root_CA_-_G3.pem \
    && if keytool -list -cacerts -storepass changeit -alias "$EMSIGN_G3_ALIAS" >/dev/null 2>&1; then \
        keytool -delete -cacerts -storepass changeit -alias "$EMSIGN_G3_ALIAS"; \
    fi \
    && ! keytool -list -cacerts -storepass changeit | grep -qi 'emsign_ecc_root_ca__g3'

# The editor's runtime libraries (X11, GL, D-Bus) and python3 (the player packaging), so the editor built in docker/linux.Dockerfile
# can make a prebuilt Android build here, with this image's SDK and JDK and no NDK or Gradle
# (S30 R13; the emulator check of a prebuilt APK). A layer of its own, after every other one.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libx11-6 libxcursor1 libxi6 libxrandr2 libxext6 libxkbcommon0 libwayland-client0 libwayland-cursor0 \
        libwayland-egl1 libgl1 libegl1 libgl1-mesa-dri libvulkan1 libdbus-1-3 dbus xvfb xauth python3 \
    && rm -rf /var/lib/apt/lists/* && dbus-uuidgen > /etc/machine-id

# qemu-user: runs the noise golden tool's ARM builds without KVM (tools/noise/cross_check.sh, T2 R41),
# so the arm64-v8a and armeabi-v7a rows can be checked on an x86-64 host. A layer of its own, last.
RUN apt-get update && apt-get install -y --no-install-recommends qemu-user-static gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
        gcc-arm-linux-gnueabihf g++-arm-linux-gnueabihf \
    && rm -rf /var/lib/apt/lists/*
