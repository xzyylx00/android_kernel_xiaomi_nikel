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
> tested on real hardware before being committed, except the fingerprint
> (the test unit's sensor module is faulty; see below).

> **Tested on one device only**
> This kernel has only been tested on a single Redmi Note 4 (nikel,
> MT6797) unit. Different panel, touch, sensor or audio variants may
> behave differently. Keep a bootable backup before flashing, and do not
> assume it is safe for daily use on another device.

> **Fingerprint could NOT be verified on the author's unit**
> The Goodix fingerprint software stack is implemented, but the author's
> second-hand sensor module does not answer on SPI: its IRQ fires on
> touch, yet every chip-id read returns 0x0 and enrollment fails with
> "Add fingerprint failed". The official kernel behaves the same, so the
> fault is in the module. See "Fingerprint status" below.

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
	* Fingerprint: see the dedicated "Fingerprint status" section below.
	  The apparent Settings hang is only the fingerprint `preEnroll()`
	  call; the PIN is written before it.

=========================================================================
* Fingerprint status — implemented, but untestable on the author's unit:

	* The sensor is **Goodix on SPI0**, not FPC. The FPC TA probes SPI1,
	  finds nothing attached and returns -3/FPC_ERROR_COMM.

	* Software implemented:
		* Goodix SPI0 child + `mediatek,goodix-fp` platform node, with
		  **no SPI0 pinmux** (the controller uses pad-macro pads;
		  GPIO58 is the touch panel regulator enable).
		* `/dev/goodix_fp`, pad-macro prime, and the IRQ pin
		  (GPIO86/EINT9) re-muxed after every chip reset.
		* MIUI TEE daemon fix: `/system/bin/mcDriverDaemon` uses
		  `/data/misc/mcregistr`, the directory is created at
		  `post-fs-data`, and `mobicore` + `goodixfpd` are restarted
		  after boot. Without this the HAL fails with 0x427.
	  With that, `gf_hal_init` returns 0 and the enrollment screen
	  starts (`GF_CMD_ENROLL`).

	* Why it is untested: the author's module is faulty.
		* The IRQ line works: `goodix_fp_irq` increments when a finger
		  touches the sensor.
		* But every SPI read returns chip id **0x0**, the HAL image
		  command returns **1023**, and the 60 s enrollment times out
		  with "Add fingerprint failed".
		* The official/stock kernel fails at exactly the same point, so
		  the kernel is not the cause. On this second-hand unit the
		  fingerprint flex/SPI data path or the sensor die is faulty.

	* If you have a healthy module, verify it like this:
		1. `grep goodix /proc/interrupts` — the counter must increment
		   when you touch the sensor.
		2. `dmesg | grep gf_check_9p_chip` — a real chip version must
		   be printed, not 0x0.
		3. Settings → Lock screen & password → Fingerprints → Add.
	  If the SPI reads remain 0x0, reseat or replace the fingerprint
	  flex/module. The software side is ready for a working module.

# Building the kernel
```
chmod a+x Build && bash Build
```

# Acknowledgements:
* [@jmpfbmx](https://github.com/jmpfbmx)
* [@R0rt1z2](https://github.com/R0rt1z2)
