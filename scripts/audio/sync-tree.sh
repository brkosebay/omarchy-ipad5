#!/usr/bin/env bash
# Copy the audio driver sources staged in build/audio/src into kernel tree
# `audio` (/src/wt/audio, branch a9/audio) and make the one-time Kconfig /
# Makefile / dts include edits (idempotent). Does not commit.
#
#   scripts/audio/sync-tree.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"

exec "$ROOT/scripts/build-kernel.sh" --run audio '
set -e
S=/work/build/audio/src
cp $S/cs42l81.c                 sound/soc/codecs/cs42l81.c
cp $S/s8000-mca.c               sound/soc/apple/s8000-mca.c
cp $S/s8003-j71t-audio.dtsi     arch/arm64/boot/dts/apple/s8003-j71t-audio.dtsi
cp $S/cirrus,cs42l81.yaml       Documentation/devicetree/bindings/sound/cirrus,cs42l81.yaml
cp $S/apple,s8000-mca.yaml      Documentation/devicetree/bindings/sound/apple,s8000-mca.yaml
# SmartIO (round 2): Pauli1Go T8010 base, S8000 + multi-endpoint + cyclic
cp $S/apple-sio-core.c          drivers/soc/apple/apple-sio-core.c
cp $S/apple-sio-dma.c           drivers/dma/apple-sio-dma.c
cp $S/sio.h                     include/linux/soc/apple/sio.h
cp $S/apple,sio.yaml            Documentation/devicetree/bindings/dma/apple,sio.yaml

python3 - <<"EOF"
import re
def edit(path, anchor, text, after=True):
    s = open(path).read()
    if text.strip() in s:
        return
    i = s.index(anchor)
    if after:
        i += len(anchor)
    s = s[:i] + text + s[i:]
    open(path, "w").write(s)

# codec Kconfig: keep alphabetical-ish, right before CS42L83
edit("sound/soc/codecs/Kconfig", "config SND_SOC_CS42L83\n", """config SND_SOC_CS42L81
	tristate "Cirrus Logic CS42L81 CODEC (Apple)"
	depends on SPI_MASTER
	select REGMAP
	help
	  Apple-specific Cirrus Logic CS42L81 codec with integrated speaker
	  drivers, controlled over SPI (A9 iPad 5).

""", after=False)
edit("sound/soc/codecs/Makefile", "snd-soc-cs42l83-i2c-y := cs42l83-i2c.o\n",
     "snd-soc-cs42l81-y := cs42l81.o\n", after=False)
edit("sound/soc/codecs/Makefile", "obj-$(CONFIG_SND_SOC_CS42L83)\t+= snd-soc-cs42l83-i2c.o\n",
     "obj-$(CONFIG_SND_SOC_CS42L81)\t+= snd-soc-cs42l81.o\n", after=False)

edit("sound/soc/apple/Kconfig", "\nendmenu", """
config SND_SOC_APPLE_S8000_MCA
	tristate "Apple S8000 (A9) MCA driver"
	depends on ARCH_APPLE || COMPILE_TEST
	depends on COMMON_CLK
	select MFD_SYSCON
	select SND_DMAENGINE_PCM
	select SND_SOC_GENERIC_DMAENGINE_PCM
	help
	  ASoC CPU DAI for the "mca-version 4" serializer of the Apple A9
	  (S8000/S8003), including its MCLK output and i2s-switch route.
""", after=False)
edit("sound/soc/apple/Makefile", "snd-soc-apple-mca-y\t:= mca.o\n",
     "snd-soc-apple-s8000-mca-y\t:= s8000-mca.o\n")
edit("sound/soc/apple/Makefile", "obj-$(CONFIG_SND_SOC_APPLE_MCA)\t+= snd-soc-apple-mca.o\n",
     "obj-$(CONFIG_SND_SOC_APPLE_S8000_MCA)\t+= snd-soc-apple-s8000-mca.o\n")

# SmartIO Kconfig help: S8000 support and generic endpoints
for p, old, new in (
    ("drivers/soc/apple/Kconfig",
     "\ttristate \"Apple T8010 SmartIO coprocessor\"",
     "\ttristate \"Apple S8000/T8010 SmartIO coprocessor\""),
    ("drivers/soc/apple/Kconfig",
     "\t  Say Y here if you have an Apple T8010 SoC.",
     "\t  Say Y here if you have an Apple S8000/S8003 or T8010 SoC."),
    ("drivers/dma/Kconfig",
     "\ttristate \"Apple T8010 SmartIO DMA support\"",
     "\ttristate \"Apple S8000/T8010 SmartIO DMA support\""),
    ("drivers/dma/Kconfig",
     "\t  coprocessor on Apple T8010 SoCs. The first supported client is the\n"
     "\t  SPI3 transmit channel used by the touchscreen controller. Transfers\n"
     "\t  are submitted through the SmartIO firmware endpoint protocol.",
     "\t  coprocessor on Apple S8000/S8003 and T8010 SoCs: one channel per\n"
     "\t  firmware endpoint (SPI, MCA audio, UART), slave scatter-gather and\n"
     "\t  cyclic transfers, submitted through the SmartIO endpoint protocol."),
):
    s = open(p).read()
    if new not in s:
        assert old in s, (p, old)
        open(p, "w").write(s.replace(old, new))

# dts: periph inserts its include right after s800x-ipad5.dtsi and touch
# appends one at the end. Put ours just before the root node, one unchanged
# blank line away from the periph hunk, so all three merge without conflict.
p = "arch/arm64/boot/dts/apple/s8003-j71t.dts"
s = open(p).read()
if "s8003-j71t-audio.dtsi" not in s:
    anchor = "#include \"s800x-ipad5.dtsi\"\n\n/ {"
    assert anchor in s, "j71t dts layout changed"
    s = s.replace(anchor, "#include \"s800x-ipad5.dtsi\"\n\n"
                  "#include \"s8003-j71t-audio.dtsi\"\n/ {")
    open(p, "w").write(s)
EOF
git status --short
'
