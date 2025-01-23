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
    iputils-ping \
    libcurl4 \
    libicu66 \
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
    cmake \
    libelf-dev \
    clang \
    clang-12 \
    llvm \
    build-essential \
    libbpf-dev

# Set preference to clang-12
update-alternatives --install /usr/bin/clang clang /usr/bin/clang-12 100

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
    # install .NET 6 for signing process and integration tests
    wget https://packages.microsoft.com/config/ubuntu/20.04/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
    dpkg -i packages-microsoft-prod.deb
    rm packages-microsoft-prod.deb
    apt -y update && apt-get install -y dotnet-runtime-8.0
    apt-get install -y dotnet-sdk-8.0
fi
