#!/usr/bin/env python3
"""Execute the new gauge status resolver and supply wrapper; no device I/O."""
import importlib.util
from pathlib import Path
import subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('test-charging.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
PRE=r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "capture.h"
enum {POWER_SUPPLY_STATUS_UNKNOWN,POWER_SUPPLY_STATUS_CHARGING,
 POWER_SUPPLY_STATUS_DISCHARGING,POWER_SUPPLY_STATUS_NOT_CHARGING,POWER_SUPPLY_STATUS_FULL};
enum {POWER_SUPPLY_PROP_ONLINE};
struct power_supply{int unused;};
union power_supply_propval{int intval;};
static struct power_supply usb;
static int online,io_error,held,released;
static bool missing;
static struct power_supply *power_supply_get_by_name(const char *name)
{assert(!held && !strcmp(name,"usb"));return missing?NULL:&usb;}
static int power_supply_get_property(struct power_supply *p,int prop,union power_supply_propval *v)
{assert(p==&usb && prop==POWER_SUPPLY_PROP_ONLINE && !held);v->intval=online;return io_error;}
static void power_supply_put(struct power_supply *p){assert(p==&usb);released++;}
'''
TESTS=r'''
int main(void){
 union power_supply_propval v={POWER_SUPPLY_STATUS_FULL};
 assert(!bq27xxx_battery_j71t_status(CAPTURE_UNPLUG_UA,&v));
 assert(v.intval==POWER_SUPPLY_STATUS_DISCHARGING);
 online=1;v.intval=POWER_SUPPLY_STATUS_FULL;
 assert(!bq27xxx_battery_j71t_status(CAPTURE_WALL_UA,&v));
 assert(v.intval==POWER_SUPPLY_STATUS_DISCHARGING);
 for(int i=0;i<2;i++){
   online=0;v.intval=POWER_SUPPLY_STATUS_FULL;
   assert(!bq27xxx_battery_j71t_status(i?1000:0,&v));
   assert(v.intval==POWER_SUPPLY_STATUS_NOT_CHARGING);
 }
 online=1;v.intval=POWER_SUPPLY_STATUS_FULL;
 assert(!bq27xxx_battery_j71t_status(0,&v) && v.intval==POWER_SUPPLY_STATUS_FULL);
 v.intval=POWER_SUPPLY_STATUS_NOT_CHARGING;
 assert(!bq27xxx_battery_j71t_status(100000,&v) && v.intval==POWER_SUPPLY_STATUS_CHARGING);
 io_error=-EIO;v.intval=POWER_SUPPLY_STATUS_FULL;
 assert(bq27xxx_battery_j71t_status(100000,&v)==-EIO);
 io_error=0;missing=true;
 assert(bq27xxx_battery_j71t_status(0,&v)==-ENODEV);
 assert(released==5);
 puts("PASS charge9 gauge: captured Full/negative unplug and wall current -> Discharging, offline zero/stale-positive -> Not charging, online Full/Charging, source errors and reference release");
}
'''
def main():
 src=(ROOT/'build/charge9/bq27xxx_battery.c').read_text()
 body='\n'.join(extract.function_text(src,f) for f in ('bq27xxx_j71t_status','bq27xxx_battery_j71t_status'))
 cached=extract.function_text(src,'bq27xxx_battery_get_cached_property')
 assert cached.index('mutex_unlock(&di->lock)') < cached.index('bq27xxx_battery_j71t_status(')
 assert 'apple,j71t-bq27540' in cached
 out=ROOT/'build/charge9/tests';out.mkdir(parents=True,exist_ok=True)
 c=out/'gauge.c';c.write_text(PRE+body+TESTS)
 subprocess.run(['clang','-std=gnu11','-Wall','-Wextra','-Werror','-O1','-fsanitize=address,undefined',str(c),'-o',str(out/'gauge')],check=True)
 subprocess.run([str(out/'gauge')],check=True)
if __name__=='__main__':main()
