/* Production C/libfdt with inaccessible linear memory before paging_init(). */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libfdt.h>
#include <errno.h>
typedef fdt32_t __be32;
typedef uint64_t phys_addr_t;
typedef unsigned long long u64;
#define __init
#define CONFIG_PSTORE_RAM pstore_enabled
#define IS_BUILTIN(x) (x)
#define SZ_2M (2UL<<20)
#define SZ_1M (1UL<<20)
#define SZ_4K 4096
#define SMP_CACHE_BYTES 64
#define __pa(x) ((uintptr_t)(x))
#define pr_info(...) do {} while (0)
#define pr_err(...) do {} while (0)
#define pr_warn(...) do {} while (0)
#define REGION_PA 0x872000000ULL
#define COPY_PA 0x871ff0000ULL
static void *initial_boot_params;
static phys_addr_t initial_boot_params_pa;
static bool capture, paging_ready, pstore_enabled=true;
static int mode, allocations, regions, freed_copy, freed_region, nomap, maps;
static void *allocated;
static size_t mapped_size;
static bool is_kdump_kernel(void) {return capture;}
static phys_addr_t memblock_phys_alloc(unsigned long size, unsigned long align) {
 assert(!paging_ready);
 if(size==SZ_2M) {
  assert(align==SZ_2M);regions++;
  return mode==1 ? 0 : REGION_PA;
 }
 assert(align==SMP_CACHE_BYTES);allocations++;
 if(mode==2)return 0;
 mapped_size=(size+getpagesize()-1)&~(getpagesize()-1);
 allocated=mmap(NULL,mapped_size,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
 assert(allocated!=MAP_FAILED);
 return COPY_PA;
}
static void *early_memremap(phys_addr_t pa,unsigned long size) {
 assert(!paging_ready && pa==COPY_PA && allocated && !maps);
 if(mode==4)return NULL;
 assert(!mprotect(allocated,mapped_size,PROT_READ|PROT_WRITE));maps++;
 return allocated;
}
static void early_memunmap(void *p,unsigned long size) {
 assert(p==allocated && maps==1 && !paging_ready);
 assert(!mprotect(allocated,mapped_size,PROT_NONE));maps--;
}
static void *phys_to_virt(phys_addr_t pa) {
 assert(paging_ready && pa==COPY_PA && allocated && !maps);
 assert(!mprotect(allocated,mapped_size,PROT_READ|PROT_WRITE));
 return allocated;
}
/* The old production code must fail here, unlike the Round 5 calloc mock. */
static void *memblock_alloc(unsigned long size,unsigned long align) {
 if(!paging_ready) {
  fputs("FAIL: memblock_alloc clears an unmapped linear address before paging_init\n",stderr);
  abort();
 }
 return calloc(1,size);
}
static void memblock_free(void *p,unsigned long size) {free(p);}
static void memblock_phys_free(phys_addr_t base,unsigned long size) {
 assert(!paging_ready);
 if(base==REGION_PA) {assert(size==SZ_2M);freed_region++;return;}
 assert(base==COPY_PA && allocated && !maps);freed_copy++;
 assert(!munmap(allocated,mapped_size));allocated=NULL;
}
static int memblock_mark_nomap(phys_addr_t base,unsigned long size) {
 assert(!paging_ready && base==REGION_PA && size==SZ_2M);
 if(mode==3)return -1;nomap++;return 0;
}
static void memblock_clear_nomap(phys_addr_t base,unsigned long size) {nomap--;}
static bool early_init_dt_verify(void *fdt,phys_addr_t pa) {
 assert(paging_ready && fdt==allocated && pa==COPY_PA && !maps);
 assert(!fdt_check_header(fdt));initial_boot_params=fdt;initial_boot_params_pa=pa;return true;
}
static int total_reserved_mem_cnt, reserved_mem_count;
static bool of_fdt_device_is_available(const void *fdt,int n) {return true;}
static int alloc_reserved_mem_array(void) {assert(total_reserved_mem_cnt==1);return 0;}
static int __reserved_mem_check_root(int n) {return 0;}
static const __be32 *of_flat_dt_get_addr_size_prop(int n,const char *name,int *len) {return fdt_getprop(initial_boot_params,n,name,len);}
static int fdt_validate_reserved_mem_node(int n,void *p) {return -ENODEV;}
static void of_flat_dt_read_addr_size(const __be32 *p,int n,u64 *base,u64 *size) {
 const fdt64_t *v=(const void *)p;*base=fdt64_to_cpu(v[0]);*size=fdt64_to_cpu(v[1]);
}
static void fdt_init_reserved_mem_node(int n,const char *name,phys_addr_t base,phys_addr_t size) {
 assert(reserved_mem_count<total_reserved_mem_cnt);reserved_mem_count++;
}
static void __rmem_check_for_overlap(void) {}
#include "production.h"
static void fresh(void *blob,bool board,bool parent) {
 if(allocated) {assert(!munmap(allocated,mapped_size));allocated=NULL;}
 assert(!fdt_create_empty_tree(blob,8192));
 assert(!fdt_setprop_string(blob,0,"compatible",board?"apple,j71t":"apple,other"));
 if(parent) {
  int n=fdt_add_subnode(blob,0,"reserved-memory");assert(n>=0);
  assert(!fdt_setprop_u32(blob,n,"#address-cells",2));
  assert(!fdt_setprop_u32(blob,n,"#size-cells",2));
  assert(!fdt_setprop(blob,n,"ranges",NULL,0));
 }
 initial_boot_params=blob;initial_boot_params_pa=0x800000000ULL;
 allocations=regions=freed_copy=freed_region=nomap=maps=0;
 apple_j71t_pstore_fdt=0;paging_ready=false;
}
int main(void) {
 _Alignas(8) unsigned char blob[8192];int n,len;const fdt64_t *reg;uint64_t a,z;
 fresh(blob,true,true);apple_j71t_reserve_pstore();
 assert(initial_boot_params==blob && regions==1 && allocations==1 && nomap==1 && maps==0);
 paging_ready=true;apple_j71t_publish_pstore();
 assert(initial_boot_params==allocated && initial_boot_params_pa==COPY_PA);
 n=fdt_path_offset(initial_boot_params,"/reserved-memory/ramoops@872000000");assert(n>=0);
 reg=fdt_getprop(initial_boot_params,n,"reg",&len);assert(len==16);
 assert(fdt64_to_cpu(reg[0])==REGION_PA && fdt64_to_cpu(reg[1])==SZ_2M);
 assert(fdt_getprop(initial_boot_params,n,"no-map",&len) && len==0);
 assert(!fdt_get_mem_rsv(initial_boot_params,0,&a,&z) && a==REGION_PA && z==SZ_2M);
 fdt_scan_reserved_mem_late();assert(reserved_mem_count==1);
 apple_j71t_pstore_fdt=0;paging_ready=false;
 apple_j71t_reserve_pstore();assert(regions==1); // Same entry reuses it.
 capture=true;apple_j71t_reserve_pstore();assert(regions==1); // Phoenix reuses it.
 paging_ready=true;apple_j71t_publish_pstore();assert(initial_boot_params==allocated);
 fresh(blob,true,true);apple_j71t_reserve_pstore();assert(!regions);capture=false;
 fresh(blob,false,true);apple_j71t_reserve_pstore();assert(!regions);
 fresh(blob,true,false);apple_j71t_reserve_pstore();assert(!regions);
 fresh(blob,true,true);pstore_enabled=false;apple_j71t_reserve_pstore();assert(!regions);pstore_enabled=true;
 for(mode=1;mode<=4;mode++) {
  fresh(blob,true,true);apple_j71t_reserve_pstore();assert(initial_boot_params==blob && nomap==0 && maps==0);
  assert(freed_region==(mode==1?0:1));assert(freed_copy==(mode>=3?1:0));
  assert(!apple_j71t_pstore_fdt);
  paging_ready=true;apple_j71t_publish_pstore();assert(initial_boot_params==blob);
 }
 puts("PASS production pstore: unmapped pre-paging memory, temporary map lifetime, post-paging publication, physical FDT address, inheritance, kdump/config/board guards, rollback and late scan");
}
