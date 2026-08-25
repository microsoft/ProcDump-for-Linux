# Install ProcDump

## Build from source

Install the prerequisites in [BUILD.md](BUILD.md), then build and install the
CLI from the repository root:

```bash
cargo build --release --bin procdump
sudo install -m 0755 target/release/procdump /usr/local/bin/procdump
```

The static C API is generated at `target/release/libprocdump.a`; its public
header is `crates/procdump-capi/include/ProcDumpLib.h`.

## Azure Linux 2.0
```sh
sudo yum install procdump
```

## Ubuntu 20.04, 22.04, 24.04
#### 1. Register Microsoft key and feed
```sh
wget -q https://packages.microsoft.com/config/ubuntu/$(lsb_release -rs)/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
```

#### 2. Install Procdump
```sh
sudo apt-get update
sudo apt-get install procdump
```

## Debian 11
#### 1. Register Microsoft key and feed
```sh
wget -q https://packages.microsoft.com/config/debian/11/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
```

#### 2. Install Procdump
```sh
sudo apt-get update
sudo apt-get install apt-transport-https
sudo apt-get update
sudo apt-get install procdump
```

## Debian 12
#### 1. Register Microsoft key and feed
```sh
wget -q https://packages.microsoft.com/config/debian/12/packages-microsoft-prod.deb -O packages-microsoft-prod.deb
sudo dpkg -i packages-microsoft-prod.deb
```

#### 2. Install Procdump
```sh
sudo apt-get update
sudo apt-get install apt-transport-https
sudo apt-get update
sudo apt-get install procdump
```

## Fedora 38
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/fedora/38/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo dnf install procdump
```

## Fedora 39
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/fedora/39/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo dnf install procdump
```

## Fedora 40
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/fedora/40/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo dnf install procdump
```

## RHEL 7
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/rhel/7/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo yum install procdump
```

## RHEL 8
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/rhel/8/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo yum install procdump
```

## RHEL 9
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/rhel/9/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo yum install procdump
```

## CentOS 7
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/centos/7/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo yum install procdump
```

## openSUSE 15
#### 1. Register Microsoft key and feed
```sh
sudo zypper install libicu
sudo rpm --import https://packages.microsoft.com/keys/microsoft.asc
wget -q https://packages.microsoft.com/config/opensuse/15/prod.repo
sudo mv prod.repo /etc/zypp/repos.d/microsoft-prod.repo
sudo chown root:root /etc/zypp/repos.d/microsoft-prod.repo
```

#### 2. Install Procdump
```sh
sudo zypper install procdump
```

## SLES 12
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/sles/12/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo zypper install procdump
```

## SLES 15
#### 1. Register Microsoft key and feed
```sh
sudo rpm -Uvh https://packages.microsoft.com/config/sles/15/packages-microsoft-prod.rpm
```

#### 2. Install Procdump
```sh
sudo zypper install procdump
```

## macOS
To install ProcDump for Mac, you'll need to install [Homebrew](https://brew.sh) if you haven't already.

1. Add the Sysinternals tap:
   ```bash
   brew tap Microsoft/sysinternalstap
   ```
   
1. Install ProcDump:
   ```bash
   brew install procdump
   ```
