// cmio_inuse: is any process using the "Opal C1" virtual camera? (kCMIODevicePropertyDeviceIsRunningSomewhere)
// Shared by opalspy (debounce) and usable standalone for testing:  clang -DCMIO_INUSE_MAIN ... && ./cmio_inuse
#import <CoreMediaIO/CMIOHardware.h>
#import <Foundation/Foundation.h>

// returns 1 = in use, 0 = not in use, -1 = device not found / error
int opal_virtualcam_in_use(void) {
    CMIOObjectPropertyAddress devsAddr = {kCMIOHardwarePropertyDevices, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain};
    UInt32 size = 0;
    if(CMIOObjectGetPropertyDataSize(kCMIOObjectSystemObject, &devsAddr, 0, NULL, &size) != 0 || size == 0) return -1;
    UInt32 n = size / sizeof(CMIOObjectID);
    CMIOObjectID ids[n];
    UInt32 used = 0;
    if(CMIOObjectGetPropertyData(kCMIOObjectSystemObject, &devsAddr, 0, NULL, size, &used, ids) != 0) return -1;
    int result = -1;
    for(UInt32 i = 0; i < used / sizeof(CMIOObjectID); i++) {
        CMIOObjectPropertyAddress nameAddr = {kCMIOObjectPropertyName, kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain};
        CFStringRef name = NULL;
        UInt32 got = 0;
        if(CMIOObjectGetPropertyData(ids[i], &nameAddr, 0, NULL, sizeof(name), &got, &name) != 0 || !name) continue;
        BOOL isOpal = [(__bridge NSString *)name isEqualToString:@"Opal C1"];  // the virtual camera, not "Opal C1 (ctrl)"
        CFRelease(name);
        if(!isOpal) continue;
        CMIOObjectPropertyAddress runAddr = {kCMIODevicePropertyDeviceIsRunningSomewhere, kCMIOObjectPropertyScopeWildcard, kCMIOObjectPropertyElementWildcard};
        UInt32 running = 0;
        if(CMIOObjectGetPropertyData(ids[i], &runAddr, 0, NULL, sizeof(running), &got, &running) == 0) result = running ? 1 : 0;
    }
    return result;
}

#ifdef CMIO_INUSE_MAIN
int main(void) {
    printf("Opal C1 virtual camera in use: %d\n", opal_virtualcam_in_use());
    return 0;
}
#endif
