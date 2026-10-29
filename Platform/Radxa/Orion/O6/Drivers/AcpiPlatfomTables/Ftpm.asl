/** @file
  Firmware TPM provided by CIX's fTPM trusted application in OP-TEE. Linux
  binds tpm_ftpm_tee to this node; the TA instance firmware measured into
  stays alive across ExitBootServices.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

Device (FTPM) {
  Name (_HID, "PRP0001")
  Name (_UID, 0)
  Name (_STA, 0xF)
  Name (_DSD, Package () {
    ToUUID ("daffd814-6eba-4d8c-8a91-bc9bbf4aa301"),
    Package () {
      Package () { "compatible", "microsoft,ftpm" },
    }
  })
}
