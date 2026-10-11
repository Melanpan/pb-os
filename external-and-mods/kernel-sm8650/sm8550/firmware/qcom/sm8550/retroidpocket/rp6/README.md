# Retroid Pocket 6 firmware

Retroid's own firmware from the RP6's stock Android (build RP6_V1.0.0.406,
2026-06-16), used instead of AYN Odin 2's on the RP6:

- `adsp.mbn`, `adsp_dtb.mbn`: ADSP `ADSP.HT.5.8-01329-KAILUA-1`, OEM build
  `sz-d-l-011216b`, from `/vendor/firmware/adsp.mdt` + `adsp.bNN` and
  `adsp_dtb.mdt` + `.bNN`, joined into one file each (segments placed at their
  ELF offsets, like pil-squasher). It carries the RP6's 6000 mAh battery
  profile (`8262735_QUECTEL_LIVESTREAMING_6000MAH`); Odin 2's ADSP carries an
  8000 mAh one. Code and data segments are identical to Armada's
  `qcom/sm8550/retroidpocket/rp6/adsp.mbn`; `adsp_dtb.mbn` is byte-identical.
- `aw883xx_acf.bin`: Awinic AW88166 speaker tuning (project A1901), unchanged
  from `/vendor/firmware`; the same file as the Nova's.
- `*.jsn`: protection-domain maps from the modem partition (`image/`),
  identical to the Nova's and Odin 2's.

With this ADSP the amps need stock's channel numbering (amp 0x34 on channel 1,
0x35 on channel 0), set in `external-and-mods/kernel-sm8650/sm8550/dts/qcs8550-retroidpocket-rp6.append`.
The Nova includes the RP6 device tree and overrides these paths with its own.
Not yet tested on an RP6.
