/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
/*
 * xpu_usage_example_ze.c - application-side usage of the OFI XPU API on
 * Intel GPUs (oneAPI Level Zero)
 *
 * This is the Level Zero counterpart of xpu_usage_example.c. The OFI side is
 * identical; everything that differs is the vendor runtime:
 *
 *   - iface is FI_HMEM_ZE, and a ZE "device" is a (driver, device) pair
 *     packed by fi_hmem_ze_device(), not a bare ordinal
 *   - the app must build a ze_context_handle_t before the callbacks can run;
 *     the callbacks only receive the packed device id, so the context lives
 *     in a global
 *   - zeMemAllocDevice() honors an arbitrary alignment, so alloc() never has
 *     to over-allocate and hand back an interior pointer
 *   - a DMA-BUF export must be requested when the allocation is made
 *     (ze_external_memory_export_desc_t in the alloc desc's pNext); the fd is
 *     then read back with zeMemGetAllocProperties()
 *   - ZE has one host-import primitive, zexDriverImportExternalPointer(), and
 *     unified addressing means the device pointer equals the host pointer
 *
 * The steps are the same as in the CUDA example:
 *
 *   1. discover a provider that advertises FI_XPU
 *   2. supply the vendor memory callbacks (alloc / import / free)
 *   3. create an XPU context and query its capabilities and sizes
 *   4. create CQ / counter / EP bound to that context
 *   5. export the EP, CQ and counter as opaque device handles
 *   6. marshal raw AV addresses and MR descriptors for the device
 *   7. copy all handles to device memory and launch the kernel
 *
 * The device-side kernel cannot live in a .c file - it is compiled by the
 * oneAPI toolchain. See the KERNEL SIDE block at the bottom of this file for
 * the companion SYCL source and its build line.
 *
 * STATUS: no in-tree provider implements FI_XPU yet, so this program is
 * expected to stop at step 3 with -FI_ENOSYS from fi_xpu_ctx(). It compiles
 * and documents the contract; it does not yet run.
 *
 * Build (host side, without Level Zero - callbacks compile to stubs,
 * ops = NULL):
 *   cc -o xpu_usage_example_ze xpu_usage_example_ze.c -lfabric
 *
 * Build (host side, with the Level Zero-backed callbacks):
 *   cc -DXPU_EXAMPLE_HAVE_ZE -o xpu_usage_example_ze xpu_usage_example_ze.c \
 *      -lfabric -lze_loader
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_xpu.h>

#define XPU_EXAMPLE_HAVE_ZE
#ifdef XPU_EXAMPLE_HAVE_ZE
#include <level_zero/ze_api.h>
#endif

#define CHECK(call)							\
	do {								\
		int _ret = (call);					\
		if (_ret) {						\
			fprintf(stderr, "%s:%d: %s = %d (%s)\n",	\
				__FILE__, __LINE__, #call,		\
				_ret, fi_strerror(-_ret));		\
			return _ret;					\
		}							\
	} while (0)

#define ZE_DRIVER	0		/* driver index */
#define ZE_DEVICE	0		/* device index within that driver */
#define BUF_SIZE	4096

/*
 * ===========================================================================
 * SECTION 1 - vendor memory callbacks (struct fi_xpu_ops)
 * ===========================================================================
 *
 * The provider calls these when it needs device memory or needs a host
 * address made visible to XPU kernels. Passing ops = NULL in fi_xpu_attr is
 * legal and asks the provider to use its own default mechanisms; supply
 * these when the allocation must come from your runtime's allocator (e.g. a
 * pool shared with the rest of the application).
 *
 * Contract reminders:
 *   - alloc() must honor FI_XPU_ALLOC_DMABUF by also returning a DMA-BUF fd
 *     plus the offset of *addr within the exported range. The provider DMAs
 *     into this memory, so it must be device-resident and pinned.
 *   - import() maps a host address into the XPU address space.
 *     FI_XPU_IMPORT_IOMEMORY means the address is PCIe BAR MMIO (a NIC
 *     doorbell); FI_XPU_IMPORT_DEVICEMAP means the result must be
 *     dereferenceable from kernel code.
 *   - free() receives the address alloc() returned in *addr.
 */

#ifdef XPU_EXAMPLE_HAVE_ZE

/*
 * zexDriverImportExternalPointer() is a driver extension: it is not declared
 * in ze_api.h and must be looked up by name.
 */
typedef ze_result_t (*zex_import_ptr_t)(ze_driver_handle_t hDriver, void *ptr,
					size_t size);
typedef ze_result_t (*zex_release_ptr_t)(ze_driver_handle_t hDriver, void *ptr);

static ze_driver_handle_t		ze_driver;
static ze_device_handle_t		ze_device;
static ze_context_handle_t		ze_context;
static ze_command_list_handle_t		ze_cmd_list;
static zex_import_ptr_t			ze_import_ptr;
static zex_release_ptr_t		ze_release_ptr;

/*
 * Only the DMA-BUF fd needs tracking: unlike cuMemAlloc(), zeMemAllocDevice()
 * takes the alignment directly, so free() gets back exactly the pointer the
 * allocator returned and zeMemFree() can be called on it as-is.
 */
#define MAX_ALLOCS 64
static struct {
	void	*ptr;
	int	dmabuf_fd;
} allocs[MAX_ALLOCS];

static int ze_check(ze_result_t res, const char *what)
{
	if (res == ZE_RESULT_SUCCESS)
		return 0;

	fprintf(stderr, "%s failed: 0x%x\n", what, (unsigned int) res);
	return -FI_EIO;
}

/*
 * fi_hmem_ze_device() packs the pair as driver_index << 16 | device_index;
 * unpack and validate against what this example set up.
 */
static int example_ze_lookup(uint64_t device, ze_device_handle_t *handle)
{
	if (device != (uint64_t) fi_hmem_ze_device(ZE_DRIVER, ZE_DEVICE)) {
		fprintf(stderr, "unexpected ZE device id 0x%" PRIx64 "\n",
			device);
		return -FI_EINVAL;
	}

	*handle = ze_device;
	return 0;
}

static int example_xpu_alloc(uint64_t device, uint64_t size,
			     uint64_t alignment, uint64_t flags,
			     void **addr, int *fd, uint64_t *offset)
{
	ze_external_memory_export_desc_t export_desc = {
		.stype	= ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_DESC,
		.flags	= ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF,
	};
	ze_device_mem_alloc_desc_t alloc_desc = {
		.stype	= ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
	};
	ze_device_handle_t dev;
	void *ptr;
	int slot, ret;

	ret = example_ze_lookup(device, &dev);
	if (ret)
		return ret;

	for (slot = 0; slot < MAX_ALLOCS; slot++)
		if (!allocs[slot].ptr)
			break;
	if (slot == MAX_ALLOCS)
		return -FI_ENOMEM;

	/*
	 * The export must be requested up front - there is no way to make an
	 * existing allocation exportable after the fact.
	 */
	if (flags & FI_XPU_ALLOC_DMABUF)
		alloc_desc.pNext = &export_desc;

	if (ze_check(zeMemAllocDevice(ze_context, &alloc_desc, size,
				      alignment ? alignment : 1, dev, &ptr),
		     "zeMemAllocDevice"))
		return -FI_ENOMEM;

	allocs[slot].ptr = ptr;
	allocs[slot].dmabuf_fd = -1;

	if (flags & FI_XPU_ALLOC_DMABUF) {
		/*
		 * The fd covers the whole allocation, so report where *addr
		 * lands inside it. This mirrors ze_hmem_get_dmabuf_fd() in
		 * src/hmem_ze.c.
		 */
		ze_external_memory_export_fd_t export_fd = {
			.stype	= ZE_STRUCTURE_TYPE_EXTERNAL_MEMORY_EXPORT_FD,
			.flags	= ZE_EXTERNAL_MEMORY_TYPE_FLAG_DMA_BUF,
		};
		ze_memory_allocation_properties_t props = {
			.stype	= ZE_STRUCTURE_TYPE_MEMORY_ALLOCATION_PROPERTIES,
			.pNext	= &export_fd,
		};
		ze_device_handle_t alloc_dev;
		void *base;
		size_t range;

		if (ze_check(zeMemGetAllocProperties(ze_context, ptr, &props,
						     &alloc_dev),
			     "zeMemGetAllocProperties"))
			goto err;

		if (ze_check(zeMemGetAddressRange(ze_context, ptr, &base,
						  &range),
			     "zeMemGetAddressRange")) {
			close(export_fd.fd);
			goto err;
		}

		*fd = export_fd.fd;
		*offset = (uint64_t) ((uintptr_t) ptr - (uintptr_t) base);
		allocs[slot].dmabuf_fd = export_fd.fd;
	} else {
		*fd = -1;
		*offset = 0;
	}

	*addr = ptr;
	return 0;
err:
	allocs[slot].ptr = NULL;
	zeMemFree(ze_context, ptr);
	return -FI_EIO;
}

static int example_xpu_import(uint64_t device, void *host_addr,
			      uint64_t size, uint64_t flags, void **dev_addr)
{
	ze_device_handle_t dev;
	int ret;

	ret = example_ze_lookup(device, &dev);
	if (ret)
		return ret;

	if (!ze_import_ptr)
		return -FI_ENOSYS;

	/*
	 * ZE exposes a single import primitive, so FI_XPU_IMPORT_DEVICEMAP and
	 * FI_XPU_IMPORT_IOMEMORY both map onto it: the driver pins the range
	 * and makes it addressable from kernels. Nothing distinguishes MMIO
	 * here, and a driver is free to reject an uncached BAR mapping - check
	 * the return value rather than assuming a doorbell can be imported.
	 */
	if (ze_check(ze_import_ptr(ze_driver, host_addr, size),
		     "zexDriverImportExternalPointer"))
		return -FI_EIO;

	/* Unified addressing: the device sees the same virtual address. */
	*dev_addr = host_addr;
	return 0;
}

static void example_xpu_free(uint64_t device, void *addr)
{
	int slot;

	for (slot = 0; slot < MAX_ALLOCS; slot++) {
		if (allocs[slot].ptr != addr)
			continue;
		if (allocs[slot].dmabuf_fd >= 0)
			close(allocs[slot].dmabuf_fd);
		zeMemFree(ze_context, addr);
		memset(&allocs[slot], 0, sizeof(allocs[slot]));
		return;
	}
	fprintf(stderr, "free of unknown XPU allocation %p\n", addr);
}

/*
 * The immediate command list is created in synchronous mode, so each append
 * below blocks until the copy has landed - no queue, fence or event needed.
 */
static int example_ze_init(void)
{
	ze_command_queue_desc_t cq_desc = {
		.stype		= ZE_STRUCTURE_TYPE_COMMAND_QUEUE_DESC,
		.ordinal	= 0,
		.index		= 0,
		.mode		= ZE_COMMAND_QUEUE_MODE_SYNCHRONOUS,
		.priority	= ZE_COMMAND_QUEUE_PRIORITY_NORMAL,
	};
	ze_context_desc_t ctx_desc = {
		.stype		= ZE_STRUCTURE_TYPE_CONTEXT_DESC,
	};
	ze_driver_handle_t drivers[ZE_DRIVER + 1];
	ze_device_handle_t devices[ZE_DEVICE + 1];
	uint32_t count;

	if (ze_check(zeInit(ZE_INIT_FLAG_GPU_ONLY), "zeInit"))
		return -FI_EIO;

	count = ZE_DRIVER + 1;
	if (ze_check(zeDriverGet(&count, drivers), "zeDriverGet"))
		return -FI_EIO;
	if (count <= ZE_DRIVER)
		return -FI_ENODEV;
	ze_driver = drivers[ZE_DRIVER];

	count = ZE_DEVICE + 1;
	if (ze_check(zeDeviceGet(ze_driver, &count, devices), "zeDeviceGet"))
		return -FI_EIO;
	if (count <= ZE_DEVICE)
		return -FI_ENODEV;
	ze_device = devices[ZE_DEVICE];

	if (ze_check(zeContextCreate(ze_driver, &ctx_desc, &ze_context),
		     "zeContextCreate"))
		return -FI_EIO;

	if (ze_check(zeCommandListCreateImmediate(ze_context, ze_device,
						  &cq_desc, &ze_cmd_list),
		     "zeCommandListCreateImmediate"))
		return -FI_EIO;

	/* Optional: import() degrades to -FI_ENOSYS without it. */
	zeDriverGetExtensionFunctionAddress(ze_driver,
					"zexDriverImportExternalPointer",
					(void **) &ze_import_ptr);
	zeDriverGetExtensionFunctionAddress(ze_driver,
					"zexDriverReleaseImportedPointer",
					(void **) &ze_release_ptr);
	return 0;
}

static int copy_to_device(void **dev, const void *host, size_t size)
{
	ze_device_mem_alloc_desc_t alloc_desc = {
		.stype	= ZE_STRUCTURE_TYPE_DEVICE_MEM_ALLOC_DESC,
	};
	void *ptr;

	if (ze_check(zeMemAllocDevice(ze_context, &alloc_desc, size, 64,
				      ze_device, &ptr),
		     "zeMemAllocDevice"))
		return -FI_ENOMEM;

	if (ze_check(zeCommandListAppendMemoryCopy(ze_cmd_list, ptr, host,
						   size, NULL, 0, NULL),
		     "zeCommandListAppendMemoryCopy")) {
		zeMemFree(ze_context, ptr);
		return -FI_EIO;
	}

	*dev = ptr;
	return 0;
}

static struct fi_xpu_ops example_ops = {
	.size	= sizeof(struct fi_xpu_ops),	/* mandatory - see below */
	.alloc	= example_xpu_alloc,
	.import	= example_xpu_import,
	.free	= example_xpu_free,
};
#define EXAMPLE_OPS (&example_ops)

#else /* !XPU_EXAMPLE_HAVE_ZE */

/* No runtime linked in: ask the provider to use its own defaults. */
#define EXAMPLE_OPS NULL

static int example_ze_init(void)
{
	return 0;
}

static int copy_to_device(void **dev, const void *host, size_t size)
{
	*dev = malloc(size);
	if (!*dev)
		return -FI_ENOMEM;
	memcpy(*dev, host, size);
	return 0;
}

#endif /* XPU_EXAMPLE_HAVE_ZE */

/*
 * ===========================================================================
 * SECTION 2 - host setup
 * ===========================================================================
 */

/*
 * Replace with your real out-of-band exchange (sockets, MPI, PMI...). The
 * loopback default makes this a self-send, which is enough to exercise the
 * full setup path.
 */
static void oob_exchange(void *local, void *remote, size_t len)
{
	memcpy(remote, local, len);
}

int main(void)
{
	struct fi_info *hints, *info;
	struct fid_fabric *fabric;
	struct fid_domain *domain;
	struct fid_xpu_ctx *xpu_ctx;
	struct fid_av *av;
	struct fid_cq *cq;
	struct fid_cntr *cntr;
	struct fid_ep *ep;
	struct fid_mr *mr;

	struct fi_xpu_ctx_attr ctx_attr = { 0 };
	struct fi_cq_attr cq_attr = { 0 };
	struct fi_cntr_attr cntr_attr = { 0 };
	struct fi_av_attr av_attr = { 0 };
	struct fi_mr_attr mr_attr = { 0 };
	struct iovec iov;

	struct fid_xpu_ep xpu_ep = { 0 };
	struct fid_xpu_cq xpu_cq = { 0 };
	struct fid_xpu_cntr xpu_cntr = { 0 };
	void *raw_addr, *raw_desc;
	size_t len;

	void *d_ep, *d_cq, *d_cntr, *d_addr, *d_desc;

	char local_name[256], peer_name[256];
	size_t name_len = sizeof(local_name);
	fi_addr_t peer;
	void *buf;

	/*
	 * The ZE context must exist before fi_xpu_ctx(): the provider may call
	 * into the callbacks from inside that call.
	 */
	CHECK(example_ze_init());

	/* --- 1. discover a provider that supports FI_XPU ------------------ */

	hints = fi_allocinfo();
	if (!hints)
		return -FI_ENOMEM;

	hints->caps = FI_MSG | FI_RMA | FI_XPU;
	hints->ep_attr->type = FI_EP_RDM;
	hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_ALLOCATED |
				      FI_MR_VIRT_ADDR | FI_MR_PROV_KEY;
	hints->mode = FI_CONTEXT;

	CHECK(fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
			 NULL, NULL, 0, hints, &info));

	/*
	 * A provider may return FI_XPU in caps yet still support zero XPU
	 * contexts on this particular domain - check both.
	 */
	if (!info->domain_attr->max_xpu_ctx_cnt) {
		fprintf(stderr, "%s: no XPU context support\n",
			info->fabric_attr->prov_name);
		return -FI_ENOSYS;
	}
	printf("provider %s: max_xpu_ctx_cnt=%zu\n",
	       info->fabric_attr->prov_name,
	       info->domain_attr->max_xpu_ctx_cnt);

	CHECK(fi_fabric(info->fabric_attr, &fabric, NULL));
	CHECK(fi_domain(fabric, info, &domain, NULL));

	/* --- 2. create the XPU context ------------------------------------ */

	/*
	 * iface + device identify the target XPU. For FI_HMEM_ZE the device is
	 * the packed (driver, device) pair, matching mr_attr.device.ze. ops is
	 * optional; when supplied, ops->size must be set so the provider can
	 * tell which callbacks are present.
	 */
	struct fi_xpu_attr xpu_attr = {
		.iface	= FI_HMEM_ZE,
		.device	= (uint64_t) fi_hmem_ze_device(ZE_DRIVER, ZE_DEVICE),
		.ops	= EXAMPLE_OPS,
	};

	/* Every in-tree provider returns -FI_ENOSYS here today. */
	CHECK(fi_xpu_ctx(domain, &xpu_attr, &xpu_ctx, NULL));

	/* --- 3. query capabilities and marshalling sizes ------------------ */

	CHECK(fi_xpu_ctx_query(xpu_ctx, &ctx_attr));
	printf("xpu caps: ep=%d cq=%d cntr=%d, av_addr_size=%zu "
	       "mr_desc_size=%zu\n",
	       !!(ctx_attr.caps & FI_XPU_CAP_EP),
	       !!(ctx_attr.caps & FI_XPU_CAP_CQ),
	       !!(ctx_attr.caps & FI_XPU_CAP_CNTR),
	       ctx_attr.av_addr_size, ctx_attr.mr_desc_size);

	/*
	 * Creating an object whose capability bit is clear returns
	 * -FI_ENOSYS. A provider may support device-side completion polling
	 * (CQ/CNTR) without device-side posting (EP).
	 */
	if (!(ctx_attr.caps & FI_XPU_CAP_EP)) {
		fprintf(stderr, "provider cannot post from the device\n");
		return -FI_ENOSYS;
	}

	/* --- 4. create the objects ---------------------------------------- */

	/*
	 * A CQ or counter created with xpu_ctx set can ONLY be read from the
	 * device: fi_cq_read() and fi_cntr_read() are unavailable on it. Bind
	 * a second, plain host CQ if you want host-visible error reporting.
	 */
	cq_attr.format = FI_CQ_FORMAT_DATA;
	cq_attr.size = 64;
	cq_attr.xpu_ctx = xpu_ctx;
	CHECK(fi_cq_open(domain, &cq_attr, &cq, NULL));

	cntr_attr.events = FI_CNTR_EVENTS_COMP;
	cntr_attr.xpu_ctx = xpu_ctx;
	CHECK(fi_cntr_open(domain, &cntr_attr, &cntr, NULL));

	/* AV and MR are domain-level: no xpu_ctx at creation time. */
	av_attr.type = FI_AV_TABLE;
	CHECK(fi_av_open(domain, &av_attr, &av, NULL));

	/*
	 * FI_XPU must be passed in the flags argument of fi_endpoint2() -
	 * fi_endpoint2() with flags == 0 falls through to fi_endpoint().
	 * The EP's data path moves to the device; fi_send() and friends are
	 * no longer available on it from the host.
	 */
	info->ep_attr->xpu_ctx = xpu_ctx;
	CHECK(fi_endpoint2(domain, info, &ep, FI_XPU, NULL));

	CHECK(fi_ep_bind(ep, &av->fid, 0));
	CHECK(fi_ep_bind(ep, &cq->fid, FI_TRANSMIT | FI_RECV));
	CHECK(fi_ep_bind(ep, &cntr->fid, FI_SEND));
	CHECK(fi_enable(ep));

	/*
	 * The data buffer must itself be device memory the kernel can touch;
	 * with the callbacks above, allocate it through the same path the
	 * provider uses.
	 */
	buf = NULL;
#ifdef XPU_EXAMPLE_HAVE_ZE
	{
		int fd;
		uint64_t off;
		CHECK(example_xpu_alloc(fi_hmem_ze_device(ZE_DRIVER, ZE_DEVICE),
					BUF_SIZE, 4096, 0, &buf, &fd, &off));
	}
#else
	buf = calloc(1, BUF_SIZE);
#endif

	iov.iov_base = buf;
	iov.iov_len = BUF_SIZE;
	mr_attr.mr_iov = &iov;
	mr_attr.iov_count = 1;
	mr_attr.access = FI_SEND | FI_RECV | FI_READ | FI_WRITE |
			 FI_REMOTE_READ | FI_REMOTE_WRITE;
	mr_attr.iface = FI_HMEM_ZE;
	mr_attr.device.ze = fi_hmem_ze_device(ZE_DRIVER, ZE_DEVICE);
	CHECK(fi_mr_regattr(domain, &mr_attr, 0, &mr));

	/* --- 5. address exchange ------------------------------------------ */

	CHECK(fi_getname(&ep->fid, local_name, &name_len));
	oob_exchange(local_name, peer_name, name_len);
	if (fi_av_insert(av, peer_name, 1, &peer, 0, NULL) != 1)
		return -FI_EOTHER;

	/* --- 6. export handles for the device ----------------------------- */

	/*
	 * The caller owns these structures; the provider fills in fclass,
	 * prov_id and prov_ctx. prov_ctx points at provider state that must
	 * stay alive, so keep the objects open for as long as the kernel runs.
	 */
	CHECK(fi_ep_export_xpu(ep, 0, &xpu_ep));
	CHECK(fi_cq_export_xpu(cq, 0, &xpu_cq));
	CHECK(fi_cntr_export_xpu(cntr, 0, &xpu_cntr));

	/*
	 * The device cannot use fi_addr_t or a struct fid_mr *. Convert both
	 * to their raw provider representations, sized by the context query.
	 */
	raw_addr = malloc(ctx_attr.av_addr_size);
	raw_desc = malloc(ctx_attr.mr_desc_size);
	if (!raw_addr || !raw_desc)
		return -FI_ENOMEM;

	len = ctx_attr.av_addr_size;
	CHECK(fi_av_lookup2(av, peer, raw_addr, &len, FI_XPU, xpu_ctx));

	len = ctx_attr.mr_desc_size;
	CHECK(fi_mr_get_xpu_desc(mr, raw_desc, &len, FI_XPU, xpu_ctx));

	/* --- 7. stage everything in device memory and run the kernel ------ */

	CHECK(copy_to_device(&d_ep, &xpu_ep, sizeof(xpu_ep)));
	CHECK(copy_to_device(&d_cq, &xpu_cq, sizeof(xpu_cq)));
	CHECK(copy_to_device(&d_cntr, &xpu_cntr, sizeof(xpu_cntr)));
	CHECK(copy_to_device(&d_addr, raw_addr, ctx_attr.av_addr_size));
	CHECK(copy_to_device(&d_desc, raw_desc, ctx_attr.mr_desc_size));

	/*
	 * launch_xpu_send(queue, d_ep, d_cntr, d_addr, d_desc, buf, BUF_SIZE);
	 *
	 * Defined in the companion SYCL file below. The host does not
	 * participate in the transfer at all from here on.
	 */
	printf("setup complete; launch the kernel with the handles above\n");

	/* --- 8. teardown -------------------------------------------------- */

	free(raw_addr);
	free(raw_desc);
	fi_close(&ep->fid);
	fi_close(&mr->fid);
	fi_close(&cq->fid);
	fi_close(&cntr->fid);
	fi_close(&av->fid);
	fi_close(&xpu_ctx->fid);	/* after all objects that used it */
	fi_close(&domain->fid);
	fi_close(&fabric->fid);
	fi_freeinfo(info);
	fi_freeinfo(hints);
	return 0;
}

/*
 * ===========================================================================
 * KERNEL SIDE - companion xpu_usage_kernel_ze.cpp
 * ===========================================================================
 *
 * Build:
 *   icpx -fsycl -c xpu_usage_kernel_ze.cpp -I/path/to/libfabric/include
 *
 * Nothing here links against libfabric: fi_xpu_device.h is a header-only
 * inline API that dispatches on the provider id embedded in each exported
 * handle. Every one of these calls returns -FI_ENOSYS until a provider adds
 * its case to the dispatch switches in fi_xpu_device.h.
 *
 * The queue must be built on the same ZE context and device the provider
 * callbacks used, otherwise the staged handles and the data buffer are not
 * valid pointers inside the kernel. Use the Level Zero backend interop rather
 * than letting SYCL pick its own platform:
 *
 *   #include <sycl/sycl.hpp>
 *   #include <sycl/ext/oneapi/backend/level_zero.hpp>
 *   #include <rdma/fi_xpu_device.h>
 *
 *   namespace ze = sycl::ext::oneapi::level_zero;
 *
 *   extern "C" sycl::queue *
 *   make_xpu_queue(ze_driver_handle_t drv, ze_device_handle_t dev,
 *                  ze_context_handle_t ctx)
 *   {
 *           auto platform = sycl::make_platform<sycl::backend::ext_oneapi_level_zero>(drv);
 *           auto device = sycl::make_device<sycl::backend::ext_oneapi_level_zero>(dev);
 *           auto context = sycl::make_context<sycl::backend::ext_oneapi_level_zero>(
 *                           {ctx, {device}, ze::ownership::keep});
 *           return new sycl::queue(context, device);
 *   }
 *
 *   // A single work item posts and waits on its own completion.
 *   extern "C" void
 *   launch_xpu_send(sycl::queue &q, struct fid_xpu_ep *ep,
 *                   struct fid_xpu_cntr *cntr, void *peer, void *desc,
 *                   void *buf, size_t len)
 *   {
 *           q.single_task([=]() {
 *                   // Sample the counter before posting so the wait below is
 *                   // not satisfied by an unrelated earlier completion.
 *                   uint64_t prev = fi_xpu_cntr_read(cntr, FI_XPU_WORK_ITEM);
 *
 *                   // desc and peer are the raw blobs from
 *                   // fi_mr_get_xpu_desc() and fi_av_lookup2(), not a
 *                   // struct fid_mr * or an fi_addr_t.
 *                   if (fi_xpu_send(ep, buf, len, desc, 0, peer, nullptr, 0,
 *                                   FI_XPU_WORK_ITEM))
 *                           return;
 *
 *                   fi_xpu_cntr_wait(cntr, prev + 1, -1, FI_XPU_WORK_ITEM);
 *           }).wait();
 *   }
 *
 *   // The scope argument states which threads issue the operation
 *   // collectively; all threads in the scope must make the same call, which
 *   // lets the provider have one thread build the descriptor and ring the
 *   // doorbell for the whole group. Here the whole work group cooperates, so
 *   // the nd_range local size must match the scope.
 *   extern "C" void
 *   launch_xpu_write(sycl::queue &q, struct fid_xpu_ep *ep, void *peer,
 *                    void *desc, void *buf, size_t len, uint64_t raddr,
 *                    uint64_t rkey)
 *   {
 *           sycl::nd_range<1> range{sycl::range<1>{32}, sycl::range<1>{32}};
 *
 *           q.parallel_for(range, [=](sycl::nd_item<1>) {
 *                   fi_xpu_write(ep, buf, len, desc, 0, peer, raddr, rkey,
 *                                nullptr, 0, FI_XPU_WORK_GROUP);
 *           }).wait();
 *   }
 */
