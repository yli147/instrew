# Instrew — LLVM-based Dynamic Binary Translation

[![builds.sr.ht status](https://builds.sr.ht/~aengelke/instrew/commits/master.svg)](https://builds.sr.ht/~aengelke/instrew/commits/master?)

Instrew is a performance-targeted transparent dynamic binary translator(/instrumenter) based on LLVM. Currently supported source/guest architectures are x86-64, AArch64, and RISC-V64 (rv64imafdc); supported host architectures are x86-64, AArch64, and RISC-V64. The original code is lifted to LLVM-IR using [Rellume](https://github.com/intel-sandbox/personal.yli147.rellume), where it can be modified and from which new machine code is generated using LLVM's JIT compiler.

### Using Instrew

After cloning and checking out submodules, compile Instrew as follows:

```
mkdir build
meson build -Dbuildtype=release
ninja -C build
# optionally, run tests
ninja -C build test
```

Afterwards, you can run an application with Instrew. Statically linked applications often have a significantly lower translation time. New glibc versions often tend to use recent syscalls that are not yet supported, therefore warnings about missing system calls can sometimes be ignored.

```
./build/server/instrew /bin/ls -l
```

You can also use some options to customize the translation:

- `-profile`: print information about the time used for translation.
- `-callret`: enable call–return optimization. Often gives higher run-time performance at higher translation-time.
- `-targetopt=n`: set LLVM optimization level, 0-3. Default is 3, use 0 for FastISel.
- `-fastcc=0`: use C calling convention instead of architecture-specific optimized calling convention; primarily useful for debugging.
- `-sysroot=path`: specify path to guest architecture's root filesystem for dynamic linking (e.g., x86-64 glibc on RISC-V host). Required for dynamically linked binaries.
- `-perf=n`: enable perf support. 1=generate memory map, 2=generate JITDUMP
- `-dumpir={lift,cc,opt,codegen}`: print IR after the specified stage. Generates lots of output.
- `-dumpobj`: dump compiled code into object files in the current working directory.
- `-help`/`-help-hidden` shows more options.

Example:

```
./build/server/instrew -profile -targetopt=0 /bin/ls -l
```

### Deploying on a RISC-V Host

Instrew supports RISC-V64 as a host architecture (tested on SpacemiT X60 / openEuler 24.03).
This section walks through building Instrew natively on the board and running the
pre-built x86-64 test binaries in `test/standalone/`.

#### Prerequisites

Install the following on the RISC-V board:

```bash
# openEuler / RHEL-based
dnf install git gcc g++ meson ninja-build llvm-devel clang lld python3

# Debian/Ubuntu-based (e.g. Ubuntu RISC-V port)
apt-get install git gcc g++ meson ninja-build llvm-17-dev clang lld python3
```

LLVM 16–18 is required.  On openEuler 24.03 the default `llvm-devel` package is
LLVM 17 which works out of the box.

#### Build Instrew on the board

```bash
git clone --recurse-submodules https://github.com/intel-sandbox/personal.yli147.instrew.git
cd instrew
mkdir build
meson build -Dbuildtype=release
ninja -C build
```

The resulting binary is `build/server/instrew`.

#### Run the standalone test programs

Pre-built x86-64 test binaries are checked in to `test/standalone/`:

| File | Type | Description |
|------|------|-------------|
| `hello_x86` | static | Hello World sanity check |
| `stream_x86` | static | STREAM memory-bandwidth benchmark |
| `stream_x86_dyn` | dynamic | STREAM built with glibc (dynamic linking) |

```bash
# Hello World — static, sanity check
./build/server/instrew test/standalone/hello_x86
# Expected output: Hello, world!

# STREAM — static, compare translated vs native
./build/server/instrew test/standalone/stream_x86

# STREAM — dynamic, exercises the dynamic linker path
./build/server/instrew test/standalone/stream_x86_dyn
```

To build the native RISC-V reference binary on the board:

```bash
gcc -static -O3 test/standalone/stream.c -o test/standalone/stream_riscv -lm
test/standalone/stream_riscv
```

#### Running dynamically linked x86 binaries

Dynamically linked x86 binaries require an x86-64 glibc (ld.so + libc.so.6)
accessible on the RISC-V host. Use the `-sysroot` flag to point Instrew to the
x86-64 root filesystem:

```bash
SYSROOT=/opt/intel/ibt/images/openEuler-24.03/openEuler-24.03-x86_64

# Example: run a dynamically linked x86 binary
./build/server/instrew -sysroot $SYSROOT /path/to/x86_binary

# Example: run the pre-built dynamic STREAM benchmark
./build/server/instrew -sysroot $SYSROOT test/standalone/stream_x86_dyn
```

**Requirements on the RISC-V board:**

- An x86-64 rootfs or sysroot containing `ld-linux-x86-64.so.2` and
  `libc.so.6`. No patching of the guest binary is required.
- On the Intel evaluation board (openEuler 24.03), this rootfs is pre-installed
  at `/opt/intel/ibt/images/openEuler-24.03/openEuler-24.03-x86_64/`.
- On other boards, copy the x86-64 glibc files manually:
  ```bash
  # On the x86 build machine:
  scp /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 \
      /lib/x86_64-linux-gnu/libc.so.6 \
      /lib/x86_64-linux-gnu/libm.so.6 \
      /lib/x86_64-linux-gnu/libpthread.so.0 \
      root@<board-ip>:/opt/x86_64-sysroot/lib/x86_64-linux-gnu/
  ```
  Then use `-sysroot /opt/x86_64-sysroot` when running Instrew.

**Multi-threading support:** Instrew now supports multi-threaded binaries
(OpenMP, pthreads) via `clone(CLONE_VM)` implementation. Both statically and
dynamically linked multi-threaded x86 binaries can run on RISC-V hosts.

#### Expected results (SpacemiT X60, LLVM 17, single-threaded)

| Function | Native RISC-V MB/s | Instrew static MB/s | Instrew dynamic MB/s |
|----------|--------------------|---------------------|----------------------|
| Copy     | ~5200              | ~5300               | ~5700                |
| Scale    | ~3500              | ~4600               | ~4600                |
| Add      | ~5400              | ~5600               | ~5500                |
| Triad    | ~4500              | ~4900               | ~4300                |

Translation overhead is minimal for memory-bound workloads because the RISC-V
JIT backend emits RVV vector instructions when lowering lifted x86 SSE2 IR.

Both `-fastcc=1` (LP64 calling convention, default) and `-fastcc=0` (C calling
convention) are fully functional.  For known limitations and future work see
`docs/riscv-host-todo.md`.

### Architecture

Instrew implements a two-process client/server architecture: the light-weight client contains the guest address space as well as the code cache and controls execution, querying rewritten objects as necessary from the server. The server performs lifting (requesting instruction bytes from the client when required), instrumentation, and code generation and sends back an ELF object file. When receiving a new object file, the client resolves missing symbols and applies relocations.

### Publications

- Alexis Engelke. Optimizing Performance Using Dynamic Code Generation. Dissertation. Technical University of Munich, Munich, 2021. ([Thesis](https://mediatum.ub.tum.de/doc/1614897/1614897.pdf))
- Alexis Engelke, Dominik Okwieka, and Martin Schulz. Efficient LLVM-Based Dynamic Binary Translation. In 17th ACM SIGPLAN/SIGOPS International Conference on Virtual Execution Environments (VEE ’21), April 16, 2021. [Paper](https://home.in.tum.de/~engelke/pubs/2104-vee.pdf)
- Alexis Engelke and Martin Schulz. Instrew: Leveraging LLVM for High Performance Dynamic Binary Instrumentation. In 16th ACM SIGPLAN/SIGOPS International Conference on Virtual Execution Environments (VEE ’20), March 17, 2020, Lausanne, Switzerland. [Paper](https://home.in.tum.de/~engelke/pubs/2003-vee.pdf) -- Please cite this paper when referring to Instrew in general.

### License
Instrew is licensed under LGPLv2.1+.
