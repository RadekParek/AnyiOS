# Exact CPU component provenance

Dynarmic 6.7.0 source pin: `a41c380246d3d9f9874f0f792d234dc0cc17c180`.
PodNative applies `patches/oaknut-wx.patch` for the bundled Oaknut allocator and
Windows x64 exception-handler initialization. These are the only upstream source
changes. Root license: 0BSD, retained at LICENSE.txt.

These subtrees are contained in that exact Dynarmic commit (not floating external
repositories). Tree object IDs identify their exact contents. License texts were
read before build and retained under notices/. MIT/0BSD/BSD/BSL terms permit this
integration with the preserved attribution/disclaimers; no endorsement is implied.

| Subtree | Tree object | Terms | Host use |
|---|---|---|---|
| fmt | `70538833f272767f928e7085db6c332bbcc3be63` | MIT | AArch64/x86-64 |
| mcl | `e6d97e810cc9042b3081387f93749b0f63e0d2ef` | MIT | AArch64/x86-64 |
| oaknut | `bfd0ee848b1e792dd6dca46808ab18065589380d` | MIT | AArch64 only |
| robin-map | `bdcc6b511a043934a869059acc192904cbe5d223` | MIT | AArch64/x86-64 |
| xbyak | `aeb20b28b913b4fcb1d3f5b48eef04c4b6b63192` | BSD-3-Clause | x86-64 tests only |
| zycore | `02951198fff575fa2f216111f3d9c99a7960f6f9` | MIT | x86-64 tests only |
| zydis | `8e460786d99b8c9694f8b533675bc0d7326e3b4c` | MIT | x86-64 tests only |

Boost 1.88.0 superproject pin: `199ef13d6034c85232431130142159af3adfce22`.
Only header dependencies of ICL/Variant are used; each module is fixed by that
superproject's gitlinks. Terms: Boost Software License 1.0 (retained).
Upstream boostdep installs that transitive header closure without test dependencies.
Header source comments remain unchanged. Build tools/dependency source checkouts,
objects and binaries stay under ignored `.local/`.

Host validation is not Android validation. The CPU adapter is not linked into the
Android app until AArch64 compilation and on-device instruction/ABI tests pass.
No code from HyperHLE or its Dynarmic wrapper is included.

## Reviewed allocator modification

The pinned Oaknut CodeBlock requests RWX on Linux/Android, treats nullptr rather
than MAP_FAILED as mmap failure, and does not change permissions on those systems
in protect/unprotect even when Dynarmic's no-execute support is enabled.
`oaknut-wx.patch` adds an opt-in OAKNUT_USE_WX path: initial RW allocation,
checked RW/RX transitions, and checked MAP_FAILED with standard system errors.
No fallback requests RWX. It also registers the x64 fast-memory exception callback
before Dynarmic seals its generated-code prelude RX; the Windows handler stores that
selector in the code buffer's unwind metadata, so registering it afterward faults.
The W^X path is enabled for Android and Windows CPU/HLE correctness builds.

Tests verify real Linux /proc/self/maps permissions, readable contents across
transitions and structured mmap failure. The Android artifact includes the same
allocator test separately from the instruction test. Windows CI also exercises the
x64 JIT initialization and CPU/HLE tests. Actual Android app-UID execution, 16 KiB
host pages and SELinux policy behavior still require a device. The modified allocator
remains under the retained Oaknut MIT license. The patch is idempotently applied to
the exact source pin; mismatching code is rejected.
