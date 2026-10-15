/*
 * ItlQwz - itlwm HAL for the Qualcomm WCN7850, driving OpenBSD's qwz(4).
 *
 * The controller (itlwm.cpp) talks to this class through ItlHalService; this
 * class owns the qwz softc and runs every call into qwz holding the
 * workloop's command gate, which stands in for OpenBSD's kernel lock.
 *
 * Copyright (c) 2026 razvanccr. GPL-2.0-or-later (see LICENSE).
 */
#ifndef ItlQwz_hpp
#define ItlQwz_hpp

#include <HAL/ItlHalService.hpp>
#include <HAL/ItlDriverInfo.hpp>
#include <HAL/ItlDriverController.hpp>

struct qwz_softc;

class ItlQwz : public ItlHalService, ItlDriverInfo, ItlDriverController {
	OSDeclareDefaultStructors(ItlQwz)

public:
	static bool qwz_match(IOPCIDevice *device);

	/* ItlHalService */
	bool attach(IOPCIDevice *device) override;
	void detach(IOPCIDevice *device) override;
	IOReturn enable(IONetworkInterface *netif) override;
	IOReturn disable(IONetworkInterface *netif) override;
	struct ieee80211com *get80211Controller() override;
	ItlDriverInfo *getDriverInfo() override;
	ItlDriverController *getDriverController() override;
	void free() override;

	/* ItlDriverInfo */
	const char *getFirmwareVersion() override;
	int16_t getBSSNoise() override;
	bool is5GBandSupport() override;
	int getTxNSS() override;
	const char *getFirmwareName() override;
	UInt32 supportedFeatures() override;
	const char *getFirmwareCountryCode() override;
	uint32_t getTxQueueSize() override;

	/* ItlDriverController */
	void clearScanningFlags() override;
	IOReturn setMulticastList(IOEthernetAddress *addr, int count) override;
#ifdef AIRPORT_WCL
	IOReturn setScanHomeAwayTime(uint32_t ms) override;
#endif

private:
	static IOReturn attachGated(OSObject *owner, void *arg0, void *, void *, void *);
	static IOReturn detachGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn enableGated(OSObject *owner, void *, void *, void *, void *);
	static IOReturn disableGated(OSObject *owner, void *, void *, void *, void *);

	struct qwz_softc *sc;
	char fwVersion[64];
};

#endif /* ItlQwz_hpp */
