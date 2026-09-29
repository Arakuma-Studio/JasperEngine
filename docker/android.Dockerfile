# Android build image: JDK 21 plus the SDK pieces the Gradle project needs.
FROM eclipse-temurin:21-jdk-jammy

ARG DEBIAN_FRONTEND=noninteractive
ARG CMDLINE_TOOLS=9862592

RUN apt-get update && apt-get install -y --no-install-recommends unzip wget git ccache \
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
