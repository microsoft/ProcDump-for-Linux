# ProcDump [![Build Status](https://oss-sysinternals.visualstudio.com/Procdump%20for%20Linux/_apis/build/status/Sysinternals.ProcDump-for-Linux?branchName=master)](https://oss-sysinternals.visualstudio.com/Procdump%20for%20Linux/_build/latest?definitionId=21&branchName=master)
ProcDump is a Linux reimagining of the classic ProcDump tool from the Sysinternals suite of tools for Windows.  ProcDump provides a convenient way for Linux developers to create core dumps of their application based on performance triggers.

![ProcDump in use](procdump.gif "Procdump in use")

# Installation & Usage 

## Requirements
* Minimum OS:
  * Red Hat Enterprise Linux / CentOS 7
  * Fedora 29
  * Ubuntu 16.04 LTS
* `gdb` >= 7.6.1
* `zlib` (build-time only)

## Install ProcDump
Checkout our [install instructions](INSTALL.md) for distribution specific steps to install Procdump.

## Build ProcDump from Scratch
To build from scratch you'll need to have a C compiler (supporting C11), `zlib`, and a `make` utility installed. Then simply run: 

```
make
make install
```

### Building Procdump Packages 
The distribution packages for Procdump for Linux are constructed utilizing `debbuild` for Debian targets and `rpmbuild` for Fedora targets.

To build a `deb` package of Procdump on Ubuntu simply run:
```sh
make && make deb
```

To build a `rpm` package of Procdump on Fedora simply run:
```sh
make && make rpm
```

## Usage
```
Usage: procdump [OPTIONS...] TARGET
   OPTIONS
      -h          Prints this help screen
      -C          Trigger core dump generation when CPU exceeds or equals specified value (0 to 100 * nCPU)
      -c          Trigger core dump generation when CPU is less than specified value (0 to 100 * nCPU)
      -M          Trigger core dump generation when memory commit exceeds or equals specified value (MB)
      -m          Trigger core dump generation when when memory commit is less than specified value (MB)
      -T          Trigger when thread count exceeds or equals specified value.
      -F          Trigger when file descriptor count exceeds or equals specified value.
      -G          Trigger when signal with the specified value (numeric) is sent (uses PTRACE and will affect performance of target process).      
      -I          Polling frequency in milliseconds (default is 1000)
      -n          Number of core dumps to write before exiting (default is 1)
      -s          Consecutive seconds before dump is written (default is 10)
      -o          Path and/or filename prefix where the core dump is written to
      -d          Writes diagnostic logs to syslog
   TARGET must be exactly one of these:
      -p          pid of the process
      -w          Name of the process executable
```
### Examples
> The following examples all target a process with pid == 1234

The following will create a core dump immediately.
```
sudo procdump -p 1234
```
The following will create 3 core dumps 10 seconds apart.
```
sudo procdump -n 3 -p 1234
```
The following will create 3 core dumps 5 seconds apart.
```
sudo procdump -n 3 -s 5 -p 1234
```
The following will create a core dump each time the process has CPU usage >= 65%, up to 3 times, with at least 10 seconds between each dump.
```
sudo procdump -C 65 -n 3 -p 1234
```
The following will create a core dump each time the process has CPU usage >= 65%, up to 3 times, with at least 5 seconds between each dump.
```
sudo procdump -C 65 -n 3 -s 5 -p 1234
```
The following will create a core dump when CPU usage is outside the range [10,65].
```
sudo procdump -c 10 -C 65 -p 1234
```
The following will create a core dump when CPU usage is >= 65% or memory usage is >= 100 MB.
```
sudo procdump -C 65 -M 100 -p 1234
```
The following will create a core dump in the `/tmp` directory immediately.
```
sudo procdump -o /tmp -p 1234
```
The following will create a core dump in the current directory with the name dump_0.1234. If -n is used, the files will be named dump_0.1234, dump_1.1234 and so on.
```
sudo procdump -o dump -p 1234
```
The following will create a core dump when a SIGSEGV occurs.
```
sudo procdump -G 11 -p 1234
```
> All options can also be used with -w instead of -p. -w will wait for a process with the given name.

The following waits for a process named `my_application` and creates a core dump immediately when it is found.
```
sudo procdump -w my_application
```

## Current Limitations
* Currently will only run on Linux Kernels version 3.5+
* Does not have full feature parity with Windows version of ProcDump, specifically, stay alive functionality, and custom performance counters

# Feedback
* Ask a question on StackOverflow (tag with ProcDumpForLinux)
* Request a new feature on GitHub
* Vote for popular feature requests
* File a bug in GitHub Issues

# Contributing
If you are interested in fixing issues and contributing directly to the code base, please see the [document How to Contribute](CONTRIBUTING.md), which covers the following:
* How to build and run from source
* The development workflow, including debugging and running tests
* Coding Guidelines
* Submitting pull requests

Please see also our [Code of Conduct](CODE_OF_CONDUCT.md).


# License
Copyright (c) Microsoft Corporation. All rights reserved.

Licensed under the MIT License.

