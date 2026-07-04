# RISC-V host — future work

This file records known limitations and planned improvements in the
RISC-V host support. The initial WIP landed across two commits:

- `9e57a1a client/rtld: Fix RISC-V PLT/stub encodings and add CFI relocations`
- `ffa8b48 server/callconv,client/dispatch: fastcc RV64 host LP64 (A1' design)`
- `aa82b3d riscv host: enable RV64GC codegen and allow SHF_WRITE sections`
- `bb1ead2 riscv host: fix medium-model constant-pool addressing`

Both `-fastcc=1` (LP64 CC) and `-fastcc=0` (cdecl) work end-to-end on
SpacemiT K1 / openEuler 24.03 / LLVM 17: hand-written static asm test
programs, and `gcc -static hello.c` (glibc-linked, full C runtime) both
print "Hello, world!" and exit cleanly.

---

## Path C — patch LLVM to lift the return-value cap

### Motivation

Instrew's LP64 CC on RISC-V currently ships **6 in-reg guest values**
(1 PC + 5 GPRs) — down from the AArch64 host's 8. The cause is a
hard-coded cap in LLVM 17:

```
llvm/lib/Target/RISCV/RISCVISelLowering.cpp:14714-14717
    // Any return value split in to more than two values can't be returned
    // directly. Vectors are returned via the available vector registers.
    if (!LocVT.isVector() && IsRet && ValNo > 1)
      return true;
```

`ValNo > 1` fails allocation → `CanLowerReturn` returns false →
SelectionDAG demotes the return to `sret` with a hidden pointer in `a0`.
That's why our physical layout starts with `a0 = sret slot pointer`
and every real argument shifts up by one, costing us one guest reg.

Fastcc does not help by itself — the cap is applied unconditionally in
`CC_RISCV`, and both `CanLowerReturn` and `LowerReturn` are hard-wired
to `CC_RISCV` regardless of calling convention. Only argument lowering
(not return lowering) branches on `CallingConv::Fast`.

### The fix (approach A, recommended)

Route return-value analysis through `CC_RISCV_FastCC` (which has a
13-GPR pool `X10-X17, X7, X28-X31` and no `ValNo > 1` cap) when the
caller/callee uses `fastcc`. Three sites must be patched together —
missing any one leaves caller and callee disagreeing:

```diff
--- a/llvm/lib/Target/RISCV/RISCVISelLowering.cpp
+++ b/llvm/lib/Target/RISCV/RISCVISelLowering.cpp
@@ LowerCall's return handling (around line 15838-15841) @@
   // Assign locations to each value returned by this call.
   SmallVector<CCValAssign, 16> RVLocs;
   CCState RetCCInfo(CallConv, IsVarArg, MF, RVLocs, *DAG.getContext());
-  analyzeInputArgs(MF, RetCCInfo, Ins, /*IsRet=*/true, RISCV::CC_RISCV);
+  analyzeInputArgs(MF, RetCCInfo, Ins, /*IsRet=*/true,
+                   CallConv == CallingConv::Fast ? RISCV::CC_RISCV_FastCC
+                                                 : RISCV::CC_RISCV);

@@ CanLowerReturn (around line 15881-15891) @@
   for (unsigned i = 0, e = Outs.size(); i != e; ++i) {
     MVT VT = Outs[i].VT;
     ISD::ArgFlagsTy ArgFlags = Outs[i].Flags;
     RISCVABI::ABI ABI = MF.getSubtarget<RISCVSubtarget>().getTargetABI();
-    if (RISCV::CC_RISCV(MF.getDataLayout(), ABI, i, VT, VT, CCValAssign::Full,
-                 ArgFlags, CCInfo, /*IsFixed=*/true, /*IsRet=*/true, nullptr,
-                 *this, FirstMaskArgument))
+    auto Fn = CallConv == CallingConv::Fast ? RISCV::CC_RISCV_FastCC
+                                            : RISCV::CC_RISCV;
+    if (Fn(MF.getDataLayout(), ABI, i, VT, VT, CCValAssign::Full,
+           ArgFlags, CCInfo, /*IsFixed=*/true, /*IsRet=*/true, nullptr,
+           *this, FirstMaskArgument))
       return false;
   }

@@ LowerReturn (around line 15905-15911) @@
   CCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), RVLocs,
                  *DAG.getContext());
-  analyzeOutputArgs(DAG.getMachineFunction(), CCInfo, Outs, /*IsRet=*/true,
-                    nullptr, RISCV::CC_RISCV);
+  analyzeOutputArgs(DAG.getMachineFunction(), CCInfo, Outs, /*IsRet=*/true,
+                    nullptr,
+                    CallConv == CallingConv::Fast ? RISCV::CC_RISCV_FastCC
+                                                 : RISCV::CC_RISCV);
```

Approach B — merely relaxing the cap at line 14716 — is smaller but
does not work on its own, because `CC_RISCV` still allocates only
`a0/a1` for returns after the cap. You would need to duplicate the
GPR pool logic, which is exactly what `CC_RISCV_FastCC` already does.
Use approach A.

### Downstream instrew changes after the LLVM patch

Once LLVM can return 8 GPRs in registers under fastcc, revert the A1'
design shift and restore the natural argidx=0 → a0 layout:

- `server/callconv.cc`: `setCallingConv(CallingConv::Fast)` on the LP64
  `nfn`; ret_ty back to `{sptr, i64 x7}` (8 elements); field maps back
  to `argidx/retidx 0..7` (one extra GPR per guest arch, e.g. add RSI
  back for x86-64).
- `client/dispatch.c`: drop the sret slot / s0 save dance; go back to
  `a0 = sptr, a1 = PC, a2..a7 = 6 packed regs`. Compare with the
  AArch64 aapcsx dispatcher for the natural shape.

### Risks / concerns to test after patching

1. **`CC_RISCV_FastCC` receives `IsRet` but never uses it.** No hidden
   guard fires today; verify no upstream caller relies on the current
   behavior.
2. **Caller/callee symmetry is load-bearing.** All three sites must
   land together. If `LowerCall`'s return read uses the standard CC
   but `LowerReturn` uses fastcc (or vice versa), returns are silently
   mis-decoded — the caller reads wrong registers.
3. **Register pool overlap.** FastCC uses `X7, X28-X31` as arg/return
   regs; those are caller-saved temporaries. Fine for a purely-internal
   CC, but any hand-written asm that expects only `a0/a1` as return regs
   will break. Instrew emits both sides so this is safe.
4. **Vectors and f16/f32/f64.** FastCC has parallel FPR/vector paths.
   Instrew currently only returns i64s via this CC, but confirm before
   extending.
5. **GHC guard in `LowerReturn` (around line 15911).** Unchanged —
   GHC returns void only, never overlaps with Fast.

### Test the patch

Add an LLVM regression test:

```llvm
define fastcc { i64, i64, i64, i64, i64, i64, i64, i64 }
  @foo(i64 %a, i64 %b, i64 %c, i64 %d, i64 %e, i64 %f, i64 %g, i64 %h) {
  ; ... build and return the struct ...
}
; CHECK-NOT: sd {{.*}}, {{.*}}(a0)   ; no sret writes
; CHECK: mv a0, {{.*}}
; CHECK: mv a7, {{.*}}
```

### Estimated effort

- LLVM patch: ~15 lines source + regression test. Rebuild LLVM.
- Instrew reversion of A1' shift: ~30 lines across callconv.cc and
  dispatch.c. Update field maps to add one guest reg per arch.
- Testing: existing test suite + a fresh binary that exercises the
  extra reg (e.g., a program that spills to memory today should stop
  spilling one slot).

### If we don't patch LLVM

The A1' design (currently shipped) works, just with 25% fewer in-reg
guest values than the AArch64 host. That translates to some extra
memory traffic at BB boundaries. See `README.md` and the paper for
the AArch64 baseline; expect RISC-V numbers to sit slightly below
until Path C lands.

---

## Other known TODOs

- **Static glibc hello-world crashes at runtime.** Section-flag check
  and RV64GC codegen both work now; translation completes cleanly for
  a `gcc -static hello.c` binary. Execution then reliably segfaults
  (exit 139), no output produced. Not yet debugged — first suspect is
  a syscall in emulate.c the guest hits before printf's write() lands
  (e.g., `brk`, `arch_prctl`, `set_tid_address`, `set_robust_list`,
  or one of the auxv/TLS setup calls). Instrumentation with `-trace`
  and reading emulate.c's default/unhandled branches is the next
  step. Simple `write("hello\n")+exit_group` asm programs work fine.
- **AARCH64_RV64_LP64** — the aarch64→rv64 CC is defined but untested.
  We do not have an easy way to build a static aarch64 test binary on
  the current board setup.
- **Signal handling on RISC-V** — `client/minilibc.c:sigaction` skips
  `SA_RESTORER` on RISC-V, but the client does not install its own
  `rt_sigreturn` trampoline. Any real signal (not SIG_DFL/SIG_IGN)
  will crash on return. Blocks any program that catches signals.
- **`patch_data_reg` register choice** — currently `t6` (x31). Under
  fastcc, x31 is in the arg pool. If a translated function is ever
  called with 13 args, x31 would carry an arg and our patch stub's
  scratch use would clash. In practice we never emit that many args,
  but a `_Static_assert` or a switch to a non-arg-pool reg (e.g., a
  saved reg with explicit spill) would harden this.
- **RISC-V vector extension** — SpacemiT K1 supports RVV 1.0 but
  Instrew does not currently lift vector guest code to vector host
  code. Left as a large open project.
