/*
 * ios-sep-art: read the 20-byte SEP anti-replay token (ART) through
 * AppleSEPManager's AppleSEPUserClient, GetART selector 0x43.
 *
 * This calls GetART once. It never calls SetART, ClearART,
 * PurgeSystemToken, or any NVRAM writer. The kernel's GetART error path may
 * re-submit a volatile ART page to SEP; it does not write NVRAM.
 *
 * Recovered from the iOS 16.7.10 kernelcache:
 *   - AppleSEPUserClient vtable/dispatch table @ 0xfffffff006eb4fc0
 *   - selector 0x43 -> 0xfffffff0062eac08 (GetART)
 *     structure output size = 20 bytes
 *   - GetART calls 0xfffffff0062efd20, which asks the SEP for a 20-byte
 *     token (tag-1 op 0x03 + five op 0x04 reads, params 0..4).
 *   - entitlement checked in AppleSEPUserClient::initWithTask:
 *       com.apple.private.applesepmanager.allow
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEP_GETART_SELECTOR 0x43
#define SEP_ART_LEN 20

static void die(const char *what, kern_return_t kr) {
    fprintf(stderr, "%s failed: 0x%x (%s)\n", what, kr,
            mach_error_string(kr));
    exit(1);
}

static void print_bool_property(io_registry_entry_t entry, CFStringRef key,
                                const char *label) {
    CFTypeRef value = IORegistryEntryCreateCFProperty(entry, key,
                                                       kCFAllocatorDefault, 0);
    if (!value) {
        printf("property %s: absent\n", label);
        return;
    }
    if (CFGetTypeID(value) == CFBooleanGetTypeID())
        printf("property %s: %s\n", label,
               CFBooleanGetValue((CFBooleanRef)value) ? "true" : "false");
    else
        printf("property %s: present (non-boolean)\n", label);
    CFRelease(value);
}

static io_service_t open_sep_service(void) {
    static const char *names[] = { "AppleSEPManager", NULL };
    for (int i = 0; names[i]; i++) {
        CFMutableDictionaryRef match =
            IOServiceMatching(names[i]);
        if (!match)
            continue;
        io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
                                                       match);
        if (svc) {
            printf("service: %s (io_service_t 0x%x)\n", names[i], svc);
            return svc;
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    umask(0077);
    const char *outpath = "/var/mobile/linux-a9/ios-sep-art/art.bin";
    int probe_only = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--probe"))
            probe_only = 1;
        else
            outpath = argv[i];
    }

    io_registry_entry_t dt = IORegistryEntryFromPath(
        kIOMainPortDefault, "IODeviceTree:/arm-io/sep");
    if (dt) {
        print_bool_property(dt, CFSTR("has-art"), "device-tree.has-art");
        print_bool_property(dt, CFSTR("sepfw-booted"),
                            "device-tree.sepfw-booted");
        print_bool_property(dt, CFSTR("sepfw-loaded"),
                            "device-tree.sepfw-loaded");
        IOObjectRelease(dt);
    } else {
        printf("device-tree SEP node: not found\n");
    }

    io_service_t svc = open_sep_service();
    if (!svc) {
        fprintf(stderr, "no AppleSEPManager\n");
        return 1;
    }

    io_connect_t conn = 0;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    print_bool_property(svc, CFSTR("sep-booted"), "manager.sep-booted");
    print_bool_property(svc, CFSTR("HasXART"), "manager.HasXART");
    print_bool_property(svc, CFSTR("has-art"), "manager.has-art");
    io_registry_entry_t parent = 0;
    if (IORegistryEntryGetParentEntry(svc, kIOServicePlane, &parent) ==
        KERN_SUCCESS) {
        print_bool_property(parent, CFSTR("has-art"), "provider.has-art");
        IOObjectRelease(parent);
    }
    IOObjectRelease(svc);
    if (kr != KERN_SUCCESS)
        die("IOServiceOpen", kr);
    printf("connection: 0x%x\n", conn);
    if (probe_only) {
        printf("probe-only: GetART was not called\n");
        IOServiceClose(conn);
        return 0;
    }

    uint8_t art[64];
    memset(art, 0, sizeof(art));
    size_t artlen = SEP_ART_LEN;

    kr = IOConnectCallStructMethod(conn, SEP_GETART_SELECTOR,
                                   NULL, 0,
                                   art, &artlen);
    if (kr != KERN_SUCCESS) {
        IOServiceClose(conn);
        die("IOConnectCallStructMethod(GetART selector 0x43)", kr);
    }

    if (artlen != SEP_ART_LEN) {
        fprintf(stderr, "unexpected ART length: %zu (expected %d)\n",
                artlen, SEP_ART_LEN);
        IOServiceClose(conn);
        return 1;
    }

    int nonzero = 0;
    for (size_t i = 0; i < artlen; i++)
        nonzero |= art[i];
    if (!nonzero) {
        fprintf(stderr,
                "GetART returned 20 zero bytes: no usable ART; not writing a token\n");
        IOServiceClose(conn);
        return 2;
    }

    uint8_t digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(art, (CC_LONG)artlen, digest);

    FILE *f = fopen(outpath, "wb");
    if (!f) {
        perror(outpath);
        IOServiceClose(conn);
        return 1;
    }
    if (fwrite(art, 1, artlen, f) != artlen) {
        perror("fwrite");
        fclose(f);
        IOServiceClose(conn);
        return 1;
    }
    fclose(f);

    printf("art: len=%zu\nsha256=", artlen);
    for (size_t i = 0; i < sizeof(digest); i++)
        printf("%02x", digest[i]);
    printf("\nwrote %s\n", outpath);

    IOServiceClose(conn);
    return 0;
}
