# SPDX-License-Identifier: GPL-2.0
snd-hda-core-y := hda_bus_type.o bus.o device.o sysfs.o \
	regmap.o controller.o stream.o array.o hdmi_chmap.o

snd-hda-core-y += trace.o
CFLAGS_trace.o := -I$(src)

# for sync with i915 gfx driver
snd-hda-core-$(CONFIG_SND_HDA_COMPONENT) += component.o
snd-hda-core-$(CONFIG_SND_HDA_I915) += i915.o

obj-$(CONFIG_SND_HDA_CORE) += snd-hda-core.o

#extended hda
obj-$(CONFIG_SND_HDA_EXT_CORE) += ext/

snd-intel-dspcfg-y := intel-dsp-config.o
snd-intel-dspcfg-$(CONFIG_SND_INTEL_NHLT) += intel-nhlt.o
obj-$(CONFIG_SND_INTEL_DSP_CONFIG) += snd-intel-dspcfg.o

snd-intel-sdw-acpi-y := intel-sdw-acpi.o
obj-$(CONFIG_SND_INTEL_SOUNDWIRE_ACPI) += snd-intel-sdw-acpi.o
