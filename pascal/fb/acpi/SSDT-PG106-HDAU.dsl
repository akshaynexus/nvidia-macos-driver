/*
 * SPDX-License-Identifier: GPL-2.0-only
 * SSDT-PG106-HDAU.dsl — Acer Predator Helios 300 (GP106, BIOS DSDT/SSDT11
 * dumped in research/hardware/helios300/raw/acpi): make the GPU's HD-audio
 * function 01:00.1 visible to macOS from boot, so NMHDA/AppleHDA can bind
 * it for HDMI audio (pascal/fb/README.md "HDMI audio").
 *
 * Why: the GPU hides its HDA function unless config register 0x488 bit 25
 * is set (Linux drivers/pci/quirks.c quirk_nvidia_hda sets it; the owner's
 * Linux dmesg says "01:00.0: Enabling HDA controller", i.e. the firmware
 * had left it clear). SSDT11 names the bit \_SB.PCI0.PEG0.PEGP.MLTF (field
 * at EBAS + 0x488, bit 25) and its _ON / _PS0 set it only when
 * GGIV (0x01080001) reads non-zero (most likely the HDMI-present GPIO), so
 * booting without an HDMI cable leaves the function hidden, and NMHDA,
 * which counts NVIDIA audio functions once when AppleHDAController loads,
 * finds none.
 *
 * What: a device HDAU at PEG0 function 1 (the HDA function's address; it
 * also gives OpenCore DeviceProperties a stable ACPI name) whose _INI sets
 * MLTF on Darwin. PEGP already has an _INI (SSDT11), so the hook cannot
 * live there. PG106FB sets the same bit again at start, power-on and after
 * a rescan (pg106_dhw_hda_function), which covers a later _PS0 clearing it.
 *
 * STATUS: compiles with iasl 0 errors/warnings; loads in acpiexec together
 * with the Helios DSDT + SSDT1..13 (no duplicate or unresolved names), and
 * with the two conditions forced true its _INI sets MLTF to 1 (acpiexec's
 * simulated memory; EBAS reads 0 there). SIMULATION only: NOT booted. Unverified: that macOS runs this _INI before IOPCIFamily
 * enumerates bus 1, and what GPIO 0x01080001 is.
 *
 * OpenCore: ACPI -> Add: SSDT-PG106-HDAU.aml (Enabled). No patches needed.
 */
DefinitionBlock ("", "SSDT", 2, "HLS300", "PG106HDA", 0x00001000)
{
    External (_SB_.PCI0.PEG0, DeviceObj)
    External (_SB_.PCI0.PEG0.PEGP, DeviceObj)
    External (_SB_.PCI0.PEG0.PEGP.EBAS, FieldUnitObj)
    External (_SB_.PCI0.PEG0.PEGP.MLTF, FieldUnitObj)

    Scope (\_SB.PCI0.PEG0)
    {
        Device (HDAU)
        {
            Name (_ADR, One)  // 01:00.1

            Method (_INI, 0, NotSerialized)
            {
                If (_OSI ("Darwin"))
                {
                    If ((\_SB.PCI0.PEG0.PEGP.EBAS != Zero))
                    {
                        \_SB.PCI0.PEG0.PEGP.MLTF = One
                    }
                }
            }
        }
    }
}
