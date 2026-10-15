/*
 * qwz_compat.h - the OpenBSD kernel API that qwz(4) uses, on macOS.
 *
 * net80211, mbuf lists, tasks and timeouts come from itl80211 (itlwm's port of
 * OpenBSD net80211). Everything else qwz needs is declared here with OpenBSD
 * semantics and implemented in qwz_compat.cpp:
 *
 *  - Concurrency: one IOCommandGate plays the role of OpenBSD's kernel lock.
 *    Interrupt handlers, tasks, timeouts and ioctls all run holding it, and
 *    tsleep_nsec() releases it while asleep (IOCommandGate::commandSleep), the
 *    way OpenBSD's tsleep releases the kernel lock. splnet() is a no-op.
 *
 *  - DMA: the WCN7850 only reaches 32-bit bus addresses. bus_dmamem_alloc()
 *    always allocates below 4 GB (OpenBSD/amd64 does the same through
 *    dma_constraint). mbufs can live anywhere on macOS, so loading an mbuf
 *    into a map goes through a per-map bounce buffer below 4 GB, copied in
 *    bus_dmamap_sync(PREWRITE) and out in bus_dmamap_sync(POSTREAD).
 *    Some qwz paths skip those syncs (harmless on coherent amd64), so a
 *    BUS_DMA_WRITE load also copies in at load time and a BUS_DMA_READ map
 *    that was never POSTREAD-synced copies out when it is unloaded.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef QWZ_COMPAT_H
#define QWZ_COMPAT_H

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOFilterInterruptEventSource.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/OSAtomic.h>
#include <sys/kpi_mbuf.h>
#include <sys/errno.h>

#include <net80211/ieee80211_var.h>
#include <net80211/ieee80211_radiotap.h>
#include <sys/_mbuf.h>
#include <sys/_task.h>
#include <sys/timeout.h>
#include <sys/pcireg.h>
#include <sys/endian.h>

#include "qwz_log.h"

/* qwz's printf output goes into the published log ring as well. */
#define printf		qwz_log

/* ---- names that differ in itl80211 ---------------------------------- */

#define ifnet		_ifnet
#define NBPFILTER	0

/* OpenBSD `struct timeout` is CTimeout * in itl80211; qwzvar.h uses this name. */
typedef CTimeout *qwz_timeout_t;

/* ---- misc kernel ---------------------------------------------------- */

#ifndef INFSLP
#define INFSLP		UINT64_MAX
#endif
#define PCATCH		0x100
#define PWAIT		0
#define IPL_NONE	0
#define IPL_MPSAFE	0
#define IPL_HIGH	12

#ifndef MSEC_TO_NSEC
#define MSEC_TO_NSEC(ms)	((uint64_t)(ms) * 1000000ULL)
#define SEC_TO_NSEC(s)		((uint64_t)(s) * 1000000000ULL)
#define USEC_TO_NSEC(us)	((uint64_t)(us) * 1000ULL)
#endif

#define DELAY(us)	IODelay(us)
#define delay(us)	IODelay(us)
#define KASSERT(e)	do { if (!(e)) panic("qwz: KASSERT %s at %s:%d", #e, __FILE__, __LINE__); } while (0)
#define KERNEL_ASSERT_LOCKED()	do {} while (0)
#define rw_assert_wrlock(rwl)	do {} while (0)

#ifndef howmany
#define howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif
#ifndef roundup
#define roundup(x, y)	((((x) + ((y) - 1)) / (y)) * (y))
#endif
#ifndef powerof2
#define powerof2(x)	((((x) - 1) & (x)) == 0)
#endif
#ifndef setbit
#define setbit(a, i)	(((uint8_t *)(a))[(i) / 8] |= 1 << ((i) % 8))
#define clrbit(a, i)	(((uint8_t *)(a))[(i) / 8] &= ~(1 << ((i) % 8)))
#define isset(a, i)	(((const uint8_t *)(a))[(i) / 8] & (1 << ((i) % 8)))
#define isclr(a, i)	(!isset(a, i))
#endif
#ifndef MIN
#define MIN(a, b)	(((a) < (b)) ? (a) : (b))
#define MAX(a, b)	(((a) > (b)) ? (a) : (b))
#endif
#define letoh16		le16toh
#define letoh64		le64toh

static inline int qwz_fls(unsigned int x) { return x ? 32 - __builtin_clz(x) : 0; }
#define fls(x)		qwz_fls(x)

uint64_t	getnsecuptime(void);
void		explicit_bzero(void *, size_t);

/*
 * malloc(9)/free(9). The returned proxy converts to any object pointer, so
 * the C idiom `struct foo *p = malloc(...)` compiles as C++ unchanged.
 */
#define M_DEVBUF	2
#define M_TEMP		127
#define M_WAITOK	0x0001
#define M_NOWAIT	0x0002
#define M_ZERO		0x0008
#define M_CANFAIL	0x0004

void	*qwz_malloc(size_t, int, int);
void	 qwz_free(void *, int, size_t);
void	*qwz_mallocarray(size_t, size_t, int, int);

struct qwz_voidp {
	void *p;
	template <class T> operator T *() const { return static_cast<T *>(p); }
};
#define malloc(sz, type, flags)		(qwz_voidp{qwz_malloc((sz), (type), (flags))})
#define mallocarray(n, sz, type, flags)	(qwz_voidp{qwz_mallocarray((n), (sz), (type), (flags))})
#define free(p, type, sz)		qwz_free((p), (type), (sz))

/* ---- device ---------------------------------------------------------- */

struct device {
	char	dv_xname[16];
};
#define DEVNAME(sc)	((sc)->sc_dev.dv_xname)

#define DVACT_QUIESCE	2
#define DVACT_SUSPEND	3
#define DVACT_RESUME	4
#define DVACT_WAKEUP	5
#define DVACT_POWERDOWN	6

/* ---- sleep / wakeup / spl (see the concurrency note above) ----------- */

void	qwz_compat_set_gate(IOCommandGate *);
void	qwz_compat_init(void);	/* before attach */
void	qwz_compat_fini(void);	/* after detach */
void	qwz_pci_unmap_bar(void);
/* Renamed so they never resolve to XNU's BSD tsleep/wakeup. */
#define tsleep_nsec	qwz_tsleep_nsec
#define wakeup		qwz_wakeup
#define wakeup_one	qwz_wakeup_one
int	qwz_tsleep_nsec(const volatile void *, int, const char *, uint64_t);
void	qwz_wakeup(const volatile void *);
void	qwz_wakeup_one(const volatile void *);
static inline int splnet_qwz(void) { return 0; }
#define splnet()	splnet_qwz()
#define splx(s)		((void)(s))

/*
 * rwlock(9). The gate already serialises everything, but a holder can sleep
 * (and so drop the gate) inside the locked section, so the lock still has to
 * exclude other threads: it records its owner and waiters tsleep on it.
 */
struct rwlock {
	const char	*rwl_name;
	void		*rwl_owner;
};
#define RW_WRITE	0x01
#define RW_READ		0x02
#define RW_INTR		0x10
#define RW_SLEEPFAIL	0x20
#define RW_NOSLEEP	0x40
void	rw_init(struct rwlock *, const char *);
int	rw_enter(struct rwlock *, int);
void	rw_enter_write(struct rwlock *);
void	rw_exit(struct rwlock *);
void	rw_exit_write(struct rwlock *);

/* refcnt(9) */
struct refcnt {
	volatile SInt32	r_refs;
};
void	refcnt_init(struct refcnt *);
void	refcnt_take(struct refcnt *);
int	refcnt_rele(struct refcnt *);
void	refcnt_rele_wake(struct refcnt *);
void	refcnt_finalize(struct refcnt *, const char *);

/* ---- bus_space ------------------------------------------------------- */

typedef volatile uint8_t	*bus_space_handle_t;
typedef int			 bus_space_tag_t;
typedef uint64_t		 bus_addr_t;
typedef uint64_t		 bus_size_t;

static inline uint32_t
bus_space_read_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o)
{
	return *(volatile uint32_t *)(h + o);
}
static inline void
bus_space_write_4(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, uint32_t v)
{
	*(volatile uint32_t *)(h + o) = v;
}
#define BUS_SPACE_BARRIER_READ	0x01
#define BUS_SPACE_BARRIER_WRITE	0x02
static inline void
bus_space_barrier(bus_space_tag_t t, bus_space_handle_t h, bus_size_t o, bus_size_t l, int f)
{
	__asm__ volatile("mfence" ::: "memory");
}

/* ---- bus_dma (OpenBSD layout; see the DMA note above) ---------------- */

#define BUS_DMA_WAITOK		0x0000
#define BUS_DMA_NOWAIT		0x0001
#define BUS_DMA_ALLOCNOW	0x0002
#define BUS_DMA_COHERENT	0x0004
#define BUS_DMA_STREAMING	0x0008
#define BUS_DMA_ZERO		0x0010
#define BUS_DMA_READ		0x0200
#define BUS_DMA_WRITE		0x0400
#define BUS_DMA_64BIT		0x2000

#define BUS_DMASYNC_PREREAD	0x01
#define BUS_DMASYNC_POSTREAD	0x02
#define BUS_DMASYNC_PREWRITE	0x04
#define BUS_DMASYNC_POSTWRITE	0x08

struct qwz_dma_tag;
typedef struct qwz_dma_tag *bus_dma_tag_t;

typedef struct qwz_bus_dma_segment {
	bus_addr_t	ds_addr;
	bus_size_t	ds_len;
	IOBufferMemoryDescriptor *_ds_md;	/* backing store from bus_dmamem_alloc */
} bus_dma_segment_t;

#define QWZ_DMA_MAXSEGS	1	/* every qwz map is single-segment */

struct qwz_bus_dmamap {
	bus_size_t	 dm_mapsize;
	int		 dm_nsegs;
	bus_dma_segment_t dm_segs[QWZ_DMA_MAXSEGS];
	/* private */
	bus_size_t	 _size;
	struct qwz_bounce *_bounce;		/* below 4 GB, at least _size bytes */
	mbuf_t		 _mbuf;			/* loaded mbuf, if bounced */
	void		*_kva;			/* loaded kva, if bounced */
	int		 _flags;		/* BUS_DMA_READ / BUS_DMA_WRITE from the load */
	int		 _postread;		/* a POSTREAD sync ran since the load */
};
typedef struct qwz_bus_dmamap *bus_dmamap_t;

#define bus_dmamap_create	qwz_bus_dmamap_create
#define bus_dmamap_destroy	qwz_bus_dmamap_destroy
#define bus_dmamap_load		qwz_bus_dmamap_load
#define bus_dmamap_load_mbuf	qwz_bus_dmamap_load_mbuf
#define bus_dmamap_load_raw	qwz_bus_dmamap_load_raw
#define bus_dmamap_unload	qwz_bus_dmamap_unload
#define bus_dmamap_sync		qwz_bus_dmamap_sync
#define bus_dmamem_alloc	qwz_bus_dmamem_alloc
#define bus_dmamem_alloc_range	qwz_bus_dmamem_alloc_range
#define bus_dmamem_free		qwz_bus_dmamem_free
#define bus_dmamem_map		qwz_bus_dmamem_map
#define bus_dmamem_unmap	qwz_bus_dmamem_unmap

int	bus_dmamap_create(bus_dma_tag_t, bus_size_t, int, bus_size_t, bus_size_t, int, bus_dmamap_t *);
void	bus_dmamap_destroy(bus_dma_tag_t, bus_dmamap_t);
int	bus_dmamap_load(bus_dma_tag_t, bus_dmamap_t, void *, bus_size_t, void *, int);
int	bus_dmamap_load_mbuf(bus_dma_tag_t, bus_dmamap_t, mbuf_t, int);
int	bus_dmamap_load_raw(bus_dma_tag_t, bus_dmamap_t, bus_dma_segment_t *, int, bus_size_t, int);
void	bus_dmamap_unload(bus_dma_tag_t, bus_dmamap_t);
void	bus_dmamap_sync(bus_dma_tag_t, bus_dmamap_t, bus_addr_t, bus_size_t, int);
int	bus_dmamem_alloc(bus_dma_tag_t, bus_size_t, bus_size_t, bus_size_t, bus_dma_segment_t *, int, int *, int);
int	bus_dmamem_alloc_range(bus_dma_tag_t, bus_size_t, bus_size_t, bus_size_t, bus_dma_segment_t *, int, int *, int, bus_addr_t, bus_addr_t);
void	bus_dmamem_free(bus_dma_tag_t, bus_dma_segment_t *, int);
int	bus_dmamem_map(bus_dma_tag_t, bus_dma_segment_t *, int, size_t, caddr_t *, int);
void	bus_dmamem_unmap(bus_dma_tag_t, caddr_t, size_t);

/* bus_dmamem_map's kva argument is caddr_t *, qwz passes void ** and caddr_t *. */
static inline int
qwz_bus_dmamem_map(bus_dma_tag_t t, bus_dma_segment_t *s, int n, size_t sz, void **kva, int f)
{
	return qwz_bus_dmamem_map(t, s, n, sz, (caddr_t *)kva, f);
}
static inline void
qwz_bus_dmamem_unmap(bus_dma_tag_t t, void *kva, size_t sz)
{
	qwz_bus_dmamem_unmap(t, (caddr_t)kva, sz);
}

/* ---- PCI ------------------------------------------------------------- */

typedef int		 pci_chipset_tag_t;
typedef IOPCIDevice	*pcitag_t;
typedef uint32_t	 pcireg_t;

struct qwz_pci_ih;
typedef struct {
	int	vec;		/* MSI vector, or -1 for INTx */
} pci_intr_handle_t;

struct pci_attach_args {
	IOPCIDevice	*pa_tag;
	pci_chipset_tag_t pa_pc;
	bus_dma_tag_t	 pa_dmat;
	pcireg_t	 pa_id;
	IOWorkLoop	*pa_workloop;
	OSObject	*pa_owner;	/* owner of the interrupt event sources */
	IOEthernetController *pa_controller;	/* for ieee80211_ifattach */
	int		 pa_msi_nvec;	/* set by pci_intr_enable_msivec */
};

#ifndef PCI_VENDOR
#define PCI_VENDOR(id)		((id) & 0xffff)
#define PCI_PRODUCT(id)		(((id) >> 16) & 0xffff)
#endif
#define PCI_VENDOR_QUALCOMM		0x17cb
#define PCI_PRODUCT_QUALCOMM_WCN7850	0x1107
#ifndef PCI_SUBSYS_ID_REG
#define PCI_SUBSYS_ID_REG	0x2c
#endif
#ifndef PCI_PMCSR_STATE_D0
#define PCI_PMCSR_STATE_D0	0x0000
#endif

pcireg_t pci_conf_read(pci_chipset_tag_t, pcitag_t, int);
void	 pci_conf_write(pci_chipset_tag_t, pcitag_t, int, pcireg_t);
int	 pci_get_capability(pci_chipset_tag_t, pcitag_t, int, int *, pcireg_t *);
pcireg_t pci_mapreg_type(pci_chipset_tag_t, pcitag_t, int);
int	 pci_mapreg_map(struct pci_attach_args *, int, pcireg_t, int, bus_space_tag_t *,
	    bus_space_handle_t *, bus_addr_t *, bus_size_t *, bus_size_t);
int	 pci_set_powerstate(pci_chipset_tag_t, pcitag_t, int);
int	 pci_intr_enable_msivec(struct pci_attach_args *, int);
int	 pci_intr_map(struct pci_attach_args *, pci_intr_handle_t *);
int	 pci_intr_map_msi(struct pci_attach_args *, pci_intr_handle_t *);
int	 pci_intr_map_msivec(struct pci_attach_args *, int, pci_intr_handle_t *);
const char *pci_intr_string(pci_chipset_tag_t, pci_intr_handle_t);
/* pci_intr_establish needs the attach args to reach the IOPCIDevice and workloop. */
void	*qwz_pci_intr_establish(struct pci_attach_args *, pci_intr_handle_t, int,
	    int (*)(void *), void *, const char *);
void	 pci_intr_disestablish(pci_chipset_tag_t, void *);
/* Every qwz call site has the attach args in scope as `pa`. */
#define pci_intr_establish(pc, ih, level, fn, arg, name) \
	qwz_pci_intr_establish(pa, (ih), (level), (fn), (arg), (name))

/* config_mountroot(9): the firmware is in the kext, so the hook runs right
 * after attach, from the HAL, holding the gate. */
void	 config_mountroot(struct device *, void (*)(struct device *));
void	 qwz_compat_run_mountroot(void);

/* ---- network glue ----------------------------------------------------- */

#define IFQ_SET_MAXLEN(ifq, len)	do {} while (0)

#include <net/ethernet.h>
#ifndef ETHER_HDR_LEN
#define ETHER_HDR_LEN	14
#endif

/* itl80211 marks 40 MHz capability with the HT40U/HT40D pair. */
#define IEEE80211_CHAN_40MHZ		IEEE80211_CHAN_HT40
#define IEEE80211_CHAN_40MHZ_ALLOWED(c)	(((c)->ic_flags & IEEE80211_CHAN_HT40) != 0)
#ifndef IEEE80211_MAX_FRAME_HDR_LEN
#define IEEE80211_MAX_FRAME_HDR_LEN	36	/* 4-address QoS data header + HT control */
#endif

/* ---- mbuf(9) on the macOS mbuf KPI ------------------------------------ */

/* OpenBSD `struct mbuf *` is the KPI's opaque mbuf_t (struct __mbuf *). */
#define mbuf		__mbuf

/* Field accessors; scripts/port/mbuf_rewrite.py turned m->m_len etc. into these. */
#define M_PKTLEN(m)	((int)mbuf_pkthdr_len(m))
#define M_LEN(m)	((int)mbuf_len(m))
#define M_FLAGS(m)	(mbuf_flags(m))
#define M_SETLEN(m, l)	do { size_t _l = (l); mbuf_setlen((m), _l); mbuf_pkthdr_setlen((m), _l); } while (0)

#define M_DONTWAIT	MBUF_DONTWAIT
#define M_WAIT		MBUF_WAITOK
#define M_EXT		MBUF_EXT
#define MT_DATA		MBUF_TYPE_DATA
#ifndef MCLBYTES
#define MCLBYTES	2048
#endif

#define m_freem(m)	mbuf_freem(m)
#define m_free(m)	mbuf_free(m)
#define m_adj(m, n)	mbuf_adj((m), (n))

static inline void
m_copydata(mbuf_t m, int off, int len, void *buf)
{
	mbuf_copydata(m, off, len, buf);
}
/*
 * OpenBSD's m_pullup() returns the mbuf as is when its first len bytes are
 * already contiguous. xnu's mbuf_pullup() does not: for a cluster mbuf it
 * copies the head into a new small mbuf and chains the cluster behind it.
 * qwz then walked the payload as one buffer (mtod + packet length) and read
 * past the small mbuf into its neighbour, so every WMI event broke at about
 * byte 300. Only call xnu when the bytes really are split.
 */
static inline mbuf_t
m_pullup(mbuf_t m, int len)
{
	if (m != NULL && (int)mbuf_len(m) >= len)
		return m;
	return mbuf_pullup(&m, len) == 0 ? m : NULL;	/* the chain is freed on failure */
}
static inline mbuf_t
m_gethdr(int how, int type)
{
	mbuf_t m;
	return mbuf_gethdr((mbuf_how_t)how, (mbuf_type_t)type, &m) == 0 ? m : NULL;
}

/*
 * MCLGET/MCLGETL attach a cluster to a fresh packet-header mbuf. On failure
 * the mbuf is left without MBUF_EXT, which qwz checks for. Clusters do not
 * have to be physically contiguous: DMA always goes through bounce buffers.
 */
mbuf_t	qwz_mclgetl(mbuf_t, int, size_t);
#define MCLGETL(m, how, len)	((m) = qwz_mclgetl((m), (how), (len)))
#define MCLGET(m, how)		MCLGETL((m), (how), MCLBYTES)

/* xnu's m_prepend frees the chain when it fails, like OpenBSD's. */
#define M_PREPEND(m, plen, how) do {					\
	if (mbuf_prepend(&(m), (plen), (mbuf_how_t)(how)) != 0)		\
		(m) = NULL;						\
} while (0)

mbuf_t	m_makespace(mbuf_t, int, int, int *);
int	m_copyback(mbuf_t, int, int, const void *, int);

/* ---- task(9) / timeout(9) differences from itl80211 -------------------- */

static inline void
task_set(struct task *t, void (*fn)(void *), void *arg)
{
	task_set(t, fn, arg, "qwz");
}
/* A process-context timeout may sleep, which the workloop thread must not:
 * its callback is handed to systq instead. */
void	qwz_timeout_set_proc(CTimeout **, void (*)(void *), void *);
#define timeout_set_proc(to, fn, arg)	qwz_timeout_set_proc((to), (fn), (arg))
void	qwz_timeout_free_proc(CTimeout **);	/* undo timeout_set_proc at detach */
#define QWZ_HZ				100	/* OpenBSD's tick rate */
#define timeout_add(to, ticks)		timeout_add_msec((to), (ticks) * (1000 / QWZ_HZ))

/* ---- firmware --------------------------------------------------------- */

/* loadfirmware(9): served from the images compiled into the kext. */
int	loadfirmware(const char *, u_char **, size_t *);

/*
 * Bring-up checkpoints. qwz_stage_ok(n, what) logs "stage n: what" and
 * returns 0 when n is past the limit set by the qwz_stage=<n> boot-arg or the
 * qwz-stage NVRAM variable (`sudo nvram qwz-stage=<n>`), so a bring-up that
 * freezes the machine can be stopped just before the step that does it.
 */
int	qwz_stage_ok(int, const char *);
/* Logs len bytes at p, 16 per line, labelled with offsets starting at base. */
void	qwz_hexdump(const char *, const void *, size_t, size_t);
int	qwz_stage_hit_limit(void);	/* a checkpoint was refused by the limit */
void	qwz_stage_reset(void);		/* clear that flag before a new attempt */
/*
 * Stages 15 and up sit inside the firmware init, after the point where the
 * power-down path itself freezes the machine (runs 8-12 of the log). When
 * one of them is refused, the bring-up is parked instead of cleaned up:
 * the chip stays powered, the firmware keeps running, the chip-side
 * interrupts are disabled, and every caller returns QWZ_EPARKED up to the
 * attach without freeing or touching anything. The driver then sits idle;
 * a reboot is needed before the next attempt.
 */
#define QWZ_EPARKED	0x5a5a
int	qwz_stage_park(struct qwz_softc *, int, const char *);	/* qwz.c */
int	qwz_stage_parked(void);
void	qwz_stage_set_parked(void);

/* Debug: attach a copy of `buf` to the controller as an IORegistry property. */
int	qwz_compat_publish_blob(const char *, const void *, size_t);
void	qwz_compat_set_controller(IOService *);

#endif /* QWZ_COMPAT_H */
