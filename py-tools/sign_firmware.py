#!/usr/bin/env python3
"""
sign_firmware.py — REMOVED. Firmware signing now lives in the openlifu-sdk.

The image format and the FwVersion encoding are owned by a single
implementation, ``openlifu_sdk.io.LIFUCrypto``, so the signer, the SDK's
update/migration tooling, and the CI all stay consistent. This local copy
would drift (it also used the legacy decimal MMmmpp version encoding, which
has been replaced by a 16-bit bitfield scheme), so it has been retired.

Sign with the SDK instead
-------------------------
    pip install 'openlifu-sdk[crypto]'
    python -m openlifu_sdk.io.LIFUCrypto sign \\
        --keys   <dir with ecdsa_private.pem + aes128.bin> \\
        --firmware app.bin \\
        --version 1.2.7            # semver; encoded as the 16-bit bitfield \\
        --output  app_signed.bin

Validate / inspect:
    python -m openlifu_sdk.io.LIFUCrypto verify SIGNED.bin --keys <dir>
    python -m openlifu_sdk.io.LIFUCrypto info   SIGNED.bin

The signer runs standalone (only the `cryptography` package is required); it
does not pull in the rest of the SDK.
"""

import sys

_MSG = __doc__


def main() -> int:
    sys.stderr.write(_MSG)
    sys.stderr.write(
        "\nERROR: this tool has moved to openlifu_sdk.io.LIFUCrypto "
        "(see above).\n")
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
