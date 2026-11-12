# Custom kernel for Xiaomi Redmi Note 4 MTK (Nikel) by Ruben1863
# Kernel version 3.18.22

> **Status: maybe usable.**
> This is a work-in-progress hardware bring-up kernel. Some peripherals
> work, many are still untested or incomplete. Treat it as experimental.

> **AI contribution notice**
> Parts of this kernel — including the BMI160 accelerometer/gyroscope,
> AH1903 hall sensor, YAS537 magnetometer/compass support, the ACCDET
> headset configuration and several device-tree fixes — were developed
> with the help of an AI coding assistant. Everything was built and
> tested on real hardware before being committed.

> **Tested on one device only**
> This kernel has only been tested on a single Redmi Note 4 (nikel,
> MT6797) unit. Different panel, touch, sensor or audio variants may
> behave differently. Keep a bootable backup before flashing, and do not
> assume it is safe for daily use on another device.

=========================================================================
* Working:
	* LCM (For now just nt35532_fhd_boe_vdo_lcm)
	* Touch (FT5346)
	* Boot
	* MTP
	* ADB
	* Flashlight (dual-tone)
	* BMI160 accelerometer
	* AH1903 hall sensor (smart cover)
	* YAS537 magnetometer / compass
	* BMI160 gyroscope
	* Wired headset detection, audio routing, mic and buttons
	* Lock-screen numeric PIN (credential set and unlock verified)

=================================================
* Not working / untested:
	* Fingerprint sensor — this unit does not answer on SPI (chip id 0x0)
	  although its IRQ fires on touch; the same happens with the official
	  kernel, so the module is likely faulty (second-hand device)
	* Everything not listed above
	* Other panel, touch, sensor and audio variants
	* Daily-driver stability

* Lock screen note:
	* The PIN itself works (verified lock/unlock). MIUI Settings can
	  appear to hang while saving because it calls the fingerprint
	  `preEnroll()` afterwards; the credential is written before that
	  call.
	* Fingerprint: the sensor is **Goodix on SPI0**, not FPC. This is
	  confirmed by the CoreXGT LineageOS device tree (BUGFIXES.md #10b):
	  the FPC TA probes SPI1 (nothing attached) and returns
	  -3/FPC_ERROR_COMM, which is the error seen here.
	* Do NOT add a pinmux on SPI0: the controller uses its pad-macro
	  pads. GPIO58 is the touch panel regulator enable
	  (ctp_pins_regulator_en_output*); claiming it makes mtk-tpd fail to
	  probe and kills touch.
	* Current state: the **software stack is complete** — Goodix on
	  SPI0, `/dev/goodix_fp`, `gf_hal_init` = 0, the IRQ pin (GPIO86
	  EINT9) is muxed and fires on touch, and the enrollment screen
	  starts with `GF_CMD_ENROLL`.
	  The TEE HAL originally failed with 0x427 because MIUI's
	  `mcDriverDaemon` stores the Goodix TA's secure objects under
	  `/data/app/mcRegistry`, which PackageManagerService deletes at
	  boot, and because the daemon starts before that store is usable
	  (CoreXGT BUGFIXES.md #10b-f). Fixed with:
	    - patched `/system/bin/mcDriverDaemon` using
	      `/data/misc/mcregistr` (same-length path);
	    - `/data/misc/mcregistr` created at `post-fs-data`;
	    - a late `mobicore` + `goodixfpd` restart after boot.
	  However, **this particular sensor module is faulty**: its IRQ
	  fires, but every SPI read returns chip id 0x0, and the HAL's
	  image command returns 1023, so enrollment times out. The official
	  kernel shows the same behaviour, so the fault is in the module
	  (SPI/flex or sensor die), not the kernel.

# Building the kernel
```
chmod a+x Build && bash Build
```

# Acknowledgements:
* [@jmpfbmx](https://github.com/jmpfbmx)
* [@R0rt1z2](https://github.com/R0rt1z2)
