
# BPI K230D Zero vs JUNROC K230D

## RTOS Board Differences

| Item | BPI K230D Zero (RTOS) | JUNROC K230D (RTOS) |
| --- | --- | --- |
| Default CSI | CSI2 | CSI1 |
| CSI ports enabled | CSI0 + CSI2 | CSI1 only |
| CSI0 reset GPIO | 63 | - |
| CSI0 MCLK | MCLK1 | - |
| CSI2 reset GPIO | 62 | - |
| CSI2 I2C | i2c4 | - |
| CSI1 reset GPIO | - | 10 |
| CSI1 I2C | - | i2c4 |
| CSI1 MCLK | - | MCLK1 |
| LCD reset GPIO | 37 | unknown |
| LCD backlight GPIO | -1 (none) | unknown |
| Sensors | GC2093, OV5647, IMX335 | GC2093, OV5647 |

## GPIO Numbering

RTOS flat GPIO numbers map to Linux GPIO bank/pin values as:

```text
GPIO_flat = bank * 32 + pin
```

Examples verified against the DTS/RTOS data:

| RTOS GPIO | Linux DTS GPIO | Meaning |
| --- | --- | --- |
| 37 | `&gpio1_ports 5` | BPI LCD reset, because `32 + 5 = 37` |
| 62 | `&gpio1_ports 30` | BPI CSI2 sensor reset, because `32 + 30 = 62` |
| 10 | `&gpio0_ports 10` | JUNROC CSI1 sensor reset |

## Linux BPI DTS Findings

The BPI Linux DTS uses `bananapi-canmv-k230d-zero.dts`.

The effective DTS in the Buildroot kernel tree shows:

```dts
&i2c4 {
	status = "okay";
	...
};

&mipi0 {
	// set to csi2
	id = <2>;
	reg = <0x0 0x9000a800 0x0 0x800>;
	interrupts = <121 IRQ_TYPE_LEVEL_HIGH>;
	resets = <&sysctl_reset
		  K230_RESET_CSI2_REG_OFFSET
		  K230_RESET_CSI2_TYPE
		  K230_RESET_CSI2_DONE_BIT
		  K230_RESET_CSI2_ASSERT_BIT>,
		 <&sysctl_reset
		  K230_RESET_M2_REG_OFFSET
		  K230_RESET_M2_TYPE
		  K230_RESET_M2_DONE_BIT
		  K230_RESET_M2_ASSERT_BIT>;
};
```

This matches the RTOS data for BPI's default camera path: CSI2 on I2C4.

What was missing when we started: the Linux BPI DTS did not provide the CSI2 sensor reset GPIO. The RTOS data says BPI CSI2 reset is GPIO62, so the Linux DTS value should be:

```dts
reset-gpios = <&gpio1_ports 30 GPIO_ACTIVE_HIGH>;
```

The BPI Linux DTS also did not expose I2C4 as `/dev/i2c-0` through aliases. The vvcam sensor drivers open `/dev/i2c-0` directly; JUNROC's working DTS had this alias, but BPI did not. For BPI OV5647 on I2C4, the DTS needs:

```dts
aliases {
	...
	i2c0 = &i2c4;
};
```

An old BPI image showed this stream startup failure:

```text
vvcam-isp-subdev vvcam-isp-subdev.0: post event 4 return error
ioctl(VIDIOC_STREAMON): Invalid argument
Cannot open input
```

Event 4 is `VVCAM_ISP_EVENT_STREAMON`, so the ISP daemon rejected stream-on while creating/starting the camera pipeline. Unlike the working JUNROC log, there were no `vvcam-mipi ... vvcam_mipi_open`, `set dev attr`, or `kd_vi_bind_source` messages, which means failure happened before MIPI setup.

Further on-device checks showed this old image was still configured for IMX335:

```text
/******sensor configuration******/
isp0 port0:
sensor   : imx335
mode     : 0
xml      : /etc/vvcam/imx335-1920x1080.xml
manu_json: /etc/vvcam/imx335-1920x1080_manual.json
auto_json: /etc/vvcam/imx335-1920x1080_auto.json
*********************************
```

The attached OV5647 was visible on `/dev/i2c-0`:

```text
i2cdetect -y 0
30: -- -- -- -- -- -- 36 --
```

So the old-image stream-on failure was not caused by missing I2C aliasing. It was caused by the runtime ISP sensor configuration still pointing to IMX335 while the hardware was OV5647. Manually switching `/proc/vsi/isp_subdev0` to OV5647 made capture start, then exposed the same pink/magenta color issue seen on JUNROC.

Both `i2c0 = &i2c4` and `reset-gpios = <&gpio1_ports 30 GPIO_ACTIVE_HIGH>` were added in `buildroot-overlay/linux/0024-bananapi-k230d-zero-add-csi2-reset-gpio.patch` and verified by rebuilding `make CONF=BPI-CanMV-K230D-Zero_defconfig linux-build`. The generated DTS contained both properties and the DTB built successfully. The I2C alias is harmless and matches the vvcam driver's `/dev/i2c-0` assumption, but the old image already had OV5647 visible on `/dev/i2c-0`.

## Linux JUNROC Findings

The JUNROC defconfig requested:

```text
BR2_LINUX_KERNEL_INTREE_DTS_NAME="canaan/k230d-canmv-junroc"
```

but the kernel tree did not originally contain `k230d-canmv-junroc.dts`. That caused the build failure:

```text
No rule to make target 'arch/riscv/boot/dts/canaan/k230d-canmv-junroc.dtb'
```

The working JUNROC Linux DTS setup used the K230D CanMV base DTS and overrode the camera path to CSI1:

```dts
&mipi0 {
	// set to csi1
	id = <1>;
	reg = <0x0 0x9000a000 0x0 0x800>;
	interrupts = <118 IRQ_TYPE_LEVEL_HIGH>;
	resets = <&sysctl_reset
		  K230_RESET_CSI1_REG_OFFSET
		  K230_RESET_CSI1_TYPE
		  K230_RESET_CSI1_DONE_BIT
		  K230_RESET_CSI1_ASSERT_BIT>,
		 <&sysctl_reset
		  K230_RESET_M1_REG_OFFSET
		  K230_RESET_M1_TYPE
		  K230_RESET_M1_DONE_BIT
		  K230_RESET_M1_ASSERT_BIT>;
	reset-gpios = <&gpio0_ports 10 GPIO_ACTIVE_HIGH>;
};
```

This matches the RTOS data for JUNROC: CSI1, I2C4, MCLK1, reset GPIO10.

## OV5647 ISP Configuration Findings

Both the BPI OV5647 branch and the JUNROC OV5647 setup use the vvcam sensor stack. On-device checks confirmed that the OV5647 ISP configuration files are present and selected at runtime:

```text
/******sensor configuration******/
isp0 port0:
sensor   : ov5647
mode     : 0
xml      : /etc/vvcam/ov5647.xml
manu_json: /etc/vvcam/ov5647.manual.json
auto_json: /etc/vvcam/ov5647.auto.json
*********************************
```

So the observed pink/magenta image on JUNROC was not caused by missing OV5647 XML/JSON files.

The root cause was the OV5647 Bayer pattern reported by the vvcam userspace sensor driver. `buildroot-overlay/package/vvcam/src/ov5647.c` originally declared:

```c
.bayer = VVCAM_BAYER_PAT_GBRG,
```

The OV5647 register table programs horizontal mirror via:

```c
{0x3821, 0x02},
{0x3820, 0x00},
```

With that register-programmed orientation, the actual raw stream reaching the ISP is `BGGR`, not `GBRG`. Changing the declared Bayer pattern to:

```c
.bayer = VVCAM_BAYER_PAT_BGGR,
```

fixed the pink/magenta color cast on JUNROC. BPI showed the same pink/magenta image after switching runtime sensor config to OV5647, so the same Bayer fix applies there too. This is a sensor-driver-level finding, not a board-DTS finding.

## Current Working Hypotheses

For BPI K230D Zero with an attached OV5647 camera:

1. The board should stay on CSI2, matching RTOS and the existing Linux DTS remap.
2. The Linux DTS needs `i2c0 = &i2c4` because the vvcam OV5647 driver opens `/dev/i2c-0`.
3. The Linux DTS needs the CSI2 reset GPIO: `&gpio1_ports 30 GPIO_ACTIVE_HIGH`.
4. The shared OV5647 Bayer fix from `GBRG` to `BGGR` in `vvcam/src/ov5647.c` is needed to avoid pink/magenta output.
5. If capture does not start at all, first check reset/I2C/CSI logs before changing Bayer order.

Useful on-device checks:

```sh
grep . /proc/vsi/isp_subdev0
ls -l /etc/vvcam/ov5647*
dmesg | grep -E 'vvcam-mipi|vvcam-isp|ov5647|i2c'
```
