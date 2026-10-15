#!/usr/bin/env python3
"""Runs the flash probe (docs/TEST_PLAN.md section 6) on attached pyro boards.

For each board: forces it into BOOTSEL, loads the probe into RAM, runs the
tests over USB CDC, saves every line the probe answers to
docs/probe_results/, and reboots the board into its own firmware.

    run_probe.py --list
    run_probe.py [--board KIND|SERIAL] [--all] [--cycles N] [--selftest]

Boards are found on USB, never named in this file: only the flash layout of
each board kind is, mirrored from pyro's boards/<kind>/board.cmake.
"""
import argparse
import datetime
import json
import pathlib
import plistlib
import subprocess
import sys
import time

import serial
import serial.tools.list_ports

ROOT = pathlib.Path(__file__).resolve().parent.parent
UF2 = ROOT / "build" / "probe" / "alfs_probe.uf2"
RESULTS = ROOT / "docs" / "probe_results"

# kind: (PYRO_FLASH_SIZE_KB, PYRO_PFB_FS_KB). The test sectors are the last
# ones of the firmware-update download slot, which lies just below littlefs.
LAYOUT = {
    "mk1a": (16384, 8192),
    "mk1b": (2048, 984),
    "mk1b_8mb": (8192, 4096),
    "mk1c": (16384, 8192),
    "mk1c_sd": (16384, 8192),
}
SECTOR = 4096
PYRO_VID, PYRO_PID = 0x2E8A, 0x4002
BOOT_PID = 0x0003
PROBE_SERIAL = "E" * 16  # the SDK's unique ID in a build that runs from RAM


def usb_devices():
    out = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0", "-a"], capture_output=True, check=True).stdout
    found = []

    def walk(node):
        if isinstance(node, list):
            for n in node:
                walk(n)
        elif isinstance(node, dict):
            if "USB Product Name" in node:
                found.append(node)
            walk(node.get("IORegistryEntryChildren", []))

    walk(plistlib.loads(out))
    return found


def boards():
    out = []
    for d in usb_devices():
        name = d.get("USB Product Name", "")
        if d.get("idVendor") == PYRO_VID and d.get("idProduct") == PYRO_PID and name.startswith("Pyro "):
            out.append({"product": name, "kind": name[5:].lower(), "serial": d.get("USB Serial Number", ""),
                        "location": d["locationID"]})
    return out


def at_location(location, pid):
    for d in usb_devices():
        if d.get("locationID") == location and d.get("idVendor") == PYRO_VID and d.get("idProduct") == pid:
            return d
    return None


def wait_for(what, check, timeout):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        got = check()
        if got:
            return got
        time.sleep(0.25)
    raise RuntimeError(f"timed out waiting for {what}")


def probe_port():
    for p in serial.tools.list_ports.comports():
        if p.serial_number == PROBE_SERIAL:
            return p.device
    return None


def test_addr(kind, sectors):
    flash_kb, fs_kb = LAYOUT[kind]
    return (flash_kb - fs_kb) * 1024 - sectors * SECTOR


def picotool(*args, timeout=60):
    subprocess.run(["picotool", *args], check=True, capture_output=True, text=True, timeout=timeout)


def to_bootsel(board):
    bus = str(board["location"] >> 24)
    dev = at_location(board["location"], PYRO_PID)
    if not dev:
        raise RuntimeError(f"{board['product']} is gone from USB")
    try:
        picotool("reboot", "-u", "-f", "--vid", hex(PYRO_VID), "--pid", hex(PYRO_PID),
                 "--bus", bus, "--address", str(dev["USB Address"]), timeout=10)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired):
        pass  # picotool can fail, or hang, after the reset has gone through
    boot = wait_for("BOOTSEL", lambda: at_location(board["location"], BOOT_PID), 20)
    return bus, str(boot["USB Address"])


class Probe:
    def __init__(self, device, log):
        self.port = serial.Serial(device, timeout=1)
        self.log = log

    def send(self, line):
        self.port.write((line + "\n").encode())

    def answer(self, timeout):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            raw = self.port.readline().decode(errors="replace").strip()
            if not raw.startswith("{"):
                continue
            self.log.write(raw + "\n")
            self.log.flush()
            return json.loads(raw)
        raise RuntimeError("the probe stopped answering")


def run_board(board, args):
    kind = board["kind"]
    if kind not in LAYOUT and args.addr is None:
        raise RuntimeError(f"no flash layout known for {board['product']}; give --addr")
    if probe_port():
        raise RuntimeError("a probe is already running on some board: send it 'reboot' or replug that board")
    addr = args.addr if args.addr is not None else test_addr(kind, args.sectors)
    seed = args.seed if args.seed is not None else int(time.time()) & 0xFFFFFFFF
    print(f"{board['product']} ({board['serial']}): test sectors at {addr:#x}")

    bus, address = to_bootsel(board)
    picotool("load", "-x", str(UF2), "--bus", bus, "--address", address)
    device = wait_for("the probe's serial port", probe_port, 20)
    time.sleep(0.5)

    RESULTS.mkdir(parents=True, exist_ok=True)
    tmp = RESULTS / f".{kind}.partial"
    with open(tmp, "w") as log:
        when = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
        log.write(json.dumps({"host": {"product": board["product"], "kind": kind, "usb_serial": board["serial"],
                                       "date": when}}) + "\n")
        probe = Probe(device, log)
        try:
            probe.send("id")
            ident = probe.answer(10)
            command = f"run {addr:#x} {args.sectors} {args.cycles} {seed}" + (" selftest" if args.selftest else "")
            probe.send(command)
            while True:
                a = probe.answer(600)
                if "refused" in a:
                    raise RuntimeError(f"the probe refused: {a}")
                if "cycle" in a:
                    print(f"  cycle {a['cycle'] + 1}/{args.cycles}: {a['programs']} programs, "
                          f"{a['mismatches']} mismatches, {a['seconds']} s")
                if "mismatch" in a or "blank_failure" in a or "error" in a:
                    print(f"  {a}")
                if a.get("done"):
                    done = a
                    break
        finally:
            probe.send("reboot")
            probe.port.close()

    name = f"{ident['jedec']}_{kind}_{ident['uid'][-6:]}_{when[:10]}" + ("_selftest" if args.selftest else "")
    path = RESULTS / f"{name}.jsonl"
    tmp.rename(path)
    wait_for(f"{board['product']} back in its firmware", lambda: at_location(board["location"], PYRO_PID), 60)
    verdict = "PASS" if done["pass"] else "FAIL"
    print(f"  {verdict}: {done['mismatches']} mismatches, {done['programs']} programs, "
          f"{done['erases']} erases, {done['seconds']} s; {path.relative_to(ROOT)}")
    return done["pass"]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--list", action="store_true", help="list attached boards and their test sectors")
    ap.add_argument("--board", help="a board kind (mk1a, mk1b_8mb, ...) or USB serial")
    ap.add_argument("--all", action="store_true", help="every attached board, one after another")
    ap.add_argument("--sectors", type=int, default=4)
    ap.add_argument("--cycles", type=int, default=20)
    ap.add_argument("--seed", type=lambda s: int(s, 0))
    ap.add_argument("--addr", type=lambda s: int(s, 0), help="test sectors' address, overriding the layout")
    ap.add_argument("--selftest", action="store_true", help="plant one mismatch; the run must report it")
    args = ap.parse_args()

    found = boards()
    if args.list or not found:
        for b in found:
            where = f"{test_addr(b['kind'], args.sectors):#x}" if b["kind"] in LAYOUT else "unknown layout"
            print(f"{b['product']:16} {b['serial']:14} test sectors at {where}")
        if not found:
            print("no pyro board on USB")
        return 0 if found else 1
    if args.board:
        found = [b for b in found if args.board.lower() in (b["kind"], b["serial"].lower())]
    if not args.all and len(found) != 1:
        print(f"{len(found)} boards match: name one with --board, or give --all", file=sys.stderr)
        return 2
    if not UF2.exists():
        print("build the probe first: make probe", file=sys.stderr)
        return 2
    results = []
    for b in found:
        try:
            results.append(run_board(b, args))
        except (RuntimeError, subprocess.CalledProcessError, subprocess.TimeoutExpired) as e:
            print(f"  {b['product']} ({b['serial']}): {e}", file=sys.stderr)
            results.append(False)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
