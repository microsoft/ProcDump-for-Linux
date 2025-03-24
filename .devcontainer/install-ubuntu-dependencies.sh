#!/bin/bash
echo "APT::Get::Assume-Yes \"true\";" > /etc/apt/apt.conf.d/90assumeyes
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt -y install software-properties-common
apt-get update
apt upgrade -y \
&& apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    jq \
    git \
    cmake \
    iputils-ping \
    libcurl4 \
    libicu70 \
    libunwind8 \
    netcat \
    gdb \
    zlib1g-dev \
    stress-ng \
    wget \
    dpkg-dev \
    fakeroot \
    lsb-release \
    gettext \
    liblocale-gettext-perl \
    pax \
    libelf-dev \
    clang \
    llvm \
    build-essential \
    libbpf-dev \
    sudo

# Build and install bpftool
rm -rf /usr/sbin/bpftool
git clone --recurse-submodules https://github.com/libbpf/bpftool.git
cd bpftool/src
make install
ln -s /usr/local/sbin/bpftool /usr/sbin/bpftool

# install debbuild
wget https://github.com/debbuild/debbuild/releases/download/22.02.1/debbuild_22.02.1-0ubuntu20.04_all.deb \
    && dpkg -i debbuild_22.02.1-0ubuntu20.04_all.deb

arch=$(uname -m)
if [[ "$arch" == "aarch64" ]]; then
    wget https://dot.net/v1/dotnet-install.sh 
    chmod +x dotnet-install.sh
    ./dotnet-install.sh --channel 8.0 --install-dir /usr/share/dotnet
else
    # Not ARM64, we can install dotnet the normal way.
    # install .NET 8 for signing process and integration tests
    apt install -y dotnet-runtime-8.0
    apt install -y dotnet-sdk-8.0
fi
