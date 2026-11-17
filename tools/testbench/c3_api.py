"""ESPHome native API access to the ESP32-C3 implant (src/esp32-c3-supermini--esphome).

Entities are addressed by their display name ("Door alarm", "Set-point", ...). States and log lines are collected in
the background; tests poll them with wait_state() / wait_log().
"""

import asyncio
import re
import time
from pathlib import Path

from aioesphomeapi import APIClient, LogLevel

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def api_key_from_secrets(path: Path) -> str:
    m = re.search(r'^api_key:\s*["\']?([^"\'\n]+)', path.read_text(), re.M)
    if not m:
        raise RuntimeError(f"no api_key in {path}")
    return m.group(1).strip()


class C3Api:
    def __init__(self, host: str, noise_psk: str, port: int = 6053):
        self.client = APIClient(host, port, None, noise_psk=noise_psk)
        self.by_name: dict[str, object] = {}  # entity name -> EntityInfo
        self.by_key: dict[int, object] = {}
        self.states: dict[str, object] = {}  # entity name -> latest value (None = unknown / unavailable)
        self.logs: list[tuple[float, str]] = []

    async def connect(self) -> None:
        await self.client.connect(login=True)
        entities, _ = await self.client.list_entities_services()
        for e in entities:
            self.by_name[e.name] = e
            self.by_key[e.key] = e
        self.client.subscribe_states(self._on_state)
        self.client.subscribe_logs(self._on_log, log_level=LogLevel.LOG_LEVEL_INFO)

    async def disconnect(self) -> None:
        await self.client.disconnect()

    def _on_state(self, st) -> None:
        e = self.by_key.get(st.key)
        if e is None:
            return
        value = getattr(st, "state", None)
        if getattr(st, "missing_state", False) or (isinstance(value, float) and value != value):  # NaN = unknown
            value = None
        self.states[e.name] = value

    def _on_log(self, msg) -> None:
        text = msg.message.decode("utf-8", "replace") if isinstance(msg.message, bytes) else str(msg.message)
        self.logs.append((time.time(), ANSI.sub("", text)))

    # ---- reading ------------------------------------------------------------------------------------------------

    def state(self, name: str):
        if name not in self.by_name:
            raise KeyError(f"no entity named '{name}'")
        return self.states.get(name)

    async def wait_state(self, name: str, expected, timeout: float) -> bool:
        end = time.time() + timeout
        while time.time() < end:
            if self.state(name) == expected:
                return True
            await asyncio.sleep(0.1)
        return False

    def log_mark(self) -> int:
        return len(self.logs)

    async def wait_log(self, pattern: str, timeout: float, since: int = 0) -> str | None:
        rx = re.compile(pattern)
        end = time.time() + timeout
        while time.time() < end:
            for _, line in self.logs[since:]:
                if rx.search(line):
                    return line
            await asyncio.sleep(0.1)
        return None

    # ---- writing ------------------------------------------------------------------------------------------------

    def set(self, name: str, value) -> None:
        """Switch -> bool, number -> float. Same path Home Assistant uses."""
        e = self.by_name[name]
        kind = type(e).__name__
        if kind == "SwitchInfo":
            self.client.switch_command(e.key, bool(value))
        elif kind == "NumberInfo":
            self.client.number_command(e.key, float(value))
        else:
            raise ValueError(f"'{name}' ({kind}) cannot be set")
