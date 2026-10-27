#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define GENMASK(h,l) ((~0U>>(31-(h)))&(~0U<<(l)))
#define H9P_CFG_PORT_STRIDE 0x8000
#define PCI_PRIMARY_BUS 0x18
#define dev_info(...) ((void)0)
struct apple_h9p_pcie {void *base_config,*dev;};
static void h9p_rmw(void *p,u32 clear,u32 set) {u32 v;memcpy(&v,p,4);v=(v&~clear)|set;memcpy(p,&v,4);}
#include "production.h"
int main(void) {
 unsigned char cfg[2*H9P_CFG_PORT_STRIDE]={0};
 struct apple_h9p_pcie p={.base_config=cfg};
 for(unsigned port=0;port<2;port++) {
  u32 v=0xab000000;void *at=cfg+port*H9P_CFG_PORT_STRIDE+PCI_PRIMARY_BUS;
  memcpy(at,&v,4);apple_s8003_rescue_bus(&p,port);memcpy(&v,at,4);
  assert(v==(0xab000000|((port+1)<<8)|((port+1)<<16)));
 }
 puts("PASS missing-port rescue reserves bus 1/2 for DT DART0/1 RIDs");
}
