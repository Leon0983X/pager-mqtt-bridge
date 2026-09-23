# AGENTS.md

Instructions for AI coding agents working in this repository.

## Purpose

This firmware runs on an **Arduino Nano RP2040 Connect**. It detects an emergency-service alarm received by a
**Motorola TPG2200** digital pager and forwards it over **MQTT** to a home-automation system.

- When the TPG2200 receives an alarm, a **potential-free (dry-contact) relay** inside its charging station closes.
- The two relay contacts of the charging station are wired to the Arduino's **D2** and **GND** pins (`D2` uses
  `INPUT_PULLUP`, active-low; see "Hardware" for why GND was chosen over 3.3V).
- The firmware polls D2 and also latches it via a pin-change interrupt. When the relay is closed, it publishes the
  payload `ALARM` to the configured MQTT broker and topic.
- In the current setup, a **Home Assistant** instance subscribes to that topic and switches on a lamp.

## Goals

1. **Turn on a light when an alarm arrives.** When the pager receives an alarm, Home Assistant switches on a light.
   The Home Assistant automation lives outside this repo, but it depends on the MQTT messages this firmware sends.
2. **Run reliably with no manual intervention.** After a one-time setup the device runs permanently. It must not
   need a computer, a serial monitor, or anyone pressing reset.
3. **Recover from every point of failure.** Every failure the device can detect must lead to automatic recovery.
   Failures the device cannot fix itself, such as power loss, must be visible in Home Assistant so the user gets
   notified.

Reliability matters more than features. This is a safety-relevant alarm path. Prefer simple, predictable code, and
never add a change that could suppress or delay an alarm.

## Requirements

Requirement IDs (R1, R2, …) are referenced from the gaps list and should be used in commit messages.

### Alarm detection

- **R1: Detect the relay reliably.** The alarm input must correctly tell a closed relay from an open one. This
  depends on the wiring and pin mode matching: the documented default is the relay wired between D2 and GND with
  `SWITCH_ACTIVE_HIGH = false` (`INPUT_PULLUP`, LOW = closed); see "Hardware" for the alternative 3.3V wiring.
- **R2: Debounce the input.** The contact must stay in its new state for a configurable time (`DEBOUNCE_MS`, e.g.
  50 ms) before the change counts. This filters out contact bounce and EMI spikes.
- **R3: Report each alarm once, whatever the closure length.** How long the TPG2200 keeps the relay closed is
  unknown (see Q1). Sources claim anything from a few seconds up to "until someone acknowledges it", which can be
  hours or days. Detection must handle the whole range:
  - **Shortest closure:** any closure of **≥ 1 s** must be detected reliably. Aim well below that: anything longer
    than `DEBOUNCE_MS` should count.
  - **Longest closure:** a closure lasting hours or days is still **one** alarm. Send `ALARM` exactly once, on the
    open→closed edge, and never resend it while the contact stays closed. The state topic stays `ON` for the
    whole closure.
  - **Re-arming:** a new alarm can only fire after the contact has been open (debounced) again.
  - **No timeouts:** do not use timers that "expire" a long closure and re-arm or re-trigger while it is still
    closed.
  - **Relay already closed at startup:** treat it as a new alarm. After debouncing, send `ALARM`, going through
    R6 if the device is not connected yet. This way an alarm that started while the device was off is never
    lost. After a reboot in the middle of a long closure, this sends the same alarm twice. The owner accepts that
    duplicate (see "Accepted limitations"). Do not add persistent storage or other logic to suppress it.
- **R4: Never miss a closure during blocking work.** Even if the firmware is busy in a blocking call (a Wi-Fi or
  MQTT connect can take several seconds) for the whole length of a short closure, the closure must still be
  detected. Use a pin-change interrupt (`attachInterrupt(…, CHANGE)`) whose ISR records the time of the latest
  closing and opening edges, and latches the most recent complete closure of at least `DEBOUNCE_MS` (with
  `millis()` into `volatile` variables, no other work). The main loop then decides from those timestamps whether
  a closure lasted at least `DEBOUNCE_MS`, even if it already ended, including when the contact bounced on release. Also poll the
  pin at least every 100 ms as a backup to the interrupt.
- **R4b: Log how long each closure lasted (to answer Q1).** When the contact opens, log how long it was closed to
  Serial, and publish it (not retained) on `homeassistant/dme_alarm/duration` in seconds. This lets the owner
  measure the real relay behaviour, for example during the monthly test alarm.

### MQTT interface

- **R5: Topics and payloads.** Keep the existing event topic unchanged so the current Home Assistant automation
  keeps working:

  | Topic | Payload | Retained | QoS | When |
  |---|---|---|---|---|
  | `homeassistant/dme_alarm` | `ALARM` | no | 1 | Once per alarm (R3), including a delayed alarm (R6) |
  | `homeassistant/dme_alarm/state` | `ON` / `OFF` | yes | 1 | Whenever the debounced contact state changes, and again after every (re)connect |
  | `homeassistant/dme_alarm/availability` | `online` / `offline` | yes | 1 | `online` right after each connect; `offline` as the LWT (R8) |
  | `homeassistant/dme_alarm/duration` | closure length in seconds | no | 1 | When the contact opens again (R4b) |

  Put all topic names in `UserConfig`. Home Assistant uses the event topic for the automation and the state topic
  to know whether the contact is closed, even after it restarts.
- **R6: Send alarms that occurred while offline, if still recent.** If an alarm occurs while Wi-Fi or MQTT is
  down, keep it and publish `ALARM` as soon as the connection is back, but only if the alarm is younger than
  `ALARM_MAX_AGE_MS` (configurable; default 10 minutes). Drop older alarms and log them to Serial. The payload
  stays exactly `ALARM`, so the automation needs no changes.

### Health monitoring (LWT)

- **R7: Short keep-alive.** Set `mqttClient.setKeepAliveInterval()` to 15–30 s (configurable), so the broker
  notices a dead device within about 1.5 × that time.
- **R8: Last Will and Testament.** Before every `connect()`, register a retained QoS 1 will with the payload
  `offline` on the availability topic (`beginWill()` / `print()` / `endWill()`). Right after connecting, publish a
  retained `online`. Before a deliberate reboot (R11), publish `offline` first.
- **R9: No heartbeat, by design.** The owner chose the LWT alone for health monitoring. For the blind spot this
  leaves, see "Accepted limitations".

### Self-recovery

- **R10: Reconnect automatically without blocking.** `loop()` checks `WiFi.status()` and `mqttClient.connected()`
  on every pass. On failure it reconnects with exponential backoff (e.g. 1 s → 2 s → … capped at 60 s). Alarm
  detection keeps running between attempts. Wi-Fi comes first; MQTT only reconnects after Wi-Fi is up. After
  every MQTT reconnect, re-register the LWT and republish `online` and the current state.
- **R11: Escalate step by step.**
  1. Reconnect with backoff (R10).
  2. After N consecutive Wi-Fi failures (configurable), reset the NINA Wi-Fi module (`WiFi.end()`, then
     `WiFi.begin()` again).
  3. If the device has been offline longer than `MAX_OFFLINE_MS` (configurable, e.g. 5 minutes), reboot the
     board (`NVIC_SystemReset()`).
  4. Never reboot while an alarm is pending but not yet sent, unless it is older than `ALARM_MAX_AGE_MS`.
- **R12: Hardware watchdog.** Enable the RP2040 hardware watchdog (`mbed::Watchdog`, maximum timeout about 8.3 s)
  in `setup()` and kick it in `loop()`. Every blocking call must finish well within the watchdog timeout, or kick
  the watchdog while it waits. This applies especially to `WiFi.begin()`, `mqttClient.connect()`, and delays in
  retry loops. Use `mqttClient.setConnectionTimeout()` where it helps.
- **R13: Start up without USB.** Startup must never wait for Serial or anything else external. Serial is only for
  debug output.
- **R14: Report the reason for a restart (optional but recommended).** Log to Serial whether the last restart was
  caused by the watchdog or by power-on. Once connected, it can also be published once, e.g. on
  `homeassistant/dme_alarm/boot`.
- **R15: Status LED (optional).** Use the RGB LED to show the state on site, for example red = no Wi-Fi,
  blue = no MQTT, green = connected, and flashing = alarm.

## Failure modes and recovery

| # | Failure | Detected by | Recovery | Visible in Home Assistant |
|---|---|---|---|---|
| F1 | Firmware or NINA call hangs | Hardware watchdog (R12) | Automatic reboot | Briefly `offline` (LWT) |
| F2 | Wi-Fi drop or access point reboot | `WiFi.status()` (R10) | Reconnect → reset NINA → reboot (R11) | `offline` (LWT) until recovered |
| F3 | Broker down or restarting | `mqttClient.connected()` (R10) | Reconnect with backoff → reboot (R11) | Home Assistant sees the broker disconnect itself |
| F4 | Half-open TCP connection | MQTT keep-alive (R7) | Reconnect | `offline` (LWT) |
| F5 | Alarm while offline | Latched edge (R4) | Sent on reconnect if younger than `ALARM_MAX_AGE_MS` (R6) | Delayed `ALARM` |
| F6 | Power loss or dead hardware | Broker keep-alive timeout | None possible on the device | `offline` (LWT) → notify user |
| F7 | Contact bounce or EMI | Debounce (R2) | Glitch is ignored | — |
| F8 | Alarm during a blocking call | Interrupt latch (R4) | Processed after the call returns | Slightly delayed `ALARM` |
| F8a | Alarm starts while the device is off, or reboot during a closure | Closed relay at startup (R3) | Treated as a new alarm | `ALARM` (possibly a duplicate, which is accepted) |
| F9 | Home Assistant restarts | — | Retained state and availability topics (R5, R8) | State recovered automatically |

## Home Assistant side

The firmware must support this Home Assistant setup. Document any changes to topics or payloads here, and keep the
importable configuration in `home_assistant/` (package with the MQTT entities, automations to paste into the UI,
and a README with the import steps) in sync.

- **Light automation:** when MQTT topic `homeassistant/dme_alarm` receives the payload `ALARM`, turn the light on.
  Turning it off, whether automatically after X minutes or by hand, is up to the owner. It must not depend on the
  relay opening, because the relay may stay closed for hours.
- **Contact state:** an MQTT `binary_sensor` on `homeassistant/dme_alarm/state` (`payload_on: ON`,
  `payload_off: OFF`) with `availability_topic: homeassistant/dme_alarm/availability`.
- **Outage notification:** an automation that triggers when the binary sensor becomes `unavailable` for longer
  than about 2 minutes, which avoids alerts during short reconnects → notify the user, e.g. via a push message
  from the companion app. It can also send a notification when the sensor is available again.
- **Duration sensor (optional):** an MQTT sensor on `homeassistant/dme_alarm/duration` (not retained) reports each
  closure's length in seconds (R4b) - useful for measuring the real relay behaviour (Q1) during the monthly test
  alarm.
- **Boot sensor (optional):** an MQTT sensor on `homeassistant/dme_alarm/boot` (not retained) reports the restart
  reason (e.g. `power-on-or-reset`, `watchdog`, `software-reboot`) once per boot (R14). Power loss and the reset
  button both report `power-on-or-reset`; see "Implementation notes" for why they are indistinguishable on this
  board.

## Accepted limitations

These gaps are known and accepted. Do not try to "fix" them without asking the owner.

- **Stale `online` after a broker restart.** When the broker restarts, the stored LWT is lost, while the retained
  `online` may survive. If the Arduino then never reconnects, Home Assistant keeps showing `online`. A heartbeat
  would close this gap, but the owner chose the LWT alone (R9). Watchdog and auto-reboot (R11, R12) make it
  unlikely that the device stays offline for good.
- **No alert if the broker or Home Assistant is down.** Nothing is left to deliver a notification. Covering this
  needs an external monitor, which is out of scope.
- **The wiring and relay cannot be supervised automatically.** With a two-wire dry contact, a disconnected cable
  looks like "no alarm". The owner checks the whole path **by hand** during the monthly test alarm (see "Manual
  end-to-end test"). No automated test-alarm monitoring should be built.
- **Duplicate `ALARM` after a reboot during a closure.** If the device restarts while the relay is still closed,
  it reports the same alarm again (R3). The owner accepts this because reboots are rare.

## Manual end-to-end test

The alerting service sends a **test alarm on every second Saturday of the month at about 10:15**. When available,
the owner checks that:

1. the light turns on (the whole path: pager → relay → Arduino → MQTT → Home Assistant → light), and
2. the closure duration published on `homeassistant/dme_alarm/duration` looks plausible (R4b).

This test is manual only, by the owner's decision. Do not build Home Assistant automations that watch for it.

## Open questions

- **Q1:** How long does the TPG2200 charging-station relay stay closed per alarm? This has not been measured yet,
  and sources disagree: from a few seconds up to "until someone acknowledges it" (hours or days). The design
  (R3, R4) must work across that whole range. Measure it during the next test alarm (R4b) and record the
  result here.

## Hardware

| Item | Detail |
|---|---|
| Board | Arduino Nano RP2040 Connect (Wi-Fi via the u-blox NINA-W102 module) |
| Core | `arduino:mbed_nano` 4.6.0 |
| Alarm input | `SWITCH_PIN = 2` (D2), `INPUT_PULLUP`, active-low (`SWITCH_ACTIVE_HIGH = false` in `UserConfig`) |
| Relay wiring | TPG2200 charging-station contacts connected to D2 and GND, chosen over 3.3V because a cable fault can't short the 3.3V rail, it picks up less Wi-Fi noise, and it's standard dry-contact practice. **The owner still has to move the relay wire from 3.3V to GND on site.** The 3.3V wiring stays supported as a fallback: set `SWITCH_ACTIVE_HIGH = true` in `UserConfig.cpp` for `INPUT_PULLDOWN`, HIGH = closed. |

## Toolchain and libraries

- Arduino IDE / `arduino-cli`. The sketch folder name must match the `.ino` file name (`sketch_sep20a`).
- FQBN: `arduino:mbed_nano:nanorp2040connect`
- Libraries, installed in `C:/Users/Leon/Documents/Arduino/libraries`:
  - `WiFiNINA` 2.1.1
  - `ArduinoMqttClient` 0.1.8
  - `Arduino_SpiNINA` 0.0.2
- `arduino-cli` (1.5.1) is **not on PATH**. Use the copy bundled with Arduino IDE 2:
  `%LOCALAPPDATA%\Programs\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe`
- Build and upload commands (Git Bash syntax):
  ```sh
  CLI="$LOCALAPPDATA/Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
  "$CLI" compile --fqbn arduino:mbed_nano:nanorp2040connect .
  "$CLI" board list                      # find the COM port
  "$CLI" upload  --fqbn arduino:mbed_nano:nanorp2040connect -p <COMx> .
  "$CLI" monitor -p <COMx> -c baudrate=9600
  ```
  The current code compiles cleanly: 109832 bytes (0%) of flash and 44612 bytes (16%) of RAM. Building with
  `--warnings all` shows warnings only from the `ArduinoMqttClient` library (`-Wvla`, variable-length arrays in
  `MqttClient.cpp`), never from a sketch file.
- `.vscode/c_cpp_properties.json` configures IntelliSense only; it plays no part in the build. It is untracked.
- The repository has no automated tests. Check changes by compiling, then by testing on hardware: close the relay
  or bridge D2↔GND (the GND pin is right next to D2) and watch the MQTT topic, for example with
  `mosquitto_sub -h <broker> -t homeassistant/# -v`.

## Mandatory build verification

**Verify every change with a successful build before you consider it done.** No exceptions: this applies to small
edits, refactors, and configuration changes alike.

1. After changing any source file (`.ino`, `.h`, or `.cpp`), run:
   ```sh
   "$CLI" compile --fqbn arduino:mbed_nano:nanorp2040connect .
   ```
2. If the build fails, **fix your changes and build again**. Repeat until the build succeeds. Never hand back code
   that does not compile. Do not work around a failing build by removing functionality, commenting out code, or
   reverting unrelated parts.
3. Do not report a change as done until you have seen a successful build. In your report, include the result: the
   flash and RAM usage lines, plus any new compiler warnings.
4. If you cannot run the build at all (for example, the CLI or the board core is missing), say so explicitly. Do
   not claim the change was verified.

A successful build does not replace a hardware test. Behaviour that depends on the relay, Wi-Fi, or MQTT still
needs testing on the device, as described above.

## Code layout

The code is plain Arduino C++. Each module has a `.h`/`.cpp` pair with `#ifndef` include guards, and modules share
state through `extern` globals.

| File | Responsibility |
|---|---|
| `sketch_sep20a.ino` | `setup()`/`loop()` orchestration only: `healthBegin()`, `alarmInputBegin()`, `connectionBegin()`, then each pass runs `healthKick()`, `alarmInputUpdate()`, `connectionUpdate()`, `publisherOnConnected()`/`publisherUpdate()` in the order documented in the header comments. |
| `UserConfig.h/.cpp` | All configuration constants: Wi-Fi/MQTT identity (populated from `arduino_secrets.h`), MQTT topics, `SWITCH_PIN`/`SWITCH_ACTIVE_HIGH`, debounce, alarm-age, MQTT/TCP timeouts, reconnect backoff, and watchdog timeout. |
| `arduino_secrets.h` (untracked) / `arduino_secrets.h.example` (committed) | Real Wi-Fi/MQTT credentials, kept out of git; the `.example` file is the template (see "Security: credentials"). |
| `AlarmInput.h/.cpp` | ISR-latched edge timestamps plus a main-loop debounce state machine; turns raw pin transitions into a FIFO of `CLOSED`/`OPENED` events (R1-R4b). |
| `AlarmPublisher.h/.cpp` | Drains `AlarmInput` events into MQTT publishes: pending-alarm retry/expiry, state (`ON`/`OFF`), duration, availability, and boot reason (R5, R6, R4b). |
| `ConnectionManager.h/.cpp` | Non-blocking Wi-Fi + MQTT supervision: backoff, NINA reset, and reboot escalation (R10, R11). |
| `WiFiConnection.h/.cpp` | Defines `wifiClient`; `wifiSetup()`/`wifiTryConnect()`/`wifiIsConnected()`/`wifiResetModule()`. |
| `MqttConnection.h/.cpp` | Defines `mqttClient`; `mqttSetup()`/`mqttTryConnect()`/`mqttIsConnected()`/`mqttPublish()`, including the LWT (R7, R8). |
| `SystemHealth.h/.cpp` | Hardware watchdog and restart-reason reporting (R12, R14). |

### Conventions

- Keep the module split (one `.h`/`.cpp` pair per concern). Put new configuration values in `UserConfig`.
- Use non-blocking timing (`millis()` comparisons). Do not use long `delay()` calls in `loop()`.
- Serial output is for debugging only. The firmware must work when no USB host is connected.
- `ArduinoMqttClient` needs `mqttClient.poll()` to be called regularly to keep the connection alive.

## Implementation notes

Non-obvious facts a future agent needs before changing this code:

- **ISR pulse latching (R4).** The ISR doesn't just record the latest edges; it latches *complete* pulses of at
  least `DEBOUNCE_MS` behind a sequence counter (`isrPulseSeq`). Latest-edge timestamps alone would lose a closure
  that bounces open again right at release, entirely inside one blocking call. `alarmInputUpdate()` also separately
  handles "release, then a fresh re-close, both inside one blocking gap" so that case isn't swallowed either.
- **QoS 1 publishes block.** `mqttClient.endMessage()` for QoS 1 blocks until PUBACK, up to
  `MQTT_CONNECTION_TIMEOUT_MS`. `AlarmPublisher` calls `healthKick()` before every publish so a slow broker cannot
  trip the watchdog.
- **Watchdog feeding during Wi-Fi connect.** `WiFi.begin()` feeds the watchdog itself via
  `WiFi.setFeedWatchdogFunc(healthKick)` (registered in `wifiSetup()`). `mqttTryConnect()`'s own worst case is
  TCP connect (up to `TCP_CONNECT_TIMEOUT_MS`, ~2.5 s) plus the CONNACK wait (up to
  `MQTT_CONNECTION_TIMEOUT_MS`, ~2.5 s) - nothing feeds the watchdog during that, so it is kicked right before.
- **`WiFi.end()` is a no-op in WiFiNINA 2.1.1.** `wifiResetModule()` instead calls `SpiDrv::end()` to actually hold
  the NINA module in hardware reset. While held in reset, `wifiIsConnected()` must avoid touching SPI (any NINA SPI
  call lazily re-inits the module) or the reset would be undone early. The next `WiFi.begin()` re-initializes the
  module, a genuine ~760 ms reset/boot cycle.
- **Restart-reason ambiguity.** `NVIC_SystemReset()` and a real watchdog timeout are indistinguishable through
  `watchdog_caused_reboot()`/`mbed::ResetReason` alone on this chip. `healthReboot()` writes a magic marker to the
  watchdog's `SCRATCH0` register right before resetting, and `healthBegin()` checks it first so a deliberate reboot
  is reported as `"software-reboot"` instead of `"watchdog"`.
- **Accepted edge case.** Two separate short pulses that both happen inside a single blocking gap are reported as
  one alarm, not two (the debounce state machine only tracks one closure boundary at a time within a gap beyond the
  first release+re-close it already handles).
- **`InterruptIn` resets the pin's pull (found 2026-09-23).** On `arduino:mbed_nano` 4.6.0,
  `attachInterrupt(..., CHANGE)` constructs an `mbed::InterruptIn` on the pin, and that constructor re-initialises
  it with `PullDefault`, which is `PullNone` on RP2040. That silently discards the pull mode `pinMode()` set just
  before it, leaving D2 floating and causing random, spurious closures. `alarmInputBegin()` in `AlarmInput.cpp`
  fixes this by calling `digitalPinToGpio(SWITCH_PIN)->mode(...)` right after `attachInterrupt()` to re-apply the
  pull on the existing `DigitalInOut`, avoiding an object swap under the running ISR, then re-seeds the ISR state
  since anything latched while the pin floated is meaningless. Keep this fix if the ISR setup is ever touched.
- **Reading Serial without a terminal.** `arduino-cli monitor` exits immediately when stdin is not a TTY, which is
  the case in an agent's shell. To read Serial output non-interactively instead, use Python's `pyserial` (already
  installed): `serial.Serial("COM3", 9600)`.
- **Check jumpers physically before trusting a "no signal" result.** During bring-up, an earlier test that bridged
  3.3V to D2 produced no response at all: the pin never read HIGH, although the firmware demonstrably controlled
  it (the pull-up self-test read HIGH). Most likely the jumper never reached the pin; it was not a software or
  wiring-polarity problem. When a wiring test shows nothing happening, verify the physical connections before
  assuming the firmware or `SWITCH_ACTIVE_HIGH` setting is wrong.

## Security: credentials

Real Wi-Fi and MQTT credentials live in `arduino_secrets.h`, which is **untracked** (listed in `.gitignore`) and
must never be committed. `UserConfig.cpp` includes it and copies the values (`SECRET_SSID`, `SECRET_PASS`,
`SECRET_MQTT_ID`, `SECRET_MQTT_USER`, `SECRET_MQTT_PASS`, `SECRET_BROKER`) into `UserConfig`'s `ssid`/`pass`/
`mqtt_id`/`mqtt_user`/`mqtt_pass`/`broker`. `arduino_secrets.h.example` is committed as a template with
placeholder values; copy it to `arduino_secrets.h` and fill in the real ones to build.

- Never copy the real values into docs, commit messages, issues, logs, or any other output.

## Known gaps and issues

Check this list before making changes, and update it when items are fixed. Remove fixed items rather than marking
them done.

- **G8: No status LED (R15), on purpose.** The RGB LED (`LEDR`/`LEDG`/`LEDB`) is driven over SPI through the NINA
  module - the same component most likely to hang (R12, F1). Adding LED traffic there was judged not worth the
  risk, so it was left unimplemented.
- **G9: Hardware testing.** The refactor compiles cleanly. Verified on real hardware on 2026-09-23:
  - **Jumper between D2 and GND:** idle read HIGH, and three presses each produced exactly one `ALARM` plus
    `ON`/`OFF`/duration (1.568 s, 2.806 s, 0.560 s).
  - **Push-button between D2 and GND:**
    - short taps, a 30 s hold, and rapid repeated presses: exactly one `ALARM` per closure, with no repeats
      while a closure was held.
    - power loss: the LWT `offline` appears in Home Assistant about 13 s after power is cut, the
      offline/back-online notification automations fire correctly, and the device reconnects about 3.5 s after
      power is restored.
    - relay already closed at power-up: `ALARM` is sent first, right after the first successful connect (R3,
      F8a).
    - a press landing inside the blocking Wi-Fi connect: caught by the interrupt latch and sent once the connect
      finishes (R4, F8).
    - the reset button: the device reconnects normally afterward. Power loss and the reset button both report
      `RESET_REASON_PIN_RESET`; the boot-reason label was renamed to `power-on-or-reset` to reflect that the two
      are indistinguishable on this board (see "Implementation notes").

  Still to test:
  - a Wi-Fi outage: an alarm queued while offline is sent on reconnect if still younger than `ALARM_MAX_AGE_MS`
    (R6), and a reboot after `MAX_OFFLINE_MS` (5 min) of being offline
  - a broker outage
  - NINA reset after `WIFI_FAILURES_BEFORE_RESET` consecutive Wi-Fi failures
  - a real watchdog timeout
  - whether the `WATCHDOG_SCRATCH0` software-reboot marker actually survives `NVIC_SystemReset()` on real
    hardware

  **Watch for:**
  - during the 2026-09-23 jumper test, the broker's PUBACKs stalled for ~9 s once; three QoS 1 state publishes
    timed out after `MQTT_CONNECTION_TIMEOUT_MS` and were retried, all eventually delivered (harmless duplicate
    retained `ON`; a duplicate `ALARM` is possible the same way and is also harmless). Suspected cause was a
    Wi-Fi hiccup, not investigated. If this recurs, check whether NINA power-saving mode is involved.
  - after a quick restart, the broker can publish the old session's LWT `offline` about 0.1 s before the new
    session's `online`, because the broker is still tearing down the previous session (session takeover). This
    is harmless: the outage automation only notifies after about 2 minutes of `unavailable`.
  - the first `WiFi.begin()` attempt after boot sometimes fails with status `4` (`WL_CONNECT_FAILED`) and then
    succeeds on the 1 s retry. Not investigated further; the reconnect-with-backoff logic already handles it.
