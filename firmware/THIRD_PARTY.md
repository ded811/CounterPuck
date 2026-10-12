# Third-party components

The source in this folder is MIT-licensed (see the repository's `LICENSE`). A built image
(`zephyr.uf2`) also contains code from the projects below, under their own licences. Their licence
texts ship with the SDK you build with (nRF Connect SDK v2.9.0, under `zephyr/LICENSE`,
`nrf/LICENSE`, `modules/hal/nordic/LICENSE`).

| Component | Licence | Where it comes from |
| --- | --- | --- |
| Zephyr RTOS (kernel, USB device stack, HID and CDC-ACM classes, shell, logging, settings) | Apache-2.0 | the SDK |
| nRF Connect SDK glue and the Enhanced ShockBurst (ESB) radio library | Nordic 5-clause (`LicenseRef-Nordic-5-Clause`): use is limited to Nordic Semiconductor devices, and the licence text must come with binaries | the SDK |
| nrfx peripheral drivers and the nRF MDK | BSD-3-Clause | the SDK |
| picolibc (C library) | BSD-3-Clause | the Zephyr SDK toolchain |
| Board definition `boards/others/promicro_nrf52840` | Apache-2.0 (kept in each file's header); adapted from Zephyr's `promicro_nrf52840` board | Zephyr |

Redistributing a built image means passing on the Nordic 5-clause licence text with it, which each
release does (`LICENSE-Nordic-5-Clause.txt`). That licence also limits the ESB part of a binary to
Nordic chips and forbids reverse engineering that part.

The SuperMini boards use the Adafruit nRF52 bootloader (BSD-3-Clause, not part of this image); it is
already on the board and is not modified or redistributed here.
