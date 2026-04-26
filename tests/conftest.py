import pytest
import os
import time


@pytest.fixture(scope="module")
def master(request):
    """Master device (full interpreter + ESP-NOW master)"""
    from pytest_embedded import Dut
    # The master DUT is the primary test device
    # Assumes serial port via environment or pytest-embedded config
    dut = Dut(
        os.path.join(request.config.getoption('app_path') or 'build'),
        target='esp32s3',
        port=os.environ.get('MASTER_PORT')
    )
    dut.expect('ESP-LEGO V1.0', timeout=30)
    yield dut
    dut.close()


@pytest.fixture(scope="module")
def sensor(request):
    """Sensor device (ESP-NOW slave)"""
    from pytest_embedded import Dut
    dut = Dut(
        os.path.join(request.config.getoption('app_path') or 'build'),
        target='esp32s3',
        port=os.environ.get('SENSOR_PORT')
    )
    dut.expect('sensor', timeout=30)
    yield dut
    dut.close()


@pytest.fixture
def duo(master, sensor):
    """Paired master+sensor for integration tests"""
    # Wait for sensor announce to reach master
    time.sleep(5)
    return master, sensor


@pytest.fixture
def script_exec(master):
    """Helper to send script and capture output"""
    def _send(script):
        master.write(script + '\n')
        # Wait for execution to complete or watchdog
        try:
            master.expect('Execute', timeout=2)
        except Exception:
            pass  # Some scripts complete silently
        time.sleep(0.5)
    return _send
