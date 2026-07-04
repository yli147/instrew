# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Instrew is a performance-targeted transparent dynamic binary translator (DBT) based on LLVM. It lifts guest machine code (x86-64, AArch64, or RISC-V64) to LLVM-IR using Rellume, optimizes it, and generates native host code (x86-64, AArch64, or RISC-V64) via LLVM's JIT compiler.

## Build System

This project uses Meson and Ninja. LLVM 16-18 is required.

### Initial Setup

```bash
# Clone and initialize submodules (fadec, farmdec, frvdec, rellume)
git submodule update --init --recursive

# Configure build
mkdir build
meson build -Dbuildtype=release

# Build
ninja -C build
```

### Running Tests

```bash
# Run all tests
ninja -C build test

# Run a specific test (example)
ninja -C build test/x86_64/your_test_name
```

Tests require clang and lld for cross-compilation. Test binaries are compiled for the host's page size to match runtime behavior. Tests are organized by guest architecture in `test/{x86_64,aarch64,riscv64}/`.

### Development Builds

For debugging with symbols and assertions:

```bash
meson build -Dbuildtype=debug
ninja -C build
```

For optimized builds with debug info:

```bash
meson build -Dbuildtype=debugoptimized
ninja -C build
```

## Architecture

Instrew uses a **two-process client/server architecture**:

### Client (`client/`)

- Lightweight process containing guest address space and code cache
- Controls execution, dispatching to translated code
- Requests translation from server when code cache misses
- Receives ELF object files, resolves symbols, applies relocations
- Minimal libc implementation (`minilibc.c`) for independence
- Built as static-PIE executable with custom link flags
- Key components:
  - `dispatch.c`: Code cache lookup and execution dispatch with quick TLB
  - `translator.c`: Protocol implementation for server communication
  - `rtld.c`: Runtime dynamic linker for received object files
  - `emulate.c`: System call emulation
  - `elf-loader.c`: Initial guest binary loading

### Server (`server/`)

- LLVM-based translation server
- Receives translation requests with guest address
- Lifts guest code to LLVM-IR via Rellume
- Optimizes and generates host machine code
- Sends back ELF object files
- Client binary is embedded into server executable (`client.inc`)
- Key components:
  - `rewriteserver.cc`: Main server loop and translation orchestration
  - `callconv.cc`: Architecture-specific calling convention optimization
  - `optimizer.cc`: LLVM IR optimization passes
  - `codegenerator.cc`: LLVM JIT code generation
  - `connection.cc`: Client/server protocol implementation
  - `cache.cc`: Translation cache management

### Protocol (`shared/instrew-protocol.inc`)

Client and server communicate via a socket-based protocol with message types:
- `C_INIT`: Client initialization with configuration
- `C_TRANSLATE`: Request translation for guest address
- `S_MEMREQ`: Server requests guest memory bytes
- `C_MEMBUF`: Client provides memory contents
- `S_OBJECT`: Server sends compiled ELF object
- `C_SIGACTION`: Signal handler registration (multi-threading support)
- `C_FORK_PREPARE`/`S_FORKED`: Thread creation protocol

The protocol is defined via macros in `instrew-protocol.inc` for client/server consistency. When adding new message types, update both the protocol definition and the handlers in `client/translator.c` and `server/connection.cc`.

## Running Instrew

Basic usage:

```bash
./build/server/instrew /path/to/guest-binary [args...]
```

### Common Options

- `-profile`: Print translation time profiling
- `-callret`: Enable call-return optimization (higher runtime performance, slower translation)
- `-targetopt=N`: LLVM optimization level 0-3 (default: 3, use 0 for FastISel)
- `-fastcc=0`: Use C calling convention instead of optimized fastcc (for debugging)
- `-sysroot=path`: Specify path to guest architecture's root filesystem for dynamic linking (required for dynamically linked binaries, e.g., x86-64 glibc on RISC-V host)
- `-perf=1|2`: Enable perf support (1=memory map, 2=JITDUMP)
- `-dumpir={lift,cc,opt,codegen}`: Print LLVM-IR after specified stage
- `-dumpobj`: Dump compiled objects to current directory
- `-trace`: Trace execution with register dumps
- `-verify-lifted`: Verify lifted LLVM-IR validity

Examples:

```bash
# Static binary
./build/server/instrew -profile -callret /bin/ls -l

# Dynamic binary with sysroot
./build/server/instrew -sysroot /path/to/x86_64-rootfs /path/to/dynamic-binary
```

## Code Organization

### Guest Architecture Support

Instrew lifts code from multiple guest architectures via Rellume (subproject). Guest ISA decoding is handled by architecture-specific decoders in `subprojects/`:
- `fadec`: x86-64 decoder
- `farmdec`: AArch64 decoder
- `frvdec`: RISC-V64 decoder

### Host Architecture Support

Host code generation is architecture-aware:
- x86-64: Custom calling convention optimization in `callconv.cc`, 8-byte stack alignment
- AArch64: Custom calling convention, optimized memset (`client/memset-aarch64.S`)
- RISC-V (WIP): Recent work adding RISC-V host support (see commit 1273a6d)

### Test Structure

Tests are organized by guest architecture in `test/{x86_64,aarch64,riscv64}/`. Each test is an assembly file compiled for the target guest architecture and executed through Instrew. Test cases are defined in `meson.build` per architecture with expected pass/fail status and Instrew arguments.

## Key Concepts

### Lifting

Guest machine code is lifted to LLVM-IR by Rellume. The server requests guest memory bytes from the client as needed during lifting, lifting one basic block or function at a time depending on configuration.

### Code Cache

The client maintains a code cache of translated basic blocks. On cache miss, execution traps to `resolve_func()` in `dispatch.c`, which requests translation from the server via the translator protocol.

### Dynamic Linking and Sysroot

Instrew supports running dynamically linked guest binaries by specifying a sysroot path containing the guest architecture's shared libraries:

```bash
./build/server/instrew -sysroot /path/to/x86_64-rootfs /path/to/dynamic-binary
```

The `-sysroot` flag tells the client's dynamic linker (`client/rtld.c`) where to find the guest's `ld.so` and shared libraries. The guest binary is loaded unmodified - no patching of PT_INTERP or RPATH is required. The client implements a minimal runtime dynamic linker that:
- Loads the guest's ld.so from the sysroot
- Sets up the guest's auxiliary vector
- Transfers control to the guest dynamic linker
- The guest ld.so then loads remaining dependencies from the sysroot

This allows cross-architecture execution of complete Linux distributions (e.g., x86-64 binaries with glibc on RISC-V hosts).

### Calling Convention Optimization

When `-fastcc` is enabled (default), `callconv.cc` transforms lifted IR to use architecture-specific register-based calling conventions instead of passing CPU state via memory. This significantly improves performance but complicates debugging.

### Call-Return Optimization

With `-callret`, Instrew attempts to preserve the semantics of call/return instructions at the machine code level rather than treating them as generic control flow. This improves performance for code with many function calls but increases translation complexity.

## Development Notes

- The client is built with `-nostdlib -fno-builtin` and implements its own syscall wrappers
- Client code must be position-independent (static-PIE) as it's embedded in the server
- Server code is C++17, client code is C11
- LLVM version must be 16-18 (checked in `meson.build`)
- Both processes must agree on host page size for memory mapping
- Server embeds full client binary as byte array via Python script in build
- When modifying calling conventions or register allocation, changes must be synchronized between `server/callconv.cc` and `client/dispatch.c`

## RISC-V Host Support

RISC-V64 is now a fully supported host architecture. Both `-fastcc=1` (LP64 calling convention, default) and `-fastcc=0` (C calling convention) work end-to-end on SpacemiT X60 / openEuler 24.03 with LLVM 17.

Multi-threaded binaries (OpenMP, pthreads) are supported via `clone(CLONE_VM)` implementation for both static and dynamic linking.

For known limitations and future optimization opportunities (e.g., LLVM return value cap causing one fewer in-register guest value than AArch64), see `docs/riscv-host-todo.md`.

## Debugging and Verification

When debugging translation issues:
1. Use `-fastcc=0` to disable calling convention optimization (simpler IR)
2. Use `-targetopt=0` to use FastISel (faster compilation, simpler codegen)
3. Use `-dumpir=lift` or `-dumpir=opt` to inspect LLVM IR at different stages
4. Use `-trace` to see execution flow with register dumps (very verbose)
5. Use `-verify-lifted` to validate lifted IR correctness
6. Use `-dumpobj` to save generated object files for inspection with objdump/readelf

When modifying the client/server protocol, test both static and dynamic binaries. When changing calling conventions, verify across all supported host architectures.
