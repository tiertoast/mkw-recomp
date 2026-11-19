// Mach CPU and memory counters and optional driver-published IORegistry properties.
#include "platform.h"
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

struct PlatLoad {
    host_cpu_load_info_data_t ticks;
    uint64_t wall, process;
    int any_gpu;
};
static PlatLoad unavailable; // immutable allocation-failure handle
PlatLoad *plat_load_open(const uint8_t *luid) {
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return &unavailable;
    l->any_gpu = !luid; // IORegistry entry IDs are not Vulkan LUIDs.
    return l;
}
void plat_load_close(PlatLoad *l) { if (l != &unavailable) free(l); }

void plat_cpu_load(PlatLoad *l, float *total, float *process, float *temp) {
    *total = *process = *temp = -1;
    if (!l || l == &unavailable) return;
    host_cpu_load_info_data_t cpu;
    mach_msg_type_number_t n = HOST_CPU_LOAD_INFO_COUNT;
    mach_port_t host = mach_host_self();
    kern_return_t kr = host_statistics(host, HOST_CPU_LOAD_INFO, (host_info_t)&cpu, &n);
    mach_port_deallocate(mach_task_self(), host);
    if (kr != KERN_SUCCESS) return;
    uint64_t now = plat_now_ns(), proc = plat_process_cpu_ns();
    if (l->wall && now > l->wall) {
        uint64_t ticks = 0;
        for (int i = 0; i < CPU_STATE_MAX; i++) ticks += (uint32_t)(cpu.cpu_ticks[i] - l->ticks.cpu_ticks[i]);
        uint32_t idle = cpu.cpu_ticks[CPU_STATE_IDLE] - l->ticks.cpu_ticks[CPU_STATE_IDLE];
        if (ticks) *total = 100.0 * (ticks - idle) / ticks;
        if (proc >= l->process) *process = 100.0 * (proc - l->process) / (now - l->wall) / plat_cpu_count();
    }
    l->wall = now;
    l->process = proc;
    l->ticks = cpu;
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    task_vm_info_data_t vm;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    *resident = *private_bytes = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vm, &n) == KERN_SUCCESS) {
        *resident = vm.resident_size;
        *private_bytes = vm.internal; // resident internal (anonymous) memory
    }
}

void plat_mem_info(float *used, float *total, float *process) {
    uint64_t bytes, rss, anon;
    size_t size = sizeof bytes;
    *used = *total = *process = -1;
    plat_process_memory(&rss, &anon);
    if (rss) *process = rss / 1048576.0;
    if (sysctlbyname("hw.memsize", &bytes, &size, NULL, 0)) return;
    *total = bytes / 1048576.0;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    mach_port_t host = mach_host_self();
    kern_return_t kr = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &n);
    mach_port_deallocate(mach_task_self(), host);
    if (kr == KERN_SUCCESS) {
        uint64_t free_bytes = (uint64_t)vm.free_count * vm_page_size;
        if (free_bytes <= bytes) *used = (bytes - free_bytes) / 1048576.0;
    }
}
void plat_heap_trim(void) { malloc_zone_pressure_relief(NULL, 0); }

static int64_t number(CFTypeRef v) {
    int64_t value;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID() &&
        CFNumberGetValue(v, kCFNumberSInt64Type, &value)) return value;
    return -1;
}

void plat_gpu_load(PlatLoad *l, float *load, float *used) {
    *load = *used = -1;
    if (!l || !l->any_gpu) return;
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it)) return;
    io_object_t dev;
    while ((dev = IOIteratorNext(it))) {
        CFTypeRef stats = IORegistryEntryCreateCFProperty(dev, CFSTR("PerformanceStatistics"), NULL, 0);
        if (stats && CFGetTypeID(stats) == CFDictionaryGetTypeID()) {
            int64_t pct = number(CFDictionaryGetValue(stats, CFSTR("Device Utilization %")));
            if (pct >= 0 && pct <= 100 && pct > *load) {
                *load = pct;
                int64_t bytes = number(CFDictionaryGetValue(stats, CFSTR("vramUsedBytes")));
                *used = bytes >= 0 ? bytes / 1048576.0 : -1;
            }
        }
        if (stats) CFRelease(stats);
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
}

static int64_t pci_property(io_object_t dev, CFStringRef key) {
    CFTypeRef v = IORegistryEntryCreateCFProperty(dev, key, NULL, 0);
    if (!v) return -1;
    int64_t value = number(v);
    if (CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength(v) == 4) {
        uint32_t word;
        memcpy(&word, CFDataGetBytePtr(v), sizeof word);
        value = CFSwapInt32LittleToHost(word);
    }
    CFRelease(v);
    return value;
}
uint64_t plat_gpu_vram_mib(uint32_t vendor, uint32_t device, const uint8_t *luid) {
    (void)luid;
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOPCIDevice"), &it)) return 0;
    int64_t result = -1;
    io_object_t dev;
    while ((dev = IOIteratorNext(it))) {
        if (pci_property(dev, CFSTR("vendor-id")) == vendor && pci_property(dev, CFSTR("device-id")) == device) {
            int64_t mb = pci_property(dev, CFSTR("VRAM,totalMB"));
            // Identical PCI IDs with different capacities cannot be disambiguated by LUID.
            if (result < 0) result = mb > 0 ? mb : 0;
            else if (result != mb) result = 0;
        }
        IOObjectRelease(dev);
    }
    IOObjectRelease(it);
    return result > 0 ? (uint64_t)result : 0;
}
