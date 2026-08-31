import time

import pytest

from conftest import expect_snapshot, simulate, wait_state


@pytest.mark.bench
def test_network_loss_does_not_change_hydraulic_control(dut):
    simulate(dut, water=True, tank=False)
    wait_state(dut, "PRODUCING", timeout=35)
    expect_snapshot(dut, state="PRODUCING", pump=True, inlet=True, flush=False)

    dut.write("network:down")
    dut.expect_exact("RO_TEST OK network:down")
    time.sleep(2)
    expect_snapshot(dut, state="PRODUCING", pump=True, inlet=True, flush=False)

    dut.write("network:up")
    dut.expect_exact("RO_TEST OK network:up")
