# Force espota to use a Wi-Fi interface IP (ignore wired Ethernet).
# SoftAP clients are on 192.168.4.x — prefer that when present.
Import("env")

import re
import subprocess


def _ips_from_powershell():
    # Wi-Fi / WLAN aliases only — skip Ethernet, vEthernet, etc.
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
    return [line.strip() for line in out.splitlines() if re.match(r"\d+\.\d+\.\d+\.\d+$", line.strip())]


def pick_host_ip():
    ips = _ips_from_powershell()
    softap = [ip for ip in ips if ip.startswith("192.168.4.")]
    if softap:
        return softap[0]
    if ips:
        return ips[0]
    return None


host = pick_host_ip()
flags = list(env.get("UPLOAD_FLAGS", []))
# Drop any prior --host_ip pair
cleaned = []
skip = False
for f in flags:
    if skip:
        skip = False
        continue
    if f == "--host_ip":
        skip = True
        continue
    cleaned.append(f)

if host and host.startswith("192.168.4."):
    cleaned.extend(["--host_ip", host])
    print(f"[ota] SoftAP connected — host_ip={host}")
elif host:
    print(
        f"[ota] WARNING: Wi-Fi IP is {host}, not SoftAP 192.168.4.x. "
        f"Join 'RenFest-Counter' before upload (ota_http_upload.py will refuse)."
    )
    cleaned.extend(["--host_ip", host])
else:
    print(
        "[ota] WARNING: no Wi-Fi IPv4 found. Join SoftAP 'RenFest-Counter' "
        "on this PC so you get 192.168.4.x, then retry."
    )

env.Replace(UPLOAD_FLAGS=cleaned)
