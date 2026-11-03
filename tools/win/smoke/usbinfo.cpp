// usbinfo: dump what an Opal C1 advertises over USB (interfaces, BOS / Microsoft OS descriptors), to see what Windows
// will bind to it. usage: usbinfo [--reset]   (--reset: UVC-reset an app-booted f63b camera first, wait for f63d)
#include <libusb-1.0/libusb.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

static libusb_device_handle* openOpal(libusb_context* ctx, uint16_t* pidOut) {
    for(uint16_t pid : {0xf63d, 0xf63b, 0xf63c, 0x2485}) {
        auto* h = libusb_open_device_with_vid_pid(ctx, 0x03e7, pid);
        if(h) { *pidOut = pid; return h; }
    }
    return nullptr;
}

int main(int argc, char** argv) {
    libusb_context* ctx;
    libusb_init(&ctx);
    uint16_t pid = 0;
    auto* h = openOpal(ctx, &pid);
    if(!h) { printf("no Opal C1 found\n"); return 1; }
    if(argc > 1 && !strcmp(argv[1], "--reset")) {
        int r = libusb_control_transfer(h, 0x00, 0xF5, 0x0DA1, 0, nullptr, 0, 1000);
        printf("reset request on %04x -> %d\n", pid, r);
        libusb_close(h);
        h = nullptr;
        for(int i = 0; i < 40 && !h; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            h = libusb_open_device_with_vid_pid(ctx, 0x03e7, 0xf63d);
        }
        if(!h) { printf("f63d did not appear\n"); return 1; }
        pid = 0xf63d;
    }
    auto* dev = libusb_get_device(h);
    libusb_device_descriptor dd;
    libusb_get_device_descriptor(dev, &dd);
    printf("device 03e7:%04x bcdUSB=%04x bcdDevice=%04x class=%d/%d/%d\n", dd.idProduct, dd.bcdUSB, dd.bcdDevice, dd.bDeviceClass, dd.bDeviceSubClass, dd.bDeviceProtocol);
    libusb_config_descriptor* cfg;
    if(libusb_get_active_config_descriptor(dev, &cfg) == 0) {
        printf("config MaxPower=%d, %d interfaces\n", cfg->MaxPower, cfg->bNumInterfaces);
        for(int i = 0; i < cfg->bNumInterfaces; i++) {
            auto& alt = cfg->interface[i].altsetting[0];
            printf("  if %d: class %02x sub %02x proto %02x, %d alts, %d endpoints\n", alt.bInterfaceNumber, alt.bInterfaceClass, alt.bInterfaceSubClass,
                   alt.bInterfaceProtocol, cfg->interface[i].num_altsetting, alt.bNumEndpoints);
        }
        libusb_free_config_descriptor(cfg);
    }
    libusb_bos_descriptor* bos;
    if(libusb_get_bos_descriptor(h, &bos) == 0) {
        printf("BOS: %d capabilities\n", bos->bNumDeviceCaps);
        for(int i = 0; i < bos->bNumDeviceCaps; i++) {
            auto* c = bos->dev_capability[i];
            printf("  cap type %d len %d", c->bDevCapabilityType, c->bLength);
            if(c->bDevCapabilityType == 5) {  // platform: MS OS 2.0 UUID D8DD60DF-4589-4CC7-9CD2-659D9E648A9F
                printf(" platform uuid:");
                for(int k = 1; k < 17 && k < c->bLength - 3; k++) printf("%02x", c->dev_capability_data[k]);
            }
            printf("\n");
        }
        libusb_free_bos_descriptor(bos);
    } else printf("BOS: none\n");
    unsigned char buf[64];
    int n = libusb_get_string_descriptor(h, 0xEE, 0, buf, sizeof buf);  // MS OS 1.0 "MSFT100"
    printf("MS OS 1.0 string (0xEE): %d bytes", n);
    if(n > 2) { printf(" \""); for(int i = 2; i < n && i < 16; i += 2) putchar(buf[i]); printf("\" vendor code 0x%02x", n > 16 ? buf[16] : 0); }
    printf("\n");
    libusb_close(h);
    libusb_exit(ctx);
}
