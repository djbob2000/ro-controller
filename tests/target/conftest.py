import re
import time


def send(dut, command: str) -> None:
    dut.write(command)


def expect_snapshot(dut, *, state: str | None = None, pump: bool | None = None,
                    inlet: bool | None = None, flush: bool | None = None,
                    timeout: float = 5.0):
    dut.write("snapshot")
    match = dut.expect(re.compile(r"RO_TEST (\{.*\})"), timeout=timeout)
    text = match.group(1).decode() if isinstance(match.group(1), bytes) else match.group(1)
    if state is not None:
        assert f'"state":"{state}"' in text
    if pump is not None:
        assert f'"pump":{str(pump).lower()}' in text
    if inlet is not None:
        assert f'"inlet":{str(inlet).lower()}' in text
    if flush is not None:
        assert f'"flush":{str(flush).lower()}' in text
    return text


def wait_state(dut, state: str, timeout: float = 10.0):
    deadline = time.monotonic() + timeout
    last = ""
    while time.monotonic() < deadline:
        remaining = max(0.5, min(2.0, deadline - time.monotonic()))
        last = expect_snapshot(dut, timeout=remaining)
        if f'"state":"{state}"' in last:
            return last
        time.sleep(0.2)
    raise AssertionError(f"controller did not reach {state}; last snapshot: {last}")


def simulate(dut, *, water: bool, tank: bool, leak: bool = False) -> None:
    dut.write(
        f"simulate:water={int(water)},tank={int(tank)},leak={int(leak)}"
    )
    dut.expect_exact("RO_TEST OK simulate")
