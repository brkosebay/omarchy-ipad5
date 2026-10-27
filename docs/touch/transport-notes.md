# J71t N1 transport and power implementation

> Historical design/test notes. Current support: [status](../release/STATUS.md); examples may need local firmware/captures.


2026-10-22, updated in Round2. No device access was
performed. **Verified** below means verified from the matching iOS kernelcache,
the pulled live iOS registry/ADT, or Linux source/build output; it does not mean
tested in Linux on this iPad.

The orchestrator subsequently verified HBPP/identity and complete PIO
firmware/calibration upload; see the Round2 audits and test logs. Round2
corrects the effective N1 inter-word delay below. Physical clock edges and
runtime reports remain unmeasured by this workstream.

The transport code is in `/src/wt/touch`, branch `a9/touch`:

- `drivers/spi/spi-apple-s8000.c`: byte-wide, synchronous, bounded legacy PIO.
- `drivers/clk/clk-apple-s8000-touch.c`: the A9 TCLK provider.
- `drivers/regulator/apple-d2365-touch.c`: the analog touch supply gate.
- `arch/arm64/boot/dts/apple/s8003-j71t-touch.dtsi`: isolated J71t wiring,
  included only by `s8003-j71t.dts`.

Required options are `CONFIG_SPI_APPLE_S8000=y`,
`CONFIG_COMMON_CLK_APPLE_S8000_TOUCH=y`,
`CONFIG_REGULATOR_APPLE_D2365_TOUCH=y`, `CONFIG_REGULATOR=y`,
`CONFIG_MFD_SIMPLE_MFD_I2C=y`, plus the existing Apple I2C, GPIO, PMGR and
clock infrastructure. The base Hoolock tree already has
`drivers/mfd/simple-mfd-i2c.c` matching `apple,i2c-pmic` with a 16-bit-address,
8-bit-value register map. No peripheral-workstream MFD commit is required for
these providers. The regulator is a DT child of the existing `pmic@3c` and
uses its parent map. It does not touch PMIC NVMEM or change voltage selectors.

## SPI controller and clock

**Verified:** J71t SPI3 is `spi-1,samsung`, `spi-version=1`, physical address
`0x20a08c000`, size `0x4000`, AIC interrupt 191, clock gate 77, clock ID
304 (`0x130`), external CS on AP GPIO102. Existing Hoolock power domain
`ps_spi3` is PMGR offset `0x801d0`, parent `ps_sio_p`.

**Verified:** the pulled live `build/firmware/private/IODeviceTree.txt`
contains 96 little-endian 32-bit `arm-io/clock-frequencies` entries. Entry
48 is **61,538,461 Hz**. The full link from clock ID to that rate is:

1. `AppleS8000IO::start`, `fffffff00642c1f0`, copies the first 58 ADT
   frequencies into a 0x48-byte-per-entry clock table.
2. The four clock-name pointers for entry 48 are at
   `fffffff006ee69c8`; its first pointer is `fffffff0055f54fd`, `"nclk"`.
3. `fffffff00642c874` and `fffffff00642c8ac` subtract **0x100** from the
   clock ID, validate the resulting index, and return the entry frequency;
   the name-based variant also checks the clock-name symbol.
4. `AppleSamsungSPIController::start`, `fffffff005b471f0`, requests
   the provider's `nclk` and fails when it is zero.

The DT's 61,538,461 Hz fixed clock is this unit's observed iOS source rate.
**Unverified in Linux:** that the boot chain leaves the SPI source PLL and
divider at the same rate. It is not a copied A10 value or a measurement of
Linux SCK. The requested 83 ns N1 period maps to `spi-max-frequency=12048192`;
rounding the divider up gives divisor **6**, effective nominal SCK
**10,256,410 Hz**. The original child `reg` tuple specifies 124 ns, then the
N1 inherited setup changes the period to 83 ns (see the runtime RE notes).

**Verified byte/phase setup:** the live touch child `reg` value starts
`00000000 7c000000 00000108 88130000 10270000 ...`. Its provider configuration
bytes +8/+9/+10/+11 are `{0,0,1,8}`. The SPI configuration builder at
`fffffff005b47a44` maps these to CPOL=0, CPHA=0, the byte-order flag set,
and 8-bit words. For SPI version 1 this is base CFG **0x4018**; PIO sets
bit 5. Do not use the A10/J172 mode-3 setup or implicitly swap every pair of
payload bytes. `fffffff005b485d0` and FIFO pump `fffffff005b49450`
load sequential bytes from the client buffer into TXDATA and store RXDATA
into sequential bytes. N1 command fields must be serialized by the N1
driver, while this controller transmits those bytes in order.

**Verified legacy registers:** controller power-on
`fffffff005b4773c` sets the CTRL word to **0x0d**; prepare
`fffffff005b48ddc` writes CLKDIV at +0x30, word delay at +0x38, STATUS at
+0x08, CFG at +0x04, CTRL at +0x00. STATUS W1C/config word is
**0x40000f**. SPI-version-1 TX FIFO level is STATUS[10:6], RX FIFO level
STATUS[15:11], FIFO depth 16; these are independently decoded in
`fffffff005b49adc` / `fffffff005b49b18`. TXDATA is +0x10, RXDATA +0x20,
RX count +0x34 and TX count +0x4c. The newer `drivers/spi/spi-apple.c`
does not implement this layout and is left unchanged.

**Verified initial tuple, corrected effective timing in Round2:** the live child's `reg` tuple supplies
**5000 ns inter-word delay** at byte offset +0x0c. The builder at
`fffffff005b47a44` computes `ceil(nclk * delay_ns / 1e9)`, stores it at
instance+0x184, and prepare at `fffffff005b48ddc` writes that value to
WORD_DELAY(+0x38). For this unit it is **308 input-clock cycles**, about
5.005 microseconds. The Linux controller now converts the transfer's
`word_delay` through `spi_delay_to_ns()` and performs the same rounding;
the SPI core already merges `spi_device.word_delay` into every transfer.
However, selected N1 initialization `fffffff005ea0d98` explicitly calls
`+0x7e8(0)`, whose setter `fffffff005e99ff4` writes setup+0x0c. This
overrides the original gap to **zero**. No provider pop restores it between
boot and runtime. The Round1 instruction to retain a 5000ns N1 minimum was
wrong; Round2 sets `spi_device.word_delay.value=0`. Generic controller
word-delay conversion remains available for other clients. This does not
alter the separate CS delay at setup+0x10. Exact instruction chains are in
[round2-transport-re.md](round2-transport-re.md).

**Verified CS timing:** tuple +0x10 supplies a common 10000 ns CS delay;
the builder rounds it to 10 microseconds and stores it at +0x1a4, +0x1a8,
and +0x1ac. The actual CS method at `fffffff005b499cc` waits
`IODelay(+0x1a8)` **after asserting** CS on a 0-to-1 active-state transition,
and `IODelay(+0x1ac)` **before deasserting** CS on a 1-to-0 transition.
The stub at `fffffff005b4ac58` resolves to `_IODelay`. There is no separate
post-deassert inactive wait in this method; +0x1a4 otherwise contributes
only to a timeout estimate. The exact iOS mapping is therefore setup10us,
hold10us, inactive0. This Hoolock SPI core currently executes GPIO CS setup
and inactive delays in both `spi_toggle_csgpiod()` and outer `spi_set_cs()`;
nominal 10us setup in the API consequently gives at least20us on that path.
The parent workstream owns the N1 device delay settings and the DT CS values.

The new driver uses the SPI core's GPIO chip select, mode flags, message
boundaries, transfer delays and actual-length accounting. It explicitly
enables runtime PM, acquires a runtime reference before the first controller
MMIO, and retains that reference until removal. No controller IRQ is enabled.
Each PIO transfer is capped at 64 KiB and has one absolute one-second
deadline, including progress loops. Invalid FIFO levels fail with `-EIO`.
RX is drained even for TX-only requests, and outstanding data is capped at
one FIFO to avoid losing received bytes. Error exit stops PIO/CTRL and clears
latched status. Scheduling points prevent prolonged CPU monopolization.

**Inferred:** the same PIO mechanism can upload the N1 image without SmartIO.
Apple's SPI controller supports both PIO and DMA; the N1 asset is below the
new transfer cap. There is no evidence that N1 requires DMA as a protocol
feature. However, long-transfer FIFO service gaps and the controller's
firmware-download timing have not been tested. A first identity failure must
be resolved before attempting to port SIO. This round deliberately does not
boot the SIO coprocessor or infer A10 firmware compatibility.

## TCLK and analog power

**Verified:** the A9 `TCLK(8,100,32768)` implementation is separate from the
SPI input clock. `fffffff006435fdc` parses the first two arguments as delays
and converts the requested frequency to an integer divisor using
**floor(24,000,000 / 32,768) = 732**. `fffffff006435f74` passes enable and
the selected delay to `fffffff00643417c`, renamed in shared Ghidra as
`s8000_pmgr_enable_touch_clock`.

That function operates at **PMGR +0x78000**, with divider bits [9:0],
enable bit 19, busy bit 18 and disable bit 31. Enable clears bit31, waits
8 microseconds, writes divisor732 and bit19, then waits for bit18 clear.
Disable clears bit19, waits for bit18 clear, sets bit31 and waits
100 microseconds. Nominal output is about **32,786.9 Hz**, not exactly
32,768 Hz and not the A10's 24 MHz touch clock. The Linux provider preserves
unrelated register bits and replaces both unbounded iOS busy loops with
20 ms deadlines, returning an error and disabling the clock on enable
failure.

**Verified:** `pmuL(0x21f)` names D2365 index31 with enable-control-only
flags. `fffffff006bbe488` parses the function data; runtime handler
`fffffff006bbe388` passes a Boolean enable value in W4, zero voltage in W2,
and zero low-bit options in X3 to `fffffff006bbcdd8`, renamed
`d2365_set_ldo`. The table row at `fffffff005915c84` selects enable register
**0x031c**, mask **bit0**. With voltage zero and options zero, the set-LDO
routine reads that register and writes only the enable bit. Both enabling
and disabling are supported. "Enable-control-only" does not mean disable is
ignored. The Linux regulator exposes exactly these three operations:
enable, disable, and is-enabled; it has no voltage-setting operation.

## GPIO semantics and remaining pinmux check

**Verified:** shared Ghidra `apple_gpio_function_call` at
`fffffff00606dc40` interprets ADT GPIO parameters. A direct output request
uses `requested_value XOR polarity_byte XOR 1`. Thus `function-power_ldo`
GPIO73/0x101 is active high; reset GPIO75/0x10001 is active low; the
standalone `enable_cs` GPIO102/0x101 request raises the external CS before
the SPI core takes over transaction selection.

Display sync GPIO74 has ADT configuration `0x03000101`. Passing function
argument 3 selects its last configuration byte, **mode3**. The GPIO
controller method at `fffffff00606fba4` maps that abstract mode to an RMW
of the pin register with **mask0x270/value0x240**: input enable and peripheral
selector **2**, with bit4 cleared. Linux's `APPLE_PINMUX(74,2)` selects that
route. It is neither literal GPIO-high nor Linux peripheral selector3.
Named `sync-on` / `sync-off` states are provided without a `default` state,
so probe does not automatically enable display sync before the N1 wake step.
The N1 driver selects `sync-on` after the 19 c1 wake transaction and 10 ms
wait; the runtime workstream documents the rest of the exact sequence.

The original Linux pinctrl mux setter uses mask0x260, preserving bit4;
iOS uses mask0x270 for this route. The orchestrator adds an S8000-specific
match-data variant that clears bit4 for peripheral selection and also sets
GPIO output mode on func0: sync-on mask0x270/value0x240 and sync-off
mask0x27e/value0x202. The J71t AP pinctrl compatible is explicitly
`apple,s8000-pinctrl`, `apple,pinctrl`. Other SoCs retain their existing
semantics. These exact pinctrl changes belong to the parent workstream.

**Unverified:** SPI3's AP GPIO99/100/101 pinmux. The neighboring CS is known
to be GPIO102, but this does not prove those three pins or their alternate
function. The DT intentionally retains the bootloader's SPI pinmux rather
than writing an invented pin group. This is the remaining platform check
if SPI transfers time out or read constant data. Inspect the bootloader/
iOS pinmux before adding a group. **Verified:** the runtime workstream
traced ADT interrupt mode0 through iOS to GPIO register mode5, which maps
to Linux `IN_IRQ_DN` / **falling edge**. The child uses falling-edge IRQ
and a separate active-low `attn-gpios` line check. Observed electrical
behavior still needs the orchestrator's device test.

## Build and evidence

The three new driver objects compile cleanly with LLVM against the touch
tree. `make LLVM=1 ARCH=arm64 apple/s8003-j71t.dtb` compiles the enabled
board DT cleanly. The parent workstream owns full Image/DTB staging,
`.config`, commits and final patch export; do not infer hardware success
from these compile checks.

Raw JSON and private auxiliary Ghidra projects are under
`build/touch/transport-re/`. `scripts/touch/transport-recon.py` saves bounded
shared-server queries. `scripts/touch/transport-offline.py` extracts only
selected transport kexts and defines entries proven by direct BL targets or
vtables in private projects; it never attaches to the shared JVM. The
matching kernelcache SHA256 is
`c6b95fcf0efe52ee9258454d01d14adc9b0cb922d94c54a04a4ce11d1ae81e0a`.
No Apple code or calibration bytes are added to tracked documentation.

The next owner-run test should first observe the new platform log
`Samsung SPI v1 PIO, input clock 61538461 Hz`, then the N1 driver's stage
diagnostics. A PIO timeout must return within one second and let probe
unwind its reset, TCLK, LDO and analog supply. Missing firmware must not
invoke the userspace firmware fallback. Follow `test-plan.md` for the full
device run and capture the first failing gate before advancing.
