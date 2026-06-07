#!/usr/bin/env python3
"""Small PC-side DeepSeek proxy for ESP-LEGO.

Run this on the computer while it is connected to the ESP SoftAP. The ESP calls
http://192.168.4.2:18082/v1/chat/completions over the SoftAP, while this proxy
uses the computer's normal internet connection to reach DeepSeek.

The API key is never logged. It can come from DEEPSEEK_API_KEY, or from the
Authorization header already stored in the ESP web console config.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class ProxyConfig:
    upstream_base = "https://api.deepseek.com"
    timeout = 60
    default_model = "deepseek-chat"


def upstream_url() -> str:
    return ProxyConfig.upstream_base.rstrip("/") + "/chat/completions"


class DeepSeekProxyHandler(BaseHTTPRequestHandler):
    server_version = "ESPDeepSeekProxy/1.0"

    def log_message(self, fmt: str, *args: object) -> None:
        sys.stdout.write("%s - %s\n" % (self.address_string(), fmt % args))
        sys.stdout.flush()

    def _send_json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path in ("/", "/health", "/v1/health"):
            self._send_json(200, {"status": "ok", "upstream": ProxyConfig.upstream_base})
            return
        self._send_json(404, {"error": "not found"})

    def do_POST(self) -> None:
        if not (self.path.endswith("/chat/completions") or self.path == "/chat/completions"):
            self._send_json(404, {"error": "expected /chat/completions"})
            return

        try:
            content_len = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_json(400, {"error": "bad content length"})
            return

        body = self.rfile.read(content_len)
        try:
            payload = json.loads(body.decode("utf-8"))
        except json.JSONDecodeError as exc:
            self._send_json(400, {"error": "bad json", "detail": str(exc)})
            return

        if not payload.get("model"):
            payload["model"] = ProxyConfig.default_model
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")

        api_key = os.environ.get("DEEPSEEK_API_KEY", "").strip()
        auth_header = "Bearer " + api_key if api_key else self.headers.get("Authorization", "")
        if not auth_header:
            self._send_json(401, {"error": "missing authorization"})
            return

        req = urllib.request.Request(
            upstream_url(),
            data=body,
            method="POST",
            headers={
                "Content-Type": "application/json",
                "Authorization": auth_header,
            },
        )

        try:
            with urllib.request.urlopen(req, timeout=ProxyConfig.timeout) as resp:
                upstream_body = resp.read()
                status = resp.status
                content_type = resp.headers.get("Content-Type", "application/json")
        except urllib.error.HTTPError as exc:
            upstream_body = exc.read()
            status = exc.code
            content_type = exc.headers.get("Content-Type", "application/json")
        except Exception as exc:
            self._send_json(502, {"error": "upstream request failed", "detail": str(exc)})
            return

        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(upstream_body)))
        self.end_headers()
        self.wfile.write(upstream_body)
        self.log_message("POST %s -> %d (%d bytes)", self.path, status, len(upstream_body))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="ESP-LEGO DeepSeek local proxy")
    parser.add_argument("--host", default="0.0.0.0", help="listen address")
    parser.add_argument("--port", default=18082, type=int, help="listen port")
    parser.add_argument("--upstream", default=ProxyConfig.upstream_base, help="DeepSeek API base URL")
    parser.add_argument("--timeout", default=ProxyConfig.timeout, type=int, help="upstream timeout seconds")
    parser.add_argument("--default-model", default=ProxyConfig.default_model, help="model used if ESP sends an empty model")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    ProxyConfig.upstream_base = args.upstream
    ProxyConfig.timeout = args.timeout
    ProxyConfig.default_model = args.default_model

    server = ThreadingHTTPServer((args.host, args.port), DeepSeekProxyHandler)
    print(f"DeepSeek proxy listening on http://{args.host}:{args.port}/v1")
    print(f"Forwarding to {upstream_url()}")
    print("Set ESP LLM Base URL to http://192.168.4.2:%d/v1" % args.port)
    sys.stdout.flush()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("Stopping proxy")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
