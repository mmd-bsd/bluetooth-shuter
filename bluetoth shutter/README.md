# ESP32 Bluetooth Camera Shutter

A DIY version of a commercial Bluetooth selfie-stick shutter. Press a physical button, the
phone takes a photo.

## How it works

The ESP32 advertises itself as a **BLE HID keyboard**. On a button press it sends the Consumer
Control **Volume Up** key (usage page `0x0C`, usage `0xE9`).

There is no camera API involved. Both iOS and Android camera apps bind a *hardware* Volume Up
press to the shutter, which is the entire trick behind the cheap commercial units. They are not
doing anything cleverer than this.

## Wiring

```
        NodeMCU-32S
      ┌─────────────┐
      │             │
 GPIO25 ──────┬─────┤   INPUT_PULLUP  (pressed = LOW)
              │     │
           [button] │
              │     │
 GPIO26 ──────┴─────┤   OUTPUT, driven LOW = switched ground
      │             │
      └─────────────┘
```

Two wires, no resistors, no capacitor.

GPIO26 is driven LOW in `setup()` to act as the ground reference, which is why no wire to a GND
pin is needed. The internal pull-up on GPIO25 supplies the only current in the loop (~70 µA).

Two notes on that choice:

- **The order of the first lines of `setup()` matters.** GPIO26 is driven LOW *before* GPIO25 is
  configured. If that ever gets reversed, GPIO25 floats and reads as permanently released — the
  button does nothing, and the symptom points at BLE rather than at the wiring. The serial log
  below exists so this is visible instead of silent.
- **If it ever reads PRESSED with nothing touching the button**, the wiring is wrong.

Prefer a real GND pin? Wire the button to any GND pin and build with `-D BTN_RETURN_PIN=-1`.

The onboard LED (GPIO2) shows state: **slow blink** = advertising, **solid** = phone connected,
**quick flash** = shutter fired.

## Build and flash

```bash
pio run              # compile
pio run -t upload    # flash
pio device monitor -b 115200
```

### If upload fails with "Wrong boot mode detected"

The chip needs to be in download mode. On the NodeMCU-32S:

1. Hold the **BOOT** button down.
2. Press and release **EN** (sometimes labelled RST).
3. Release **BOOT**.
4. Re-run `pio run -t upload`.

Some boards need you to keep holding BOOT for the first second of the upload.

## Pairing

1. Power the ESP32. The LED should slow-blink.
2. On the phone: **Settings → Bluetooth → Pair new device → "ESP32 Shutter"**.
3. The LED goes solid once connected.

Pairing uses **Just Works** — there is no PIN prompt, and that is expected.

Once bonded, the ESP32 reconnects on power-up with no re-pairing (the MAC address comes from
eFuse, so it never changes).

## First test — do this before blaming the camera

Open any app with a text field, or just the home screen, and watch the **volume indicator**
while pressing the button. You should see the volume step up 3 times.

This is the single most useful check, because it cleanly separates the two things that can fail:

- Volume does **not** change → the BLE HID link is the problem.
- Volume **does** change, but the camera ignores it → the link is fine, and the camera app's
  volume-key setting is the problem.

## If it pairs but the camera won't shoot

The Volume Up key reaches the camera app identically whether it came from a real button or from
BLE — but the *camera app* decides what to do with it. Check these, in order:

| Phone | Setting |
|---|---|
| **Samsung One UI** | Camera → Settings → *Shooting methods → Press Volume keys to* → must be **"Take pictures"**, not "Zoom" or "Control sound volume" |
| **Xiaomi MIUI** | Camera → Settings → *Volume button function* → **Shutter** |
| **Pixel / stock Android** | There is an unresolved report ([T-vK#39](https://github.com/T-vK/ESP32-BLE-Keyboard/issues/39)) of Vol+ silently not working on Android 10+. If you land here, rebuild with `-D SHUTTER_SEND_ENTER=1` |

`SHUTTER_SEND_ENTER=1` additionally sends Enter after each Vol+. It is off by default because
outside the camera app that Enter goes to whatever text field has focus.

## Tuning

Edit the `build_flags` in `platformio.ini`:

| Flag | Default | Meaning |
|---|---|---|
| `SHUTTER_BURST_COUNT` | `3` | Photos per press. Set to `1` for one-shot. |
| `SHUTTER_BURST_GAP_MS` | `400` | Gap between shots. |
| `SHUTTER_SEND_ENTER` | `0` | Also send Enter (Android fallback). |
| `BTN_RETURN_PIN` | `26` | `-1` if wired to a real GND pin. |

**Do not lower `SHUTTER_BURST_GAP_MS` below ~300 ms.** Android camera apps need roughly
300–600 ms per capture (autofocus, encode, save) and coalesce presses inside that window, so a
faster burst usually yields only one photo rather than three.

A full burst blocks the main loop for about 1.2 s, during which a second press is ignored. That
is deliberate — it stops a double-tap from firing six shots.

## Things worth knowing

- **Android hides the on-screen keyboard** once this is bonded, because it now sees a physical
  keyboard. This is expected, not a broken pairing. A notification offers "use on-screen
  keyboard" if you need it back.
- **A stale bond is the most common failure.** BLE bonds live in NVS and survive a normal
  reflash. If the phone refuses to connect after you re-flash the ESP32, *Forget* the device in
  the phone's Bluetooth settings and reboot the phone.
- **Outside the camera app**, each press is 3 discrete volume steps.
- **Long or unshielded button wires** can pick up interference and cause phantom triggers. If
  that happens, a 100 nF capacitor from GPIO25 to GND fixes it. Software debounce alone will
  not help against EMI.

## Why the versions are pinned

`platformio.ini` pins `espressif32@6.11.0` and `ESP32 BLE Keyboard@0.3.2`. Do not unpin them.

`BleKeyboard.cpp` includes `<driver/adc.h>`, a legacy ESP-IDF ADC driver that was **removed in
ESP-IDF 5.x**. espressif32 6.11.0 ships Arduino core 2.0.17 (IDF 4.4.7) where it still exists;
7.x ships core 3.x (IDF 5.x) where it does not, and the library will not compile.

The library's last release is 0.3.2, from February 2022, so it is not going to be updated for
core 3.x. If you ever need core 3.x, the library has to be vendored into `lib/` and patched.

`board_build.partitions = huge_app.csv` gives the app a 3 MB slot. The default layout's 1280 KB
slot is tight for Bluedroid + BLE HID. Costs OTA, which this project does not need.
