#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BIT(n) (1U << (n))
struct apple_ans_host_state { uint32_t dart_err, dart_addr; };
#include "production.h"
int main(void) {
    struct apple_ans_host_state state;
    for (unsigned valid=0;valid<2;valid++)
        for (unsigned record=0;record<4;record++)
            for (unsigned address=0;address<3;address++) {
                state.dart_err=(valid?BIT(31):0) | (record&1?0x100:0) | (record&2?0x400:0);
                state.dart_addr=address==0?0:address==1?0x3bfff110:0x3bfff070;
                assert(nvme_pci_apple_ans_new_dart_fault(&state)==!!valid);
            }
    puts("PASS DART: 0x100/0x500 and changing addresses are not faults; valid bit required");
}
