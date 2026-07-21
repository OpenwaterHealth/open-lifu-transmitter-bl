# Bench test tools

`dfu-i2c-test.py` is copied unmodified from the legacy bootloader
(openlifu-transmitter-bl). The I2C DFU **transport protocol is identical**
(same commands 0x01-0x06, framing, status/state codes, slave address 0x72),
so `erase`, `mass-erase`, `write`, `getstatus`, `get-version`, `info` and
`reboot` work against this secure bootloader as-is.

Differences under secure boot (SBSFU):

- There is **no metadata page** anymore. The signed image carries its own
  `SFU1` header at the slot base (0x08010000). A DNLOAD/ERASE aimed at the
  old metadata address (0x0800F800) is rejected with STATUS_BAD_ADDR — that
  page is now inside the read-only bootloader region, and the anti-rollback
  floor log lives at 0x0803F000.
- The legacy signing subcommands (`sign-metadata`, `program-signed`,
  `pack-signed`, `verify-*`, `program-package`) target the old WFM1/PGK1
  scheme and do not apply. Sign with the SDK signer
  (`openlifu_sdk.io.LIFUCrypto`) and write the signed image to 0x08010000
  with `write`, then `reboot`:

      python -m openlifu_sdk.io.LIFUCrypto sign --keys py-tools/keys --firmware app.bin --version X.Y.Z --output signed.bin
      python test/dfu-i2c-test.py write --file signed.bin --address 0x08010000 --erase-pages
      python test/dfu-i2c-test.py reboot

The bootloader enters I2C DFU slave mode (I2C1 PB6/PB7, addr 0x72) only when
no USB host is detected within ~1.9 s of entering DFU mode; with USB attached
it presents standard DfuSe (0483:df11) instead.
