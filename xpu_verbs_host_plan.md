# XPU host-side plan — `prov/verbs`

Scope: everything that runs on the **CPU**. Discovery, XPU context creation,
resource creation/binding, exporting handles, and marshalling raw AV/MR data.

The kernel-side counterpart (WQE build, doorbell ring, CQ poll) is in
`xpu_verbs_device_plan.md`. The two meet at exactly one place: the `prov_ctx`
field of `struct fid_xpu` — see [Contract](#7-contract-with-the-device-side).

**This plan is accelerator-agnostic.** Nothing in it is Level Zero specific.
There are two independent axes:

- **NIC axis** (mlx5 vs EFA vs …) — how the QP and CQ are created, where the
  doorbell and rings live, the WQE format, the `prov_ctx` contents. This is what
  the plan is about, and all of it is provider code.
- **Accelerator axis** (ZE vs CUDA vs ROCm) — how device memory is obtained and
  how a host VA is made reachable from a kernel. This lives in the
  **application's** `fi_xpu_ops` callbacks, not in libfabric.

The proof is `xpu_usage_example.c` in the tree root: it implements the callbacks
with **CUDA** (`cuMemAlloc`, `cuMemHostRegister`), and the provider side would be
byte-identical against a Level Zero app. The one place vendor-specific code does
enter libfabric is the *device* header's memory fences — see device plan §5.

---

## 1. What already exists in core

The XPU API landed as commits `c8dd8211f`..`0602a82a6` (13 commits, author Shi Jin).
It is **API only** — no provider implements any of it, and there is no device-side
implementation either. `FI_XPU_PROV_EFA = 1` in `enum fi_xpu_provider` is a
reserved slot, not shipped code. Nothing under `include/` or `prov/` ever calls
`fi_xpu_ops::alloc`, `::import`, or `::free`; only `xpu_usage_example.c`
implements them.

| Piece | Location |
|---|---|
| `FI_XPU` capability (caps bit 44, recycled from `FI_XPU_TRIGGER`) | `include/rdma/fabric.h:169` |
| `struct fi_xpu_ops` — app-supplied `alloc`/`import`/`free` | `include/rdma/fi_xpu.h:40` |
| `struct fi_xpu_attr` — `{ iface, device, ops }`, input to ctx create | `include/rdma/fi_xpu.h:54` |
| `struct fid_xpu_ctx` + `fi_ops_xpu_ctx::query` | `include/rdma/fi_xpu.h:86` |
| `fi_ops_domain::xpu_ctx` | `include/rdma/fi_domain.h:329` |
| `fi_domain_attr::max_xpu_ctx_cnt` (0 = unsupported) | `include/rdma/fabric.h:471` |
| `fi_ep_attr::xpu_ctx` | `include/rdma/fabric.h:433` |
| `fi_cq_attr::xpu_ctx` / `fi_cntr_attr::xpu_ctx` | `include/rdma/fi_eq.h:262,304` |
| `fi_ops_ep::export_xpu` | `include/rdma/fi_endpoint.h:110` |
| `fi_ops_cq::export_xpu` / `fi_ops_cntr::export_xpu` | `include/rdma/fi_eq.h:279,316` |
| `fi_ops_av::lookup2` (AV → raw device address) | `include/rdma/fi_domain.h:112` |
| `fi_mr_get_xpu_desc()` via `fi_control(FI_GET_MR_XPU_DESC)` | `include/rdma/fi_domain.h:509`, `fabric.h:676,714` |
| `FI_MR_XPU_DESC` mr_mode bit | `include/rdma/fabric.h:253` |
| `FI_CLASS_XPU_CTX` | `include/rdma/fabric.h:575` |
| default `ofi_av_lookup2()` (forwards when `flags==0`, else `-FI_ENOSYS`) | `prov/util/src/util_av.c:941` |
| man page | `man/fi_xpu.3.md` |

### The model in one line

Control path on the host, data path on the device. The host does `fi_getinfo` →
`fi_domain` → `fi_xpu_ctx` → `fi_cq_open`/`fi_endpoint2` → `fi_ep_bind`/`fi_enable`
→ `*_export_xpu`, then copies 16-byte opaque handles plus raw AV addresses and raw
MR descriptors into device memory and launches the kernel.

### How the callbacks are reached

```
app         app_ops  →  fi_xpu_attr.ops
              ↓  fi_xpu_ctx(domain, &attr, &ctx, NULL)
core        fi_xpu.h:146  →  domain->ops->xpu_ctx
              ↓
provider    vrb_xpu_ctx()                    [H2, new prov/verbs/src/verbs_xpu.c]
              copies *attr->ops into vrb_xpu_ctx.ops    ← stashed for later

app         fi_cq_open(dom, attr{flags|=FI_XPU, .xpu_ctx=ctx}, &cq, NULL)
              ↓
provider    vrb_cq_open()                    [H4, verbs_cq.c:493]
              ibv_create_cq() as today
              mlx5dv_init_obj(MLX5DV_OBJ_CQ) → buf, dbrec, cqe_cnt, cqe_size, cqn
              ctx->ops.import(...)  ×3       → device-reachable addresses
```

- `alloc(device, size, align, flags, &addr, &fd, &offset)` — provider asks the app
  for accelerator memory. `FI_XPU_ALLOC_DMABUF` ⇒ the app must also return a
  dma-buf fd so the NIC can DMA to it.
- `import(device, host_addr, size, flags, &dev_addr)` — provider hands over a host
  VA the kernel must reach. `FI_XPU_IMPORT_IOMEMORY` = PCIe BAR MMIO (the doorbell),
  `FI_XPU_IMPORT_DEVICEMAP` = must be dereferenceable from kernel code.
- `free`.

---

## 2. Blockers in core — must land first

These are bugs/omissions in the merged series. Without them nothing works.

1. **`FI_XPU` is silently stripped from `caps`.**
   `ofi_get_caps()` (`prov/util/src/util_attr.c:1239`) masks user hints with
   `OFI_PRIMARY_CAPS | OFI_SECONDARY_CAPS`, and `FI_XPU` is in neither set
   (`include/ofi.h`, macros `OFI_PRIMARY_TX_CAPS`/`OFI_PRIMARY_RX_CAPS`/
   `OFI_DOMAIN_PRIMARY_CAPS`). verbs calls `ofi_alter_info()`
   (`prov/verbs/src/verbs_info.c:2422`, `verbs_eq.c:212`), so an app hinting
   `FI_XPU` gets back `fi_info` **without** it.
   → Add `FI_XPU` to `OFI_DOMAIN_PRIMARY_CAPS`.

2. **`FI_XPU` in `cq_attr->flags` is rejected by the util layer.**
   `ofi_cq_init()` returns `-FI_EINVAL` for any flag outside
   `FI_AFFINITY | FI_PEER` (`prov/util/src/util_cq.c:231`), and `vrb_cq_open()`
   calls it (`prov/verbs/src/verbs_cq.c:512`).
   → Add `FI_XPU` to that mask.

3. **Three unguarded function-pointer calls will segfault, not return `-FI_ENOSYS`.**
   `fi_ep_export_xpu` (`fi_endpoint.h:366`), `fi_cq_export_xpu` / `fi_cntr_export_xpu`
   (`fi_eq.h:489,496`) and `fi_xpu_ctx` (`fi_xpu.h:142`) dereference the op with no
   `FI_CHECK_OP`, and no provider sets them, and there are no `fi_no_*` stubs.
   Compare `fi_av_lookup2` (`fi_domain.h:601`), which does it correctly.
   → Wrap all four in `FI_CHECK_OP` and add `fi_no_ep_export_xpu`,
   `fi_no_cq_export_xpu`, `fi_no_cntr_export_xpu`, `fi_no_xpu_ctx`.

4. **`FI_MR_XPU_DESC` is not printed by `fi_tostr`** — the mr_mode printer stops at
   `FI_MR_COLLECTIVE` (`src/fi_tostr.c:452`). Cosmetic but it makes debugging
   `fi_info -v` output misleading.

Non-blocking inconsistencies worth a cleanup patch: `man/fi_xpu.3.md:88` declares
`int iface` where the header has `enum fi_hmem_iface`; commit `739bccbba`'s message
describes a `fi_ops_mr::get_desc` that does not exist (it became
`fi_control(FI_GET_MR_XPU_DESC)`).

---

## 3. Design decisions for verbs

Settle these before writing code; they shape every later commit.

### 3.1 Route B — extract from a normal QP, do not build one with DEVX

For a kernel to post work it needs the SQ buffer, the CQ buffer, the doorbell
record, and the BF/UAR doorbell register as addresses it can dereference.
`ibv_create_qp()` does not *return* any of them — but mlx5 direct-verbs will hand
them over for a QP and CQ created the ordinary way. Two routes:

**Route A — DEVX.** Build the QP and CQ from scratch with
`mlx5dv_devx_obj_create()` over memory obtained from `alloc()`, so the rings live
in accelerator memory. Needs `mlx5dv_devx_umem_reg_ex()` over dma-buf fds,
hand-built QPC/CQC bit layouts, `mlx5dv_devx_alloc_uar()`, and DEVX `MODIFY_QP`
commands for the RST→INIT→RTR→RTS transitions. **And it breaks connection
management**: `rdma_connect()` requires an `ibv_qp`, which a DEVX object is not,
so all of `verbs_cm.c` stops applying to XPU MSG endpoints and the RC handshake
has to be hand-rolled.

**Route B — `mlx5dv_init_obj()`.** Create the QP and CQ exactly the way verbs
does today — `ibv_create_qp`, `rdma_create_qp`, the whole of `verbs_ep.c` and
`verbs_cm.c` untouched — then extract the addresses and `import()` them so the
kernel can reach them. Confirmed present in `/usr/include/infiniband/mlx5dv.h`
(rdma-core 58):

```c
struct mlx5dv_qp { __be32 *dbrec;
                   struct { void *buf; uint32_t wqe_cnt, stride; } sq, rq;
                   struct { void *reg; uint32_t size; } bf; ... uint32_t sqn; };
struct mlx5dv_cq { void *buf; __be32 *dbrec;
                   uint32_t cqe_cnt, cqe_size; void *cq_uar; uint32_t cqn; ... };
int mlx5dv_init_obj(struct mlx5dv_obj *obj, uint64_t obj_type);   /* :1028 */
bool mlx5dv_is_supported(struct ibv_device *device);              /* :1699 */
```

That is the entire §7 contract blob, with zero DEVX.

**Decision: Route B for v1.** It reaches a working `fi_xpu_write` +
`fi_xpu_cq_read` for a fraction of the code, keeps connection establishment
intact, and turns the riskiest unknown (is the UAR importable into the GPU
address space at all?) into a spike you can run in a day rather than after weeks
of DEVX work. Gate the whole path on an `mlx5dv.h` configure check plus a runtime
`mlx5dv_is_supported()`; report `max_xpu_ctx_cnt = 0` otherwise, so non-mlx5 and
non-DV builds are untouched.

What Route B costs, stated plainly:

- Every WQE store and CQE poll crosses PCIe from the accelerator, because the
  rings stay in host memory. Per-operation latency is materially worse than rings
  in HBM. This is the reason Route A exists as a follow-up.
- The XPU endpoint must be **exclusively kernel-driven**. Host and kernel would
  otherwise race on one producer index and one dbrec. Reject host-side
  `fi_send`/`fi_write` on an exported XPU EP, and `fi_cq_read` on an exported XPU
  CQ, with `-FI_ENOSYS`.
- libmlx5 may hand you a BlueFlame write-combining doorbell mapping — see §3.5.

Route A stays in the plan as **phase 2** (§6). D4, the device-side WQE build, is
identical under both routes, so nothing about the device work is wasted.

### 3.2 Which objects are in scope

| Object | Decision | Why |
|---|---|---|
| EP | in scope | `FI_XPU_CAP_EP` |
| CQ | in scope | `FI_XPU_CAP_CQ` |
| Counter | **out of scope** | verbs has `cntr_open = fi_no_cntr_open` in both domain ops (`verbs_domain.c:289,303`) — no counter support at all. Report `caps` without `FI_XPU_CAP_CNTR`. |
| MR raw desc | in scope | return `ibv_mr->lkey`, `mr_desc_size = sizeof(uint32_t)` |
| AV raw addr | MSG: N/A. DGRAM: in scope | MSG domain has `av_open = fi_no_av_open` — for RC the peer is baked into the QP, so report `av_addr_size = 0`. Only UD needs a real `lookup2` (AH / QPN / QKey). |

### 3.3 `attr->ops == NULL` policy — the app must always supply ops

`ops == NULL` nominally means "provider default mechanism". **For the XPU data
path it is not implementable, and `vrb_xpu_ctx()` must return `-FI_ENOSYS`
rather than falling back to `ofi_hmem`.**

The reason is context ownership, not missing primitives. Level Zero device
allocations are scoped to a `ze_context_handle_t`, and libfabric creates its own
private context in `ze_hmem_init()` (`src/hmem_ze.c:717`). A pointer from
`zeMemAllocDevice()` in libfabric's context is **not dereferenceable from a
kernel launched on the application's SYCL queue**, which carries a different
context. Everything here exists specifically to be dereferenced by the app's
kernel, so it has to come from the app's context. CUDA behaves the same way
(`cuMemAlloc` is per-`CUcontext`).

`fi_xpu_attr` offers no way out: `device` is an ordinal, and there is no field
for a `ze_context_handle_t`, `CUcontext`, or SYCL queue. The primitives are
already present — `hmem_ze.c:705-715` binds `zexDriverImportExternalPointer` and
`zexDriverReleaseImportedPointer`, and `cuda_set_sync_memops()` exists at
`hmem_cuda.c:291` — they are simply bound to the wrong context.

Three things must happen against one app-owned allocation, which is why they
cannot be split between app and provider: allocate in the app's context, export
the dma-buf fd from *that* allocation, and set the no-reorder-around-DMA
attribute on it (`cuPointerSetAttribute(SYNC_MEMOPS)` in
`xpu_usage_example.c:141`, or the ZE equivalent). `example_xpu_alloc()` in that
file does all three together; see also §9 on what an API change would need.

### 3.4 ABI hygiene

`fi_cq_attr` and `fi_cntr_attr` are app-allocated and have no version compat
layer, yet both grew an `xpu_ctx` pointer. **Only read `attr->xpu_ctx` when
`attr->flags & FI_XPU` is set**, or an app built against an older header will be
read out of bounds. (`fi_ep_attr` is safe — it comes from `fi_getinfo`, and
`c459d6a51` + `4ecb239b6` bumped that to `FABRIC_1.10` with a 1.9 compat layer
in `src/abi_1_0.c`.)

Also copy `attr->ops` defensively, honouring `ops->size`, so a future callback
added to `fi_xpu_ops` does not read past a short app struct.

### 3.5 Doorbell: force the non-BlueFlame path

BlueFlame inlines the WQE as one 64-byte write-combining burst to the UAR.
Nothing guarantees a GPU EU emits that as a single non-reordered PCIe
transaction, so the kernel must use the plain doorbell instead: one 8-byte write
to the doorbell register, WQE always fetched by the NIC from the SQ.

Under Route A this was a choice at `mlx5dv_devx_alloc_uar()` time
(`MLX5DV_UAR_ALLOC_TYPE_NC` rather than `_BF`). Under Route B libmlx5 already
made the choice, and `mlx5dv_qp.bf.reg` is whatever it mapped. Set
**`MLX5_SHUT_UP_BF=1`** in the process environment so libmlx5 uses the regular
doorbell path, and verify `bf.size` reflects it. Two consequences worth writing
down:

- The provider cannot silently set this for the app — it must be in place before
  `ibv_open_device()`. Either document it as a requirement for XPU endpoints and
  fail `fi_xpu_ctx()` when the environment does not have it, or set it in the
  verbs provider's own init before any device is opened. Prefer the latter, but
  only when an XPU-capable build is in use.
- Revisit BlueFlame only with a measured, platform-specific justification.

### 3.6 The doorbell record comes free under Route B

The DBR is an 8-byte-per-queue location that software writes with the producer
index (SQ/RQ) or consumer index (CQ) and the **NIC DMA-reads**. Under Route A the
provider had to allocate a page for it and register it as a umem. Under Route B
libmlx5 already allocated it, already told the firmware about it, and
`mlx5dv_init_obj()` hands back the pointer — so the provider only has to
`import(FI_XPU_IMPORT_DEVICEMAP)` it. No `alloc`, no umem, no `dbr_umem_id`.

One trap, and it is easy to get wrong: **the dbrec is 8 bytes at an arbitrary
offset inside a page that libmlx5 shares between many queues.** Host-VA import
primitives are page-granular — `cuMemHostRegister()` requires a page-aligned
address, and so does `zexDriverImportExternalPointer()`. So the provider must
round the dbrec address down to the page boundary, import the **page**, and add
the offset back onto the returned device address. See §4.1 for why that also
forces a refcounted import cache.

Note that neither route removes the ordering problem: the dbrec store and the
doorbell-register store target different devices, so the kernel needs an explicit
system-scope fence between them regardless (device plan §5).

---

## 4. Resource inventory

Exactly what each callback must produce, per XPU endpoint + CQ pair. SRQ/XRC is
out of scope.

| Resource | How obtained | Callback | Item |
|---|---|---|---|
| SQ ring | `mlx5dv_qp.sq.{buf,wqe_cnt,stride}` | `import(DEVICEMAP)` | H5 |
| RQ ring | `mlx5dv_qp.rq.*` | `import(DEVICEMAP)` | H5, only once `fi_xpu_recv` is in scope |
| QP dbrec | `mlx5dv_qp.dbrec` | `import(DEVICEMAP)`, page-rounded (§3.6) | H5 |
| Doorbell register | `mlx5dv_qp.bf.{reg,size}` | `import(IOMEMORY)` | H5 |
| CQ ring | `mlx5dv_cq.{buf,cqe_cnt,cqe_size}` | `import(DEVICEMAP)` | H4 |
| CQ dbrec | `mlx5dv_cq.dbrec` | `import(DEVICEMAP)`, page-rounded | H4 |
| CQ UAR | `mlx5dv_cq.cq_uar` | `import(IOMEMORY)` | H4 |
| EP `prov_ctx` blob | provider-defined struct | `alloc`, no dma-buf | H6 |
| CQ `prov_ctx` blob | provider-defined struct | `alloc`, no dma-buf | H6 |
| SQ context shadow array | provider-defined, `uint64_t[sq.wqe_cnt]` | `alloc`, no dma-buf | H5/H6 |
| AV raw addresses | `lookup2` | — app marshals it | H7 |
| MR raw descriptors | `FI_GET_MR_XPU_DESC` | — app marshals it | H3 |

So under Route B **`alloc` is used for exactly two things** — the `prov_ctx`
blobs and the shadow array — and `FI_XPU_ALLOC_DMABUF` is never needed at all,
because every buffer the NIC touches was allocated and registered by libmlx5.
The queue buffers reach the kernel through `import`, not `alloc`.

The **SQ context shadow array** is easy to miss. A raw CQE carries a WQE counter,
not a `wr_id` — that mapping lives in libmlx5's private `wrid` array, which the
kernel cannot see. `fi_xpu_cq_read` must return `op_context`, so the provider
needs a device-resident `uint64_t ctx[sq_wqe_cnt]` that `fi_xpu_write` stores
into and `fi_xpu_cq_read` indexes by `wqe_counter & (sq_wqe_cnt - 1)`. It is part
of the §7 contract, so it must be in the shared header.

Scalars — QPN/`sqn`, `cqn`, ring sizes, strides, the dbrec byte offset within its
imported page, lkey/rkey — are fields of the `prov_ctx` blob, not separate
allocations.

### 4.1 Import is page-granular, and pages are shared

Both consequences of §3.6, and both are provider bugs waiting to happen:

- **Round to page boundaries.** Import `page_base = addr & ~(page_size - 1)` for
  a length covering the object, then put `addr - page_base` in the blob as an
  offset. Applies to both dbrecs. The SQ/CQ buffers and the UAR mappings are
  already page-aligned, but the arithmetic should be uniform rather than
  special-cased.
- **Refcount the imports.** libmlx5 packs many queues' dbrecs into one page, and
  many QPs share one UAR. Importing the same page twice fails —
  `cuMemHostRegister()` returns `CUDA_ERROR_HOST_MEMORY_ALREADY_REGISTERED`. The
  XPU context therefore needs a small `{page_base, dev_addr, refcount}` table,
  consulted on import and decremented on teardown, so the second QP on a shared
  dbrec page reuses the first import instead of failing.

### 4.2 Initialisation: mostly solved by Route B

Route A had a hard blocker here — `alloc()` returns a device address with no
promise of host visibility (Level Zero device USM is not host-accessible unless
the app allocates shared USM), and `fi_xpu_ops` has no memset or copy, so the
provider could not stamp the CQE ring's ownership bits.

Route B removes that for every NIC-visible buffer: libmlx5 allocated the CQ ring
in host memory and already stamped the invalid ownership bits at create time
(verify against `mlx5_alloc_cq_buf` in rdma-core), and the SQ and dbrecs are
likewise host memory the provider can write directly.

What remains is the two `alloc`-backed objects — the `prov_ctx` blobs and the
shadow array. The blobs must be written once before kernel launch and the shadow
array zeroed. Options, in preference order:

1. Allocate them as host memory and `import(DEVICEMAP)` instead of using `alloc`
   at all. Then Route B needs **only** `import`, and initialisation is a plain
   `memcpy`. Costs a PCIe read per kernel-side blob access; the blob is small and
   read once per operation, so measure before rejecting this.
2. Add `memset`/`copy` callbacks to `fi_xpu_ops` (**C5**) and use `alloc`.
3. Require `alloc` to return host-writable memory (shared USM). Simplest, but
   silently constrains the app.

Recommendation: **(1) for v1**, since it makes the whole path importable and
defers C5's init half entirely. Switch to (2) if blob access shows up in profiles.

### 4.3 Teardown: `free()` does not cover the import or the fd

`void free(uint64_t device, void *addr)` has no size, no flags, and no
counterpart to `import`. Three concrete problems:

- **No unimport.** Releasing an L0 imported pointer is
  `zexDriverReleaseImportedPointer(host_ptr)`, keyed on the **host** address,
  while the provider holds `dev_addr`. The app cannot tell which one it was
  handed, and IOMEMORY versus DEVICEMAP mappings may need different release
  paths. Route B makes this the *dominant* lifecycle concern, since nearly
  everything is an import.
- **Nobody owns the dma-buf fd** returned by `alloc`. Provider closes it or app
  does; the header does not say, so it leaks or double-closes. Moot under Route B
  (no dma-buf), live again under Route A.
- **Ordering is unspecified.** The `ibv_qp` and `ibv_cq` must be destroyed only
  *after* every import over their buffers is released, or the kernel holds a
  mapping of freed host memory. The provider must enforce this; the man page
  should state it.

Fixing the first two is core work — **C5**.

---

## 5. New code layout

New file `prov/verbs/src/verbs_xpu.c`, added to `_verbs_files` in
`prov/verbs/Makefile.include` (both the DL and built-in branches pick it up from
that one list).

```c
/* verbs_ofi.h */

/* Refcounted page import, §4.1 */
struct vrb_xpu_page {
	void			*page_base;	/* host VA, page aligned */
	void			*dev_addr;	/* from import() */
	size_t			len;
	uint64_t		flags;		/* IOMEMORY | DEVICEMAP */
	int			refcount;
	struct dlist_entry	entry;
};

struct vrb_xpu_ctx {
	struct fid_xpu_ctx	xpu_ctx_fid;
	struct vrb_domain	*domain;
	enum fi_hmem_iface	iface;		/* opaque, not validated — §3.3 */
	uint64_t		device;		/* opaque, passed to callbacks */
	struct fi_xpu_ops	ops;		/* copy, honouring ops->size */
	struct dlist_entry	entry;		/* on domain->xpu_ctx_list */

	struct dlist_entry	pages;		/* vrb_xpu_page cache */
	ofi_mutex_t		lock;
};
```

`struct vrb_domain` (`verbs_ofi.h:407`) gains a `xpu_ctx_list` + lock so
`fi_close(domain)` can refuse or clean up outstanding contexts.
`struct vrb_cq` (`verbs_ofi.h:457`) and the EP each gain a
`struct vrb_xpu_ctx *xpu_ctx`, their `mlx5dv_cq`/`mlx5dv_qp` extraction results,
the device addresses returned for each import, and the `prov_ctx` blob pointer.
Everything page-granular goes through the context's `vrb_xpu_page` cache.

Note what is *absent* compared to Route A: no `mlx5dv_devx_uar`, no
`mlx5dv_devx_umem`, no dbrec bump allocator, no dma-buf fd bookkeeping.

---

## 6. Work items (one commit each, all buildable in isolation)

Repo rule: no squash on merge, so each commit must stand alone and carry
`Signed-off-by` (`git commit -s`).

**Core precursors** — separate PR, they are useful without verbs:

- **C1** `core: add FI_XPU to OFI_DOMAIN_PRIMARY_CAPS` — blocker 1.
- **C2** `core: accept FI_XPU in fi_cq_attr flags` — blocker 2.
- **C3** `core: guard XPU export ops with FI_CHECK_OP` — blocker 3, incl. `fi_no_*` stubs.
- **C4** `core: print FI_MR_XPU_DESC in fi_tostr` — blocker 4.
- **C5** `core: complete the fi_xpu_ops memory lifecycle` — §4.3. Adds
  `unimport(device, dev_addr, flags)` so imported mappings can be released with
  the flags they were created with, and documents dma-buf fd ownership plus the
  destroy-after-release ordering rule in `man/fi_xpu.3.md`. ABI-safe behind
  `ops->size`. Optionally also `memset`/`copy` for §4.2 option (2) — not needed
  if v1 takes option (1).

**verbs host side (Route B):**

- **H1** `prov/verbs: advertise FI_XPU capability`
  `FI_XPU` into `VERBS_DOMAIN_CAPS` (`verbs_info.c:52`), `max_xpu_ctx_cnt` into
  `verbs_domain_attr` (`verbs_info.c:97`), `FI_MR_XPU_DESC` into `.mr_mode`
  (`verbs_info.c:103`). Gate on a configure check for `mlx5dv.h` plus a runtime
  `mlx5dv_is_supported()` (`mlx5dv.h:1699`); the existing dmabuf probe at
  `verbs_info.c:809` is the model for the probe structure. Verify with
  `fi_info -p verbs -v` that `FI_XPU` survives `ofi_alter_info`.
- **H2** `prov/verbs: add XPU context creation and query`
  New `verbs_xpu.c`: `vrb_xpu_ctx()`, `vrb_xpu_ctx_query()`, close. Validation is
  deliberately minimal — **require `ops != NULL` with a non-NULL `alloc`,
  `import`, and `free`, reject `iface == FI_HMEM_SYSTEM`, and otherwise pass
  `iface` and `device` through opaquely.** The provider never dereferences a
  device pointer, so it has no basis to prefer ZE over CUDA or to validate an
  ordinal whose meaning is the app's. Report
  `caps = FI_XPU_CAP_EP | FI_XPU_CAP_CQ`, `av_addr_size`, `mr_desc_size`. Wire
  `.xpu_ctx` into `vrb_msg_domain_ops` (`verbs_domain.c:289`) and
  `vrb_dgram_domain_ops` (`:303`). At this point `xpu_usage_example.c` gets past
  step 4 instead of failing at `fi_xpu_ctx()`.
- **H3** `prov/verbs: add MR raw descriptor query`
  `vrb_mr_control()` handling `FI_GET_MR_XPU_DESC` → `ibv_mr->lkey` from
  `struct vrb_mem_desc` (`verbs_ofi.h:479`). Wire into **both** `vrb_mr_fi_ops`
  (`verbs_mr.c:52`) and `vrb_mr_cache_fi_ops` (`verbs_mr.c:266`) — easy to miss
  the cache one. Honour `desc->len` as in/out and reject undersized buffers.
- **H3a** `prov/verbs: add XPU page import cache`
  The `vrb_xpu_page` helper from §5: page-round, import, refcount, release. Every
  later import goes through it. Small, self-contained, and the only place the
  §4.1 traps have to be got right. Lands before H4/H5.
- **H4** `prov/verbs: support FI_XPU completion queues`
  `vrb_cq_open()` (`verbs_cq.c:493`) honours `FI_XPU` in `attr->flags` +
  `attr->xpu_ctx`. Create the `ibv_cq` unchanged, then
  `mlx5dv_init_obj(MLX5DV_OBJ_CQ)` and import `buf`, `dbrec`, and `cq_uar`
  through H3a. Host-side `fi_cq_read` on an XPU CQ must return `-FI_ENOSYS` once
  exported (§3.1 exclusivity).
- **H5** `prov/verbs: support FI_XPU endpoints`
  `.endpoint2 = vrb_open_ep2` on both domain ops, handling `FI_XPU` +
  `ep_attr->xpu_ctx`. The QP is created by the existing path — `ibv_create_qp` /
  `rdma_create_qp`, CM untouched — then `mlx5dv_init_obj(MLX5DV_OBJ_QP)` and
  import `sq.buf`, `dbrec`, and `bf.reg`. Allocate the SQ context shadow array
  (§4.2 option 1: host memory + import) and zero it. Reject host-side transmit
  ops on the EP. Without `endpoint2`, `fi_endpoint2` returns `-FI_ENOSYS` through
  `FI_CHECK_OP` (`fi_endpoint.h:194`). Enforce "EP and CQ must share one context".
- **H6** `prov/verbs: export EP and CQ for XPU access`
  `.export_xpu` on `vrb_ep_base_ops` (`verbs_ep.c:381`) and `vrb_cq_ops`
  (`verbs_cq.c:383`). Fill the `prov_ctx` blob from the H4/H5 state — ring device
  addresses, sizes, strides, dbrec device address plus in-page offset, doorbell
  register address, shadow array base, `sqn`/`cqn`, indices — then set `fclass`,
  `prov_id`, `prov_ctx`. Requires the `prov_id` reservation from the device plan
  (**D1**) — that one commit is shared between the two series.
- **H7** `prov/verbs: return raw AV addresses for XPU` *(DGRAM only)*
  Real `lookup2` on `vrb_dgram_av_ops` (`verbs_dgram_av.c:216`, currently
  `ofi_av_lookup2`) returning the AH/QPN/QKey triple, sized to match
  `av_addr_size` from H2.
- **H8** `fabtests: add fi_xpu host-side setup test`
  Host-only: create ctx, query, open XPU CQ/EP, export, marshal AV/MR, no kernel
  launch. Catches every regression in H1–H7 without a GPU toolchain in CI. Pair
  it with a stub `fi_xpu_ops` that records every `alloc`/`import` and asserts the
  matching release at teardown — that is the only automated check for §4.3. The
  stub can `mmap` and hand back the same pointer, so the test needs no
  accelerator at all.

**Phase 2 — Route A (rings in accelerator memory).** Only after the device side
works end to end under Route B, and only if profiling justifies it. Sketch, not a
commitment: DEVX UAR + umem registration over dma-buf fds; QPC/CQC construction
and `mlx5dv_devx_obj_create`; DEVX `MODIFY_QP` state transitions; a hand-rolled
RC handshake to replace `rdma_cm`; the §4.2 initialisation problem returns in
full, so C5's `memset`/`copy` half becomes mandatory. D4 is unaffected — the WQE
format does not change — so phase 2 is purely a resource-provenance swap behind
the same §7 contract.

---

## 7. Contract with the device side

`struct fid_xpu` (`include/rdma/fi_xpu.h:121`) is 16 bytes:
`{ uint32_t fclass; uint32_t prov_id; uint64_t prov_ctx; }`. `prov_ctx` is the
**only** channel from host to kernel, so it must be a device-accessible pointer
to a provider-private blob holding everything a kernel needs: SQ base + count +
stride + producer index, CQ base + CQE size + consumer index, the dbrec device
address for each queue, the doorbell register address, the SQ context shadow
array base, `sqn`, and the lkey/rkey fields. Layout is defined once and shared by
`verbs_xpu.c` (writer) and `fi_xpu_device_verbs.h` (reader) — see
`xpu_verbs_device_plan.md` §3. Any change to it must land as a single commit
touching both.

**The blob is route-independent by design.** It carries device addresses and
sizes, never anything about how they were obtained, so switching from Route B to
Route A changes `verbs_xpu.c` only and leaves the device code alone.

Fields the device-plan sketch is still missing and that **D2** must add: the SQ
context shadow array pointer (without it `fi_xpu_cq_read` cannot return
`op_context`), an explicit doorbell-register offset since §3.5 rules out
BlueFlame, and the dbrec in-page offset from §4.1.

---

## 8. Validation

- `fi_info -p verbs -v` shows `FI_XPU` in caps and non-zero `max_xpu_ctx_cnt`
  (this is the check for blocker 1).
- `xpu_usage_example.c` in the tree root runs to step 12 without `-FI_ENOSYS`.
- Old-ABI app (built against a pre-1.10 header) still runs — validates §3.4.
- Non-mlx5 or non-DV machine: `max_xpu_ctx_cnt == 0`, `fi_xpu_ctx()` returns
  `-FI_ENOSYS`, no crash, everything else unaffected.
- Two XPU endpoints on one domain, to prove the §4.1 refcounted import cache
  handles a shared dbrec page and a shared UAR. A single-EP test passes with a
  broken cache.
- Teardown under valgrind/ASAN with the H8 stub allocator asserting every
  `alloc` is freed and every `import` released exactly once.
- Assert the extracted geometry against reality: `sq.wqe_cnt * sq.stride` equals
  the imported length, `cqe_size` is 64 or 128, `bf.size` is what `MLX5_SHUT_UP_BF`
  implies (§3.5).

## 9. Open questions

Roughly in the order they block work.

- **Is the doorbell register importable into the accelerator's address space at
  all** on the target platform? `import(bf.reg, ..., FI_XPU_IMPORT_IOMEMORY)` →
  a kernel doing one 8-byte store. This is the highest-risk unknown in the whole
  effort and it is route-independent. Spike it **first**, before H3a; a
  standalone program using `ibv_create_qp` + `mlx5dv_init_obj` +
  `cuMemHostRegister(IOMEMORY)` is a day's work and answers it. Nothing
  downstream matters if the answer is no.
- Where do the `prov_ctx` blob and shadow array live (§4.2)? Option (1) makes
  Route B import-only and needs no core change; option (2) needs C5's init half.
  Decide before H5.
- Can the provider set `MLX5_SHUT_UP_BF` itself before `ibv_open_device()`, or
  must it be an app-side requirement (§3.5)? Affects whether H1 can be enabled by
  default or needs opt-in.
- Multiple XPU contexts per domain, or `max_xpu_ctx_cnt = 1`? Under Route B the
  import cache is per-context (§5), so >1 context on a domain means the same
  dbrec page can be imported once per context — decide whether the cache belongs
  on the domain instead.
- Should `attr->ops == NULL` ever be supportable (§3.3)? It would require
  `fi_xpu_attr` to carry the app's accelerator context — a `ze_context_handle_t`
  or `CUcontext`. `fi_xpu_attr` is app-allocated with no compat layer, so
  extending it needs the §3.4 treatment. Not needed for v1; worth raising
  upstream before another provider hardcodes a different answer.
- Is `FI_MR_XPU_DESC` required, i.e. can the kernel use a normal `void *desc`?
  Affects H1 and the whole MR story.
- Does the PCIe cost of host-resident rings actually matter for the target
  workload? This is the sole justification for phase 2 (Route A), so measure it
  with the Route B implementation before committing to that work.
