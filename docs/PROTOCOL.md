# HALOT network protocol

What ResinDeck knows about how Creality HALOT resin printers talk over the network.
It comes from [HalotMageProSharp](https://github.com/mrtska/HalotMageProSharp) (MIT) and from
watching what HALOT BOX 4.7.0.10 does with a **HALOT-R6** (firmware `version: "1"`).
Other models and firmware may differ; corrections are welcome.

## Transport

- Plain WebSocket at `ws://<printer-ip>:18188`, no TLS.
- Commands are JSON text frames, `{"cmd": "...", ...}`. Every value is a string, including numbers.
- File data goes in binary frames.
- One connection can stay open; HALOT BOX polls the status every few seconds.

## Token

Every command except `VERSION_CHECK` and the upload commands carries `"token"`, derived from the
network password shown on the printer screen (1 to 8 ASCII characters):

1. Pad the password with PKCS#7 to 8 bytes.
2. Encrypt with single DES, CBC mode, key `a85e9d68` (ASCII), IV of eight zero bytes.
3. Keep the first 8 bytes of the ciphertext.
4. HalotMageProSharp appends the constant `AC 47 B0 02 88 15 6E 5C` when the password is longer
   than 4 characters. HALOT BOX 4.7 was seen sending only the 8 bytes. ResinDeck tries both.
5. Base64.

A wrong token does not close the connection. `GET_PRINT_STATUS` answers with
`"printStatus": "TOKEN_ERROR"` and every other field empty.

## Commands

| Command | Extra fields | Reply |
|---|---|---|
| `VERSION_CHECK` | none | `{"version": "1"}` (no `cmd` field) |
| `GET_PRINT_STATUS` | `token` | status object (below) |
| `START_PRINT` | `token`, `filename` | `{"cmd": "START_PRINT", ...}` |
| `PRINT_PAUSE` | `token` | pauses, or resumes when paused |
| `PRINT_STOP` | `token` | cancels the print |
| `PRINT_PARA_SET` | `token`, `bottomExposureNum`, `delayLight`, `eleSpeed`, `initExposure`, `printExposure`, `printHeight` | not used by ResinDeck |
| `START_FILE` / `START_DATA` / `CHECK_DATA` / `CANCEL_FILE` | see [Upload](#upload) | |

### Status

```json
{"cmd": "GET_PRINT_STATUS", "printStatus": "PRINT_PROCESSING", "filename": "part.cxdlpv4",
 "curSliceLayer": "200", "sliceLayerCount": "380", "printRemainTime": "2700",
 "initExposure": "", "printExposure": "", "delayLight": "", "eleSpeed": "",
 "bottomExposureNum": "", "layerThickness": "", "printHeight": "", "resin": "",
 "camera": "0", "mode": "0"}
```

`printRemainTime` is in seconds. `printStatus` values found in HALOT BOX:
`PRINT_GENERAL` (idle), `PRINT_PROCESSING`, `PRINT_PAUSE`, `PRINT_STOPING` (sic), `PRINT_STOP`,
`PRINT_COMPLETING`, `PRINT_COMPLETE`, `PRINT_END`, `PRINT_FAIL`, `TOKEN_ERROR`.

## Upload

This is the part that differs most from HalotMageProSharp. Sending the whole file in one
uncompressed frame does not work on the R6: the printer acknowledges with `"received": "0"` and
then waits forever.

1. Send `START_FILE`, with no token:

   ```json
   {"cmd": "START_FILE", "filename": "part.cxdlpv4", "key": "<key>", "offset": "0", "size": "<bytes>"}
   ```

   `key` comes from the file itself: its last 4 bytes (the CRC footer of a `.cxdlpv4`), read as a
   **signed little-endian** 32-bit integer and written in lowercase hex, with a `-` sign when
   negative (for example `-307ee63f`).

2. The printer answers:

   ```json
   {"cmd": "START_FILE", "compress": true, "errorcode": 0, "filename": "...", "key": "...", "offset": "0", "size": "..."}
   ```

   `errorcode: 1` (with `wifistate`) means it is still busy with another transfer.

3. Send the file in **1 MiB** chunks, one binary frame each. When `compress` is true, every chunk
   goes through Qt's `qCompress`: a 4-byte big-endian length of the uncompressed chunk followed by
   a zlib stream of it. Wait for the reply to each chunk before sending the next:

   ```json
   {"cmd": "START_DATA", "errorcode": 0, "key": "...", "received": "1048576", "size": "..."}
   ```

   `received` counts uncompressed bytes.

4. After the last chunk the printer checks the file and sends:

   ```json
   {"cmd": "CHECK_DATA", "checkstate": 1, "key": "..."}
   ```

   On the R6, `checkstate: 1` means the file was accepted. HalotMageProSharp documents `0` as
   success for the Mage Pro.

5. To abort, send `{"cmd": "CANCEL_FILE", "key": "<key>", "filename": "<name>"}`. The printer
   answers with `errorcode: 0`. A transfer left half-way stays tied to its connection: the printer
   refuses new uploads until that connection closes.

## `.cxdlpv4` files

The parser in `cxdlp.py` follows the layout documented by
[UVtools](https://github.com/sn4k3/UVtools) (`CrealityCXDLPv4File.cs`): a big-endian `CXSW3DV2`
magic and version, then a little-endian header with resolution, bed size, layer height and
offsets to two RGB565-RLE previews (120×120 and 300×300), the print parameters and the layer
table. Each layer is 7-bit gray RLE (or pixel-pair RLE on the HALOT-X1). The file ends with a
big-endian CRC-32 of everything before it (reflected polynomial, init 0, no final XOR).
