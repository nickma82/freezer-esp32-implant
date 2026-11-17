"""Test cases for the testbench: freezer emulator (ESP8266) <-> ESP32-C3 implant.

Each case acts on one side and checks the other:
  emu2c3  the emulator changes its freezer state (console) -> the C3 entity must follow
  c32emu  an entity on the C3 is set (ESPHome API, like Home Assistant) -> the write must reach the emulator
  resilience  the C3 reboots while the emulator keeps running -> it must come back on its own

A case has `act` steps, `expect` checks, then `restore` steps and `restore_expect` checks that bring the bench back to
the emulator's boot defaults (set-point -18, temperature -17, mode normal, door closed, no alarms, party off).

Steps:   Emu("p 0C 00 20 04")            console line command (ack is checked)
         Set("SuperFrost", True)          ESPHome entity command
         RestartC3()                      press the C3's Restart button, reconnect the API when it is back
Checks:  State("Door alarm", True)        C3 entity value
         EmuLine(r"# SETPOINT .* -20 ")   regex on an emulator console line printed after the act/restore steps
         C3Log(r"write confirmed: ...")   regex on a C3 log line (INFO and up) printed after the steps
         NoEmuLine(r"...")                regex that must NOT be on any emulator line since the steps (no waiting)
State, EmuLine and C3Log take an optional timeout=<seconds> that replaces the run's --timeout.
"""

from dataclasses import dataclass, field


@dataclass
class Emu:
    cmd: str


@dataclass
class Set:
    entity: str
    value: object


@dataclass
class RestartC3:
    pass


@dataclass
class State:
    entity: str
    value: object
    timeout: float | None = None


@dataclass
class EmuLine:
    pattern: str
    timeout: float | None = None


@dataclass
class C3Log:
    pattern: str
    timeout: float | None = None


@dataclass
class NoEmuLine:
    pattern: str


@dataclass
class Case:
    name: str
    direction: str  # emu2c3 | c32emu | resilience
    what: str
    act: list
    expect: list
    restore: list = field(default_factory=list)
    restore_expect: list = field(default_factory=list)


# event flag pairs for the emulator's next 1 s event block; they make the C3 read the state right away instead of
# waiting for its 10 s periodic read
VALUE_CHANGED = Emu("e 01000000 08000000")
DOOR_ALARM_CHANGED = Emu("e 00000100 08000000")
PARTY_CHANGED = Emu("e 00800000 04000000")


def t16(degc: int) -> str:
    """A temperature as the 2-byte value of a flag-40 parameter (the freezer uses the low byte as int8)."""
    return f"00 {degc & 0xFF:02X}"


def sp(degc: int) -> str:
    """The C3's Set-point as its dropdown option text (select entity), e.g. "-18 °C"."""
    return f"{degc} °C"


def write_frame(grp: str, rid: str, flag: str, value: str) -> str:
    """Regex for the emulator's M line of a box write (svc 24) in slot 02, CRC ok."""
    n = 3 + len(value.split())
    return rf"^M \S+ 02 \S+ 1 FF 02 24 01 {n:02X} {grp} {rid} {flag} {value} "


# the emulator's M line of its hello to node 02 (slot 82, svc 01)
EMU_HELLO = r"^M \S+ 82 \S+ 1 82 7F 01 01 "


CASES = [
    # ---- emulator -> C3 ---------------------------------------------------------------------------------------
    Case("door_alarm", "emu2c3", "door alarm on the freezer (sets door open + door alarm)",
         act=[Emu("a 1")],
         expect=[State("Door alarm", True), State("Door", True)],
         restore=[Emu("a 0")],
         restore_expect=[State("Door alarm", False), State("Door", False)]),
    Case("door_only", "emu2c3", "door opened without alarm",
         act=[Emu("p 0C 31 20 01"), DOOR_ALARM_CHANGED],
         expect=[State("Door", True), State("Door alarm", False)],
         restore=[Emu("p 0C 31 20 00"), DOOR_ALARM_CHANGED],
         restore_expect=[State("Door", False)]),
    Case("temperature", "emu2c3", "current temperature changes to -21 degC",
         act=[Emu("t -21")],
         expect=[State("Temperature", -21.0)],
         restore=[Emu("t -17")],
         restore_expect=[State("Temperature", -17.0)]),
    Case("power_failure", "emu2c3", "power-failure alarm with -48 degC peak temperature",
         act=[Emu(f"p 0C 12 40 {t16(-48)}"), Emu("p 0C 11 20 01"), DOOR_ALARM_CHANGED],
         expect=[State("Power-failure alarm", True), State("Power-failure temperature", -48.0)],
         restore=[Emu(f"p 0C 12 40 {t16(-55)}"), Emu("p 0C 11 20 00"), DOOR_ALARM_CHANGED],
         restore_expect=[State("Power-failure alarm", False), State("Power-failure temperature", -55.0)]),
    Case("superfrost_on_freezer", "emu2c3", "SuperFrost switched on at the freezer's own panel",
         act=[Emu("p 0C 00 20 04"), VALUE_CHANGED],
         expect=[State("SuperFrost", True)],
         restore=[Emu("p 0C 00 20 01"), VALUE_CHANGED],
         restore_expect=[State("SuperFrost", False)]),
    Case("setpoint_on_freezer", "emu2c3", "set-point changed to -20 at the freezer's own panel",
         act=[Emu(f"p 0C 06 40 {t16(-20)}"), VALUE_CHANGED],
         expect=[State("Set-point", sp(-20))],
         restore=[Emu(f"p 0C 06 40 {t16(-18)}"), VALUE_CHANGED],
         restore_expect=[State("Set-point", sp(-18))]),
    Case("party_on_freezer", "emu2c3", "party mode switched on at the freezer",
         act=[Emu("p 08 02 20 02"), PARTY_CHANGED],
         expect=[State("Party mode", True)],
         restore=[Emu("p 08 02 20 00"), PARTY_CHANGED],
         restore_expect=[State("Party mode", False)]),
    # ---- C3 -> emulator ---------------------------------------------------------------------------------------
    Case("setpoint", "c32emu", "set-point -20 degC from Home Assistant",
         act=[Set("Set-point", -20)],
         expect=[EmuLine(write_frame("0C", "06", "40", t16(-20))),
                 EmuLine(r"^# SETPOINT \d+ box wrote target temperature -20 degC"),
                 C3Log(r"write confirmed: Set-point = -20"),
                 State("Set-point", sp(-20))],
         restore=[Set("Set-point", -18)],
         restore_expect=[EmuLine(r"^# SETPOINT \d+ box wrote target temperature -18 degC"),
                         C3Log(r"write confirmed: Set-point = -18"),
                         State("Set-point", sp(-18))]),
    Case("superfrost", "c32emu", "SuperFrost on from Home Assistant",
         act=[Set("SuperFrost", True)],
         expect=[EmuLine(write_frame("0C", "00", "20", "04")),
                 C3Log(r"write confirmed: SuperFrost"),
                 State("SuperFrost", True)],
         restore=[Set("SuperFrost", False)],
         restore_expect=[EmuLine(write_frame("0C", "00", "20", "01")),
                         C3Log(r"write confirmed: SuperFrost"),
                         State("SuperFrost", False)]),
    Case("party", "c32emu", "party mode on from Home Assistant",
         act=[Set("Party mode", True)],
         expect=[EmuLine(write_frame("08", "02", "20", "02")),
                 C3Log(r"write confirmed: Party mode"),
                 State("Party mode", True)],
         restore=[Set("Party mode", False)],
         restore_expect=[EmuLine(write_frame("08", "02", "20", "00")),
                         C3Log(r"write confirmed: Party mode"),
                         State("Party mode", False)]),
    # ---- resilience -------------------------------------------------------------------------------------------
    # The freezer sends its hello only at power-up, so a rebooted C3 must rejoin without one (after 30 s in waiting).
    # The restore's set-point write proves that writes work again after the rejoin.
    Case("c3_restart", "resilience", "C3 reboots while the freezer runs on: rejoins without a hello",
         act=[RestartC3()],
         expect=[C3Log(r"start-up reads without handshake \(rejoin\)", timeout=60),
                 State("Start-up phase", "running"),
                 State("Set-point", sp(-18)),
                 State("Power-failure temperature", -55.0),  # not Temperature: it drifts after set-point writes
                 State("Model", "FNc    5076"),
                 NoEmuLine(EMU_HELLO)],
         restore=[Set("Set-point", -18)],
         restore_expect=[EmuLine(r"^# SETPOINT \d+ box wrote target temperature -18 degC"),
                         C3Log(r"write confirmed: Set-point = -18")]),
]
