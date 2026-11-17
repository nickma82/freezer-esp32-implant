#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyserial>=3.5", "aioesphomeapi>=40"]
# ///
"""Testbench: freezer emulator (ESP8266, serial console) <-> ESP32-C3 implant (ESPHome API).

  testbench.py run [--suite emu2c3|c32emu|resilience|all] [--only NAME ...]   run test cases, write a report
  testbench.py list                                                  list test cases
  testbench.py state                                                 show the C3's entity states + emulator status
  testbench.py emu "a 1"                                             send one emulator console command
  testbench.py set "Set-point" -20                                   set one C3 entity

Exit code: 0 = all passed, 1 = a test failed, 2 = the bench is not ready (preflight).
"""

import argparse
import asyncio
import json
import sys
import time
from datetime import datetime
from pathlib import Path

from c3_api import C3Api, api_key_from_secrets
from cases import CASES, C3Log, Emu, EmuLine, NoEmuLine, RestartC3, Set, State
from emu_console import EmuConsole

REPO = Path(__file__).resolve().parents[2]
DEFAULT_SECRETS = REPO / "src/esp32-c3-supermini--esphome/secrets.yaml"
HEALTH_SENSORS = ["CRC errors", "Echo mismatches", "Missed slots"]


# ---- bench connection -------------------------------------------------------------------------------------------

class Bench:
    def __init__(self, args):
        self.args = args
        self.emu = EmuConsole(args.emu_port)
        self.c3 = C3Api(args.host, api_key_from_secrets(Path(args.secrets)))

    async def __aenter__(self):
        self.emu.open()
        await self.c3.connect()
        await asyncio.sleep(1.0)  # initial states arrive right after subscribing
        return self

    async def __aexit__(self, *exc):
        await self.c3.disconnect()
        self.emu.close()

    async def emu_cmd(self, cmd: str) -> str:
        return await asyncio.to_thread(self.emu.send_any, cmd)

    async def emu_status(self, timeout: float = 21.0) -> dict | None:
        """The next emulator S line (printed every 10 s)."""
        since = self.emu.mark()
        if await asyncio.to_thread(self.emu.wait_for, r"^S ", timeout, since):
            return self.emu.last_status(since)
        return None


# ---- preflight --------------------------------------------------------------------------------------------------

async def preflight(b: Bench) -> list[str]:
    """Problems that make testing pointless; empty = ready."""
    problems = []
    st = await b.emu_status()
    if st is None:
        problems.append(f"no status line from the emulator on {b.args.emu_port} (wrong port, port busy, emulator not running?)")
    else:
        if st.get("master") != "on":
            problems.append("emulator master is stopped (press g on its console)")
        if st.get("hs") != "running":
            problems.append(f"emulator handshake is '{st.get('hs')}', not 'running': the C3 does not answer in slot 02")
        if st.get("box_answers", "0") == "0":
            problems.append("emulator heard no C3 answers in the last 10 s (box_answers=0)")
    c3 = b.c3
    await c3.wait_state("Start-up phase", "running", 20.0)  # after an emulator restart the start-up takes ~10 s
    if c3.state("Bus connected") is not True:
        problems.append("C3: Bus connected is not on (no slot 02 seen for 2 s)")
    if c3.state("Bus transmit") is not True:
        problems.append("C3: Bus transmit is off (switched off after an echo mismatch? check its log)")
    if c3.state("Start-up phase") != "running":
        problems.append(f"C3: start-up phase is '{c3.state('Start-up phase')}', not 'running'")
    return problems


# ---- running cases ----------------------------------------------------------------------------------------------

async def do_steps(b: Bench, steps: list) -> None:
    for s in steps:
        if isinstance(s, Emu):
            await b.emu_cmd(s.cmd)
        elif isinstance(s, Set):
            b.c3.set(s.entity, s.value)
        elif isinstance(s, RestartC3):
            await b.c3.restart()
        else:
            raise TypeError(s)


async def check(b: Bench, c, timeout: float, emu_since: int, log_since: int) -> dict:
    t0 = time.time()
    timeout = getattr(c, "timeout", None) or timeout
    if isinstance(c, State):
        ok = await b.c3.wait_state(c.entity, c.value, timeout)
        got = b.c3.state(c.entity)
        text, evidence = f"C3 '{c.entity}' == {c.value!r}", f"is {got!r}"
    elif isinstance(c, EmuLine):
        m = await asyncio.to_thread(b.emu.wait_for, c.pattern, timeout, emu_since)
        ok = m is not None
        text, evidence = f"emulator line /{c.pattern}/", m.string if m else "not seen"
    elif isinstance(c, C3Log):
        line = await b.c3.wait_log(c.pattern, timeout, log_since)
        ok = line is not None
        text, evidence = f"C3 log /{c.pattern}/", line or "not seen"
    elif isinstance(c, NoEmuLine):
        m = b.emu.find(c.pattern, emu_since)
        ok = m is None
        text, evidence = f"no emulator line /{c.pattern}/", m.string if m else "none seen"
    else:
        raise TypeError(c)
    return {"check": text, "passed": ok, "evidence": evidence, "seconds": round(time.time() - t0, 1)}


async def phase(b: Bench, steps: list, checks: list, timeout: float) -> tuple[list[dict], str | None]:
    emu_since, log_since = b.emu.mark(), b.c3.log_mark()
    try:
        await do_steps(b, steps)
    except Exception as e:  # an unacked console command, unknown entity, ...
        return [], f"{type(e).__name__}: {e}"
    return [await check(b, c, timeout, emu_since, log_since) for c in checks], None


async def run_case(b: Bench, case, timeout: float) -> dict:
    print(f"--- {case.name} ({case.direction}): {case.what}", flush=True)
    res = {"name": case.name, "direction": case.direction, "what": case.what}
    res["checks"], res["error"] = await phase(b, case.act, case.expect, timeout)
    res["restore_checks"], res["restore_error"] = await phase(b, case.restore, case.restore_expect, timeout)
    res["passed"] = not res["error"] and all(c["passed"] for c in res["checks"])
    res["restored"] = not res["restore_error"] and all(c["passed"] for c in res["restore_checks"])
    for label, err, checks in (("act", res["error"], res["checks"]), ("restore", res["restore_error"], res["restore_checks"])):
        if err:
            print(f"    {label}: ERROR {err}")
        for c in checks:
            print(f"    {label}: {'ok  ' if c['passed'] else 'FAIL'} {c['check']}  [{c['evidence']}] {c['seconds']}s")
    return res


def health_snapshot(b: Bench) -> dict:
    return {n: b.c3.state(n) for n in HEALTH_SENSORS}


def health_delta(before: dict, after: dict) -> dict:
    """Increase of each C3 counter during the run (the counters are cumulative since the C3 booted).
    A counter below its start value means the C3 rebooted during the run: then its value since that reboot counts."""
    delta = {}
    for n in before:
        b, a = before[n] or 0, after[n] or 0
        delta[n] = a - b if a >= b else a
    return delta


def emu_health(b: Bench, since: int) -> dict:
    """Sums of the per-10-s emulator counters and the change of the cumulative ones over the run."""
    sums = {"box_bad": 0, "echo_err": 0, "framing": 0}
    core_bad = []
    for _, line in b.emu.lines[since:]:
        if line.startswith("S "):
            kv = dict(x.split("=", 1) for x in line.split()[2:] if "=" in x)
            for k in sums:
                sums[k] += int(kv.get(k, 0))
            core_bad.append(int(kv.get("core_bad_crc", 0)))
    sums["core_bad_crc_delta"] = core_bad[-1] - core_bad[0] if len(core_bad) > 1 else 0
    sums["E_lines"] = [l for _, l in b.emu.lines[since:] if l.startswith("E ")]
    return sums


def select_cases(args) -> list:
    cases = [c for c in CASES if args.suite in ("all", c.direction)]
    if args.only:
        unknown = set(args.only) - {c.name for c in CASES}
        if unknown:
            sys.exit(f"unknown case(s): {', '.join(sorted(unknown))}")
        cases = [c for c in cases if c.name in args.only]
    return cases


def write_report(out: Path, report: dict, b: Bench) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / "report.json").write_text(json.dumps(report, indent=2, default=str))
    (out / "emulator.log").write_text("\n".join(l for _, l in b.emu.lines) + "\n")
    (out / "c3.log").write_text("\n".join(l for _, l in b.c3.logs) + "\n")


def print_summary(report: dict) -> None:
    print("\n=== summary")
    for r in report["results"]:
        flag = "PASS" if r["passed"] else "FAIL"
        note = "" if r["restored"] else "  (restore FAILED: bench may not be at defaults)"
        print(f"  {flag}  {r['direction']:7} {r['name']}{note}")
    h = report["health"]
    print(f"  C3 counters during the run: {h['c3_delta']}  (totals since C3 boot: {h['c3_after']})")
    e = h["emulator"]
    print(f"  emulator: box_bad={e['box_bad']} echo_err={e['echo_err']} framing={e['framing']} "
          f"core_bad_crc+={e['core_bad_crc_delta']} E-lines={len(e['E_lines'])}")
    restores = f", {report['restore_failed']} restore(s) FAILED" if report["restore_failed"] else ""
    print(f"  {report['passed']}/{report['total']} passed{restores}; report in {report['out']}")


async def cmd_run(args) -> int:
    cases = select_cases(args)
    out = Path(args.out or REPO / "tools/testbench/results" / datetime.now().strftime("%Y-%m-%d_%H%M%S"))
    async with Bench(args) as b:
        problems = await preflight(b)
        start = b.emu.mark()  # after the preflight's S line: the emulator counters below cover only the run
        report = {"started": datetime.now().isoformat(timespec="seconds"), "out": str(out),
                  "preflight": problems, "results": []}
        if problems:
            print("bench NOT ready:\n  " + "\n  ".join(problems))
            write_report(out, report, b)
            return 2
        print("preflight ok: emulator hs=running, C3 connected/transmitting/running", flush=True)
        before = health_snapshot(b)
        for case in cases:
            report["results"].append(await run_case(b, case, args.timeout))
        await b.emu_status()  # one more S line so the emulator counters cover the last case
        after = health_snapshot(b)
        report["health"] = {"c3_before": before, "c3_after": after, "c3_delta": health_delta(before, after),
                            "emulator": emu_health(b, start)}
        report["total"] = len(cases)
        report["passed"] = sum(r["passed"] for r in report["results"])
        report["restore_failed"] = sum(not r["restored"] for r in report["results"])
        write_report(out, report, b)
        print_summary(report)
        return 0 if report["passed"] == report["total"] and not report["restore_failed"] else 1


async def cmd_state(args) -> int:
    async with Bench(args) as b:
        for name in sorted(b.c3.by_name):
            print(f"  {name:28} {b.c3.states.get(name)!r}")
        print(f"emulator: {await b.emu_status()}")
    return 0


async def cmd_emu(args) -> int:
    emu = EmuConsole(args.emu_port)
    emu.open()
    try:
        print(emu.send_any(args.command))
    finally:
        emu.close()
    return 0


async def cmd_set(args) -> int:
    async with Bench(args) as b:
        value = {"on": True, "off": False}.get(args.value.lower())
        b.c3.set(args.entity, value if value is not None else float(args.value))
        await asyncio.sleep(1.0)
        line = await b.c3.wait_log(r"write (confirmed|NOT confirmed|not sent)|refused", 6.0)
        print(line or "no write result logged within 6 s")
        print(f"{args.entity} = {b.c3.state(args.entity)!r}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--emu-port", default="/dev/ttyUSB0", help="emulator console (57600 baud)")
    p.add_argument("--host", default="freezer-implant.local", help="C3 ESPHome API host")
    p.add_argument("--secrets", default=str(DEFAULT_SECRETS), help="secrets.yaml with api_key")
    sub = p.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("run")
    r.add_argument("--suite", choices=["emu2c3", "c32emu", "resilience", "all"], default="all")
    r.add_argument("--only", nargs="+", metavar="NAME")
    r.add_argument("--timeout", type=float, default=20.0, help="seconds per check (C3 reads every 10 s)")
    r.add_argument("--out", help="report directory (default tools/testbench/results/<timestamp>)")
    sub.add_parser("list")
    sub.add_parser("state")
    e = sub.add_parser("emu")
    e.add_argument("command")
    s = sub.add_parser("set")
    s.add_argument("entity")
    s.add_argument("value", help="on/off or a number")
    args = p.parse_args()
    if args.cmd == "list":
        for c in CASES:
            print(f"  {c.direction:7} {c.name:22} {c.what}")
        return 0
    return asyncio.run({"run": cmd_run, "state": cmd_state, "emu": cmd_emu, "set": cmd_set}[args.cmd](args))


if __name__ == "__main__":
    sys.exit(main())
