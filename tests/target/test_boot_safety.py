import pytest

from conftest import expect_snapshot, simulate


@pytest.mark.bench
def test_outputs_can_be_observed_safe_with_no_water(dut):
    """With semantic low-water asserted, all hydraulic outputs must be off."""
    simulate(dut, water=False, tank=False)
    dut.expect('RO_TEST')
    # Low-water is a highest-priority software stop. The test hook substitutes
    # only semantic inputs; it never writes output GPIOs directly.
    for _ in range(3):
        text = expect_snapshot(dut, timeout=3)
        if '"state":"WAIT_WATER"' in text:
            assert '"inlet":false' in text
            assert '"pump":false' in text
            assert '"flush":false' in text
            return
    pytest.fail("controller did not settle in WAIT_WATER with all outputs off")
