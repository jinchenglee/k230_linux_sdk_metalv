# K230 / K230D board CSI camera-interface comparison

Comparison of the MIPI CSI-2 camera interfaces across the three schematics in this
`docs/` directory. Derived directly from the PDFs (net names, VDDIO straps, pull-up
rails and LDO parts extracted from the schematic text).

| PDF | Board | SoC package |
|---|---|---|
| `01Studio_CanMV-K230_SCH.pdf` | 01Studio CanMV-K230 | K230 (BGA, external LPDDR4) |
| `Canmv-K230_V1.1_Schematic.pdf` | Canaan / official CanMV-K230 V1.1 | K230 (BGA, external LPDDR3/4) |
| `BPI_CanMV-K230D-Zero_V1.1_schematic_2025-02-12.pdf` | Banana Pi CanMV-K230D-Zero | **K230D** (in-package LPDDR — no external DDR sheet) |

---

## 1. SoC side is identical on all three

The K230 exposes three MIPI CSI-2 D-PHY receivers, wired the same way everywhere:

- **CSI0 / RX0** → clock + **D0, D1** (2 lanes)
- **CSI1 / RX1** → clock + **D2, D3** (2 lanes routed). On the full-K230 boards RX1
  physically has D0–D3, but D0/D1 are left unconnected.
- **CSI2 / RX2** → clock + **D4, D5** (2 lanes)

Every board routes CSI1 as **D2/D3** (never D0/D1) to preserve the K230 4-lane
"combine" mode: a 4-data-lane sensor uses CSI0's D0/D1 as lanes 0–1 and CSI1's
D2/D3 as lanes 2–3. Only the BPI board physically exploits this (see §2).

---

## 2. Connector topology & lane exposure

| | CSI0 | CSI1 | CSI2 |
|---|---|---|---|
| **01Studio** | FPC2, 24-pin FPC | FPC1, 24-pin FPC | **J4, DF30 board-to-board mezzanine** (onboard GC2093) |
| **Canaan V1.1** | **DF30 mezzanine** (OV5647 module) | J2, 22-pin FPC | J1, 22-pin FPC |
| **BPI K230D-Zero** | **combined with CSI1 on J3** | **combined with CSI0 on J3** (31-pin 0.3 mm FPC) | X1, 22-pin FPC |

Key structural differences:

- **BPI merges CSI0 + CSI1 onto a single 31-pin 0.3 mm FPC (J3)** — a Pi-camera-style
  connector carrying both 2-lane ports (both clocks present). It can take two
  independent 2-lane sensors **or a single 4-lane sensor**. The two full-K230 boards
  keep CSI0/CSI1 on **separate** connectors, so each port is 2-lane only and neither
  exposes a native 4-lane path.
- Each of the two full-K230 boards has its best-equipped camera on a **DF30 mezzanine**
  (01Studio → CSI2 / GC2093; Canaan → CSI0 / OV5647), complete with dedicated
  2.8V/1.5V LDOs. **BPI has no mezzanine and no on-board camera LDOs.**

---

## 3. I²C control-bus assignment (per port)

| Port | 01Studio | Canaan V1.1 | BPI K230D-Zero |
|---|---|---|---|
| CSI0 | **I2C0** | **I2C3** (OV5647) | I2C0 / I2C1 (on J3) — *pairing not fixed in SW* |
| CSI1 | **I2C1** | **I2C0** | I2C0 / I2C1 (on J3) — *pairing not fixed in SW* |
| CSI2 | **I2C4** | **I2C1** | **I2C4** (shared with the DSI touch-panel bus TP_SCL/SDA) |

The sensor-control I²C bus number differs on **every** board, so device trees are not
portable across them without remapping. On BPI, watch the I2C4 conflict between CSI2
and the DSI touch panel.

### Software cross-check (SDK sources) — verified 2026-08-03

Confirmed against the u-boot board DTS (`buildroot-overlay/boot/uboot/
u-boot-2022.10-overlay/arch/riscv/dts/`) and the mainline kernel DTS
(`dl/linux/git/arch/riscv/boot/dts/canaan/`):

- **BPI CSI2 ↔ I2C4 — CONFIRMED.** `k230d_canmv_bpi.dts` muxes `// IIC4 -> CAM0` on
  IO7/IO8. The kernel `bananapi-canmv-k230d-zero.dts` also puts the FT5306 touch panel
  on `&i2c4` → **the CSI2 / touch-panel I2C4 conflict is real.**
- **BPI J3 carries both I2C0 and I2C1 — CONFIRMED.** `k230d_canmv_bpi.dts` muxes
  `// IIC1 -> CAM` (IO40/IO41) and `//iic0` (IO48/IO49), plus MCLK1 and CAM1_GPIO.
  **But the exact CSI0-vs-CSI1 → I2C0-vs-I2C1 pairing is NOT determined in software:**
  the mainline kernel DTS defines no camera-sensor I²C nodes, and the vvcam sensor
  drivers hard-code `/dev/i2c-0` (see below). The pairing is a physical-routing /
  sensor-slot convention only.
- **01Studio kernel aliases** `i2c0 = &i2c4; i2c1 = &i2c3` (`k230-canmv-01studio.dts`).
  Note: mainline (cyyself) DTS, *not* the SDK big-core camera path — informational.
- **vvcam sensor drivers hard-code `/dev/i2c-0`** (`buildroot-overlay/package/vvcam/
  src/` — gc2093/gc2053/ov5647/imx335/bf3238). In practice a sensor is expected on
  logical bus 0 regardless of board wiring; the per-CSI I²C routing matters for HW
  but the driver assumes bus 0.
- **K230 I²C controller base addresses** (`k230.dtsi`): i2c0 `0x91405000`,
  i2c1 `0x91406000`, i2c2 `0x91407000`, i2c3 `0x91408000`, i2c4 `0x91409000`.

---

## 4. Interface voltage — the "1.8V vs 3.3V" issue

The MIPI differential lanes run off `AVDD*_MIPI` and are not the concern. What matters
is the **camera sideband** — I²C (SCL/SDA), MCLK, RESET/PWDN — plus the sensor's
**DOVDD** (digital I/O rail). Those signals hang off K230 GPIO banks, and **each K230
GPIO bank's VDDIO is strap-selectable 1.8V or 3.3V**. Camera control I²C lands in
**BANK3** (I2C0/1/3 on IO40–49) and/or **BANK0** (I2C4 on IO7/IO8); MCLK on BANK0/BANK5.

### GPIO bank VDDIO — strap **and** software (both confirmed, verified 2026-08-03)

The software voltage is set in the **u-boot board DTS** via `BANK_VOLTAGE_IOx_IOy`
macros → the pad IOMUX **MSC bit (bit 9)**: `K230_MSC_1V8 = 1`, `K230_MSC_3V3 = 0`
(`.../u-boot-2022.10-overlay/include/dt-bindings/pinctrl/k230_evb.h`). The u-boot
`BANK_VOLTAGE_*` IO ranges map onto the hardware VDDIO banks as: BANK0 = IO0–13,
BANK1 = IO14–25, BANK2 = IO26–37, BANK3 = IO38–49, BANK4 = IO50–61, BANK5 = IO62–63.
**The software config matches the schematic straps byte-for-byte on all three boards.**

| VDDIO bank (IO range) | 01Studio | **Canaan V1.1** | BPI K230D-Zero | source DTS |
|---|---|---|---|---|
| BANK0 (IO2–13) | 3.3V | 1.8V | 3.3V | `k230_canmv_01studio.dts` / `k230_canmv.dts` / `k230d_canmv_bpi.dts` |
| BANK1 (IO14–25) | 3.3V | 1.8V | 3.3V | ″ |
| BANK2 (IO26–37) | 3.3V | 1.8V | 3.3V | ″ |
| BANK3 (IO38–49, camera I²C) | **3.3V** | **1.8V** | 3.3V | ″ |
| BANK4 (IO50–61) | 3.3V | 3.3V | 3.3V | ″ |
| BANK5 (IO62–63, MCLK/cam GPIO) | **1.8V** | **1.8V** | 3.3V | ″ |

IO0–IO1 is **force-fixed at 1.8V** on every board (`BANK_VOLTAGE_IO0_IO1 = K230_MSC_1V8`).
Note the camera **I2C4** pad is **IO7/IO8 = BANK0**, so it tracks the BANK0 voltage
(1.8V on Canaan, 3.3V on 01Studio/BPI) — this is why 01Studio drives I2C4 at 3.3V and
level-shifts (SI2302) down to its 1.8V onboard sensor. ⚠️ Mismatching a bank's strap
and its MSC setting can damage the SoC (u-boot DTS comments warn of this explicitly).

### Camera sideband & power summary

| | 01Studio | **Canaan V1.1** | BPI K230D-Zero |
|---|---|---|---|
| Camera I²C pull-up rail | CSI0/CSI1 → **3V3**; CSI2 → 1V8 | all → **1V8** | → 3V3 |
| I²C level shifters | **SI2302** on CSI2 (3V3↔1V8) | none | none |
| On-board cam LDOs | XC6206 **2.8V + 1.5V** | XC6206 **2.8V + 1.5V** | **none** |
| Connector-supplied rails | 3V3, 5V (+2V8/1V5/1V8 on CSI2) | 3V3, 5V (+2V8/1V5/1V8 on CSI0) | **3V3, 5V only** |

### Why Canaan V1.1 needs sensors at 1.8V

Canaan straps **BANK3 (and BANK0–2 + the MCLK bank BANK5) to 1.8V** — confirmed both by
the schematic strap and by `k230_canmv.dts` (`BANK_VOLTAGE_IO2..IO49 = K230_MSC_1V8`) —
because its reference sensor (OV5647 on CSI0) uses **DOVDD = 1.8V**, and the camera I²C
bus is pulled to 1.8V.
The two FPC ports (CSI1/CSI2) share that same 1.8V BANK3 bus, so **any** attached
sensor must present 1.8V logic on SCL/SDA/MCLK/RESET. A generic module with a
2.8V or 3.3V DOVDD LDO mismatches the 1.8V bus (I²C won't ACK / back-drives the SoC).
**Fix: reconfigure or replace the camera module's DOVDD LDO to output 1.8V** (or add a
level translator). There is no on-board translation on the Canaan FPC ports.

### How the other two differ

- **01Studio** — BANK3 = **3.3V**, so the two external FPC ports (CSI0/CSI1) accept
  3.3V-I/O sensors directly, no modification. Its only 1.8V sensor is the *onboard*
  GC2093 on CSI2, and the board absorbs that mismatch itself: **SI2302 I²C
  level-shifters (3.3V↔1.8V)** + BANK5 at 1.8V for MCLK/RST + dedicated 2.8V/1.5V LDOs.
- **BPI K230D-Zero** — **all** GPIO banks = **3.3V**, and the connectors supply only
  3V3/5V with **no on-board camera LDOs**. Expects fully self-regulating, 3.3V-I/O
  camera modules (the module makes its own AVDD/DOVDD/DVDD). No 1.8V change needed,
  but no on-board sensor-power help either.

---

## 5. Practical notes for driver / device-tree bring-up

1. **Device trees are not portable across boards** — the control I²C bus index per CSI
   differs on all three, and lane routing metadata differs.
2. **4-lane MIPI sensor** is only supported in hardware by the **BPI K230D-Zero**
   (combined CSI0+CSI1 on J3); the two K230 boards top out at 2 lanes per port.
3. **BPI**: mind the **I2C4 conflict** between CSI2 and the DSI touch panel.
4. **Interface voltage** — Canaan camera sideband is **1.8V** (BANK0–3=1.8V); 01Studio
   external ports are **3.3V** (onboard CSI2 is 1.8V, handled on-board); BPI is **3.3V**
   everywhere. This is set by the `BANK_VOLTAGE_*` MSC bits in each board's u-boot DTS
   and confirmed to match the straps (§4) — the strap is necessary but the software
   setting governs, so verify both when porting to a new board.
5. For a self-powered module test, the **01Studio CSI2 (GC2093)** and **Canaan CSI0
   (OV5647)** mezzanine ports are "batteries-included" (on-board 2.8V/1.5V); every
   plain FPC port needs the module to supply its own analog/core rails.

---

## 6. Verification status (2026-08-03)

- ✅ **Bank/pad voltages** — cross-checked against the u-boot board DTS
  (`BANK_VOLTAGE_*` → MSC bit) and found to match the schematic straps on all three
  boards. See §4.
- ✅ **BPI CSI2 ↔ I2C4** and the **CSI2 / touch-panel I2C4 conflict** — confirmed in
  `k230d_canmv_bpi.dts` + `bananapi-canmv-k230d-zero.dts`. See §3.
- ⚠️ **BPI CSI0-vs-CSI1 → I2C0-vs-I2C1 pairing** — *unresolved by design.* Both buses
  are wired to J3, but no software (kernel DTS / vvcam driver) binds a specific CSI half
  to a specific bus; vvcam hard-codes `/dev/i2c-0`. Left intentionally ambiguous. See §3.
- Everything in §1–§2 is derived from the schematic PDFs and not independently
  re-verified against software (the camera pipeline runs on the big RT-Smart core, not
  the Linux DT).

---

*Sources: schematic PDFs in this directory; SDK device trees under
`buildroot-overlay/boot/uboot/u-boot-2022.10-overlay/arch/riscv/dts/` and
`dl/linux/git/arch/riscv/boot/dts/canaan/`; vvcam drivers under
`buildroot-overlay/package/vvcam/src/`. Verify against the target board's device tree
before relying on any pin/bus/voltage mapping.*
