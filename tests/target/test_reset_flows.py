import os

import pytest


@pytest.mark.bench
@pytest.mark.physical_fixture
def test_password_and_factory_reset_boot_shortcuts_require_fixture(dut):
    """Reserved for a fixture that can hold GPIO10/11/12 during reset.

    This is intentionally not faked through the diagnostic protocol: reset
    authorization is a physical-presence security boundary and should be proven
    using real button GPIOs.
    """
    if os.environ.get("RO_RESET_FIXTURE") != "1":
        pytest.skip("set RO_RESET_FIXTURE=1 only when the button fixture is wired")

    # Fixture-specific GPIO actuation belongs in the lab adapter. The DUT side
    # must show the local confirmation screen before any erase is accepted.
    dut.expect("RESET ADMIN|FACTORY RESET", timeout=15)
