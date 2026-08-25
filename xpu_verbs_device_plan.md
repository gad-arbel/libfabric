# XPU device-side plan — `prov/verbs` kernels under Level Zero / SYCL

Scope: everything that runs **on the GPU**. WQE construction, doorbell ring, CQ
polling, and the dispatch plumbing that routes the generic `fi_xpu_*()` calls to
verbs.

The CPU-side counterpart (context creation, resource creation, `export_xpu`) is in
`xpu_verbs_host_plan.md`. The two meet only at `prov_ctx` — see §3.

---

## 1. What already exists in core

`include/rdma/fi_xpu_device.h` (276 lines) is a **complete set of empty dispatch
stubs**. Every function is:

```c
FI_XPU_FUNC int
fi_xpu_send(struct fid_xpu_ep *ep, const void *buf, size_t len, void *desc,
	    uint64_t data, void *dest_addr, void *context,
	    uint64_t flags, int scope)
{
	switch (ep->fid.prov_id) {
	default:
		return -FI_ENOSYS;
	}
}
```

Present as stubs: `fi_xpu_write`, `fi_xpu_read`, `fi_xpu_send`, `fi_xpu_recv`,
`fi_xpu_tsend`, `fi_xpu_trecv`, `fi_xpu_atomic`, `fi_xpu_fetch_atomic`,
`fi_xpu_compare_atomic`, `fi_xpu_cntr_{read,readerr,wait,add,set,adderr,seterr}`,
`fi_xpu_cq_{read,readfrom,readerr,sread,sreadfrom}`.

There is **no** `fi_xpu_device_efa.h` and no provider case in any switch. The
comment block at `fi_xpu_device.h:41-52` is the template for adding one.

### Dispatch is per-provider, not per-vendor

This is the thing that is easy to get wrong: nothing switches on the HMEM
interface. There is no "ZE implementation" of the device API — there is a
**verbs** implementation that happens to be compiled by a Level Zero / SYCL
toolchain. `enum fi_xpu_provider` (`include/rdma/fi_xpu.h:100`) currently holds
only `FI_XPU_PROV_EFA = 1`; values are deliberately a tight range starting at 1 so
the compiler emits a jump table.

### Compiler adaptation

```c
/* include/rdma/fi_xpu_device.h:33-39 */
#if defined(__CUDACC__) || (defined(__HIP_DEVICE_COMPILE__) && __HIP_DEVICE_COMPILE__)
  #define FI_XPU_FUNC __device__ static inline
#elif defined(__SYCL_DEVICE_ONLY__)
  #define FI_XPU_FUNC static inline
#else
  #define FI_XPU_FUNC static inline
#endif
```

The header includes `<rdma/fi_xpu.h>` → `fabric.h` → `stdint.h`, `stddef.h`,
`sys/types.h`, `sys/uio.h`, `fi_errno.h`. It compiles clean as C today
(`gcc -Iinclude -c` on a TU that includes only it).

**Consequence for ZE:** this works under `icpx -fsycl` compiling a `.cpp`, because
both the host and device passes see the host system headers. It does **not** work
for OpenCL-C `.cl` sources or a standalone SPIR-V compilation, where `sys/uio.h`
does not exist. Decide the kernel language in §2 before writing any code.

### Scope

Every op takes a trailing `scope` from the anonymous enum at
`include/rdma/fi_xpu.h:18` — `FI_XPU_WORK_ITEM`, `FI_XPU_SUBGROUP`,
`FI_XPU_WORK_GROUP`, `FI_XPU_DEVICE`. It is a promise from the app that *all*
threads in that scope issue the same call, which lets the implementation elect one
lane to build the WQE and ring the doorbell. Mapping (from `man/fi_xpu.3.md:356`):

| Scope | CUDA | SYCL |
|---|---|---|
| `FI_XPU_WORK_ITEM` | thread | work item |
| `FI_XPU_SUBGROUP` | warp | subgroup |
| `FI_XPU_WORK_GROUP` | thread block | work group |
| `FI_XPU_DEVICE` | device | device |

---

## 2. Decisions to make first

### 2.1 Kernel language

- **(a) SYCL (`icpx -fsycl`)** — recommended. `FI_XPU_FUNC` already resolves,
  `sycl::atomic_ref` / `sycl::atomic_fence` / `sycl::group_barrier` give the
  memory-ordering primitives, and subgroup election is `sycl::ext::oneapi::this_sub_group()`.
- **(b) Native L0 modules / OpenCL-C** — requires a device-only-safe header split,
  because `fi_xpu.h` drags in host system headers. Extra core work
  (**D0** below).

### 2.2 Which ops to implement

Do **not** try to fill all 21 stubs. Minimum useful set, in order:

1. `fi_xpu_write` — RMA write is the simplest WQE and needs no receive side.
2. `fi_xpu_cq_read` — poll for the completion.
3. `fi_xpu_read`, then `fi_xpu_send`/`fi_xpu_recv`.

Out of scope for v1: tagged (`fi_xpu_tsend`/`trecv` — verbs has no tag matching),
atomics, and all seven `fi_xpu_cntr_*` (the host plan drops counters entirely
because verbs has `cntr_open = fi_no_cntr_open`). Leave those falling through to
`default: -FI_ENOSYS`, which is the correct answer for verbs.

### 2.3 Scope support

v1: implement `FI_XPU_WORK_ITEM` correctly and return `-FI_ENOSYS` for the wider
scopes rather than silently treating them as per-thread — a wrong scope
implementation produces duplicate WQEs, which is very hard to debug.

---

## 3. Contract with the host side

`struct fid_xpu` (`include/rdma/fi_xpu.h:121`) is all the kernel gets:

```c
struct fid_xpu {
	uint32_t	fclass;		/* FI_CLASS_EP, _CQ, _CNTR */
	uint32_t	prov_id;	/* enum fi_xpu_provider */
	uint64_t	prov_ctx;	/* provider-internal pointer */
};
```

So `prov_ctx` must be a **device-accessible pointer to a provider-private blob**
that `export_xpu` (host plan H6) fills in. Sketch:

```c
/* include/rdma/fi_xpu_device_verbs.h — shared by host and device */
struct fi_xpu_verbs_ep_ctx {
	volatile void	*sq_base;	/* kernel-reachable SQ ring */
	uint32_t	sq_wqe_cnt;
	uint32_t	sq_stride;
	volatile uint8_t *sq_dbrec;	/* imported page base */
	uint32_t	sq_dbrec_off;	/* byte offset within it, host plan §4.1 */
	volatile uint8_t *uar;		/* BAR MMIO, via import(IOMEMORY) */
	uint32_t	uar_offset;	/* plain doorbell reg; no BF, host plan §3.5 */
	uint32_t	qpn;		/* mlx5dv_qp.sqn */
	uint32_t	sq_pi;		/* producer index, device-updated */
	volatile uint64_t *sq_wqe_ctx;	/* [sq_wqe_cnt] op_context shadow */
};

struct fi_xpu_verbs_cq_ctx {
	volatile void	*cq_base;
	uint32_t	cqe_cnt;
	uint32_t	cqe_size;	/* 64, or 128 with CQE padding */
	volatile uint8_t *cq_dbrec;	/* imported page base */
	uint32_t	cq_dbrec_off;
	uint32_t	cqn;
	uint32_t	ci;		/* consumer index */
};
```

`sq_wqe_ctx` is the one non-obvious member. A raw CQE carries a WQE counter, not a
`wr_id` — that mapping lives in libmlx5's private `wrid` array, which the kernel
cannot see. `fi_xpu_write` stores the caller's `context` at
`sq_wqe_ctx[pi & (sq_wqe_cnt - 1)]` and `fi_xpu_cq_read` reads it back by
`wqe_counter`, otherwise there is no way to populate `fi_cq_entry::op_context`.

The `*_dbrec_off` fields exist because host-VA import primitives are
page-granular while a dbrec is 8 bytes at an arbitrary offset inside a page
libmlx5 shares between queues — the host imports the page and passes the offset
through. See host plan §4 for the full resource inventory.

Rules:
- Layout is defined **once**, in the shared header, and must be identical in the
  host and device compilations — no `#ifdef`-varying members, explicit widths,
  explicit padding.
- Any change to it is a single commit touching both `verbs_xpu.c` and the device
  header.
- **The blob carries device addresses and sizes only, never provenance.** The host
  plan's §3.1 v1 decision (Route B: extract from a normal `ibv_qp` with
  `mlx5dv_init_obj` and `import` the result) and its phase-2 alternative (Route A:
  DEVX objects over `alloc`'d accelerator memory) produce the same struct. Nothing
  in this plan depends on which one the host side uses, so none of the device work
  is invalidated by that swap. Under Route B the rings are host memory reached over
  PCIe, so expect worse per-access latency but identical semantics.

---

## 4. Work items (one commit each, all buildable in isolation)

- **D0** *(only if §2.1 chooses native L0/OpenCL-C)*
  `core: allow fi_xpu_device.h in device-only compilation` — split the minimal
  type set (`struct fid_xpu` and friends, `FI_XPU_*` scope enum) out of
  `fi_xpu.h` so the device header does not pull in `sys/uio.h`.
- **D1** `core: reserve XPU provider id for verbs`
  Add `FI_XPU_PROV_VERBS = 2` to `enum fi_xpu_provider` (`include/rdma/fi_xpu.h:100`)
  and to the enum table in `man/fi_xpu.3.md:316`. **Shared with the host series** —
  host H6 depends on it.
- **D2** `core: add verbs device-side XPU header`
  New `include/rdma/fi_xpu_device_verbs.h` with the `prov_ctx` structs from §3 and
  the ops from §2.2 as `FI_XPU_FUNC` functions. Add to `rdmainclude_HEADERS` in
  `Makefile.am`, plus `libfabric.vcxproj` and `libfabric.vcxproj.filters` — the
  pattern is commit `8c51c0c89`, which did exactly this for `fi_xpu.h` and
  `fi_xpu_device.h`.
- **D3** `core: dispatch XPU device ops to verbs`
  `#include <rdma/fi_xpu_device_verbs.h>` in `fi_xpu_device.h` and add
  `case FI_XPU_PROV_VERBS: return fi_xpu_write_verbs(ep, ...);` to each switch
  that D2 implements. Leave the rest at `default`.
- **D4** `prov/verbs: implement device-side RMA write and CQ read`
  The real WQE build + doorbell + CQE parse. Split further if it gets long
  (write / cq_read / read / send+recv).
- **D5** `fabtests: add fi_xpu device kernel test`
  A SYCL kernel doing one `fi_xpu_write` + `fi_xpu_cq_read` against a host-side
  peer. Must be conditional on an `icpx`/L0 toolchain being present so CI without
  a GPU still builds.

---

## 5. Level Zero / SYCL implementation notes

These are the traps specific to writing NIC-driving code in a GPU kernel:

- **This is the one place vendor-specific code enters libfabric.** The host plan is
  accelerator-agnostic — ZE, CUDA and ROCm differ only in the app's `fi_xpu_ops`
  callbacks. The device header is not: the fence is
  `sycl::atomic_fence(..., memory_scope::system)` under SYCL but
  `__threadfence_system()` under CUDA, and the volatile/atomic access idioms differ
  too. So `fi_xpu_device_verbs.h` will carry
  `#if defined(__SYCL_DEVICE_ONLY__) / defined(__CUDACC__)` branches around a small
  set of ordering wrappers. Confine them to that set — the WQE and CQE logic itself
  must stay vendor-neutral, or the §6 layout assertions stop proving anything.
- **No host memory-barrier macros.** `ofi_wmb()`/`ofi_rmb()` from
  `include/ofi_mb.h` are host-only. Use `sycl::atomic_fence(memory_order,
  memory_scope::system)` — the doorbell write must be ordered *after* the WQE
  stores are visible to the NIC, which is `memory_scope::system`, not `device`.
- **WQE stores must not be cached in a way the NIC cannot see.** Under host plan
  Route B the SQ is host memory reached over PCIe; under Route A it is accelerator
  memory the NIC reads over PCIe. Either way the stores must actually reach memory,
  so use relaxed `atomic_ref` stores or the appropriate USM/`volatile` access, not
  plain writes the compiler may sink.
- **The doorbell is BAR MMIO** imported with `FI_XPU_IMPORT_IOMEMORY`. Writes must
  be single, correctly sized, non-coalesced, non-reordered. This is where a
  vectorizing compiler will silently break correctness.
- **CQE polling** reads memory the NIC writes. The ownership/validity bit must be
  read with device-or-system-scope acquire semantics, and the payload read *after*
  it — a relaxed read of both lets the compiler hoist the payload load.
- **One lane rings the doorbell.** Even for `FI_XPU_WORK_ITEM`, if a whole subgroup
  calls in lockstep every lane produces its own WQE — correct but slow. That is
  exactly what the wider `scope` values exist to optimise; see §2.3 for why v1
  should reject them rather than approximate them.
- **No `printf`, no `assert`, no allocation** in the device path. Error reporting is
  the return value only.
- **Doorbell record *and* UAR write, both, in that order.** Host plan §3.5 rules
  out BlueFlame (a 64-byte WC burst is not something a GPU EU reliably emits as
  one transaction), so the sequence is: store the WQE, system-scope release fence,
  update `sq_dbrec` with the producer index, another fence, then one 8-byte write
  to `uar + uar_offset`. Doing only the dbrec update means the NIC never wakes up,
  with no error reported anywhere. The two stores target different devices — GPU
  memory and the NIC BAR — so nothing orders them implicitly.

---

## 6. Validation

- `gcc -Iinclude -c` on a TU including only `fi_xpu_device.h` still passes after
  D2/D3 (it does today) — catches accidental C++-only or device-only constructs
  leaking into the generic header.
- `icpx -fsycl -c` on the same TU.
- Struct layout assertion: a `static_assert(sizeof(struct fi_xpu_verbs_ep_ctx) == N)`
  compiled in **both** the host and the device TU. Cheapest possible guard against
  the §3 layout drifting.
- D5 end-to-end: kernel writes 64 B to a host-registered buffer, host verifies
  content, kernel sees the CQE.
- Verify unimplemented ops still return `-FI_ENOSYS` rather than dispatching into
  a half-written verbs function.

## 7. Open questions

- Kernel language: SYCL vs native L0 (§2.1) — decides whether **D0** exists.
- Is the WQE format going to be written against `mlx5dv` definitions, and can
  those headers be included from device code, or must the layout be duplicated?
  Duplication needs the §6 `static_assert` guard to be taken seriously.
- Is the doorbell register mappable into the GPU address space via
  `import(FI_XPU_IMPORT_IOMEMORY)` on the target platform at all, and does an
  8-byte store from a kernel reach the NIC intact? Route-independent, and the
  single highest-risk unknown in the whole effort — see host plan §9, which puts
  the spike ahead of all other work. Nothing in D4 matters if the answer is no.
- Completion model: is `fi_xpu_cq_read` enough, or do kernels need the counter
  path? Counters would require adding counter support to verbs first (host plan
  §3.2).
