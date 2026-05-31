#!/bin/sh
modprobe snd_hda_intel 2>/dev/null || true
modprobe snd_acp3x_rn 2>/dev/null || true
modprobe snd_rn_pci_acp3x 2>/dev/null || true
