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
    libelf-dev \
    libssl-dev

# Install later version of clang needed for libbpf build
wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
./llvm.sh 12

# Build openssl3
wget https://www.openssl.org/source/openssl-3.1.2.tar.gz
tar xzf openssl-3.1.2.tar.gz
cd openssl-3.1.2
./config --prefix=/usr/local/openssl-3
make
make install

arch=$(uname -m)

# Build and install bpftool
update-alternatives --install /usr/bin/clang clang /usr/bin/clang-12 200
update-alternatives --config clang

export CFLAGS="$CFLAGS -I/usr/local/openssl-3/include"

if [[ "$arch" == "aarch64" ]]; then
    export LDFLAGS="-L/usr/local/openssl-3/lib -lssl -lcrypto $LDFLAGS"
    export PKG_CONFIG_PATH=/usr/local/openssl-3/lib/pkgconfig:$PKG_CONFIG_PATH    
    export LD_LIBRARY_PATH=/usr/local/openssl-3/lib:$LD_LIBRARY_PATH    
else
    export LDFLAGS="-L/usr/local/openssl-3/lib64 -lssl -lcrypto $LDFLAGS"
    export PKG_CONFIG_PATH=/usr/local/openssl-3/lib64/pkgconfig:$PKG_CONFIG_PATH    
    export LD_LIBRARY_PATH=/usr/local/openssl-3/lib64:$LD_LIBRARY_PATH
fi

rm -rf /usr/sbin/bpftool
cd ~
git clone --recurse-submodules https://github.com/libbpf/bpftool.git
cd bpftool/src
make install
ln -s /usr/local/sbin/bpftool /usr/sbin/bpftool

# install debbuild
wget https://github.com/debbuild/debbuild/releases/download/22.02.1/debbuild_22.02.1-0ubuntu20.04_all.deb \
    && dpkg -i debbuild_22.02.1-0ubuntu20.04_all.deb

# Install .NET SDK
cd ~
wget https://dot.net/v1/dotnet-install.sh 
chmod +x dotnet-install.sh
./dotnet-install.sh --channel 10.0 --install-dir /usr/share/dotnet
