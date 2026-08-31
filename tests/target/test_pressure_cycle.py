import time

import pytest

from conftest import expect_snapshot, simulate


@pytest.mark.bench
def test_low_water_stop_and_tank_full_final_flush_do_not_short_cycle(dut):
    simulate(dut, water=True, tank=False)

    # Cold boot / recovery path includes PREPARE + 20 s startup flush.
    dut.expect('"state":"PRODUCING"', timeout=35)
    expect_snapshot(dut, state="PRODUCING", pump=True, inlet=True, flush=False)

    simulate(dut, water=False, tank=False)
    dut.expect('"state":"WAIT_WATER"', timeout=3)
    expect_snapshot(dut, state="WAIT_WATER", pump=False, inlet=False, flush=False)

    simulate(dut, water=True, tank=False)
    # 3 s stable + 10 s restart delay + prepare + startup flush.
    dut.expect('"state":"PRODUCING"', timeout=40)

    simulate(dut, water=True, tank=True)
    dut.expect('"state":"FINAL_FLUSH"', timeout=5)
    expect_snapshot(dut, state="FINAL_FLUSH", pump=True, inlet=True, flush=True)

    # Opening the flush path may release the high-pressure switch. The state
    # must stay latched in FINAL_FLUSH until the configured flush is complete.
    simulate(dut, water=True, tank=False)
    time.sleep(1)
    expect_snapshot(dut, state="FINAL_FLUSH", pump=True, inlet=True, flush=True)

    dut.expect('"state":"STANDBY"', timeout=25)
    expect_snapshot(dut, state="STANDBY", pump=False, inlet=False, flush=False)
