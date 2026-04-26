# ESP-LEGO Integration Tests

## Prerequisites

- 2x ESP32-S3 development boards (master + sensor)
- USB serial cables for both boards
- Python 3.8+ with pytest-embedded: `pip install pytest-embedded`

## Running Tests

### Environment Setup

```bash
export MASTER_PORT=/dev/ttyUSB0
export SENSOR_PORT=/dev/ttyUSB1
```

### Run all hardware tests (excluding 24h stability)

```bash
pytest tests/test_smoke.py tests/test_espnow_integration.py tests/test_robustness.py \
    tests/test_full_integration.py -m generic -v
```

### Run only ESP-NOW tests

```bash
pytest tests/test_espnow_integration.py -v
```

### Run 24h stability test (overnight)

```bash
pytest tests/test_full_integration.py -k test_24h_stability -m stress -v --timeout=86400
```

## Test Markers

- `@pytest.mark.generic` — Standard ESP32 hardware tests
- `@pytest.mark.host_test` — x86 host runnable tests
- `@pytest.mark.stress` — Long-running stability tests
