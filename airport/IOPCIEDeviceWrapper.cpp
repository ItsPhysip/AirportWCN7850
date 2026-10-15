//
//  IOPCIEDeviceWrapper.cpp
//  AirportItlwm-Sonoma
//
//  Created by qcwap on 2023/6/27.
//  Copyright © 2023 钟先耀. All rights reserved.
//

#include "IOPCIEDeviceWrapper.hpp"
#include "qwz_log.h"
#include <kern/thread_call.h>
#include <kern/clock.h>
#include "Apple80211.h"

#include "ItlQwz.hpp"

#define super IOService
OSDefineMetaClassAndStructors(IOPCIEDeviceWrapper, IOService);

#define  PCI_MSI_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSI        0x05    /* Message Signalled Interrupts */
#define  PCI_MSIX_FLAGS        2    /* Message Control */
#define  PCI_CAP_ID_MSIX    0x11    /* MSI-X */
#define  PCI_MSIX_FLAGS_ENABLE    0x8000    /* MSI-X enable */
#define  PCI_MSI_FLAGS_ENABLE    0x0001    /* MSI feature enabled */

static IOPMPowerState powerStateArray[2] =
{
    {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}
};

static void pciMsiSetEnable(IOPCIDevice *device, UInt8 msiCap, int enable)
{
    UInt16 control;
    
    control = device->configRead16(msiCap + PCI_MSI_FLAGS);
    control &= ~PCI_MSI_FLAGS_ENABLE;
    if (enable)
        control |= PCI_MSI_FLAGS_ENABLE;
    device->configWrite16(msiCap + PCI_MSI_FLAGS, control);
}

static void pciMsiXClearAndSet(IOPCIDevice *device, UInt8 msixCap, UInt16 clear, UInt16 set)
{
    UInt16 ctrl;
    
    ctrl = device->configRead16(msixCap + PCI_MSIX_FLAGS);
    ctrl &= ~clear;
    ctrl |= set;
    device->configWrite16(msixCap + PCI_MSIX_FLAGS, ctrl);
}

extern IOWorkLoop *_fWorkloop;

IOWorkLoop *IOPCIEDeviceWrapper::getWorkLoop() const
{
    return _fWorkloop;
}

IOService* IOPCIEDeviceWrapper::
probe(IOService *provider, SInt32 *score)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    bool isMatch = false;
    super::probe(provider, score);
    UInt8 msiCap;
    UInt8 msixCap;
    IOPCIDevice* device = OSDynamicCast(IOPCIDevice, provider);
    if (!device)
        return NULL;
    qwz_log_set_target(device);	/* QwzMacLog/QwzMacStage on the PCI device, as the HeliPort build */
    qwz_log_stage("probe (AirportQwz)");
    {
        int off = 0;
        if (PE_parse_boot_argn("-qwzoff", &off, sizeof(off))) {
            IOLog("qwz: AirportQwz disabled by the -qwzoff boot-arg\n");
            return NULL;
        }
    }
    if (ItlQwz::qwz_match(device)) {
        isMatch = true;
        fHalService = new ItlQwz;
    }
    if (isMatch) {
        XYLog("%s Found\n", __FUNCTION__);
        device->findPCICapability(PCI_CAP_ID_MSIX, &msixCap);
        if (msixCap)
            pciMsiXClearAndSet(device, msixCap, PCI_MSIX_FLAGS_ENABLE, 0);
        device->findPCICapability(PCI_CAP_ID_MSI, &msiCap);
        /* QwzMac: MSI is enabled by IOPCIFamily when the interrupt is registered. */
        if (!msiCap && !msixCap) {
            XYLog("%s No MSI cap\n", __FUNCTION__);
            fHalService->release();
            fHalService = NULL;
            return NULL;
        }
        this->pciNub = device;
        return this;
    }
    return NULL;
}

bool IOPCIEDeviceWrapper::
start(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    _fWorkloop = IO80211WorkQueue::workQueue();
    if (!super::start(provider)) {
        return false;
    }
    IOLog("%s::super start succeed\n", getName());
    UInt8 builtIn = 0;
    setProperty("built-in", OSData::withBytes(&builtIn, sizeof(builtIn)));
    PMinit();
    registerPowerDriver(this, powerStateArray, 2);
    provider->joinPMtree(this);
    logTimer = IOTimerEventSource::timerEventSource(this, &IOPCIEDeviceWrapper::logTimerFired);
    if (logTimer && _fWorkloop->addEventSource(logTimer) == kIOReturnSuccess)
        logTimer->setTimeoutMS(500);
    else
        OSSafeReleaseNULL(logTimer);
    /*
     * QwzMac: qwz_airport_delay=<s> (boot-arg) holds the Wi-Fi layer back
     * for <s> seconds after boot, with every log line also synced to
     * QWZ_LOG_FILE, so a crash in AirportItlwm::start leaves its last step
     * in the file instead of restarting the machine before any display.
     */
    uint32_t delay = 0;
    PE_parse_boot_argn("qwz_airport_delay", &delay, sizeof(delay));
    if (delay > 0) {
        thread_call_t tc = thread_call_allocate(&IOPCIEDeviceWrapper::delayedRegister, this);
        if (tc != NULL) {
            uint64_t deadline;
            qwz_log("qwz: AirportQwz: Wi-Fi layer start delayed by %u s\n", delay);
            retain();
            clock_interval_to_deadline(delay, kSecondScale, &deadline);
            thread_call_enter_delayed(tc, deadline);
            return true;
        }
    }
    registerService();
    return true;
}

void IOPCIEDeviceWrapper::
delayedRegister(thread_call_param_t owner, thread_call_param_t)
{
    IOPCIEDeviceWrapper *self = (IOPCIEDeviceWrapper *)owner;

    qwz_log_file_begin();
    qwz_log("qwz: ==== AirportQwz: registering the PCI wrapper (Wi-Fi layer matches now) ====\n");
    self->registerService();
    self->release();
}

void IOPCIEDeviceWrapper::
publishLog()
{
    qwz_log_publish();	/* QwzMacLog on the PCI device, as in the HeliPort build */
}

void IOPCIEDeviceWrapper::
logTimerFired(OSObject *owner, IOTimerEventSource *sender)
{
    IOPCIEDeviceWrapper *self = OSDynamicCast(IOPCIEDeviceWrapper, owner);
    if (!self)
        return;
    self->publishLog();
    sender->setTimeoutMS(2000);
}

void IOPCIEDeviceWrapper::
stop(IOService *provider)
{
    XYLog("%s\n", __PRETTY_FUNCTION__);
    if (logTimer) {
        logTimer->cancelTimeout();
        _fWorkloop->removeEventSource(logTimer);
        OSSafeReleaseNULL(logTimer);
    }
    PMstop();
    super::stop(provider);
}

IOReturn IOPCIEDeviceWrapper::
setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice)
{
    return IOPMAckImplied;
}
