# Vendored code

Compiled into the plugins as they are, apart from the build flags in
`mpc/build.sh` (no configure scripts run).

| Folder | What | Version | License |
|---|---|---|---|
| `rtl-sdr/` | librtlsdr, the RTL-SDR Blog fork (adds the Blog V4's HF upconverter and detection) | 2.0.3 | GPL-2.0-or-later (`rtl-sdr/COPYING`) |
| `libusb/` | libusb, Linux backend only (`mpc/libusb_config/config.h` stands in for configure) | 1.0.30 | LGPL-2.1-or-later (`libusb/COPYING`) |
| `eurorack/` | Emilie Gillet's eurorack DSP: the reverb and limiter, for Haze (desktop and SDR Haze only) | commit f4cfa76, stmlib e3bd7c9 | MIT (`eurorack/LICENSE`) |

librtlsdr is built with `-DDETACH_KERNEL_DRIVER=1` so the dongle is taken from
the Linux DVB driver if it claimed it.
