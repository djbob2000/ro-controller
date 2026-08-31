import pytest

from conftest import expect_snapshot, simulate, wait_state


@pytest.mark.bench
def test_outputs_can_be_observed_safe_with_no_water(dut):
    """With semantic low-water asserted, all hydraulic outputs must be off."""
    simulate(dut, water=False, tank=False)
    wait_state(dut, "WAIT_WATER", timeout=6)
    # Low-water is a highest-priority software stop. The test hook substitutes
    # only semantic inputs; it never writes output GPIOs directly.
    expect_snapshot(dut, state="WAIT_WATER", inlet=False, pump=False, flush=False)
