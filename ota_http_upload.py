#!/usr/bin/env python3
"""HTTP OTA upload — SoftAP (192.168.4.1) or LAN STA (OTA_HTTP_URL / mDNS)."""
import os
import re
import socket
import subprocess
import sys
import urllib.error
import urllib.request
from urllib.parse import urlparse

SOFTAP_SSID = "RenFest-Counter"
SOFTAP_PREFIX = "192.168.4."
SOFTAP_URL = "http://192.168.4.1/update"
MDNS_URL = "http://ren-fest-counter.local/update"


def _wifi_ipv4s():
    """IPv4 addresses on Wi-Fi / WLAN adapters (Windows)."""
    ps = (
        "Get-NetIPAddress -AddressFamily IPv4 | "
        "Where-Object { "
        "  $_.InterfaceAlias -match 'Wi-?Fi|WLAN|Wireless' "
        "  -and $_.InterfaceAlias -notmatch 'vEthernet|Virtual|Ethernet' "
        "  -and $_.IPAddress -notlike '169.254.*' "
        "} | Select-Object -ExpandProperty IPAddress"
    )
    try:
        out = subprocess.check_output(
            ["powershell", "-NoProfile", "-Command", ps],
            stderr=subprocess.DEVNULL,
            text=True,
            timeout=10,
        )
    except (subprocess.SubprocessError, OSError, FileNotFoundError):
        return []
    return [
        line.strip()
        for line in out.splitlines()
        if re.match(r"\d+\.\d+\.\d+\.\d+$", line.strip())
    ]


def _probe_device(base_url: str, timeout_s: float = 2.0) -> bool:
    """Quick GET to device root to confirm the ESP is reachable."""
    try:
        p = urlparse(base_url)
        probe = f"{p.scheme}://{p.netloc}/"
    except Exception:
        probe = "http://192.168.4.1/"

    try:
        req = urllib.request.Request(probe, method="GET")
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            return resp.status < 500
    except Exception:
        try:
            host = urlparse(base_url).hostname or "192.168.4.1"
            port = urlparse(base_url).port or 80
            with socket.create_connection((host, port), timeout=timeout_s):
                return True
        except Exception:
            return False


def resolve_upload_url():
    """Prefer SoftAP when joined; else OTA_HTTP_URL; else mDNS hostname."""
    wifi = _wifi_ipv4s()
    on_softap = [ip for ip in wifi if ip.startswith(SOFTAP_PREFIX)]
    env_url = os.environ.get("OTA_HTTP_URL", "").strip()

    if on_softap:
        print(f"[ota-http] SoftAP client IP: {on_softap[0]}")
        if _probe_device(SOFTAP_URL):
            return SOFTAP_URL
        print(
            f"[ota-http] SoftAP joined but {SOFTAP_URL} not reachable.",
            file=sys.stderr,
        )

    candidates = []
    if env_url:
        candidates.append(env_url)
    candidates.append(MDNS_URL)

    for url in candidates:
        label = "OTA_HTTP_URL" if url == env_url else "mDNS"
        print(f"[ota-http] trying {label}: {url}")
        if _probe_device(url, timeout_s=3.0):
            return url

    print(
        f"[ota-http] Device not reachable. Join SoftAP '{SOFTAP_SSID}', or set "
        f"OTA_HTTP_URL=http://<esp-ip>/update (e.g. http://10.0.0.92/update).",
        file=sys.stderr,
    )
    return None


def main():
    if len(sys.argv) < 2:
        print("usage: ota_http_upload.py <firmware.bin>", file=sys.stderr)
        return 1

    path = sys.argv[1]
    if not os.path.isfile(path):
        print(f"missing firmware: {path}", file=sys.stderr)
        return 1

    url = resolve_upload_url()
    if not url:
        return 1

    boundary = "----RenFestOTABoundary7MA4YWxkTrZu0gW"
    filename = os.path.basename(path)

    with open(path, "rb") as f:
        blob = f.read()

    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="firmware"; filename="{filename}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode("utf-8") + blob + f"\r\n--{boundary}--\r\n".encode("utf-8")

    req = urllib.request.Request(
        url,
        data=body,
        method="POST",
        headers={
            "Content-Type": f"multipart/form-data; boundary={boundary}",
            "Content-Length": str(len(body)),
        },
    )

    print(f"[ota-http] uploading {path} ({len(blob)} bytes) -> {url}")
    try:
        with urllib.request.urlopen(req, timeout=120) as resp:
            text = resp.read().decode("utf-8", errors="replace")
            print(f"[ota-http] {resp.status}: {text}")
            return 0 if resp.status == 200 else 1
    except urllib.error.HTTPError as e:
        print(
            f"[ota-http] HTTP {e.code}: {e.read().decode('utf-8', errors='replace')}",
            file=sys.stderr,
        )
        return 1
    except Exception as e:
        print(f"[ota-http] failed: {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
