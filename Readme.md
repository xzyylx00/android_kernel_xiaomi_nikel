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
	* Fingerprint sensor
	* Everything not listed above
	* Other panel, touch, sensor and audio variants
	* Daily-driver stability

* Lock screen note:
	* The PIN itself works (verified lock/unlock). MIUI Settings can
	  appear to hang while saving because it calls the fingerprint
	  `preEnroll()` afterwards; the credential is written before that
	  call.
	* Fingerprint: the Goodix driver finds no sensor on SPI0 (chip ID
	  reads all zero), and the FPC TEE trustlet fails its INIT step
	  (-3/-4). The stock FPC TEE kernel driver/integration is not in
	  this tree yet, so fingerprint remains unsupported.
	* SPI0 pins 57-60 cannot be reused for a fingerprint sensor on this
	  board: GPIO58 is the touch panel regulator enable
	  (ctp_pins_regulator_en_output*). Enabling the SPI0 pinmux there
	  makes mtk-tpd fail to probe (touch dies).

# Building the kernel
```
chmod a+x Build && bash Build
```

# Acknowledgements:
* [@jmpfbmx](https://github.com/jmpfbmx)
* [@R0rt1z2](https://github.com/R0rt1z2)
