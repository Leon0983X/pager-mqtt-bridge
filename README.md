# DME Alarm → MQTT Bridge

Firmware for an **Arduino Nano RP2040 Connect** that detects an alarm on a **Motorola TPG2200** digital pager
(DME, *Digitaler Meldeempfänger*) and forwards it over **MQTT**. When the pager gets an alarm, a potential-free
relay in its charging station closes. The Arduino detects that and publishes `ALARM` to an MQTT broker, where
**Home Assistant** can, for example, switch on a light.

The firmware is built to run unattended for a long time:

- **Detection:** a pin-change interrupt plus polling, with debouncing. Each alarm is reported exactly once,
  however long the relay stays closed.
- **Offline alarms:** an alarm that arrives while Wi-Fi or MQTT is down is sent once the connection is back,
  if it is still recent (default 10 minutes).
- **Health monitoring:** a Last Will (`online`/`offline`) and a short keep-alive, so Home Assistant notices
  when the device goes dark.
- **Self-recovery:** reconnects with backoff, resets the Wi-Fi module, and reboots if needed. A hardware
  watchdog covers hangs.

> **Adapting it to other setups.** Nothing in the firmware is specific to the TPG2200 except the wiring
> description. Any device with a **dry contact** (door contacts, other pagers, alarm panels, doorbells, float
> switches, …) works the same way. The pin, contact polarity, debounce time, MQTT topics, and timeouts are all
> set in [`UserConfig.cpp`](UserConfig.cpp). The MQTT side isn't tied to Home Assistant either: any broker and
> any consumer (Node-RED, openHAB, ioBroker, …) can subscribe to the topics. Other Wi-Fi boards need changes
> in `WiFiConnection` (WiFiNINA-specific) and `SystemHealth` (RP2040/mbed watchdog).

## MQTT interface

| Topic | Payload | Retained | When |
|---|---|---|---|
| `homeassistant/dme_alarm` | `ALARM` | no | Once per alarm, including alarms delayed by an outage |
| `homeassistant/dme_alarm/state` | `ON` / `OFF` | yes | When the contact changes, and after every (re)connect |
| `homeassistant/dme_alarm/availability` | `online` / `offline` | yes | `online` after each connect; `offline` as Last Will |
| `homeassistant/dme_alarm/duration` | closure length in seconds | no | When the contact opens again |
| `homeassistant/dme_alarm/boot` | `power-on-or-reset` / `watchdog` / `software-reboot` | no | Once after every boot |

All messages use QoS 1.

## Hardware

| Item | Detail |
|---|---|
| Board | Arduino Nano RP2040 Connect |
| Alarm input | Pin **D2** |
| Wiring | The two relay contacts of the charging station go to **D2** and **GND**. No extra parts are needed; the internal pull-up is used. |

The relay is a dry contact, so polarity doesn't matter. If you would rather wire it between **D2** and **3.3V**,
set `SWITCH_ACTIVE_HIGH = true` in `UserConfig.cpp` (the pin then uses the internal pull-down). GND is the
recommended wiring: a cable fault can't short the 3.3V rail, and it picks up less noise.

## Home Assistant setup

You need Home Assistant with a working **MQTT integration** that connects to the same broker as the Arduino.
The files are in [`home_assistant/`](home_assistant/); [`home_assistant/README.md`](home_assistant/README.md)
has more detail.

### 1. Add the MQTT entities (package)

1. In your Home Assistant config folder (next to `configuration.yaml`), create a folder `packages` and copy
   [`home_assistant/packages/dme_alarm.yaml`](home_assistant/packages/dme_alarm.yaml) into it. The File editor
   or Studio Code Server add-on, Samba, or SSH all work for this.
2. Enable packages in `configuration.yaml`. If a `homeassistant:` block already exists, add only the
   `packages:` line inside it:
   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```
3. Go to **Developer tools → YAML → Check configuration**, then **Restart**.
4. You should now have these entities:
   - `binary_sensor.dme_alarm_contact`: relay closed or open, `unavailable` when the device is offline
   - `sensor.dme_alarm_closure_duration`: how long the last closure lasted
   - `sensor.dme_alarm_boot_reason`: why the device last restarted

### 2. Add the automations

For each file in [`home_assistant/automations/`](home_assistant/automations/):

| File | Purpose |
|---|---|
| `dme_alarm_light_on.yaml` | Between sunset and sunrise: turns on a light when `ALARM` arrives, and off again after 3 minutes |
| `dme_alarm_offline.yaml` | Sends a notification when the device has been offline for 2 minutes |
| `dme_alarm_back_online.yaml` | Optional: sends a notification when it is back online |

1. **Settings → Automations & scenes → Create automation → Create new automation**.
2. Open the menu (⋮, top right) → **Edit in YAML**, and replace everything with the file's contents.
3. Replace the placeholders:
   - `light.YOUR_LIGHT` with the light to switch on
   - `notify.mobile_app_YOUR_PHONE` with your phone's notify action (find it under
     **Developer tools → Actions**, search for `notify.mobile_app`)
4. **Save**.

The automations use the current syntax (`triggers:` / `trigger:` / `action:`). Home Assistant versions older
than 2024.10 need `trigger:` / `platform:` / `service:` instead.

> Don't make the light turn off when the contact opens. Depending on the pager, the relay can stay closed for
> hours. The provided automation uses a 3-minute timer instead; change the `delay` to adjust it.

### 3. Test without the Arduino (optional)

With the Mosquitto command-line tools (add `-u <user> -P <password>` if your broker needs them):

```sh
mosquitto_pub -h <broker> -t homeassistant/dme_alarm -m ALARM                      # light should turn on
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/availability -m online -r
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/state -m ON -r
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/availability -m offline -r   # notification after 2 min
# Remove the fake retained messages afterwards:
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/state -r -n
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/availability -r -n
```

## Firmware: configure, build, and flash

### Requirements

- [Arduino IDE 2](https://www.arduino.cc/en/software) or [`arduino-cli`](https://arduino.github.io/arduino-cli/)
- Board core **Arduino Mbed OS Nano Boards** (`arduino:mbed_nano`, tested with 4.6.0)
- Libraries (tested versions):
  - `WiFiNINA` 2.1.1
  - `ArduinoMqttClient` 0.1.8
  - `Arduino_SpiNINA` 0.0.2

### 1. Get the code

```sh
git clone https://github.com/Leon0983X/pager-mqtt-bridge.git sketch_sep20a
```

The folder name must be `sketch_sep20a`, the same as the `.ino` file, or the Arduino tools won't open it.

### 2. Enter your credentials

Copy `arduino_secrets.h.example` to `arduino_secrets.h` and fill in your values:

```c
#define SECRET_SSID      "your-wifi-ssid"
#define SECRET_PASS      "your-wifi-password"
#define SECRET_MQTT_ID   "your-mqtt-client-id"   // must be unique on the broker
#define SECRET_MQTT_USER "your-mqtt-username"
#define SECRET_MQTT_PASS "your-mqtt-password"
#define SECRET_BROKER    "192.168.1.100"         // broker IP or hostname
```

`arduino_secrets.h` is listed in `.gitignore`. Never commit it.

### 3. Adjust the settings (optional)

Everything else is in [`UserConfig.cpp`](UserConfig.cpp). The defaults work for the setup described above.

| Setting | Default | Meaning |
|---|---|---|
| `port` | `1883` | MQTT broker port |
| `TOPIC_*` | `homeassistant/dme_alarm/...` | MQTT topics. If you change them, update the Home Assistant files too. |
| `SWITCH_PIN` | `2` | Input pin for the relay |
| `SWITCH_ACTIVE_HIGH` | `false` | `false`: relay to GND (pull-up). `true`: relay to 3.3V (pull-down). |
| `DEBOUNCE_MS` | `50` | How long the contact must stay in a new state before it counts |
| `ALARM_MAX_AGE_MS` | 10 min | Alarms older than this are dropped instead of being sent after an outage |
| `MQTT_KEEP_ALIVE_MS` | `20000` | MQTT keep-alive. The broker declares the device offline after about 1.5× this. |
| `WIFI_FAILURES_BEFORE_RESET` | `5` | Failed Wi-Fi attempts before the Wi-Fi module is reset |
| `MAX_OFFLINE_MS` | 5 min | Offline time before the board reboots itself |
| `WATCHDOG_TIMEOUT_MS` | `8000` | Hardware watchdog timeout (the RP2040 maximum is about 8.3 s) |

### 4. Build and flash

**Arduino IDE:**

1. Install the board core: **Tools → Board → Boards Manager**, search for "Arduino Mbed OS Nano Boards".
2. Install the three libraries through **Tools → Manage Libraries**.
3. Open `sketch_sep20a.ino`, select **Tools → Board → Arduino Nano RP2040 Connect** and the right port.
4. Click **Upload**.

**arduino-cli:**

```sh
arduino-cli core install arduino:mbed_nano
arduino-cli lib install WiFiNINA ArduinoMqttClient Arduino_SpiNINA

arduino-cli compile --fqbn arduino:mbed_nano:nanorp2040connect .
arduino-cli board list                                             # find the port
arduino-cli upload  --fqbn arduino:mbed_nano:nanorp2040connect -p <PORT> .
```

### 5. Check that it works

1. Open the serial monitor at **9600 baud** (`arduino-cli monitor -p <PORT> -c baudrate=9600`). It shows the
   Wi-Fi and MQTT connection and every contact change. Serial is only for debugging; the device runs without
   a computer attached.
2. Watch the MQTT topics:
   ```sh
   mosquitto_sub -h <broker> -t 'homeassistant/dme_alarm/#' -v
   ```
3. Briefly bridge **D2** and **GND** (the pins are next to each other). You should see `ALARM`, then
   `state ON`, and, once you release it, `state OFF` and the closure duration. The light in Home Assistant
   should turn on.

Then disconnect USB and power the board from any USB power supply. Test the whole chain from time to time with
a real (test) alarm, because the device can't tell a disconnected relay cable from "no alarm".

## Project structure

| File | Responsibility |
|---|---|
| `sketch_sep20a.ino` | `setup()` / `loop()`, which only call the modules below |
| `UserConfig.h/.cpp` | All settings |
| `AlarmInput.h/.cpp` | Interrupt-latched, debounced contact detection |
| `AlarmPublisher.h/.cpp` | Turns contact events into MQTT messages; queues alarms while offline |
| `ConnectionManager.h/.cpp` | Wi-Fi/MQTT supervision, backoff, module reset, reboot |
| `WiFiConnection.h/.cpp` | Wi-Fi (WiFiNINA) |
| `MqttConnection.h/.cpp` | MQTT client, keep-alive, Last Will |
| `SystemHealth.h/.cpp` | Hardware watchdog and restart reason |
| `home_assistant/` | Home Assistant package and automations |

[`AGENTS.md`](AGENTS.md) holds the full requirements, the failure modes and how each is handled, and
implementation notes.

## License

[GNU General Public License v3.0](LICENSE)
