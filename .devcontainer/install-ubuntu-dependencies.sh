#!/bin/bash
echo "APT::Get::Assume-Yes \"true\";" > /etc/apt/apt.conf.d/90assumeyes
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt -y install software-properties-common
apt-get update
# NOTE: Do NOT run `apt upgrade` here. In the emulated/seccomp-restricted CI
# container it upgrades libc-bin, whose post-install ldconfig segfaults (exit 139),
# aborting the whole install chain. Only install the packages we actually need.
apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    jq \
    git \
    pkg-config \
    iputils-ping \
    libcurl4 \
    libicu67 \
    libunwind8 \
    netcat \
    gdb \
    zlib1g-dev \
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
    gnupg \
    libelf-dev

# Install later version of clang needed for libbpf/bpftool build
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
./llvm.sh 13

# Build and install bpftool
update-alternatives --install /usr/bin/clang clang /usr/bin/clang-13 200
update-alternatives --config clang

rm -rf /usr/sbin/bpftool
cd ~
git clone --recurse-submodules https://github.com/libbpf/bpftool.git
cd bpftool/src
make SKIP_CRYPTO=1 install
ln -s /usr/local/sbin/bpftool /usr/sbin/bpftool

# install debbuild
wget https://github.com/debbuild/debbuild/releases/download/22.02.1/debbuild_22.02.1-0ubuntu20.04_all.deb \
    && dpkg -i debbuild_22.02.1-0ubuntu20.04_all.deb

# Install .NET SDK
cd ~
wget https://dot.net/v1/dotnet-install.sh
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 10.0 --install-dir /usr/share/dotnet

# Install the Rust workspace toolchain.
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
/root/.cargo/bin/rustup component add rustfmt clippy
