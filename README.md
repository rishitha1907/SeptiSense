# SeptiSense – ML + Backend

Sensors → ESP32 (Wi-Fi) → **laptop backend + ML** → web dashboard.
This repo is the laptop part: model training/evaluation, the telemetry backend, alerting, and a reference dashboard.

> Educational/research prototype. Not a clinically validated diagnostic system.

```
config.py                 all knobs (temp offset, thresholds, alert rules)
ml/
  data.py                 load PhysioNet 2019, 2h/4h/6h labels, patient-level split
  features.py             feature engineering (shared by training AND live backend)
  train.py                LightGBM x3 horizons + baselines + eval report + plots
  predict.py              inference wrapper the backend uses
  explain.py              SHAP contributions -> "↑ HR rising 18 bpm over 6h"
  calibration.py          Platt calibration
  synth.py                FAKE data for smoke tests only
backend/
  app.py                  FastAPI: telemetry ingest + all dashboard endpoints
  cleaning.py             noise handling + data-quality score + ECG filter/HR
  engine.py               windows -> hourly -> predictions -> alerts (+ replay mode)
  alerts.py               3-level alerts with hysteresis, ack/snooze, guardian escalation
  notify.py               Telegram + templated patient summary
  static/index.html       reference dashboard
tools/fake_esp.py         noisy fake ESP32 for testing without hardware
firmware_example/         ESP32 sending template (untested, for the hardware teammate)
```

## 0. Install

```bash
pip install -r requirements.txt
```

## 1. Get the dataset (start this FIRST – ~40k small files, takes a while)

```bash
wget -r -N -c -np https://physionet.org/files/challenge-2019/1.0.0/training/
# data ends up in: physionet.org/files/challenge-2019/1.0.0/training/  (training_setA, training_setB)
```

Open access, no login. Cite: Reyna MA et al., *Crit Care Med* 2020;48(2):210–217.
A combined CSV (e.g. the Kaggle version with a `Patient_ID` column) also works with `--data file.csv`.

**While it downloads**, smoke-test everything on fake data (do NOT report these numbers – fake data is too easy, AUROC ≈ 0.99):

```bash
python -m ml.synth --out fake_data --n 1500
python -m ml.train --data fake_data
```

## 2. Train on the real data

```bash
# quick first pass
python -m ml.train --data physionet.org/files/challenge-2019/1.0.0/training --max-patients 8000
# full run (+ all-40-variable comparison model)
python -m ml.train --data physionet.org/files/challenge-2019/1.0.0/training --full
# optional external validation: train on hospital A, test on hospital B
python -m ml.train --data ... --split hospital --full
```

Everything lands in `artifacts/`: `eval_report.md` (tables ready for the report), `roc_pr.png`,
`calibration.png`, `confusion_matrix.png`, `feature_importance.png`, `metrics.json`, models, and
`demo_patients.csv` (held-out test patients for replay mode). First load is cached, re-runs are faster.

Expect **much lower numbers than the smoke test** – vitals-only prediction of sepsis is hard.
Report what you get honestly, next to the baselines and the all-variable model.

**What the model does:** every hour, P(sepsis onset within 2h / 4h / 6h). Features use only what our
hardware measures (HR, SpO₂, Temp; + Resp if `SEPTI_USE_RESP=1`) plus age/sex. Patient-level split,
forward-fill only, thresholds + calibration fitted on validation – no leakage.

## 3. Run the backend

```bash
uvicorn backend.app:app --host 0.0.0.0 --port 8000
```

- Open http://localhost:8000 (not 0.0.0.0) → login page.
  - **Doctor login:** `doctor` / `septisense` (change with env vars `SEPTI_DOCTOR_USER`, `SEPTI_DOCTOR_PASS` before the demo).
  - **Patient / guardian login:** create it when registering a patient (optional username + password), or later in the dashboard. They only see their own status page and report.
- Pages: `/` doctor dashboard · `/patient` patient view · `/report/<id>` printable report (Print → Save as PDF) · `/docs` API docs.
- ESP posts to `http://<laptop-ip>:8000/api/telemetry` (same Wi-Fi/hotspot; on Windows allow port 8000 in the firewall).

Test without hardware:

```bash
python tools/fake_esp.py --scenario deteriorating --ecg
```

## 3b. Upload a patient CSV (dashboard → "Upload patient CSV")

Any CSV with vitals columns – names are matched loosely (`HR`/`heart_rate`/`pulse`, `SpO2`/`O2Sat`, `Temp`/`temperature` (°F auto-converted), `Resp`/`RR`)
plus optional `hour` or `timestamp`, `age`, `sex`. PhysioNet `.psv` files work directly (their `SepsisLabel` marks the true onset on the charts).
Every hour is scored at once; the patient then appears with forecast, charts, alerts (logged only, no Telegram – it is historical data),
summary, report and **Download results CSV** (hour, vitals, risk_2h/4h/6h, level). "Download a sample CSV" gives a ready test file.

Staff accounts: the dashboard's **Staff accounts** card adds more doctor logins.

## 4. ESP ↔ backend contract (give this to the hardware teammate)

POST JSON, 1 packet per second:

```json
{"device_id":"esp32-01","seq":42,"ts":1793361900,"hr":96.5,"spo2":97,"temp":34.6,
 "finger_on":true,"leads_off":false,"hr_ecg":95.0}
```

- `ts` = unix seconds from NTP. If NTP isn't synced, send `null` – server time is used.
- **Invalid reading → `null`, never 0.** `temp` = skin temperature in °C.
- `seq` increments every packet (backend computes packet loss from gaps).
- Optional: `hr_ecg` (if computed on ESP) **or** `"ecg":[...samples...], "ecg_fs":250` – backend filters it and finds R-peaks itself.
- Offline buffer: when Wi-Fi returns, POST a **JSON array** of the stored packets (+ `"buffered":true`). Their windows/hours are recomputed.
- Response: `{"ack": <highest seq>, "level": "normal|watch|warning|critical"}`. ESP buzzes on warning/critical;
  if no ACK for 30 s it falls back to local thresholds (SpO₂ < 90, HR > 130, core temp > 39).

## 4b. Device monitor page (`/device`, link in the dashboard header)

Live view of the raw ESP stream: connection, packets/min, packet loss, lag, cleaning flags, HR (PPG vs ECG),
SpO₂, skin temperature, filtered ECG strip, and a table of the last 40 packets (rejected raw values struck through).
**Every packet is saved** (vitals + raw ECG) in `septisense.db`; download with **Vitals CSV** / **Raw ECG CSV**.
The backend accepts common field-name variants (`HR_ppg`, `SpO2`, `HR_ecg`, `Temp`, `finger`, `alert`, …) and
estimates the ECG sample rate from samples per second when `ecg_fs` is not sent.

## 5. Noise handling (backend/cleaning.py)

1. Per sample: finger-off / leads-off flags, valid ranges, spike limit (e.g. HR can't jump > 30 bpm in 1 s), SpO₂ 0 treated as invalid.
2. Per 60 s window: Hampel outlier removal → **median**; window value = missing if < 60 % valid samples.
3. Quality score 0–100 from % valid HR/SpO₂/temp/ECG, packet delivery, and **ECG-HR vs PPG-HR agreement** → High/Medium/Low.
4. Hourly value for the model = median of non-Low windows. Low-quality data → prediction marked low confidence → alerts can't escalate.
5. Skin → core temp: `SEPTI_TEMP_OFFSET` (default 3.0 °C). **Calibrate it** against a thermometer on the same person.

## 6. Alerts

- Levels watch / warning / critical = 80 % / 60 % / 40 % sensitivity points on the validation set (`config.ALERT_LEVEL_SENSITIVITY`).
- Raised only after 3 consecutive predictions above a level, cleared after 3 below (configurable in the dashboard).
- Doctor: Telegram on warning + critical. Guardian: critical left unacknowledged for 5 min. Snooze silences, but critical always notifies.
- Telegram: create a bot with @BotFather → run with `TELEGRAM_BOT_TOKEN=...` → doctor/guardian open the bot and press Start →
  `GET /api/telegram/updates` shows their `chat_id` → add them in the dashboard. (Bots can't message a phone number directly.)

## 6b. Two-key alerting (hospital workflow)

The ML model alone has low precision (~3–4 %), so a doctor is paged only when **bedside criteria agree**:
NEWS2 ≥ 5 or any vital in the red zone, qSOFA ≥ 2, lactate ≥ 2 mmol/L, or clinician-flagged suspected infection (`backend/clinical.py`).

| Situation | Who is notified |
|---|---|
| Watch | dashboard only |
| Warning/Critical, bedside criteria **not** met | **nurse**: "bedside check – enter BP, resp rate, consciousness, lactate" |
| Warning/Critical, criteria met | **doctor** (+ nurse) |
| Nurse alert not acknowledged in 10 min (`nurse_escalate_sec`) | doctor |
| Nurse enters observations that make criteria agree | doctor, immediately |
| Critical, doctor paged, unacknowledged for 5 min | guardian – plain language, no numbers |

Nurses enter observations in the dashboard's **Bedside check** card (these feed NEWS2/qSOFA, not the ML model).
Each alert stores the bedside snapshot, routing and an escalation log; clinicians rate alerts **Useful / False alarm**,
giving measured real-world precision (`GET /api/alerts/performance`). Toggle with the `two_key_enabled` setting.

## 7. Demo for judges

1. Replay a septic test patient (sidebar → Replay demo, 2 s per hour): risk rises before the dataset's onset line,
   reasons update, alert escalates, Telegram fires, ack it, generate the summary.
2. Live sensors next to it: show data quality reacting when you lift the finger / pull an electrode → "Low confidence – check sensor".
3. Show `artifacts/eval_report.md` plots.

Note: live mode needs real hours of data for trend features; a 10-minute live demo will show mostly current-value risk.
That's why replay mode exists – say so openly.

## Main API endpoints

| Method | Path | What |
|---|---|---|
| POST | /api/telemetry | ESP packets (one or a list) |
| GET/POST | /api/patients | list / register (name, age, gender, device_id) |
| GET | /api/patients/{id}/latest | vitals now, connectivity, data quality, prediction + reasons, alert state |
| GET | /api/patients/{id}/vitals?minutes=60 | per-minute cleaned windows |
| GET | /api/patients/{id}/hourly | hourly model inputs |
| GET | /api/patients/{id}/predictions | risk history (2h/4h/6h) |
| GET | /api/patients/{id}/alerts | alert history |
| POST | /api/alerts/{id}/ack, /snooze | acknowledge / snooze |
| GET | /api/patients/{id}/summary | plain-text summary |
| GET/POST | /api/patients/{id}/contacts | Telegram contacts |
| GET/PUT | /api/settings | alert configuration |
| GET | /api/devices, /api/devices/{id}/ecg | connectivity, filtered ECG strip |
| GET | /api/model | model card + metrics |
| GET/POST | /api/replay/demos, /api/replay/start | replay mode |
