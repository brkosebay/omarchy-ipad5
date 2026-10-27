/* Production NVMMU C; fake registers, WC buffers, FIFO and init failures. */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint64_t dma_addr_t;
#define BIT(n) (1UL << (n))
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, v) ((x) = (v))
#define EXPORT_SYMBOL_GPL(x)
#define APPLE_ANS_NVMMU_MAX_REQS 36
#define APPLE_ANS_NVMMU_MAX_PAGES 256
#define APPLE_ANS_NVMMU_PAGE_SIZE 4096
#define H9P_NVMMU_TCB_CTRL 4
#define H9P_NVMMU_TCB_CTRL_INIT 0x10000
#define H9P_NVMMU_TCB_BASE_LO 8
#define H9P_NVMMU_TCB_BASE_HI 12
#define H9P_NVMMU_TCB_TABLE_LO 16
#define H9P_NVMMU_TCB_TABLE_HI 20
#define H9P_NVMMU_SART_CTRL 32
#define H9P_NVMMU_SART_VA_BASE 36
#define H9P_NVMMU_SART_VA_END 40
#define H9P_NVMMU_SART_PA_BASE 44
#define S8003_PORT_NVMMU_INV_FIFO 0x22c
#define S8003_PORT_NVMMU_INV_FIFO_EMPTY 1
#define S8003_NVMMU_INV_FIFO_TIMEOUT_US 20000
#define S8003_NVMMU_INV_DELAY_US 8
#define H9P_NVMMU_TCB_BYTES 128
#define H9P_NVMMU_TCB_DWORDS 32
#define H9P_NVMMU_SGL_WORDS 256
#define H9P_NVMMU_TCB_NPAGES 1
#define H9P_NVMMU_TCB_WRITE 0x200
#define H9P_NVMMU_TCB_READ 0x100
#define H9P_NVMMU_FLATDMA_BASE 0x40000000ULL
#define H9P_NVMMU_FLATDMA_STRIDE 0x800000ULL
#define lower_32_bits(x) ((u32)(x))
#define upper_32_bits(x) ((u32)((u64)(x) >> 32))
#define ilog2(x) (__builtin_ctz(x))
#define spin_lock_irqsave(l, f) ((f) = 0)
#define spin_unlock_irqrestore(l, f) ((void)(f))
#define dev_info(...) ((void)0)
#define dev_err(...) ((void)0)
#define dev_err_ratelimited(...) ((void)0)
struct device { void *bus; struct device *parent; };
struct apple_h9p_nvmmu {
    struct apple_h9p_pcie *pcie;
    char *base;
    bool ready, active, sart, poisoned;
    int inv_lock;
    unsigned inv_fifo;
    unsigned long used[1];
    u64 pa_base, tcb_dma, tcb_table_dma, tcb_sgl_dma;
    u32 va_base, size;
    void *tcb, *tcb_table, *tcb_sgl;
    size_t tcb_size, tcb_table_size, tcb_sgl_size;
};
struct apple_h9p_pcie {
    struct apple_h9p_nvmmu nvmmu;
    struct device *dev;
    unsigned enabled_ports;
    char *base_port[1];
    struct { u32 nvmmu_init_busy; } *hw;
};
static struct apple_h9p_pcie host;
static bool fifo_stuck, init_stuck;
static bool host_present = true;
static int owner_port;
static int barriers, inits, invalidates, sart_off, writes;
static bool test_bit(unsigned b, unsigned long *p) { return !!(*p & BIT(b)); }
static void set_bit(unsigned b, unsigned long *p) { *p |= BIT(b); }
static void clear_bit(unsigned b, unsigned long *p) { *p &= ~BIT(b); }
static void bitmap_zero(unsigned long *p, unsigned n) { *p = 0; }
static void dma_wmb(void) { barriers++; }
static void udelay(unsigned n) { assert(n <= 8); }
static int apple_h9p_pcie_device_port(struct apple_h9p_pcie *p, struct device *d) { return owner_port; }
static struct apple_h9p_pcie *apple_h9p_pcie_lookup(struct device *d) { return &host; }
static struct apple_h9p_pcie *apple_ans_host(struct device *d) { return host_present ? &host : NULL; }
static u32 readl(void *p) { return fifo_stuck ? 0 : 1; }
#define readl_poll_timeout(p, v, cond, delay, timeout) \
    ({ assert((timeout) > 0); (v) = readl(p); (cond) ? 0 : -ETIMEDOUT; })
#define readl_poll_timeout_atomic readl_poll_timeout
static void writel(u32 v, void *p) {
    assert(p == host.nvmmu.base + H9P_NVMMU_TCB_CTRL);
    assert(v < 36);
    assert(!fifo_stuck);
    invalidates++;
}
static void h9p_writel_flush(u32 v, void *p) {
    writes++;
    if (p == host.nvmmu.base + H9P_NVMMU_SART_CTRL) {
        if (!v) sart_off++;
        else assert(inits && barriers);
    }
    if (p == host.nvmmu.base + H9P_NVMMU_TCB_CTRL) {
        assert(v == 0x10000 && sart_off && barriers);
        inits++;
    }
}
static int apple_h9p_wait(void *p, u32 mask, u32 lo, u32 hi, unsigned timeout) {
    assert(mask == BIT(16) && timeout > 0);
    return init_stuck ? -ETIMEDOUT : 0;
}
#include "production.h"
int main(void) {
    char regs[128], port[1024];
    u32 tcb[128 * 32], scratch[1024], sgl[36 * 256];
    struct device parent = {0}, bridge = {.parent = &parent}, ep = {.bus = (void *)1, .parent = &bridge};
    typeof(*host.hw) hw = {.nvmmu_init_busy = BIT(16)};
    host.dev = &parent; host.hw = &hw; host.base_port[0] = port; host.enabled_ports = 1;
    host.nvmmu = (struct apple_h9p_nvmmu){.base = regs, .ready = true, .sart = true,
        .inv_fifo = 1, .tcb = tcb, .tcb_table = scratch, .tcb_sgl = sgl,
        .tcb_size = sizeof(tcb), .tcb_table_size = sizeof(scratch), .tcb_sgl_size = sizeof(sgl),
        .tcb_dma = 0x800100000ULL, .tcb_table_dma = 0x800200000ULL,
        .tcb_sgl_dma = 0x800300000ULL, .va_base = 0xbc000000, .pa_base = 0x87ee00000ULL,
        .size = 0x1200000};
    memset(tcb, 0xff, sizeof(tcb)); memset(sgl, 0xff, sizeof(sgl)); memset(scratch, 0xff, sizeof(scratch));
    host.nvmmu.used[0] = BIT(2); host.nvmmu.poisoned = true;
    assert(!apple_ans_nvmmu_reset(&ep));
    assert(!host.nvmmu.poisoned && !host.nvmmu.used[0] && !tcb[0] && !scratch[0] && !sgl[0]);
    u64 page = 0x812340000ULL; dma_addr_t iova;
    assert(!apple_ans_nvmmu_map(&ep, 2, &page, 1, true, &iova));
    assert(iova == 0x41000000 && tcb[2 * 32] == 0x200 && tcb[2 * 32 + 1] == 1);
    assert(!apple_ans_nvmmu_map(&ep, 2, NULL, 0, false, NULL));
    assert(invalidates == 1 && !test_bit(2, host.nvmmu.used));
    page += 0x1000;
    assert(!apple_ans_nvmmu_map(&ep, 2, &page, 1, false, &iova));
    fifo_stuck = true;
    assert(apple_ans_nvmmu_map(&ep, 2, NULL, 0, false, NULL) == -ETIMEDOUT);
    assert(host.nvmmu.poisoned && test_bit(2, host.nvmmu.used));
    assert(apple_ans_nvmmu_map(&ep, 3, &page, 1, false, &iova) == -EIO);
    int prev = writes;
    assert(apple_ans_nvmmu_reset(&ep) == -ETIMEDOUT && writes == prev);
    fifo_stuck = false; init_stuck = true;
    assert(apple_ans_nvmmu_reset(&ep) == -ETIMEDOUT && host.nvmmu.poisoned);
    init_stuck = false;
    assert(!apple_ans_nvmmu_reset(&ep) && !host.nvmmu.poisoned);
    /* Local rejection happens before touching the TCB or invalidate FIFO. */
    memset(tcb, 0x5a, sizeof(tcb));
    int old_inv=invalidates, old_barriers=barriers;
    host_present=false;
    assert(apple_ans_nvmmu_map(&ep,14,NULL,0,false,NULL)==-ENODEV);
    host_present=true;
    int owners[]={-ENODEV,1,4};
    for(unsigned i=0;i<sizeof(owners)/sizeof(*owners);i++) {
        owner_port=owners[i];
        assert(apple_ans_nvmmu_map(&ep,14,NULL,0,false,NULL)==-ENODEV);
    }
    owner_port=0; host.enabled_ports=0;
    assert(apple_ans_nvmmu_map(&ep,14,NULL,0,false,NULL)==-ENODEV);
    host.enabled_ports=1;
    assert(tcb[14*32]==0x5a5a5a5a && invalidates==old_inv && barriers==old_barriers);
    assert(!host.nvmmu.poisoned);
    assert(apple_ans_nvmmu_map(&ep, 36, &page, 1, false, &iova) == -EINVAL);
    host.nvmmu.ready = false; assert(apple_ans_nvmmu_reset(&ep) == -ENODEV);
    puts("PASS NVMMU: stopped reset, SART off before base/init, WC barrier, map direction, tag reuse, FIFO poison, finite FIFO/init failures, recovery");
}
