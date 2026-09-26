ARG UBUNTU_IMAGE=ubuntu:22.04
FROM ${UBUNTU_IMAGE}
ENV DEBIAN_FRONTEND=noninteractive
ENV LANG=C.UTF-8
ARG UBUNTU_APT_MIRROR=http://archive.ubuntu.com/ubuntu
RUN sed -i -E "s#http://(archive|security).ubuntu.com/ubuntu#${UBUNTU_APT_MIRROR}#g" /etc/apt/sources.list
RUN apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 update && \
    apt-get -o Acquire::Retries=2 -o Acquire::http::Timeout=20 install -y --no-install-recommends \
    build-essential ninja-build git python3 python3-pip python3-venv ca-certificates \
    libgl1-mesa-dev libegl1-mesa-dev libxkbcommon-dev libxkbcommon-x11-0 \
    libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-render-util0 \
    libxcb-randr0 libxcb-shape0 libxcb-xfixes0 libxcb-sync1 libxcb-xinerama0 \
    libx11-xcb-dev libfontconfig1-dev libdbus-1-dev libssl-dev libwayland-dev \
    libglib2.0-dev patchelf xvfb xauth openbox weston dbus-x11 desktop-file-utils \
    dpkg-dev fakeroot file fonts-noto-cjk && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/tools && /opt/tools/bin/pip install --no-cache-dir \
    aqtinstall==3.3.0 cmake==3.30.5
RUN /opt/tools/bin/aqt install-qt linux desktop 6.8.3 linux_gcc_64 \
    --outputdir /opt/Qt --archives qtbase qtsvg qtwayland icu
ENV PATH=/opt/tools/bin:/opt/Qt/6.8.3/gcc_64/bin:$PATH
ENV CMAKE_PREFIX_PATH=/opt/Qt/6.8.3/gcc_64
RUN useradd --create-home --uid 1000 builder && mkdir /workspace && chown builder:builder /workspace
USER builder
WORKDIR /workspace
