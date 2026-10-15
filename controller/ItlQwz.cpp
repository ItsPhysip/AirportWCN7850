/*
 * ItlQwz - itlwm HAL for the Qualcomm WCN7850, driving OpenBSD's qwz(4).
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#include "ItlQwz.hpp"

/* IOKit and net80211 come first: qwz_compat.h then defines malloc/free/... macros. */
#include "qwz_compat.h"
#include "qwzreg.h"
#include "qwzvar.h"
#undef malloc
#undef free
#undef wakeup

#define QWZLOG(fmt, ...)	qwz_log("qwz: " fmt "\n", ##__VA_ARGS__)

/* if_qwz_pci.c */
struct qwz_softc *qwz_pci_macos_alloc(void);
int	qwz_pci_macos_attach(struct qwz_softc *, struct pci_attach_args *);
void	qwz_pci_macos_detach(struct qwz_softc *);
/* qwz.c */
int	qwz_init(struct _ifnet *);

#define super ItlHalService
OSDefineMetaClassAndStructors(ItlQwz, ItlHalService)

/*
 * The controller calls if_start from the network stack's output thread and
 * if_watchdog from its own timer, neither holding the gate. qwz expects the
 * kernel lock there, so both are wrapped to run gated.
 */
static IOCommandGate *qwz_hal_gate;
static void (*qwz_if_start)(struct _ifnet *);
static void (*qwz_if_watchdog)(struct _ifnet *);

static IOReturn
qwz_start_gated(OSObject *, void *ifp, void *, void *, void *)
{
	qwz_if_start((struct _ifnet *)ifp);
	return kIOReturnSuccess;
}

static void
qwz_if_start_wrapper(struct _ifnet *ifp)
{
	qwz_hal_gate->runAction(qwz_start_gated, ifp);
}

static IOReturn
qwz_watchdog_gated(OSObject *, void *ifp, void *, void *, void *)
{
	qwz_if_watchdog((struct _ifnet *)ifp);
	return kIOReturnSuccess;
}

static void
qwz_if_watchdog_wrapper(struct _ifnet *ifp)
{
	qwz_hal_gate->runAction(qwz_watchdog_gated, ifp);
}

static volatile int qwz_test_thread_ran;

static void
qwz_test_thread(void *, wait_result_t)
{
	qwz_log("qwz: test thread running, blocking for 20 ms\n");
	IOSleep(20);	/* block, so the thread is context-switched like a taskq thread */
	qwz_test_thread_ran = 1;
	qwz_log("qwz: test thread resumed, exiting\n");
	thread_terminate(current_thread());
}

bool
ItlQwz::qwz_match(IOPCIDevice *device)
{
	return device->configRead16(kIOPCIConfigVendorID) == PCI_VENDOR_QUALCOMM &&
	    device->configRead16(kIOPCIConfigDeviceID) == PCI_PRODUCT_QUALCOMM_WCN7850;
}

IOReturn
/*
 * IOCommandGate::runAction() passes the gate's OWNER (the itlwm controller)
 * as the first argument, not the HAL. The HAL is passed explicitly in arg1.
 * Casting owner to ItlQwz wrote sc into the controller object and left the
 * HAL's own sc NULL: the attach "succeeded" and the next line dereferenced
 * NULL, a panic that looks like a hard freeze with the NVIDIA driver.
 */
ItlQwz::attachGated(OSObject *, void *arg0, void *arg1, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg1;
	struct pci_attach_args *pa = (struct pci_attach_args *)arg0;
	int err;

	if (!qwz_stage_ok(4, "inside the qwz attach, holding the gate"))
		return kIOReturnError;

	that->sc = qwz_pci_macos_alloc();
	if (that->sc == NULL)
		return kIOReturnNoMemory;
	err = qwz_pci_macos_attach(that->sc, pa);
	QWZLOG("trace: qwz_pci_macos_attach returned %d", err);
	if (err == QWZ_EPARKED) {
		/* Parked at a stage >= 15: keep sc and the running firmware. */
		QWZLOG("parked: the driver stays idle with the firmware running");
		return kIOReturnError;
	}
	if (err) {
		QWZLOG("attach failed: %d", err);
		qwz_pci_macos_detach(that->sc);
		that->sc = NULL;
		return kIOReturnError;
	}
	return kIOReturnSuccess;
}

bool
ItlQwz::attach(IOPCIDevice *device)
{
	struct pci_attach_args pa;
	struct _ifnet *ifp;
	IOReturn ret;

	QWZLOG("QwzMac %s attaching to %04x:%04x subsystem %04x:%04x", QWZMAC_VERSION,
	    device->configRead16(kIOPCIConfigVendorID), device->configRead16(kIOPCIConfigDeviceID),
	    device->configRead16(kIOPCIConfigSubSystemVendorID),
	    device->configRead16(kIOPCIConfigSubSystemID));

#ifdef QWZ_DEBUG
	/* qwz_debug=<mask of QWZ_D_* from qwzvar.h> sets the trace level. */
	PE_parse_boot_argn("qwz_debug", &qwz_debug, sizeof(qwz_debug));
	QWZLOG("debug build, qwz_debug=0x%x", qwz_debug);
#endif
	if (qwz_stage_parked()) {
		QWZLOG("parked by an earlier attempt; reboot before starting again");
		return false;
	}
	QWZLOG("trace: qwz_compat_init");
	qwz_compat_init();
	qwz_stage_reset();	/* each attempt starts with a clean limit flag */
	QWZLOG("trace: getMainCommandGate");
	qwz_hal_gate = getMainCommandGate();
	qwz_compat_set_gate(qwz_hal_gate);
	QWZLOG("trace: gate %p, set controller", qwz_hal_gate);
	qwz_compat_set_controller(getController());
	/*
	 * Stage 1 only creates a kernel thread that logs and exits, to tell a
	 * broken kernel_thread_start apart from the task-queue code itself.
	 */
	if (!qwz_stage_ok(1, "kernel_thread_start test (a thread that logs and exits)"))
		return false;
	{
		thread_t t;
		if (kernel_thread_start(qwz_test_thread, NULL, &t) == KERN_SUCCESS) {
			thread_deallocate(t);
			IOSleep(200);
			QWZLOG("test thread result: %d (1 = it ran)", qwz_test_thread_ran);
		} else
			QWZLOG("kernel_thread_start failed");
	}
	if (!qwz_stage_ok(2, "driver setup: task queue thread (taskq_init)"))
		return false;
	static bool taskq_ready;
	QWZLOG("trace: taskq_init (already done: %d)", taskq_ready);
	if (!taskq_ready) {	/* a retried bring-up must not start systq twice */
		taskq_init();
		taskq_ready = true;
	}
	QWZLOG("trace: taskq_init returned; pausing 300 ms to see if a delayed effect hits");
	IOSleep(300);
	QWZLOG("trace: pause over; filling attach args (%zu bytes at %p)", sizeof(pa), &pa);

	bzero(&pa, sizeof(pa));
	QWZLOG("trace: bzero done");
	pa.pa_tag = device;
	QWZLOG("trace: pa_tag set (%p); reading vendor id", device);
	{
		UInt16 vid = device->configRead16(kIOPCIConfigVendorID);
		QWZLOG("trace: vendor 0x%04x; reading device id", vid);
		UInt16 did = device->configRead16(kIOPCIConfigDeviceID);
		QWZLOG("trace: device 0x%04x", did);
		pa.pa_id = vid | ((pcireg_t)did << 16);
	}
	QWZLOG("trace: config reads done, id 0x%08x", pa.pa_id);
	pa.pa_workloop = getMainWorkLoop();
	pa.pa_owner = this;
	pa.pa_controller = getController();
	QWZLOG("trace: workloop %p controller %p", pa.pa_workloop, pa.pa_controller);

	/*
	 * Attach runs on the IOService start thread holding the gate, so the
	 * firmware boot can tsleep() while the workloop delivers interrupts.
	 */
	if (!qwz_stage_ok(3, "take the command gate (runAction)"))
		return false;
	ret = qwz_hal_gate->runAction(attachGated, &pa, this);
	QWZLOG("trace: attachGated returned 0x%x (gate released)", ret);
	if (ret != kIOReturnSuccess)
		return false;

	QWZLOG("trace: sc %p", sc);
	ifp = &sc->sc_ic.ic_if;
	QWZLOG("trace: ifp %p if_start %p if_watchdog %p", ifp, ifp->if_start, ifp->if_watchdog);
	qwz_if_start = ifp->if_start;
	qwz_if_watchdog = ifp->if_watchdog;
	ifp->if_start = qwz_if_start_wrapper;
	ifp->if_watchdog = qwz_if_watchdog_wrapper;

	QWZLOG("trace: if_start/if_watchdog wrapped");
	snprintf(fwVersion, sizeof(fwVersion), "%s", sc->qmi_target.fw_build_id);
	QWZLOG("attached: MAC %s, firmware %s", ether_sprintf(sc->sc_ic.ic_myaddr), fwVersion);
	return true;
}

IOReturn
ItlQwz::detachGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */

	if (that->sc != NULL) {
		qwz_pci_macos_detach(that->sc);
		that->sc = NULL;
	}
	return kIOReturnSuccess;
}

void
ItlQwz::detach(IOPCIDevice *device)
{
	if (qwz_hal_gate != NULL)
		qwz_hal_gate->runAction(detachGated, this);
	qwz_compat_fini();
}

void
ItlQwz::free()
{
	super::free();
}

IOReturn
ItlQwz::enableGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	struct _ifnet *ifp = &that->sc->sc_ic.ic_if;
	int err = 0;

	/* What qwz_ioctl(SIOCSIFFLAGS) does when the interface goes up. */
	rw_enter_write(&that->sc->ioctl_rwl);
	ifp->if_flags |= IFF_UP;
	if (!(ifp->if_flags & IFF_RUNNING))
		err = qwz_init(ifp);
	rw_exit(&that->sc->ioctl_rwl);
	if (err)
		QWZLOG("init failed: %d", err);
	return err ? kIOReturnError : kIOReturnSuccess;
}

IOReturn
ItlQwz::enable(IONetworkInterface *netif)
{
	return sc ? qwz_hal_gate->runAction(enableGated, this) : kIOReturnNotReady;
}

IOReturn
ItlQwz::disableGated(OSObject *, void *arg0, void *, void *, void *)
{
	ItlQwz *that = (ItlQwz *)arg0;	/* runAction's owner is the controller */
	struct _ifnet *ifp = &that->sc->sc_ic.ic_if;

	rw_enter_write(&that->sc->ioctl_rwl);
	ifp->if_flags &= ~IFF_UP;
	if (ifp->if_flags & IFF_RUNNING)
		qwz_stop(ifp);
	rw_exit(&that->sc->ioctl_rwl);
	return kIOReturnSuccess;
}

IOReturn
ItlQwz::disable(IONetworkInterface *netif)
{
	return sc ? qwz_hal_gate->runAction(disableGated, this) : kIOReturnNotReady;
}

struct ieee80211com *
ItlQwz::get80211Controller()
{
	return sc ? &sc->sc_ic : NULL;
}

ItlDriverInfo *
ItlQwz::getDriverInfo()
{
	return this;
}

ItlDriverController *
ItlQwz::getDriverController()
{
	return this;
}

const char *
ItlQwz::getFirmwareVersion()
{
	return fwVersion;
}

int16_t
ItlQwz::getBSSNoise()
{
	return -95;	/* qwz does not report a noise floor */
}

bool
ItlQwz::is5GBandSupport()
{
	return true;
}

int
ItlQwz::getTxNSS()
{
	return 2;
}

const char *
ItlQwz::getFirmwareName()
{
	return "qwz-wcn7850-hw2.0";
}

UInt32
ItlQwz::supportedFeatures()
{
	return kIONetworkFeatureMultiPages;
}

const char *
ItlQwz::getFirmwareCountryCode()
{
	return "ZZ";
}

uint32_t
ItlQwz::getTxQueueSize()
{
	return 256;
}

void
ItlQwz::clearScanningFlags()
{
}

IOReturn
ItlQwz::setMulticastList(IOEthernetAddress *addr, int count)
{
	return kIOReturnSuccess;
}
