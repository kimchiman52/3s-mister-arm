ARG BASE_IMAGE=ubuntu:20.04
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
ARG QUARTUS_INSTALL_DIR=/opt/intelFPGA_lite/17.0

RUN if grep -q '16.04' /etc/os-release; then \
      sed -i 's|http://archive.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g; s|http://security.ubuntu.com/ubuntu|http://old-releases.ubuntu.com/ubuntu|g' /etc/apt/sources.list; \
    fi && \
    dpkg --add-architecture i386 && \
    apt-get update && \
    apt-get install -y --no-install-recommends \
      ca-certificates \
      file \
      libc6:i386 \
      libbz2-1.0:i386 \
      libfontconfig1 \
      libfreetype6:i386 \
      libglib2.0-0 \
      libglib2.0-0:i386 \
      libice6:i386 \
      libncurses5:i386 \
      libsm6:i386 \
      libstdc++6:i386 \
      libx11-6:i386 \
      libxau6:i386 \
      libxcb1:i386 \
      libxdmcp6:i386 \
      libxext6:i386 \
      libxft2:i386 \
      libxrender1:i386 \
      libxt6:i386 \
      libxtst6:i386 \
      perl \
      python3 \
      python-is-python3 \
      tar \
      wget \
      xz-utils \
      zlib1g:i386 && \
    rm -rf /var/lib/apt/lists/*

COPY install-quartus.sh /tmp/install-quartus.sh
COPY installer/ /tmp/quartus-installer/

RUN chmod +x /tmp/install-quartus.sh && \
    /tmp/install-quartus.sh "${QUARTUS_INSTALL_DIR}" && \
    rm -rf /tmp/quartus-installer /tmp/install-quartus.sh

ENV QUARTUS_INSTALL_DIR=${QUARTUS_INSTALL_DIR}
ENV QUARTUS_ROOTDIR=${QUARTUS_INSTALL_DIR}/quartus
ENV PATH=${QUARTUS_INSTALL_DIR}/quartus/bin:${PATH}

WORKDIR /workspaces/3sx-mister
