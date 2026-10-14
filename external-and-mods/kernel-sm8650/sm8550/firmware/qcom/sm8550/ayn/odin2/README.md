# AYN Odin 2 ADSP

AYN's ADSP from the Odin 2's stock Android (build 1.0.0.387, 2025-09-10),
replacing the older Odin 2 ADSP from ROCKNIX extra-firmware at the same path
(the Odin 2 DT's `firmware-name` is unchanged):

- `adsp.mbn`, `adsp_dtb.mbn`: ADSP `ADSP.HT.5.8-01153-KAILUA-1`, rebuilt by
  AYN 2025-05-19, from `/vendor/firmware/adsp.mdt` + `adsp.bNN` and
  `adsp_dtb.mdt` + `.bNN`, joined into one file each (segments placed at
  their ELF offsets, like pil-squasher). Same battery profile
  (`QUECTEL_SG8550_8000MAH…JUN26TH2023`) and same `adsp_dtb` settings as
  ROCKNIX's copy; it adds the charge separation handler (USB property 0x0e,
  `patches/0551`), which the older builds do not have.

The `.jsn` protection-domain maps and `aw883xx_acf.bin` in the stock build are
byte-identical to ROCKNIX's, so those stay ROCKNIX's.
Not yet tested on an Odin 2.
