"""Serial console of the ESP8266 freezer emulator (protocol-decoding/esp-freezer-emulator-poc).

The console is bit-banged SoftwareSerial on the emulator: characters that arrive while it transmits get lost. After each
received character it holds its own output back for 300 ms. So a command is sent as: Esc (clears a half-typed line and
opens the hold window), the command characters with a short gap, Enter; then the ack line is awaited and the whole
thing retried if it does not come.
"""

import re
import threading
import time

import serial

# lines a command line can never start with: the emulator treats these first characters as single-key commands
SINGLE_KEYS = set("?hgsrvk")


class EmuConsole:
    def __init__(self, port: str, baud: int = 57600):
        self.port = port
        self.lines: list[tuple[float, str]] = []  # (host time, line) for everything the emulator printed
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._ser = serial.Serial()
        self._ser.port = port
        self._ser.baudrate = baud
        self._ser.timeout = 0.1
        # NodeMCU auto-reset: leave DTR and RTS at pyserial's default (both asserted together, EN stays high).
        # Deasserting DTR alone (e.g. dtr=False before open) pulls EN low for a moment and reboots the emulator.
        self._thread = threading.Thread(target=self._reader, daemon=True)

    def open(self) -> None:
        self._ser.open()
        self._thread.start()

    def close(self) -> None:
        self._stop.set()
        self._thread.join(timeout=1)
        self._ser.close()

    def _reader(self) -> None:
        buf = b""
        while not self._stop.is_set():
            buf += self._ser.read(512)
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", "replace").strip("\r")
                with self._lock:
                    self.lines.append((time.time(), line))

    # ---- reading ------------------------------------------------------------------------------------------------

    def mark(self) -> int:
        """Index of the next line to arrive; pass it to find()/wait_for() to look only at newer lines."""
        with self._lock:
            return len(self.lines)

    def find(self, pattern: str, since: int = 0) -> re.Match | None:
        rx = re.compile(pattern)
        with self._lock:
            for _, line in self.lines[since:]:
                m = rx.search(line)
                if m:
                    return m
        return None

    def wait_for(self, pattern: str, timeout: float, since: int = 0) -> re.Match | None:
        end = time.time() + timeout
        while time.time() < end:
            m = self.find(pattern, since)
            if m:
                return m
            time.sleep(0.05)
        return None

    def last_status(self, since: int = 0) -> dict | None:
        """Fields of the newest `S` status line (printed every 10 s), e.g. {'hs': 'running', 'box_answers': '17'}."""
        with self._lock:
            for _, line in reversed(self.lines[since:]):
                if line.startswith("S "):
                    return dict(kv.split("=", 1) for kv in line.split()[2:] if "=" in kv)
        return None

    # ---- writing ------------------------------------------------------------------------------------------------

    def _type(self, text: str) -> None:
        self._ser.write(b"\x1b")
        time.sleep(0.05)
        for ch in text:
            self._ser.write(ch.encode())
            time.sleep(0.005)
        self._ser.write(b"\r")

    def command(self, cmd: str, ack: str, retries: int = 6, timeout: float = 1.5) -> str:
        """Send a line command and wait for its ack (regex). Returns the ack line; raises if it never comes."""
        if cmd[:1] in SINGLE_KEYS:
            raise ValueError(f"'{cmd}' starts with a single-key command character")
        for _ in range(retries):
            since = self.mark()
            self._type(cmd)
            m = self.wait_for(ack, timeout, since)
            if m:
                return m.string
            time.sleep(0.3)
        raise RuntimeError(f"emulator did not ack '{cmd}' (expected /{ack}/)")

    def key(self, ch: str, ack: str, retries: int = 6, timeout: float = 1.5) -> str:
        """Send a single-key command (g, s, r, ...) and wait for its ack."""
        for _ in range(retries):
            since = self.mark()
            self._ser.write(ch.encode())
            m = self.wait_for(ack, timeout, since)
            if m:
                return m.string
            time.sleep(0.3)
        raise RuntimeError(f"emulator did not ack key '{ch}' (expected /{ack}/)")

    def send_any(self, cmd: str) -> str:
        """Send a command with the ack the emulator prints for its command letter."""
        return self.command(cmd, ACKS.get(cmd[:1], r"^# "))


# ack line per command letter (see welcome()/line_command() in the emulator's src/main.cpp)
ACKS = {
    "a": r"^# door alarm (ON|OFF)",
    "t": r"^# current temperature -?\d+ degC",
    "e": r"^# event flags [0-9A-F]{8} [0-9A-F]{8} raised",
    "p": r"^# param [0-9A-F]{2}/[0-9A-F]{2} set",
    "x": r"^# queued service message",
    "X": r"^# queued raw body",
}
