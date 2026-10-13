/*
 * The kext's kmod_info record, which Xcode generates for kext targets.
 * The kernel and OpenCore find the kext through _kmod_info; _start/_stop come
 * from MacKernelSDK's libkmod and run the C++ constructors via _realmain.
 * Compiled as C (see build.sh) so the symbols keep their C names.
 */
#include <mach/mach_types.h>

extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.razvanccr.QwzMac, QWZMAC_VERSION, _start, _stop)

__private_extern__ kmod_start_func_t *_realmain = 0;
__private_extern__ kmod_stop_func_t *_antimain = 0;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
