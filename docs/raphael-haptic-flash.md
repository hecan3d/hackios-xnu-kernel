# raphael: the vibration motor and the flashlight, from the phone's own device tree

Taken from Android's `dtbo` partition (dumped through the loader, read-only) — the base DTB is not in
`boot` (no `d00dfeed` there), the overlays are in `dtbo` (lba 24576, 32 MB, one DTB at offset 0x40,
233 906 bytes). Read with a small FDT parser; the nodes speak for themselves.

**This is not the hardware the port's `hakios.haptic.c` and `hakios.torch.c` are written for.** Those
two files describe the POCO X5 Pro (redwood): an AW86927 on I2C (GPIO 36/37, reset 35) and a PM8350C
flash module (SPMI slave 2, 0xee00). raphael has neither, and writing the redwood pins from raphael
drops the SPMI bus — after which the buttons stop working (p353/p355, and the same from `torch=3` in
p347).

## The vibration motor: the PM8150B's own haptics block

```
fragment@3/__overlay__/qcom,pm8150b@3/qcom,haptics@c000
    compatible                   qcom,haptics
    reg                          0xc000, size 0x100        (SPMI slave 3)
    interrupts                   hap-sc-irq, hap-play-irq
    qcom,actuator-type           lra
    qcom,vmax-mv                 3000
    qcom,play-rate-us            6667
    qcom,lra-resonance-sig-shape sine
```

Six waveforms follow as children (`wf_0` … `wf_5`), each `qcom,effect-id` 0…5 with
`qcom,wf-vmax-mv 3000` and:

| node | effect-id | wf-pattern (hex)      | play-rate-us | extra |
|------|-----------|-----------------------|--------------|-------|
| wf_0 | 0         | 3e3e3e                | 6667         | brake-pattern 0x01000000 |
| wf_1 | 1         | 7e7e020202020202      | 7143         | repeat 2, s-repeat 1 |
| wf_2 | 2         | 7e7e                  | 4000         | |
| wf_3 | 3         | 7e7e7e                | 6667         | |
| wf_4 | 4         | 7e7e                  | 5000         | |
| wf_5 | 5         | 7e7e7e                | 6667         | brake-pattern 0x03000000 |

All but wf_0 carry `qcom,lra-auto-resonance-disable`.

## The flashlight: PM8150L's flash LED block

```
fragment@5/__overlay__/qcom,pm8150l@5/qcom,leds@d300
    compatible                   qcom,qpnp-flash-led-v2
    reg                          0xd300, size 0x100        (SPMI slave 5)
    label                        flash
    qcom,thermal-derate-current  200, 500, 1000 mA
    qcom,isc-delay               192
  qcom,flash_0   led:flash_0, max 1500 mA, default 1000 mA, duration 1280 ms, ires 12500 uA
  qcom,flash_1   led:flash_1, max 1500 mA, default 1000 mA, duration 1280 ms, ires 12500 uA
  qcom,flash_2   led:flash_2, max  750 mA
```

## What this means for the port

- `hakios.haptic.c` has to talk to PM8150B 0xc000 over **SPMI**, with the same bus lock the buttons
  driver uses (`hakios_spmi_lock()`, `bsd/dev/hakios.buttons.c`), not bit-banged I2C. The effect
  patterns above are the ones Android plays.
- `hakios.torch.c` has to talk to PM8150L 0xd300 (`qpnp-flash-led-v2`), not PM8350C 0xee00.
- The redwood drivers are worth keeping as reference for the register dance, but every address in
  them is wrong for this phone.

## How it was dumped

```
fastboot oem read-part dtbo 0 400      # 4 MB per call, relative lbas, 8 calls = 32 MB
fastboot oem read-part boot 0 400      # boot: ANDROID! page 4096, kernel 19 883 110 bytes, no DTB
```
