# AYN Odin 2 Portal ADSP

AYN's ADSP from the Odin 2 Portal's stock Android (build 20250312),
replacing the Portal ADSP from ROCKNIX extra-firmware at the same path (the
Portal DT's `firmware-name` is unchanged):

- `adsp.mbn`, `adsp_dtb.mbn`: ADSP `ADSP.HT.5.8-01329-KAILUA-1`, rebuilt by
  AYN 2025-03-05, from `/vendor/firmware/adsp.mdt` + `adsp.bNN` and
  `adsp_dtb.mdt` + `.bNN`, joined into one file each (segments placed at
  their ELF offsets, like pil-squasher; every segment matches the signed
  hash table). Same battery profile
  (`QUECTEL_SG8550_8000MAH_PM8550B…DEC7TH2024`) and same `adsp_dtb` settings
  as ROCKNIX's copy; it adds the charge separation handler (USB property
  0x0f, `patches/0551`), which ROCKNIX's copy and the older build in the
  stock modem image (`NON-HLOS.bin`, 2024-06-20) do not have.

The `.jsn` protection-domain maps and `aw883xx_acf.bin` in the stock build are
byte-identical to ROCKNIX's, so those stay ROCKNIX's.
Not yet tested on an Odin 2 Portal.
