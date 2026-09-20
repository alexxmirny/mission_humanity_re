"""Raw ASGI middleware enforcing a hard body-size cap in-process.

Caddy's `request_body { max_size 64MB }` (see ../Caddyfile) is the primary
edge guard in production. This middleware makes the collector ALSO refuse an
oversized body itself, so the app is safe when it is run directly behind
uvicorn with no Caddy in front -- which is exactly how this repo's tests and
`scripts/send_report.py` exercise it, and how RP2's done_when 413 clause is
proven on a machine without Docker.

We do not trust `Content-Length` alone (a client can omit it under chunked
transfer-encoding, or simply lie), so this buffers the incoming body itself
and aborts with 413 the moment the running total exceeds the cap -- before
any of it is ever handed to the FastAPI application.
"""

from __future__ import annotations

from starlette.types import ASGIApp, Message, Receive, Scope, Send


class MaxBodySizeMiddleware:
    def __init__(self, app: ASGIApp, max_bytes: int) -> None:
        self.app = app
        self.max_bytes = max_bytes

    async def __call__(self, scope: Scope, receive: Receive, send: Send) -> None:
        if scope["type"] != "http":
            await self.app(scope, receive, send)
            return

        buffered: list[Message] = []
        total = 0

        while True:
            message = await receive()
            if message["type"] != "http.request":
                buffered.append(message)
                break
            total += len(message.get("body", b""))
            if total > self.max_bytes:
                await self._reject(receive, message, send)
                return
            buffered.append(message)
            if not message.get("more_body", False):
                break

        state = {"i": 0}

        async def replay() -> Message:
            if state["i"] < len(buffered):
                msg = buffered[state["i"]]
                state["i"] += 1
                return msg
            return {"type": "http.request", "body": b"", "more_body": False}

        await self.app(scope, replay, send)

    @staticmethod
    async def _reject(receive: Receive, last_message: Message, send: Send) -> None:
        # Drain whatever is left of the client's stream so it doesn't hang
        # waiting for us to read a body we've already decided to refuse.
        more_body = last_message.get("more_body", False)
        while more_body:
            drained = await receive()
            more_body = drained.get("more_body", False)
        await send(
            {
                "type": "http.response.start",
                "status": 413,
                "headers": [(b"content-type", b"application/json")],
            }
        )
        await send(
            {
                "type": "http.response.body",
                "body": b'{"error":"request body too large"}',
            }
        )
