#!/usr/bin/env python3
"""ffremote.py - drive a FlashFloppy ESP32 Remote gadget from the CLI.

Uploads a disk image to the gadget over its existing HTTP API,
optionally selects it, and optionally power-cycles the host machine
(the gadget's Kasa power control). No third-party packages: Python 3
standard library only.

Examples:
    ffremote.py CPM816.HFE --host 192.168.0.50
    ffremote.py CPM816.HFE --host 192.168.0.50 --select --power
    ffremote.py --host 192.168.0.50            (status only)

The sequence mirrors the web UI exactly: eject (uploads only run
while the drive is ejected), upload the raw file body, then either
reinsert (upload only) or select the new image. With --select and
--power together, the power-cycle rides along with the select (the
firmware restarts the machine once the swap has landed); with
--power alone, a restart is sent after the card is handed back.

Exit status: 0 on success, 1 on any refusal or failure (the
gadget's own error text is printed), 2 on usage errors.
"""

import argparse
import http.client
import json
import os
import re
import sys
import time
from urllib.parse import quote

RESTING = ("idle", "presented")


class GadgetError(Exception):
    def __init__(self, status, text):
        self.status = status
        super().__init__(text or f"HTTP {status}")


def _clean(text):
    """ESP httpd error pages are small HTML documents carrying the
    message as text; plain-text replies (our 409s) pass through."""
    if "<" in text and ">" in text:
        text = re.sub(r"<[^>]+>", " ", text)
        text = re.sub(r"\s+", " ", text).strip()
    return text


class Gadget:
    def __init__(self, host, timeout=15):
        h = host.strip()
        if "://" in h:
            h = h.split("://", 1)[1]
        h = h.rstrip("/")
        if ":" in h:
            self.host, port = h.rsplit(":", 1)
            self.port = int(port)
        else:
            self.host, self.port = h, 80
        self.timeout = timeout

    def _conn(self, timeout=None):
        return http.client.HTTPConnection(
            self.host, self.port, timeout=timeout or self.timeout)

    def request(self, method, path, body=None, headers=None):
        conn = self._conn()
        try:
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            return resp.status, resp.read()
        finally:
            conn.close()

    def get_json(self, path):
        code, data = self.request("GET", path)
        if code != 200:
            raise GadgetError(code, _clean(data.decode("utf-8", "replace")))
        return json.loads(data.decode("utf-8"))

    def post_json(self, path, obj=None):
        body = json.dumps(obj).encode() if obj is not None else b""
        code, data = self.request(
            "POST", path, body=body,
            headers={"Content-Type": "application/json"})
        if code != 200:
            raise GadgetError(code, _clean(data.decode("utf-8", "replace")))
        return json.loads(data.decode("utf-8")) if data else {}

    def status(self):
        return self.get_json("/api/status")

    def upload(self, name, filepath, size):
        """POST the file as the raw request body, like the web UI."""
        conn = self._conn(timeout=120)
        try:
            conn.putrequest("POST", "/api/upload?name=" + quote(name))
            conn.putheader("Content-Type", "application/octet-stream")
            conn.putheader("Content-Length", str(size))
            conn.endheaders()
            sent = 0
            last_pct = -1
            with open(filepath, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    conn.send(chunk)
                    sent += len(chunk)
                    pct = int(sent * 100 / size) if size else 100
                    if pct >= last_pct + 10 or pct == 100:
                        print(f"    {pct}% ({sent}/{size} bytes)")
                        last_pct = pct
            resp = conn.getresponse()
            data = resp.read()
        finally:
            conn.close()
        if resp.status != 200:
            raise GadgetError(resp.status,
                              _clean(data.decode("utf-8", "replace")))
        return json.loads(data.decode("utf-8")) if data else {}


def wait_until(g, pred, what, timeout, force=False):
    """Poll /api/status until pred(status) holds. A gadget error
    that settles is raised with the gadget's own text. With
    force=True, a swap stuck waiting for the Gotek to go idle is
    forced once (/api/force) instead of waiting it out."""
    deadline = time.time() + timeout
    forced = False
    while True:
        st = g.status()
        if pred(st):
            return st
        if st.get("error") and st.get("state") in RESTING:
            raise GadgetError(0, f"gadget reported: {st['error']}")
        if force and st.get("waiting_usb") and not forced:
            g.post_json("/api/force")
            print("    (Gotek still busy - forcing the swap, as asked)")
            forced = True
        if time.time() >= deadline:
            raise GadgetError(
                0, f"timed out after {timeout}s waiting for {what} "
                   f"(state={st.get('state')}, "
                   f"current={st.get('current')!r})")
        time.sleep(0.4)


def human(n):
    for unit in ("B", "k", "M", "G"):
        if n < 1000 or unit == "G":
            return f"{n:.3g}{unit}" if unit != "B" else f"{n}B"
        n /= 1000.0
    return f"{n}G"


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Upload a disk image to a FlashFloppy ESP32 "
                    "Remote gadget, optionally select it and "
                    "optionally power-cycle the host machine.")
    ap.add_argument("file", nargs="?",
                    help="local image file to upload (omit for a "
                         "status-only run)")
    ap.add_argument("--host",
                    default=os.environ.get("FFREMOTE_HOST",
                                           "192.168.4.1"),
                    help="gadget host or IP (default: $FFREMOTE_HOST "
                         "or 192.168.4.1, its access-point address; "
                         "on your home LAN use its station address)")
    ap.add_argument("--name",
                    help="name for the file on the card "
                         "(default: the local file's basename)")
    ap.add_argument("--select", action="store_true",
                    help="select the uploaded image after uploading")
    ap.add_argument("--power", action="store_true",
                    help="power-cycle the host machine (with "
                         "--select it rides along with the swap; "
                         "alone it is sent after reinsert)")
    ap.add_argument("--force", action="store_true",
                    help="if a swap waits for the Gotek to go idle, "
                         "force it once instead of waiting (a write "
                         "in progress could be lost - your call)")
    ap.add_argument("--timeout", type=int, default=180,
                    help="seconds to wait for each stage "
                         "(default: 180)")
    args = ap.parse_args(argv)

    g = Gadget(args.host)
    try:
        st = g.status()
    except Exception as e:
        print(f"cannot reach the gadget at {args.host}: {e}",
              file=sys.stderr)
        print("check the address - 192.168.4.1 is its access point; "
              "on your LAN it has a different (station) address.",
              file=sys.stderr)
        return 1
    print(f"gadget at {args.host}: state={st.get('state')}, "
          f"current={st.get('current')!r}, wifi={st.get('wifi')!r}")

    if not args.file:
        try:
            pw = g.get_json("/api/power")
            if pw.get("configured"):
                print(f"power: {pw.get('name')!r} "
                      f"on={pw.get('on')} watts={pw.get('watts')}")
        except GadgetError:
            pass
        return 0

    if not os.path.isfile(args.file):
        print(f"no such file: {args.file}", file=sys.stderr)
        return 2
    name = args.name or os.path.basename(args.file)
    size = os.path.getsize(args.file)

    try:
        # 1. Eject (uploads only run in the ejected state).
        if st.get("ejected"):
            print("drive already ejected")
        else:
            print("ejecting the drive...")
            g.post_json("/api/eject")
            wait_until(g, lambda s: s.get("ejected")
                       and not s.get("uploading"),
                       "the drive to eject", args.timeout)

        # 2. Upload (raw body, streamed).
        print(f"uploading {name} ({human(size)})...")
        res = g.upload(name, args.file, size)
        if res.get("reboot_needed"):
            print("    note: this config file is read at boot - "
                  "the gadget needs a restart to use it")

        # 3. What actually landed on the card?
        lib = g.get_json("/api/library")
        entry = next((f for f in lib.get("files", [])
                      if f.get("name", "").lower() == name.lower()),
                     None)
        if entry is None:
            raise GadgetError(
                0, f"upload reported success but {name!r} is not in "
                   f"the gadget's library listing")
        stored = entry["path"]
        if entry.get("kind") != "image" and args.select:
            raise GadgetError(
                0, f"{name!r} landed as a {entry.get('kind')} file, "
                   f"not a selectable image")
        print(f"on the card as {stored!r} "
              f"({human(entry.get('size', 0))}, {entry.get('kind')})")

        # 4. Select it, or just hand the card back.
        if args.select:
            print(f"selecting {stored!r}"
                  + (" (power-cycle after the swap)" if args.power
                     else "") + "...")
            g.post_json("/api/select",
                        {"path": stored, "restart": bool(args.power)})
            wait_until(g, lambda s: s.get("presented")
                       and s.get("state") in RESTING
                       and s.get("current") == stored,
                       "the swap to complete", args.timeout,
                       force=args.force)
            print(f"selected: {stored!r} is now the current image")
        else:
            print("handing the card back to the Gotek...")
            g.post_json("/api/reinsert")
            wait_until(g, lambda s: s.get("presented")
                       and s.get("state") in RESTING,
                       "the card to be presented again", args.timeout)

        # 5. Standalone power-cycle (with --select it already rode
        #    along; here it was asked for on its own).
        if args.power and not args.select:
            print("power-cycling the host machine...")
            g.post_json("/api/power", {"action": "restart"})
            deadline = time.time() + 30
            while time.time() < deadline:
                pw = g.get_json("/api/power")
                if pw.get("error"):
                    raise GadgetError(0, "power: " + pw["error"])
                if not pw.get("busy") and not pw.get("restarting"):
                    break
                time.sleep(0.5)
            print("power-cycle sent")
        elif args.power and args.select:
            print("power-cycle was requested with the select - the "
                  "gadget restarts the machine as the swap lands")

    except GadgetError as e:
        print(f"failed: {e}", file=sys.stderr)
        return 1
    print("done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
