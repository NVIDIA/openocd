<!-- SPDX-FileCopyrightText: Copyright (c) 2026 NVidia Corporation. All rights reserved. -->
<!-- SPDX-License-Identifier: BSD-3-Clause -->

# NVidia's fork of OpenOCD
This repository contains NVidia's forks of various versions of OpenOCD
modified to support NVidia's chips and platforms.

# Overview
The versions of OpenOCD available on this repository can be used to attach
and debug various NVidia chips including:
-Grace (branch [openocd-0.12.0](https://github.com/NVIDIA/openocd/blob/openocd-0.12.0))
-Thor (branch [openocd-0.12.0](https://github.com/NVIDIA/openocd/blob/openocd-0.12.0))
-Vera (branch [openocd-0.12.0](https://github.com/NVIDIA/openocd/blob/openocd-0.12.0))
OpenOCD is used in this context to act as a gdb server and convert GDB commands
into debug requests to the various debug agents built into NVidia's chips then
transmit them over JTAG or CSWP to the part.

# Origin of this derivative work
The work in this repository is based on the Open Source project
[OpenOCD](https://openocd.org/).

# Getting Started
See each branch README for instructions on how to build and run that specific
version of OpenOCD.
## Quick
In general the following instructions should work:
```bash
./bootstrap
mkdir build
cd build
../configure --prefix=$DEST
make install -j8
```
## With CSWP support
To enable CSWP support you must first clone the [CSWP repository](https://github.com/NVIDIA/cswp),
build and install the code into $DEST. Then in your clone of the appropriate
branch of this repository run:
```bash
./bootstrap
mkdir build
cd build
../configure --prefix=$DEST --enable-cswp --enable-ftdi CFLAGS='-I$DEST/include'
make install -j8
mkdir -p $DEST/share/openocd/contrib/nvidia
cp $DEST/lib/libcdebug_client.so $DEST/lib/libcstrace_client.so $DEST/share/openocd/contrib/nvidia/
```

# Requirements
Building is supported on x86_64 Linux host only (tested on Ubuntu 18.04+,
CentOS 7+ and Rocky Linux 8+).
Supported OS: Ubuntu 18.04+, CentOS 7+, Rocky Linux 8+ and OpenBMC.

# Reporting an issue
Please report any issue or feature request [here](https://github.com/NVIDIA/openocd/issues/new/choose)

# Community
Please email swteg-debug-help@nvidia.com if you have any question or feedback.

# License
This project is licensed under the GPLv2 License with some exceptions - see the LICENSE.md file for details
