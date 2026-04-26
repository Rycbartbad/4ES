import pytest


@pytest.mark.generic
def test_master_boots(dut_single):
    """TC-P7.0: Verify master firmware boots and shows ready message"""
    dut_single.expect('ESP-LEGO V1.0 starting', timeout=30)
    dut_single.expect('Ready', timeout=10)
