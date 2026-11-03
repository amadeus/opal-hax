#!/usr/bin/env python3
"""Patch XLink (luxonis/XLink b7d8e600, the version depthai a3c01e446 uses) so its Windows USB backend can open a
composite Opal C1.

Opal's firmware adds a USB microphone (and the flashed program USB video), which makes the camera a *composite* USB
device. Windows then binds WinUSB only to interface 0 (the camera's MS OS descriptor says WINUSB for it), not to the
whole device. XLink's win_usb.c enumerates whole devices (GUID_DEVINTERFACE_USB_DEVICE) and calls WinUsb_Initialize
on that path, which fails for composite devices -> X_LINK_COMMUNICATION_NOT_OPEN ("Failed to connect to device").
The patch: when that fails, find interface 0's own device path (Windows' generic WinUSB interface class, or the
Myriad interface GUIDs) belonging to the same physical device, and open that instead.

usage: patch-xlink.py <XLink source dir>   (idempotent)
"""
import re, sys

src = sys.argv[1]
p = src + "/src/pc/Win/src/win_usb.c"
s = open(p).read()
already = "opal-win" in s

helper = r'''
// ---- opal-win: open composite devices through their WinUSB interface (see tools/win/xlink/patch-xlink.py) ----
#include <cfgmgr32.h>
#include <ctype.h>
#ifdef _MSC_VER
#pragma comment(lib, "cfgmgr32.lib")
#endif

// interface-0 function of the last composite camera we could not open (and its driver), for the app's USB setup fix
static char opal_unbound_id[512];
static char opal_unbound_service[64];
const char *opal_xlink_unbound_interface(void) { return opal_unbound_id; }
const char *opal_xlink_unbound_service(void) { return opal_unbound_service; }

static void opal_lower(char *s) { for(; *s; s++) *s = (char)tolower((unsigned char)*s); }

static int opal_parse_guid(const char *s, GUID *g) {
    unsigned long d1;
    unsigned int d2, d3, b[8];
    int i;
    if(sscanf(s, "{%8lx-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x}", &d1, &d2, &d3, &b[0], &b[1], &b[2], &b[3], &b[4], &b[5], &b[6], &b[7]) != 11) return 0;
    g->Data1 = d1;
    g->Data2 = (unsigned short)d2;
    g->Data3 = (unsigned short)d3;
    for(i = 0; i < 8; i++) g->Data4[i] = (unsigned char)b[i];
    return 1;
}

// device path of an interface the function `child` (id childId) registered, via the GUIDs in its registry key
// (DeviceInterfaceGUIDs / DeviceInterfaceGUID, what WinUSB registers) or the Myriad GUIDs
static int opal_iface_path(DEVINST child, const char *childId, char *out, size_t outLen) {
    GUID guids[10];
    int n = 0, i;
    HKEY k;
    if(CM_Open_DevNode_Key(child, KEY_READ, 0, RegDisposition_OpenExisting, &k, CM_REGISTRY_HARDWARE) == CR_SUCCESS) {
        char buf[1024];
        DWORD sz = sizeof(buf), type;
        char *p;
        if(RegQueryValueExA(k, "DeviceInterfaceGUIDs", NULL, &type, (BYTE *)buf, &sz) == ERROR_SUCCESS) {
            buf[sizeof(buf) - 2] = buf[sizeof(buf) - 1] = 0;
            for(p = buf; *p && n < 6; p += strlen(p) + 1)
                if(opal_parse_guid(p, &guids[n])) n++;
        }
        sz = sizeof(buf);
        if(RegQueryValueExA(k, "DeviceInterfaceGUID", NULL, &type, (BYTE *)buf, &sz) == ERROR_SUCCESS && n < 7) {
            buf[sizeof(buf) - 1] = 0;
            if(opal_parse_guid(buf, &guids[n])) n++;
        }
        RegCloseKey(k);
    }
    guids[n++] = GUID_DEVINTERFACE_MyriadX;
    guids[n++] = GUID_DEVINTERFACE_Myriad2;
    for(i = 0; i < n; i++) {
        ULONG len = 0;
        if(CM_Get_Device_Interface_List_SizeA(&len, &guids[i], (DEVINSTID_A)childId, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2) continue;
        char *list = (char *)malloc(len);
        if(!list) continue;
        if(CM_Get_Device_Interface_ListA(&guids[i], (DEVINSTID_A)childId, list, len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS && list[0]) {
            strncpy(out, list, outLen - 1);
            out[outLen - 1] = 0;
            free(list);
            return 1;
        }
        free(list);
    }
    return 0;
}

// devPath: \\?\usb#vid_03e7&pid_f63b#<instance>#{guid} -> path of interface 0 (...&MI_00) of that device
static char *opal_find_interface_path(const char *devPath) {
    char p[512], parentId[300], childId[512], path[1024];
    char *v, *inst, *end;
    DEVINST parent, child;
    CONFIGRET cr;
    if(!devPath) return NULL;
    strncpy(p, devPath, sizeof(p) - 1);
    p[sizeof(p) - 1] = 0;
    v = strstr(p, "vid_");
    if(!v) v = strstr(p, "VID_");
    if(!v || strlen(v) < 17) return NULL;
    inst = strchr(v, '#');
    if(!inst) return NULL;
    end = strchr(inst + 1, '#');
    if(!end) return NULL;
    snprintf(parentId, sizeof(parentId), "USB\\%.17s\\%.*s", v, (int)(end - inst - 1), inst + 1);
    if(CM_Locate_DevNodeA(&parent, parentId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) {
        mvLog(MVLOG_WARN, "opal-win: device node %s not found", parentId);
        return NULL;
    }
    for(cr = CM_Get_Child(&child, parent, 0); cr == CR_SUCCESS; cr = CM_Get_Sibling(&child, child, 0)) {
        char lid[512];
        if(CM_Get_Device_IDA(child, childId, sizeof(childId), 0) != CR_SUCCESS) continue;
        strncpy(lid, childId, sizeof(lid) - 1);
        lid[sizeof(lid) - 1] = 0;
        opal_lower(lid);
        if(!strstr(lid, "&mi_00")) continue;
        if(opal_iface_path(child, childId, path, sizeof(path))) {
            opal_unbound_id[0] = 0;
            return _strdup(path);
        }
        {
            char service[64] = "";
            ULONG sz = sizeof(service), status = 0, problem = 0;
            CM_Get_DevNode_Registry_PropertyA(child, CM_DRP_SERVICE, NULL, service, &sz, 0);
            CM_Get_DevNode_Status(&status, &problem, child, 0);
            mvLog(MVLOG_WARN, "opal-win: interface %s has driver '%s' (problem %lu) but no device interface to open", childId, service, problem);
            strncpy(opal_unbound_id, childId, sizeof(opal_unbound_id) - 1);
            strncpy(opal_unbound_service, service, sizeof(opal_unbound_service) - 1);
        }
        return NULL;
    }
    mvLog(MVLOG_WARN, "opal-win: %s has no interface 0 function (not composite?)", parentId);
    return NULL;
}
// ---- end opal-win ----

void * usb_open_device(usb_dev dev, uint8_t *ep, uint8_t intfaceno, char *err_string_buff, size_t err_max_len) {'''

if not already:
    s = s.replace("void * usb_open_device(usb_dev dev, uint8_t *ep, uint8_t intfaceno, char *err_string_buff, size_t err_max_len) {", helper, 1)

old_init = '''    if(!WinUsb_Initialize(devHan, &winUsbHan)) {
        if (err_string_buff != NULL)
            wstrerror(err_string_buff, "WinUsb_Initialize");
        goto exit_err;
    }'''
new_init = '''    if(!WinUsb_Initialize(devHan, &winUsbHan)) {
        // opal-win: composite device -> retry through its WinUSB interface
        char *alt = opal_find_interface_path((const char *)dev);
        BOOL ok = FALSE;
        if(alt) {
            CloseHandle(devHan);
            devHan = CreateFileA(alt, GENERIC_WRITE | GENERIC_READ, FILE_SHARE_WRITE | FILE_SHARE_READ, NULL, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, NULL);
            ok = devHan != INVALID_HANDLE_VALUE && WinUsb_Initialize(devHan, &winUsbHan);
            mvLog(MVLOG_INFO, "opal-win: composite device, opened interface 0 at %s: %s", alt, ok ? "ok" : "failed");
            free(alt);
        } else {
            mvLog(MVLOG_WARN, "opal-win: WinUsb_Initialize failed and no WinUSB interface found for %s", (const char *)dev);
        }
        if(!ok) {
            if (err_string_buff != NULL)
                wstrerror(err_string_buff, "WinUsb_Initialize");
            goto exit_err;
        }
    }'''
if not already:
    assert old_init in s, "WinUsb_Initialize block not found"
    s = s.replace(old_init, new_init, 1)
    open(p, "w").write(s)

# 2) dead links: XLink's event reader retries a failed read forever (no backoff, never exits) when the device vanishes
#    mid-session (seen on Windows: camera dropped during boot -> 372k "Cannot find file descriptor" lines, app stuck).
#    After repeated failures, stop the reader and wake the dispatcher so pending calls fail and depthai can retry.
d = src + "/src/shared/XLinkDispatcher.c"
ds = open(d).read()
if "opal-win" not in ds:
    old = """        if (sc) {
            mvLog(MVLOG_DEBUG,"Failed to receive event (err %d)", sc);
            XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, NULL);
            dispatcherFreeEvents(&curr->lQueue, EVENT_PENDING);
            dispatcherFreeEvents(&curr->lQueue, EVENT_BLOCKED);
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
            continue;
        }"""
    new = """        if (sc) {
            mvLog(MVLOG_DEBUG,"Failed to receive event (err %d)", sc);
            XLINK_RET_ERR_IF(pthread_mutex_lock(&(curr->queueMutex)) != 0, NULL);
            dispatcherFreeEvents(&curr->lQueue, EVENT_PENDING);
            dispatcherFreeEvents(&curr->lQueue, EVENT_BLOCKED);
            XLINK_RET_ERR_IF(pthread_mutex_unlock(&(curr->queueMutex)) != 0, NULL);
            // opal-win: give up on a link that keeps failing (device gone) instead of spinning forever
            if (++opalReadFailures >= 50) {
                mvLog(MVLOG_WARN, "opal-win: link lost (%d read failures), stopping event reader", opalReadFailures);
                curr->resetXLink = 1;
                XLink_sem_post(&curr->notifyDispatcherSem);
                break;
            }
#if (defined(_WIN32) || defined(_WIN64))
            Sleep(2);
#else
            usleep(2000);
#endif
            continue;
        }
        opalReadFailures = 0;"""
    assert old in ds, "eventReader error block not found"
    ds = ds.replace(old, new, 1)
    ds = ds.replace("""    mvLog(MVLOG_INFO,"eventReader thread started");
""", """    mvLog(MVLOG_INFO,"eventReader thread started");
    int opalReadFailures = 0;  // opal-win
""", 1)
    if "#include <windows.h>" not in ds:
        ds = ds.replace("#include", "#if (defined(_WIN32) || defined(_WIN64))\n#include <windows.h>\n#else\n#include <unistd.h>\n#endif\n#include", 1)
    open(d, "w").write(ds)

c = src + "/CMakeLists.txt"
cs = open(c).read()
if "cfgmgr32" not in cs:
    cs = cs.replace("            libsetupapi.a\n", "            libsetupapi.a\n            cfgmgr32\n", 1)
    open(c, "w").write(cs)
print("patched", p)
