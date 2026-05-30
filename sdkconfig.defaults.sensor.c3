# ESP32-C3 doorbell sensor defaults.
CONFIG_IDF_TARGET="esp32c3"

# C3-compatible doorbell wiring defaults. Adjust if your board is wired
# differently.
CONFIG_DOORBELL_PIN=4
CONFIG_DOORBELL_ACTIVE_LEVEL=1
CONFIG_BUZZER_PIN=5
