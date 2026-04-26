import pytest
import os
from pytest_embedded import Dut


@pytest.fixture(scope="module")
def dut_single(request):
    """Single master device"""
    dut = Dut(
        os.path.join(request.config.getoption('app_path') or 'build'),
        target='esp32s3',
        port=os.environ.get('ESP_PORT')
    )
    dut.expect('ESP-LEGO V1.0', timeout=30)
    yield dut
    dut.close()
