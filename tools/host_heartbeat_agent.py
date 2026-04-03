#!/usr/bin/env python3
import argparse
import getpass
import json
import socket
import sys
import time
import urllib.parse
import urllib.request
from pathlib import Path


def read_uptime_seconds() -> int:
    with open("/proc/uptime", "r", encoding="utf-8") as handle:
        return int(float(handle.read().split()[0]))


def read_memory_percent() -> float:
    meminfo = {}
    with open("/proc/meminfo", "r", encoding="utf-8") as handle:
        for line in handle:
            key, value = line.split(":", 1)
            meminfo[key] = int(value.strip().split()[0])

    total = meminfo.get("MemTotal", 0)
    available = meminfo.get("MemAvailable", 0)
    if total <= 0:
        return 0.0
    return round((1.0 - (available / total)) * 100.0, 1)


def read_cpu_times() -> tuple[int, int]:
    with open("/proc/stat", "r", encoding="utf-8") as handle:
        fields = handle.readline().split()

    values = [int(value) for value in fields[1:]]
    idle = values[3] + values[4]
    total = sum(values)
    return idle, total


def cpu_percent(previous: tuple[int, int] | None) -> tuple[float, tuple[int, int]]:
    if previous is None:
        previous = read_cpu_times()
        time.sleep(0.2)
        current = read_cpu_times()
    else:
        current = read_cpu_times()

    idle_delta = current[0] - previous[0]
    total_delta = current[1] - previous[1]
    if total_delta <= 0:
        return 0.0, current

    busy = 1.0 - (idle_delta / total_delta)
    return round(max(0.0, busy * 100.0), 1), current


def build_payload(previous_cpu: tuple[int, int] | None) -> tuple[dict, tuple[int, int]]:
    cpu_pct, current_cpu = cpu_percent(previous_cpu)
    payload = {
        "schema": "rpc.host.v1",
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": "linux",
        "host": socket.gethostname(),
        "user": getpass.getuser(),
        "cpu_pct": cpu_pct,
        "memory_pct": read_memory_percent(),
        "uptime_s": read_uptime_seconds(),
        "python": ".".join(str(part) for part in sys.version_info[:3]),
    }
    return payload, current_cpu


def send_payload(base_url: str, auth_token: str, source: str, payload: dict, timeout_seconds: int) -> None:
    body = urllib.parse.urlencode(
        {
            "source": source,
            "payload": json.dumps(payload, separators=(",", ":")),
        }
    ).encode("utf-8")

    request = urllib.request.Request(
        urllib.parse.urljoin(base_url.rstrip("/") + "/", "api/report"),
        data=body,
        method="POST",
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    if auth_token:
        request.add_header("X-Auth-Token", auth_token)

    with urllib.request.urlopen(request, timeout=timeout_seconds):
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Send structured host payloads to the ESP32.")
    parser.add_argument("--esp32-base-url", required=True)
    parser.add_argument("--auth-token", default="")
    parser.add_argument("--source", default="linux-agent")
    parser.add_argument("--interval-seconds", type=int, default=5)
    parser.add_argument("--timeout-seconds", type=int, default=5)
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()

    if not Path("/proc").exists():
        print("warning: /proc not found; this script is intended for Debian/Linux hosts", flush=True)
        return 1

    previous_cpu = None
    while True:
        payload, previous_cpu = build_payload(previous_cpu)
        try:
            send_payload(args.esp32_base_url, args.auth_token, args.source, payload, args.timeout_seconds)
            print(
                "[{time}] payload sent source={source} host={host} cpu={cpu}% mem={mem}%".format(
                    time=time.strftime("%H:%M:%S"),
                    source=args.source,
                    host=payload["host"],
                    cpu=payload["cpu_pct"],
                    mem=payload["memory_pct"],
                ),
                flush=True,
            )
        except Exception as exc:  # noqa: BLE001
            print(f"[{time.strftime('%H:%M:%S')}] warning: {exc}", flush=True)

        if args.once:
            return 0
        time.sleep(args.interval_seconds)


if __name__ == "__main__":
    raise SystemExit(main())
