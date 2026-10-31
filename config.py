"""Central config for SeptiSense. Edit values here, not scattered in code."""
import os
from pathlib import Path

ROOT = Path(__file__).resolve().parent
ARTIFACTS = ROOT / "artifacts"          # trained models, thresholds, eval plots
DB_PATH = Path(os.getenv("SEPTI_DB", ROOT / "septisense.db"))

# ---------------- ML ----------------
# Vitals our hardware can actually measure live. Resp only if ECG-derived resp works.
SENSOR_VITALS = ["HR", "O2Sat", "Temp"]
USE_RESP = os.getenv("SEPTI_USE_RESP", "0") == "1"
if USE_RESP:
    SENSOR_VITALS = SENSOR_VITALS + ["Resp"]
STATIC_FEATURES = ["Age", "Gender"]       # entered on the dashboard at registration

HORIZONS = [2, 4, 6]                      # hours ahead
MAIN_HORIZON = 6                          # used for alerts + explanations

# Sensitivity targets (on validation set) that define the 3 alert levels for the 6h model
ALERT_LEVEL_SENSITIVITY = {"watch": 0.80, "warning": 0.60, "critical": 0.40}

# ---------------- Sensor cleaning ----------------
VALID_RANGES = {            # outside these -> treated as missing
    "hr": (30, 220),
    "hr_ecg": (30, 220),
    "spo2": (70, 100),
    "temp": (30.0, 40.0),   # SKIN temperature: >40 °C on skin is not physiological (sensor heating / not on body)
    "resp": (4, 60),
}
MAX_RATE_PER_SEC = {        # bigger jumps than this between consecutive samples = spike
    "hr": 30, "hr_ecg": 30, "spo2": 5, "temp": 0.5, "resp": 10,
}
# Skin -> core temperature offset. CALIBRATE: compare your sensor with a clinical
# thermometer on the same person and set the difference here.
SKIN_TO_CORE_OFFSET = float(os.getenv("SEPTI_TEMP_OFFSET", "3.0"))
HR_AGREEMENT_BPM = 10       # ECG-HR vs PPG-HR disagreement limit
WINDOW_SEC = 60             # cleaning/aggregation window
MIN_VALID_FRACTION = 0.6    # window needs >=60% valid samples, else value = missing
EXPECTED_PACKET_HZ = 1.0    # ESP sends 1 packet/sec
OFFLINE_AFTER_SEC = 10      # no packet for this long -> device offline

# ---------------- Live inference / alerts ----------------
PREDICT_EVERY_SEC = int(os.getenv("SEPTI_PREDICT_EVERY", "30"))
HOUR_SEC = 3600             # live mode: one model "hour" = real hour
ALERT_CONSECUTIVE_UP = 3    # risk must stay above a level this many predictions in a row
ALERT_CONSECUTIVE_DOWN = 3  # and below for this many to step down
SNOOZE_MIN_DEFAULT = 15
GUARDIAN_ESCALATE_SEC = 300  # unacknowledged CRITICAL -> notify guardian after 5 min

# ---------------- Notifications ----------------
TELEGRAM_BOT_TOKEN = os.getenv("TELEGRAM_BOT_TOKEN", "")   # never commit the token

DISCLAIMER = ("Educational/research prototype. Not a clinically validated "
              "diagnostic system. Do not use for medical decisions.")
