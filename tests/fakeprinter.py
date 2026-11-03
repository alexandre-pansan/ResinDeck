"""A fake Creality HALOT: speaks the WebSocket protocol of halot.py (see docs/PROTOCOL.md)."""
import json
import struct
import zlib

import websockets

from drivers.halot import token


class FakePrinter:
    def __init__(self, password="secret1", version="1"):
        self.password = password
        self.version = version
        self.short_token_only = False  # like HALOT BOX 4.7 on the R6: 8 DES bytes, no LOWER_KEY
        self.compress = True
        self.busy = False              # START_FILE answers errorcode 1
        self.refuse = False            # START_FILE answers another error code
        self.stall = False             # START_DATA never advances
        self.check_inline = False      # last chunk answered with CHECK_DATA only
        self.checkstate = 1
        self.status = {"printStatus": "PRINT_GENERAL"}
        self.files = {}                # filename -> bytes received
        self.commands = []             # every JSON command received
        self.cancelled = []
        self.server = None
        self.port = None

    async def start(self):
        self.server = await websockets.serve(self.handler, "127.0.0.1", 0, max_size=None)
        self.port = self.server.sockets[0].getsockname()[1]

    async def stop(self):
        self.server.close()
        await self.server.wait_closed()

    def valid(self, tok):
        tokens = {token(self.password, lower_key=False)}
        if not self.short_token_only:
            tokens.add(token(self.password, lower_key=True))
        return tok in tokens

    async def handler(self, ws):
        upload = None
        async for msg in ws:
            if isinstance(msg, bytes):
                upload = await self.on_chunk(ws, upload, msg)
                continue
            r = json.loads(msg)
            self.commands.append(r)
            cmd = r.get("cmd")

            async def reply(cmd=cmd, **fields):
                await ws.send(json.dumps({"cmd": cmd, **fields}))

            if cmd == "VERSION_CHECK":
                await reply(version=self.version)
            elif cmd == "GET_PRINT_STATUS":
                if self.valid(r.get("token")):
                    await reply(**self.status)
                else:
                    await reply(printStatus="TOKEN_ERROR")
            elif cmd == "START_FILE":
                if self.busy:
                    await reply(errorcode="1")
                elif self.refuse:
                    await reply(errorcode="7")
                else:
                    upload = {"name": r["filename"], "key": r["key"], "size": int(r["size"]), "data": b""}
                    await reply(errorcode="0", compress=self.compress, key=r["key"])
            elif cmd == "CANCEL_FILE":
                self.cancelled.append(r["filename"])
                upload = None
                await reply(errorcode="0")
            elif cmd in ("START_PRINT", "PRINT_PAUSE", "PRINT_STOP"):
                if not self.valid(r.get("token")):
                    await reply(printStatus="TOKEN_ERROR")
                    continue
                if cmd == "START_PRINT":
                    self.status = {"printStatus": "PRINT_PROCESSING", "filename": r["filename"],
                                   "sliceLayerCount": "12", "curSliceLayer": "1", "printRemainTime": "3700"}
                elif cmd == "PRINT_PAUSE":
                    self.status["printStatus"] = "PRINT_PAUSE"
                else:
                    self.status = {"printStatus": "PRINT_STOP"}
                await reply(errorcode="0")
            else:
                await reply(errorcode="99")

    async def on_chunk(self, ws, upload, msg):
        if self.compress:
            (size,) = struct.unpack(">I", msg[:4])
            msg = zlib.decompress(msg[4:])
            assert len(msg) == size
        if not self.stall:
            upload["data"] += msg
        received = len(upload["data"])
        done = received >= upload["size"]
        if not (done and self.check_inline):
            # a reply for another transfer first: the client must skip it
            await ws.send(json.dumps({"cmd": "START_DATA", "key": "other", "received": "0"}))
            await ws.send(json.dumps({"cmd": "START_DATA", "key": upload["key"], "errorcode": "0",
                                      "received": str(received)}))
        if done:
            self.files[upload["name"]] = upload["data"]
            await ws.send(json.dumps({"cmd": "CHECK_DATA", "checkstate": str(self.checkstate)}))
        return upload
