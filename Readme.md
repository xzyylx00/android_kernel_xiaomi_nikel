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

=================================================
* Not working / untested:
	* Everything not listed above
	* Other panel, touch, sensor and audio variants
	* Daily-driver stability

# Building the kernel
```
chmod a+x Build && bash Build
```

# Acknowledgements:
* [@jmpfbmx](https://github.com/jmpfbmx)
* [@R0rt1z2](https://github.com/R0rt1z2)
