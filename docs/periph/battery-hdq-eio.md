# Battery gauge: `driver failed to report 'charge_full' property: -5`

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


Kernel branch `a9/battery-fix` (based on `a9/periph`, tree `battery-fix`).
Touches `drivers/w1/masters/w1-uart.c` and
`drivers/power/supply/bq27xxx_battery.c`.

## Symptom

Intermittently during boot (22 lines on one cold boot, some warm boots, none on
others) the console prints, in red:

```
power_supply battery: driver failed to report `charge_full' property: -5
```

Later reads work (`charge_full` 8015000 uAh, capacity, voltage and temp fine).

## Cause

Not bus contention and not baud switching. In HDQ mode `w1-uart` sets 57600 baud
once at open, and the W1 core never calls `reset_bus` for this master (it has a
custom `search`). Every HDQ transaction runs under `bus_mutex`, and sysfs/uevent
reads never touch HDQ: `bq27xxx` runs in `cache_only` mode, so only the
30 s refresh worker (`bq27xxx_battery_refresh_cache`) talks to the gauge.

The path that produced the message:

1. One HDQ transaction is lost. `w1_uart_serdev_receive_buf()` accepts the 8 echo
   symbols and 8 response symbols in order and silently skips an echo symbol
   that does not match. After one corrupted symbol the 16 symbols never arrive,
   so `w1_uart_hdq_xfer()` waits the full 500 ms and returns `-ETIMEDOUT`.
   `w1_uart_hdq_write_byte()` is `void` and only `dev_dbg`s it, so
   `w1_read_block()` returns 0 and `w1_bq27000_read()` returns `-EIO`. There was
   no retry.
2. FCC is a 16-bit register: three transactions (upper, lower, upper), so it is
   three times as exposed as the 8-bit reads.
3. FCC is read after the safety sample (flags, soc, current, voltage, temp), as an
   optional value. Its failure did not invalidate the cache; it was stored as
   `cache.charge_full.error = -EIO` and, because `bq27xxx_cache_value()` also
   dropped the old value, stayed that way until the next refresh 30 s later.
4. `bq27xxx_battery_get_cached_property()` returned the raw `-EIO` for
   `CHARGE_FULL` (only `CHARGE_NOW` was already mapped to `-ENODATA`).
   `power_supply_sysfs` does not suppress `-EIO`, so every uevent walk
   (udev, upower, `power_supply_changed`) during those 30 s logged one line.
   Cold boot has the most readers, hence 22 lines.

## Fix

- `w1-uart`: `w1_uart_hdq_xfer()` retries a lost cycle (`-ETIMEDOUT`) up to 3
  times. Each retry starts with a new break, which resynchronizes the gauge's
  HDQ state. The HDQ response timeout drops from 500 ms to 100 ms (a transaction
  takes a few ms), so retries do not hold `bus_mutex`/`di->lock` for seconds.
  Every lost cycle logs `HDQ command XX timed out (try n/3)` at `dev_dbg`.
- `bq27xxx`: `bq27xxx_cache_value()` keeps the last good sample when a refresh
  fails with a bus error (`-ENODATA` is still stored). If the safety sample
  fails, the existing `flags < 0` gate still hides everything, so a dead gauge is
  not masked. The cached getter maps any remaining cache error of the optional
  properties (`charge_now`, `charge_full`, `cycle_count`, `power_avg`,
  `time_to_empty_now`) to `-ENODATA` (`bq27xxx_cached_optional_value()`), which
  `power_supply_sysfs` logs only at debug level.

Limitation: an optional sample that fails permanently while the safety sample
keeps working stays at its last good value (or `-ENODATA` if it never had one).
That is not expected on a single HDQ link.

## Verification (orchestrator, on the device)

"No message" alone does not tell a masked failure from one that never happened,
so also count retries. Enable the debug lines for the boot with `w1_uart.dyndbg=+p`
on the kernel command line, or after boot:

```
echo 'file w1-uart.c +p' | sudo tee /sys/kernel/debug/dynamic_debug/control
```

Across at least 3 cold and 3 warm (kexec) boots:

```
sudo dmesg | grep -c "charge_full"             # expect 0
sudo dmesg | grep -c "battery cache refresh failed"   # expect 0 (rate-limited warn)
sudo dmesg | grep "HDQ command .* timed out"   # retries that were absorbed (dyndbg boots only)
```

Retries showing up with a `charge_full` count of 0 means the fix works. A
`battery cache refresh failed` line, or retries showing `try 3/3` followed by
failures, means the link is worse than a transient and needs a scope/trace.

Under load, check that reads never fail and the values stay sane:

```
for i in $(seq 1 2000); do cat /sys/class/power_supply/battery/uevent >/dev/null || echo FAIL $i; done
# run next to: stress-ng --cpu 4 --timeout 120s  and  udevadm trigger --subsystem-match=power_supply
sudo dmesg | grep -c charge_full
cat /sys/class/power_supply/battery/{charge_full,capacity,voltage_now,temp}
```

Expected: no `FAIL`, no `charge_full` lines, `charge_full` ~8015000.
