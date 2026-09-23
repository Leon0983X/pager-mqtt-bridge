# Home Assistant setup

Configuration for the Home Assistant side of the DME alarm forwarder. Needs a working MQTT integration.

| File | What it is | How to import |
|---|---|---|
| `packages/dme_alarm.yaml` | MQTT entities: contact sensor, closure duration, boot reason | Package (step 1) |
| `automations/dme_alarm_offline.yaml` | Notify when the device has been offline for 2 min | Paste in UI (step 2) |
| `automations/dme_alarm_back_online.yaml` | Optional: notify when it's back after an outage | Paste in UI (step 2) |
| `automations/dme_alarm_light_on.yaml` | Reference only: your existing light automation | Only if you rebuild it |

## Step 1: entities (package)

1. In your Home Assistant config folder (where `configuration.yaml` lives), create a folder `packages` and copy
   `packages/dme_alarm.yaml` into it. Use the File editor or Studio Code Server add-on, or Samba/SSH.
2. Add this to `configuration.yaml` once. If a `homeassistant:` block already exists, add just the `packages:`
   line inside it:
   ```yaml
   homeassistant:
     packages: !include_dir_named packages
   ```
3. Developer tools → YAML → **Check configuration**, then **Restart**.
4. You should now have `binary_sensor.dme_alarm_contact`, `sensor.dme_alarm_closure_duration`, and
   `sensor.dme_alarm_boot_reason`. If Home Assistant named them differently, adjust the entity IDs in the
   automations.

## Step 2: automations (UI)

For each automation file:

1. Settings → Automations & scenes → **Create automation** → **Create new automation**.
2. Menu (⋮, top right) → **Edit in YAML**, replace everything with the file's contents.
3. Replace `notify.mobile_app_YOUR_PHONE` with your phone's notify action (find it under Developer tools →
   Actions, search "notify.mobile_app").
4. **Save**.

These use the current syntax (`triggers:` / `trigger:` / `action:`). Home Assistant versions before 2024.10 need
`trigger:` / `platform:` / `service:` instead.

## Testing without the Arduino

From any machine with the Mosquitto CLI tools (add `-u <user> -P <password>` if your broker needs them):

```sh
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/availability -m online -r
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/state -m ON -r
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/duration -m 12.345
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/availability -m offline -r   # wait 2 min → notification
# Clean up the fake retained messages afterwards:
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/state -r -n
mosquitto_pub -h <broker> -t homeassistant/dme_alarm/availability -r -n
```
