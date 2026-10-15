/*
 * qwz_compat.cpp - OpenBSD kernel services for qwz(4) on macOS.
 * See qwz_compat.h for the concurrency and DMA model.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "qwz_compat.h"
#include "qwz_fw.h"

#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <kern/clock.h>
#include <kern/thread.h>

#define QWZLOG(fmt, ...)	qwz_log("qwz: " fmt "\n", ##__VA_ARGS__)

/* ---- malloc(9) -------------------------------------------------------- */

/* free(9) may be called with size 0, so every block records its own size. */
struct qwz_mhdr {
	size_t	size;
	size_t	magic;
};
#define QWZ_MAGIC	0x71777a6dUL	/* "qwzm" */

void *
qwz_malloc(size_t sz, int type, int flags)
{
	size_t tot = sz + sizeof(struct qwz_mhdr);
	struct qwz_mhdr *h;

	if (tot < sz)
		return NULL;
	h = (struct qwz_mhdr *)IOMalloc(tot);
	if (h == NULL) {
		if (!(flags & M_NOWAIT))
			QWZLOG("malloc(%zu) failed with M_WAITOK", sz);
		return NULL;
	}
	h->size = tot;
	h->magic = QWZ_MAGIC;
	if (flags & M_ZERO)
		bzero(h + 1, sz);
	return h + 1;
}

void *
qwz_mallocarray(size_t n, size_t sz, int type, int flags)
{
	if (sz != 0 && n > SIZE_MAX / sz)
		return NULL;
	return qwz_malloc(n * sz, type, flags);
}

void
qwz_free(void *p, int type, size_t sz)
{
	struct qwz_mhdr *h;

	if (p == NULL)
		return;
	h = (struct qwz_mhdr *)p - 1;
	if (h->magic != QWZ_MAGIC)
		panic("qwz: free(%p) of a block malloc() did not return", p);
	h->magic = 0;
	IOFree(h, h->size);
}

/*
 * itl80211's ieee80211_var.h has "#define explicit_bzero bzero", which
 * turned this definition into a bzero() that called itself: every bzero()
 * in the kext bound to it and overflowed the kernel stack (a double fault,
 * so the machine froze or reset with no panic). Undo the macro here.
 */
#undef explicit_bzero
void
explicit_bzero(void *p, size_t n)
{
	bzero(p, n);
	__asm__ volatile("" ::: "memory");
}

uint64_t
getnsecuptime(void)
{
	uint64_t abs, ns;

	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

/* ---- sleep / wakeup --------------------------------------------------- */

static IOCommandGate *qwz_gate;

void
qwz_compat_set_gate(IOCommandGate *gate)
{
	qwz_gate = gate;
}

/*
 * Sleep on `ident`, releasing the gate while asleep. Only legal from a thread
 * that holds the gate and is not the workloop thread itself; OpenBSD forbids
 * the equivalent (sleeping in interrupt context) too.
 */
int
qwz_tsleep_nsec(const volatile void *ident, int prio, const char *wmesg, uint64_t nsecs)
{
	IOWorkLoop *wl;
	IOReturn ret;

	if (qwz_gate == NULL || (wl = qwz_gate->getWorkLoop()) == NULL) {
		QWZLOG("tsleep(%s) before the gate exists", wmesg);
		IOSleep(nsecs == INFSLP ? 1000 : (uint32_t)(nsecs / 1000000) + 1);
		return EWOULDBLOCK;
	}
	if (wl->onThread() || !wl->inGate()) {
		QWZLOG("tsleep(%s) from %s; not sleeping", wmesg,
		    wl->onThread() ? "the workloop thread" : "outside the gate");
		return EWOULDBLOCK;
	}

	if (nsecs == INFSLP) {
		ret = qwz_gate->commandSleep((void *)ident, THREAD_UNINT);
	} else {
		uint64_t interval, deadline;

		nanoseconds_to_absolutetime(nsecs, &interval);
		deadline = mach_absolute_time() + interval;
		ret = qwz_gate->commandSleep((void *)ident, deadline, THREAD_UNINT);
	}
	switch (ret) {
	case kIOReturnSuccess:
		return 0;
	case kIOReturnTimeout:
		return EWOULDBLOCK;
	case kIOReturnAborted:
		return EINTR;
	default:
		QWZLOG("tsleep(%s): commandSleep returned 0x%x", wmesg, ret);
		return EWOULDBLOCK;
	}
}

void
qwz_wakeup(const volatile void *ident)
{
	if (qwz_gate != NULL)
		qwz_gate->commandWakeup((void *)ident, false);
}

void
qwz_wakeup_one(const volatile void *ident)
{
	if (qwz_gate != NULL)
		qwz_gate->commandWakeup((void *)ident, true);
}

/* ---- rwlock(9) / refcnt(9) -------------------------------------------- */

void
rw_init(struct rwlock *rwl, const char *name)
{
	rwl->rwl_name = name;
	rwl->rwl_owner = NULL;
}

int
rw_enter(struct rwlock *rwl, int flags)
{
	void *self = current_thread();
	int err;

	while (rwl->rwl_owner != NULL) {
		if (rwl->rwl_owner == self)
			panic("qwz: rwlock %s recursed", rwl->rwl_name);
		if (flags & RW_NOSLEEP)
			return EBUSY;
		err = qwz_tsleep_nsec(rwl, 0, rwl->rwl_name, INFSLP);
		if (err && (flags & RW_INTR))
			return err;
	}
	rwl->rwl_owner = self;
	return 0;
}

void
rw_enter_write(struct rwlock *rwl)
{
	rw_enter(rwl, RW_WRITE);
}

void
rw_exit(struct rwlock *rwl)
{
	rwl->rwl_owner = NULL;
	qwz_wakeup(rwl);
}

void
rw_exit_write(struct rwlock *rwl)
{
	rw_exit(rwl);
}

void
refcnt_init(struct refcnt *r)
{
	r->r_refs = 1;
}

void
refcnt_take(struct refcnt *r)
{
	OSIncrementAtomic(&r->r_refs);
}

int
refcnt_rele(struct refcnt *r)
{
	return OSDecrementAtomic(&r->r_refs) == 1;	/* returns the old value */
}

void
refcnt_rele_wake(struct refcnt *r)
{
	if (refcnt_rele(r))
		qwz_wakeup(r);
}

void
refcnt_finalize(struct refcnt *r, const char *wmesg)
{
	refcnt_rele(r);
	while (r->r_refs > 0)
		qwz_tsleep_nsec(r, 0, wmesg, INFSLP);
}

/* ---- process-context timeouts ----------------------------------------- */

struct qwz_proc_timeout {
	struct task	task;
};

static void
qwz_proc_timeout_fire(void *arg)
{
	struct qwz_proc_timeout *pt = (struct qwz_proc_timeout *)arg;

	task_add(systq, &pt->task);
}

void
qwz_timeout_set_proc(CTimeout **to, void (*fn)(void *), void *arg)
{
	/* Lives as long as the driver: one per timeout_set_proc() at attach. */
	struct qwz_proc_timeout *pt = (struct qwz_proc_timeout *)
	    qwz_malloc(sizeof(*pt), M_DEVBUF, M_WAITOK | M_ZERO);

	task_set(&pt->task, fn, arg, "qwz_proc_timeout");
	timeout_set(to, qwz_proc_timeout_fire, pt);
}

void
qwz_timeout_free_proc(CTimeout **to)
{
	struct qwz_proc_timeout *pt;

	if (to == NULL || *to == NULL)
		return;
	pt = (struct qwz_proc_timeout *)(*to)->to_arg;
	task_del(systq, &pt->task);
	timeout_free(to);
	qwz_free(pt, M_DEVBUF, sizeof(*pt));
}

/* ---- PCI ---------------------------------------------------------------- */

pcireg_t
pci_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	return tag->configRead32(reg);
}

void
pci_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg, pcireg_t val)
{
	tag->configWrite32(reg, val);
}

int
pci_get_capability(pci_chipset_tag_t pc, pcitag_t tag, int capid, int *offp, pcireg_t *valp)
{
	UInt8 off = 0;

	if (tag->findPCICapability((UInt8)capid, &off) == 0)
		return 0;
	if (offp)
		*offp = (int)off;
	if (valp)
		*valp = tag->configRead32(off);
	return 1;
}

pcireg_t
pci_mapreg_type(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	return tag->configRead32(reg) & 0x7;	/* I/O-vs-memory and 32/64-bit bits */
}

static IOMemoryMap *qwz_bar_map;

int
pci_mapreg_map(struct pci_attach_args *pa, int reg, pcireg_t type, int flags,
    bus_space_tag_t *tagp, bus_space_handle_t *handlep, bus_addr_t *basep,
    bus_size_t *sizep, bus_size_t maxsize)
{
	IOMemoryMap *map = pa->pa_tag->mapDeviceMemoryWithRegister(reg);

	if (map == NULL)
		return ENOMEM;
	OSSafeReleaseNULL(qwz_bar_map);	/* a retried bring-up maps again */
	qwz_bar_map = map;
	if (tagp)
		*tagp = 0;
	if (handlep)
		*handlep = (bus_space_handle_t)map->getVirtualAddress();
	if (basep)
		*basep = map->getPhysicalAddress();
	if (sizep)
		*sizep = map->getLength();
	QWZLOG("BAR 0x%x: phys 0x%llx, %llu bytes", reg,
	    (unsigned long long)map->getPhysicalAddress(),
	    (unsigned long long)map->getLength());
	return 0;
}

void
qwz_pci_unmap_bar(void)
{
	OSSafeReleaseNULL(qwz_bar_map);
}

int
pci_set_powerstate(pci_chipset_tag_t pc, pcitag_t tag, int state)
{
	UInt8 off = 0;
	uint16_t pmcsr;

	if (tag->findPCICapability(kIOPCIPowerManagementCapability, &off) == 0)
		return EOPNOTSUPP;
	pmcsr = tag->configRead16(off + 4);
	if ((pmcsr & 0x3) != (state & 0x3)) {
		tag->configWrite16(off + 4, (pmcsr & ~0x3) | (state & 0x3));
		IOSleep(10);	/* D3hot -> D0 needs 10 ms */
	}
	return 0;
}

/* Multiple MSI vectors are not set up yet; qwz falls back to one vector. */
int
pci_intr_enable_msivec(struct pci_attach_args *pa, int num_vec)
{
	return EOPNOTSUPP;
}

static int
qwz_find_intr_index(IOPCIDevice *dev, bool msi)
{
	int index, type;

	for (index = 0; dev->getInterruptType(index, &type) == kIOReturnSuccess; index++) {
		if (((type & kIOInterruptTypePCIMessaged) != 0) == msi)
			return index;
	}
	return -1;
}

int
pci_intr_map_msi(struct pci_attach_args *pa, pci_intr_handle_t *ih)
{
	ih->vec = qwz_find_intr_index(pa->pa_tag, true);
	return ih->vec < 0 ? ENXIO : 0;
}

int
pci_intr_map_msivec(struct pci_attach_args *pa, int vec, pci_intr_handle_t *ih)
{
	return vec == 0 ? pci_intr_map_msi(pa, ih) : EOPNOTSUPP;
}

int
pci_intr_map(struct pci_attach_args *pa, pci_intr_handle_t *ih)
{
	ih->vec = qwz_find_intr_index(pa->pa_tag, false);
	return ih->vec < 0 ? ENXIO : 0;
}

const char *
pci_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih)
{
	return "msi";
}

/* An interrupt handler: the filter always claims (MSI is never shared) and
 * the action runs the qwz handler on the workloop, holding the gate. */
class QwzIntr : public OSObject {
	OSDeclareDefaultStructors(QwzIntr)
public:
	int (*fn)(void *);
	void *arg;
	IOFilterInterruptEventSource *src;
	IOWorkLoop *workloop;

	bool
	init() override
	{
		return OSObject::init();
	}
	static bool
	filter(OSObject *owner, IOFilterInterruptEventSource *)
	{
		return true;
	}
	static void
	action(OSObject *owner, IOInterruptEventSource *, int)
	{
		QwzIntr *ih = (QwzIntr *)owner;
		ih->fn(ih->arg);
	}
};
OSDefineMetaClassAndStructors(QwzIntr, OSObject)

void *
qwz_pci_intr_establish(struct pci_attach_args *pa, pci_intr_handle_t ih, int level,
    int (*fn)(void *), void *arg, const char *name)
{
	QwzIntr *h = new QwzIntr;

	if (h == NULL || !h->init()) {
		OSSafeReleaseNULL(h);
		return NULL;
	}
	h->fn = fn;
	h->arg = arg;
	h->workloop = pa->pa_workloop;
	h->src = IOFilterInterruptEventSource::filterInterruptEventSource(h,
	    &QwzIntr::action, &QwzIntr::filter, pa->pa_tag, ih.vec);
	if (h->src == NULL || h->workloop->addEventSource(h->src) != kIOReturnSuccess) {
		QWZLOG("%s: cannot attach interrupt index %d", name, ih.vec);
		OSSafeReleaseNULL(h->src);
		h->release();
		return NULL;
	}
	h->src->enable();
	QWZLOG("%s: interrupt index %d established", name, ih.vec);
	return h;
}

void
pci_intr_disestablish(pci_chipset_tag_t pc, void *cookie)
{
	QwzIntr *h = (QwzIntr *)cookie;

	if (h == NULL)
		return;
	if (h->src) {
		h->src->disable();
		h->workloop->removeEventSource(h->src);
		OSSafeReleaseNULL(h->src);
	}
	h->release();
}

/* ---- bus_dma ------------------------------------------------------------ */

/*
 * Physically contiguous memory below 4 GB. Every qwz allocation and bounce
 * buffer comes from here. Page-granular masks keep alignment >= 4 KB.
 */
static IOBufferMemoryDescriptor *
qwz_alloc_low(size_t size, bus_size_t align, bus_addr_t high, bus_addr_t *pa)
{
	IOBufferMemoryDescriptor *md;
	mach_vm_address_t mask;

	if (align < PAGE_SIZE)
		align = PAGE_SIZE;
	mask = (high & 0xffffffffULL) & ~((mach_vm_address_t)align - 1);
	md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryPhysicallyContiguous, size, mask);
	if (md == NULL)
		return NULL;
	if (md->prepare() != kIOReturnSuccess) {
		md->release();
		return NULL;
	}
	*pa = md->getPhysicalSegment(0, NULL, kIOMemoryMapperNone);
	bzero(md->getBytesNoCopy(), size);
	return md;
}

static void
qwz_free_low(IOBufferMemoryDescriptor *md)
{
	md->complete();
	md->release();
}

/*
 * Bounce buffers: fixed-size slots carved out of 256 KB low chunks, one
 * free list per size class. Maps keep their slot until they are destroyed,
 * so the ring buffers that qwz reloads constantly never touch the allocator.
 */
struct qwz_bounce {
	struct qwz_bounce	*next;
	uint8_t			*kva;
	bus_addr_t		 pa;
	size_t			 size;
	IOBufferMemoryDescriptor *own;	/* set when not from a pool */
};

#define QWZ_POOL_CHUNK	(256 * 1024)
static const size_t qwz_pool_sizes[] = { 2048, 4096, 16384, 65536 };
#define QWZ_NPOOLS	(sizeof(qwz_pool_sizes) / sizeof(qwz_pool_sizes[0]))

struct qwz_pool_chunk {
	struct qwz_pool_chunk	*next;
	IOBufferMemoryDescriptor *md;
};

static IOLock *qwz_dma_lock;
static struct qwz_bounce *qwz_pool_free[QWZ_NPOOLS];
static struct qwz_pool_chunk *qwz_pool_chunks;

static int
qwz_pool_grow(int cls)
{
	struct qwz_pool_chunk *c;
	size_t sz = qwz_pool_sizes[cls], i;
	bus_addr_t pa;
	uint8_t *kva;

	c = (struct qwz_pool_chunk *)qwz_malloc(sizeof(*c), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (c == NULL)
		return ENOMEM;
	c->md = qwz_alloc_low(QWZ_POOL_CHUNK, PAGE_SIZE, 0xffffffffULL, &pa);
	if (c->md == NULL) {
		qwz_free(c, M_DEVBUF, sizeof(*c));
		return ENOMEM;
	}
	kva = (uint8_t *)c->md->getBytesNoCopy();
	for (i = 0; i + sz <= QWZ_POOL_CHUNK; i += sz) {
		struct qwz_bounce *b = (struct qwz_bounce *)
		    qwz_malloc(sizeof(*b), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (b == NULL)
			break;
		b->kva = kva + i;
		b->pa = pa + i;
		b->size = sz;
		b->next = qwz_pool_free[cls];
		qwz_pool_free[cls] = b;
	}
	c->next = qwz_pool_chunks;
	qwz_pool_chunks = c;
	return 0;
}

static struct qwz_bounce *
qwz_bounce_get(size_t size)
{
	struct qwz_bounce *b = NULL;
	unsigned int cls;

	for (cls = 0; cls < QWZ_NPOOLS; cls++)
		if (size <= qwz_pool_sizes[cls])
			break;

	if (cls == QWZ_NPOOLS) {
		/* Larger than any pool: a dedicated allocation. */
		b = (struct qwz_bounce *)qwz_malloc(sizeof(*b), M_DEVBUF, M_NOWAIT | M_ZERO);
		if (b == NULL)
			return NULL;
		b->own = qwz_alloc_low(size, PAGE_SIZE, 0xffffffffULL, &b->pa);
		if (b->own == NULL) {
			qwz_free(b, M_DEVBUF, sizeof(*b));
			return NULL;
		}
		b->kva = (uint8_t *)b->own->getBytesNoCopy();
		b->size = size;
		return b;
	}

	IOLockLock(qwz_dma_lock);
	if (qwz_pool_free[cls] == NULL)
		qwz_pool_grow(cls);
	b = qwz_pool_free[cls];
	if (b != NULL)
		qwz_pool_free[cls] = b->next;
	IOLockUnlock(qwz_dma_lock);
	return b;
}

static void
qwz_bounce_put(struct qwz_bounce *b)
{
	unsigned int cls;

	if (b->own != NULL) {
		qwz_free_low(b->own);
		qwz_free(b, M_DEVBUF, sizeof(*b));
		return;
	}
	for (cls = 0; cls < QWZ_NPOOLS; cls++)
		if (b->size == qwz_pool_sizes[cls])
			break;
	IOLockLock(qwz_dma_lock);
	b->next = qwz_pool_free[cls];
	qwz_pool_free[cls] = b;
	IOLockUnlock(qwz_dma_lock);
}

/* kva -> bus address for memory from bus_dmamem_map(), for bus_dmamap_load(). */
struct qwz_dmamem_ent {
	struct qwz_dmamem_ent	*next;
	uint8_t			*kva;
	size_t			 size;
	bus_addr_t		 pa;
};
static struct qwz_dmamem_ent *qwz_dmamem_list;

void
qwz_compat_init(void)
{
	if (qwz_dma_lock == NULL)
		qwz_dma_lock = IOLockAlloc();
}

void
qwz_compat_fini(void)
{
	unsigned int cls;

	for (cls = 0; cls < QWZ_NPOOLS; cls++) {
		while (qwz_pool_free[cls] != NULL) {
			struct qwz_bounce *b = qwz_pool_free[cls];
			qwz_pool_free[cls] = b->next;
			qwz_free(b, M_DEVBUF, sizeof(*b));
		}
	}
	while (qwz_pool_chunks != NULL) {
		struct qwz_pool_chunk *c = qwz_pool_chunks;
		qwz_pool_chunks = c->next;
		qwz_free_low(c->md);
		qwz_free(c, M_DEVBUF, sizeof(*c));
	}
	qwz_pci_unmap_bar();
	if (qwz_dma_lock != NULL) {
		IOLockFree(qwz_dma_lock);
		qwz_dma_lock = NULL;
	}
}

int
qwz_bus_dmamap_create(bus_dma_tag_t t, bus_size_t size, int nsegs, bus_size_t maxsegsz,
    bus_size_t boundary, int flags, bus_dmamap_t *mapp)
{
	bus_dmamap_t map;

	map = (bus_dmamap_t)qwz_malloc(sizeof(*map), M_DEVBUF, M_ZERO |
	    ((flags & BUS_DMA_NOWAIT) ? M_NOWAIT : M_WAITOK));
	if (map == NULL)
		return ENOMEM;
	map->_size = size;
	*mapp = map;
	return 0;
}

void
qwz_bus_dmamap_destroy(bus_dma_tag_t t, bus_dmamap_t map)
{
	if (map == NULL)
		return;
	if (map->_bounce)
		qwz_bounce_put(map->_bounce);
	qwz_free(map, M_DEVBUF, sizeof(*map));
}

static int
qwz_map_bounce(bus_dmamap_t map, bus_size_t len)
{
	if (len > map->_size)
		return EFBIG;
	if (map->_bounce == NULL && (map->_bounce = qwz_bounce_get(map->_size)) == NULL)
		return ENOMEM;
	map->dm_nsegs = 1;
	map->dm_mapsize = len;
	map->dm_segs[0].ds_addr = map->_bounce->pa;
	map->dm_segs[0].ds_len = len;
	return 0;
}

int
qwz_bus_dmamap_load(bus_dma_tag_t t, bus_dmamap_t map, void *buf, bus_size_t len,
    void *proc, int flags)
{
	struct qwz_dmamem_ent *e;
	uint8_t *kva = (uint8_t *)buf;

	/* Memory from bus_dmamem_map() is already low and contiguous. */
	IOLockLock(qwz_dma_lock);
	for (e = qwz_dmamem_list; e != NULL; e = e->next) {
		if (kva >= e->kva && kva + len <= e->kva + e->size) {
			map->dm_nsegs = 1;
			map->dm_mapsize = len;
			map->dm_segs[0].ds_addr = e->pa + (kva - e->kva);
			map->dm_segs[0].ds_len = len;
			IOLockUnlock(qwz_dma_lock);
			return 0;
		}
	}
	IOLockUnlock(qwz_dma_lock);

	/*
	 * Not from bus_dmamem_map(): bounce it. Nothing is copied unless the
	 * caller syncs (PREWRITE/POSTREAD), so say so: every qwz caller today
	 * passes dmamem, and a new one that does not would fail silently.
	 */
	QWZLOG("bus_dmamap_load: %zu bytes at %p are not dmamem, bouncing (flags 0x%x)",
	    (size_t)len, buf, flags);
	map->_kva = buf;
	return qwz_map_bounce(map, len);
}

int
qwz_bus_dmamap_load_mbuf(bus_dma_tag_t t, bus_dmamap_t map, mbuf_t m, int flags)
{
	size_t len = 0;
	mbuf_t n;
	int err;

	/* OpenBSD sums m_len over the chain; the pkthdr length may be stale. */
	for (n = m; n != NULL; n = mbuf_next(n))
		len += mbuf_len(n);
	err = qwz_map_bounce(map, len);
	if (err)
		return err;
	map->_mbuf = m;
	map->_flags = flags;
	map->_postread = 0;
	/* Transmit paths may not PREWRITE-sync (see the header): copy in now. */
	if (flags & BUS_DMA_WRITE)
		mbuf_copydata(m, 0, map->dm_mapsize, map->_bounce->kva);
	return 0;
}

int
qwz_bus_dmamap_load_raw(bus_dma_tag_t t, bus_dmamap_t map, bus_dma_segment_t *segs,
    int nsegs, bus_size_t size, int flags)
{
	if (nsegs != 1)
		return EINVAL;
	map->dm_nsegs = 1;
	map->dm_mapsize = size;
	map->dm_segs[0].ds_addr = segs[0].ds_addr;
	map->dm_segs[0].ds_len = size;
	return 0;
}

void
qwz_bus_dmamap_unload(bus_dma_tag_t t, bus_dmamap_t map)
{
	/* Receive paths may unload without POSTREAD-syncing: copy out now. */
	if (map->_mbuf != NULL && (map->_flags & BUS_DMA_READ) && !map->_postread)
		qwz_bus_dmamap_sync(t, map, 0, map->dm_mapsize, BUS_DMASYNC_POSTREAD);
	map->_flags = 0;
	map->dm_nsegs = 0;
	map->dm_mapsize = 0;
	map->_mbuf = NULL;
	map->_kva = NULL;
}

/*
 * PCIe on x86 is cache-coherent, so syncing only matters for bounced maps:
 * copy into the bounce buffer before the device reads it, and back out after
 * the device wrote it.
 */
void
qwz_bus_dmamap_sync(bus_dma_tag_t t, bus_dmamap_t map, bus_addr_t off, bus_size_t len, int ops)
{
	uint8_t *bounce;

	__asm__ volatile("mfence" ::: "memory");
	if (map->_bounce == NULL || (map->_mbuf == NULL && map->_kva == NULL))
		return;
	if (off >= map->dm_mapsize)
		return;
	if (off + len > map->dm_mapsize)
		len = map->dm_mapsize - off;
	bounce = map->_bounce->kva + off;

	/*
	 * PREREAD hands the buffer back to the device, so a copy-out done by
	 * an earlier POSTREAD is stale: unload must copy out again. (qwz's CE
	 * rx enqueue POSTREAD-syncs the new buffer before posting it, and the
	 * receive path then unloads without a sync; with the flag left set,
	 * the firmware's HTC ready message was never copied into the mbuf.)
	 */
	if (ops & BUS_DMASYNC_PREREAD)
		map->_postread = 0;
	if (ops & BUS_DMASYNC_PREWRITE) {
		if (map->_mbuf)
			mbuf_copydata(map->_mbuf, off, len, bounce);
		else
			memcpy(bounce, (uint8_t *)map->_kva + off, len);
	}
	if (ops & BUS_DMASYNC_POSTREAD) {
		map->_postread = 1;
		if (map->_mbuf) {
			if (mbuf_next(map->_mbuf) == NULL && off + len <= mbuf_len(map->_mbuf))
				memcpy((uint8_t *)mbuf_data(map->_mbuf) + off, bounce, len);
			else
				mbuf_copyback(map->_mbuf, off, len, bounce, MBUF_DONTWAIT);
#ifdef QWZ_DEBUG
			/* Diagnostic: the mbuf must now match what the device wrote. */
			{
				uint8_t chk[64];
				size_t o;
				for (o = 0; o < len; o += sizeof(chk)) {
					size_t n = len - o < sizeof(chk) ? len - o : sizeof(chk);
					mbuf_copydata(map->_mbuf, off + o, n, chk);
					if (memcmp(chk, bounce + o, n) != 0) {
						QWZLOG("bounce copy-out MISMATCH at %zu of %zu (mbuf len %zu, chain %d)",
						    (size_t)(off + o), (size_t)len, (size_t)mbuf_len(map->_mbuf),
						    mbuf_next(map->_mbuf) != NULL);
						break;
					}
				}
			}
#endif
		} else {
			memcpy((uint8_t *)map->_kva + off, bounce, len);
		}
	}
	__asm__ volatile("mfence" ::: "memory");
}

int
qwz_bus_dmamem_alloc_range(bus_dma_tag_t t, bus_size_t size, bus_size_t align,
    bus_size_t boundary, bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags,
    bus_addr_t low, bus_addr_t high)
{
	bus_addr_t pa;
	IOBufferMemoryDescriptor *md = qwz_alloc_low(size, align, high, &pa);

	if (md == NULL) {
		QWZLOG("cannot allocate %llu contiguous bytes below 4 GB",
		    (unsigned long long)size);
		return ENOMEM;
	}
	segs[0].ds_addr = pa;
	segs[0].ds_len = size;
	segs[0]._ds_md = md;
	*rsegs = 1;
	return 0;
}

int
qwz_bus_dmamem_alloc(bus_dma_tag_t t, bus_size_t size, bus_size_t align, bus_size_t boundary,
    bus_dma_segment_t *segs, int nsegs, int *rsegs, int flags)
{
	/* The device only reaches 32-bit addresses (see the header). */
	return qwz_bus_dmamem_alloc_range(t, size, align, boundary, segs, nsegs, rsegs,
	    flags, 0, 0xffffffffULL);
}

void
qwz_bus_dmamem_free(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs)
{
	if (segs[0]._ds_md != NULL) {
		qwz_free_low(segs[0]._ds_md);
		segs[0]._ds_md = NULL;
	}
}

int
qwz_bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *segs, int nsegs, size_t size,
    caddr_t *kvap, int flags)
{
	struct qwz_dmamem_ent *e;

	if (segs[0]._ds_md == NULL)
		return EINVAL;
	e = (struct qwz_dmamem_ent *)qwz_malloc(sizeof(*e), M_DEVBUF, M_WAITOK | M_ZERO);
	if (e == NULL)
		return ENOMEM;
	e->kva = (uint8_t *)segs[0]._ds_md->getBytesNoCopy();
	e->size = size;
	e->pa = segs[0].ds_addr;
	IOLockLock(qwz_dma_lock);
	e->next = qwz_dmamem_list;
	qwz_dmamem_list = e;
	IOLockUnlock(qwz_dma_lock);
	*kvap = (caddr_t)e->kva;
	return 0;
}

void
qwz_bus_dmamem_unmap(bus_dma_tag_t t, caddr_t kva, size_t size)
{
	struct qwz_dmamem_ent **pp, *e;

	IOLockLock(qwz_dma_lock);
	for (pp = &qwz_dmamem_list; (e = *pp) != NULL; pp = &e->next) {
		if (e->kva == (uint8_t *)kva) {
			*pp = e->next;
			break;
		}
	}
	IOLockUnlock(qwz_dma_lock);
	if (e != NULL)
		qwz_free(e, M_DEVBUF, sizeof(*e));
}

/* ---- mbuf(9) ------------------------------------------------------------ */

static void
qwz_extfree(caddr_t buf, u_int size, caddr_t arg)
{
	IOFree(buf, size);
}

mbuf_t
qwz_mclgetl(mbuf_t m, int how, size_t len)
{
	caddr_t buf;

	if (m == NULL)
		return NULL;
	if (len <= MCLBYTES) {
		errno_t e = mbuf_mclget((mbuf_how_t)how, MBUF_TYPE_DATA, &m);
		if (e != 0 || !(mbuf_flags(m) & MBUF_EXT))
			QWZLOG("mbuf_mclget(%zu) failed: %d", len, e);
		return m;
	}
	/*
	 * Bigger than a 2 KB cluster: attach our own buffer so the data stays
	 * contiguous for mtod(), whatever the size (MHI uses 64 KB).
	 */
	buf = (caddr_t)IOMalloc(len);
	if (buf == NULL)
		return m;
	errno_t e = mbuf_attachcluster((mbuf_how_t)how, MBUF_TYPE_DATA, &m, buf,
	    qwz_extfree, len, NULL);
	if (e != 0) {
		QWZLOG("mbuf_attachcluster(%zu) failed: %d", len, e);
		IOFree(buf, len);
	}
	return m;
}

/* Only the append case (skip == packet length) is used: making room for a MIC. */
mbuf_t
m_makespace(mbuf_t m0, int skip, int hlen, int *off)
{
	mbuf_t m, last = m0, n;

	if (skip != (int)mbuf_pkthdr_len(m0)) {
		QWZLOG("m_makespace: insertion at %d not supported", skip);
		return NULL;
	}
	for (m = m0; m != NULL; m = mbuf_next(m))
		last = m;
	if (mbuf_trailingspace(last) >= (size_t)hlen) {
		*off = (int)mbuf_len(last);
		mbuf_setlen(last, mbuf_len(last) + hlen);
		n = last;
	} else {
		if (mbuf_get(MBUF_DONTWAIT, MBUF_TYPE_DATA, &n) != 0)
			return NULL;
		mbuf_setlen(n, hlen);
		mbuf_setnext(last, n);
		*off = 0;
	}
	mbuf_pkthdr_setlen(m0, mbuf_pkthdr_len(m0) + hlen);
	return n;
}

int
m_copyback(mbuf_t m, int off, int len, const void *src, int how)
{
	return mbuf_copyback(m, off, len, src, (mbuf_how_t)how);
}

/* ---- firmware / autoconf ----------------------------------------------- */

int
loadfirmware(const char *name, u_char **bufp, size_t *lenp)
{
	int i;

	for (i = 0; i < qwz_fw_count; i++) {
		if (strcmp(qwz_fw_table[i].name, name) != 0)
			continue;
		*bufp = (u_char *)qwz_malloc(qwz_fw_table[i].size, M_DEVBUF, M_WAITOK);
		if (*bufp == NULL)
			return ENOMEM;
		memcpy(*bufp, qwz_fw_table[i].data, qwz_fw_table[i].size);
		*lenp = qwz_fw_table[i].size;
		return 0;
	}
	QWZLOG("firmware %s is not built in", name);
	return ENOENT;
}

static struct device *qwz_mountroot_dev;
static void (*qwz_mountroot_fn)(struct device *);

void
config_mountroot(struct device *dev, void (*fn)(struct device *))
{
	qwz_mountroot_dev = dev;
	qwz_mountroot_fn = fn;
}

void
qwz_compat_run_mountroot(void)
{
	void (*fn)(struct device *) = qwz_mountroot_fn;

	qwz_mountroot_fn = NULL;
	if (fn != NULL)
		fn(qwz_mountroot_dev);
}

/* ---- debug -------------------------------------------------------------- */

static IOService *qwz_controller;

void
qwz_compat_set_controller(IOService *controller)
{
	qwz_controller = controller;
}

int
qwz_compat_publish_blob(const char *key, const void *buf, size_t len)
{
	OSData *d;

	if (qwz_controller == NULL)
		return ENXIO;
	d = OSData::withBytes(buf, (unsigned int)len);
	if (d == NULL)
		return ENOMEM;
	qwz_controller->setProperty(key, d);
	d->release();
	QWZLOG("published %zu bytes as IORegistry property %s", len, key);
	return 0;
}

void
qwz_hexdump(const char *label, const void *p, size_t len, size_t base)
{
	const uint8_t *b = (const uint8_t *)p;
	char line[16 * 3 + 1];
	size_t i, j;

	QWZLOG("hexdump %s: %zu bytes", label, len);
	for (i = 0; i < len; i += 16) {
		size_t n = len - i < 16 ? len - i : 16;
		for (j = 0; j < n; j++)
			snprintf(line + j * 3, 4, "%02x ", b[i + j]);
		line[n * 3] = 0;
		QWZLOG("  %04zx: %s", base + i, line);
	}
}

/* ---- bring-up checkpoints ------------------------------------------------ */

int qwz_stage_sysctl = -1;	/* debug.qwz.stage: -1 = use the boot-time limit */

static int
qwz_stage_limit(void)
{
	static int limit = -1;

	IORegistryEntry *opts;
	int v;

	if (qwz_stage_sysctl >= 0)
		return qwz_stage_sysctl;
	if (limit >= 0)
		return limit;
	/*
	 * Full bring-up unless qwz_stage=<n> (boot-args; NVRAM writes do not
	 * persist on some boards) limits it. -qwzoff disables the driver.
	 */
	limit = 1000;
	if (PE_parse_boot_argn("qwz_stage", &v, sizeof(v))) {
		limit = v;
	} else if ((opts = IORegistryEntry::fromPath("/options", gIODTPlane)) != NULL) {
		OSObject *o = opts->copyProperty("qwz-stage");
		OSData *d = OSDynamicCast(OSData, o);
		OSString *str = OSDynamicCast(OSString, o);
		if (d != NULL && d->getLength() > 0 && d->getLength() < 8) {
			char buf[8] = { 0 };
			memcpy(buf, d->getBytesNoCopy(), d->getLength());
			limit = (int)strtoul(buf, NULL, 10);
		} else if (str != NULL) {
			limit = (int)strtoul(str->getCStringNoCopy(), NULL, 10);
		}
		OSSafeReleaseNULL(o);
		opts->release();
	}
	QWZLOG("bring-up stage limit: %d", limit);
	return limit;
}

static bool qwz_stage_limited;

int
qwz_stage_hit_limit(void)
{
	return qwz_stage_limited;
}

void
qwz_stage_reset(void)
{
	qwz_stage_limited = false;
}

/* Set once a stage >= 15 was refused and the bring-up was parked (qwz.c). */
static bool qwz_stage_is_parked;

int
qwz_stage_parked(void)
{
	return qwz_stage_is_parked;
}

void
qwz_stage_set_parked(void)
{
	qwz_stage_is_parked = true;
	qwz_stage_limited = true;
}

int
qwz_stage_ok(int n, const char *what)
{
	char stage[160];

	if (n > qwz_stage_limit()) {
		snprintf(stage, sizeof(stage), "%d %s: not run (qwz_stage limit)", n, what);
		qwz_log_stage(stage);
		qwz_stage_limited = true;
		return 0;
	}
	snprintf(stage, sizeof(stage), "%d %s", n, what);
	qwz_log_stage(stage);
	return 1;
}
