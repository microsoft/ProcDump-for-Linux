#!/bin/bash

echo "assumeyes=1" >> /etc/yum.conf
yum install http://opensource.wandisco.com/rhel/8/git/x86_64/wandisco-git-release-8-1.noarch.rpm
dnf install dnf-plugins-core && dnf install epel-release && dnf config-manager --set-enabled powertools && dnf update
yum install git \
    gdb \
    python3 \
    zlib-devel \
    gcc-c++ \
    pkg-config \
    gcc-toolset-10 \
    rpm-build \
    make \
    curl \
    libcurl-devel \
    libicu-devel \
    libunwind-devel \
    nmap \
    wget \
    clang \
    redhat-lsb \
    elfutils-libelf-devel \
    libbpf-devel \
    bpftool

pip3 uninstall -y setuptools
pip3 uninstall -y pip
curl https://stedolan.github.io/jq/download/linux64/jq > /usr/bin/jq && chmod +x /usr/bin/jq
yum install dotnet-runtime-6.0
yum install dotnet-sdk-6.0

curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
/root/.cargo/bin/rustup component add rustfmt clippy