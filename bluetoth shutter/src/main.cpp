/*
 * ESP32 Bluetooth Camera Shutter
 * ------------------------------
 * A DIY version of a commercial Bluetooth selfie-stick shutter.
 *
 * The ESP32 advertises itself as a BLE HID keyboard and, on a button press,
 * sends the Consumer Control "Volume Up" key (usage page 0x0C, usage 0xE9).
 * No camera API is involved -- both iOS and Android camera apps bind a
 * hardware Volume Up press to the shutter, which is exactly how the cheap
 * commercial units work.
 *
 * Hardware: NodeMCU-32S (ESP32). The button bridges GPIO25 and GPIO26, with
 * GPIO26 driven LOW to act as a switched ground, so no wire to a GND pin is
 * needed. GPIO25 uses its internal pull-up, so pressed == LOW.
 */

#include <Arduino.h>
#include <BleKeyboard.h>

// ------------------------------------------------------------------ config ---
// Defaults for every tunable; override any of them from build_flags.

#ifndef SHUTTER_BURST_COUNT
#define SHUTTER_BURST_COUNT 3
#endif

#ifndef SHUTTER_BURST_GAP_MS
#define SHUTTER_BURST_GAP_MS 400
#endif

#ifndef SHUTTER_SEND_ENTER
#define SHUTTER_SEND_ENTER 0
#endif

// GPIO26 as switched ground. Build with -D BTN_RETURN_PIN=-1 if you rewire the
// button to a real GND pin instead.
#ifndef BTN_RETURN_PIN
#define BTN_RETURN_PIN 26
#endif

static const uint8_t PIN_BTN = 25;
static const uint8_t PIN_LED = LED_BUILTIN;  // GPIO2 on the NodeMCU-32S

static const uint32_t DEBOUNCE_MS       = 40;
static const uint32_t KEY_HOLD_MS       = 30;   // gap between key-down and key-up
static const uint32_t ENTER_GAP_MS      = 50;
static const uint32_t CONNECT_SETTLE_MS = 500;
static const uint32_t LED_BLINK_MS      = 500;
static const uint32_t LED_PULSE_MS      = 60;

// The library truncates the device name to 15 characters.
BleKeyboard bleKeyboard("ESP32 Shutter", "DIY", 100);

// ------------------------------------------------------------------- state ---
static bool     buttonRaw        = false;  // last raw read, true = pressed
static bool     buttonStable     = false;  // debounced, true = pressed
static uint32_t lastRawChangeMs  = 0;
static bool     linkUp           = false;
static bool     ledBlinkOn       = false;
static uint32_t ledToggleMs      = 0;
static bool     ledPulse         = false;
static uint32_t ledPulseUntilMs  = 0;

// ----------------------------------------------------------------- helpers ---
// The NodeMCU-32S LED is active HIGH (R9 + LED to GND).
static inline void ledWrite(bool on) {
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

static void ledPulseNow() {
  ledPulse = true;
  ledPulseUntilMs = millis() + LED_PULSE_MS;
}

/*
 * One shutter press: explicit down / hold / up.
 *
 * BleKeyboard::write() is deliberately NOT used here. It fires the key-down and
 * key-up GATT notifications back to back with no gap, and if the release is
 * lost on a congested link the phone sees Volume Up stuck down -- a runaway
 * volume ramp, or a repeating shutter. The hold also better mimics a real
 * button tap, which some Android input stacks require.
 */
static void sendShutterKey() {
  bleKeyboard.press(KEY_MEDIA_VOLUME_UP);
  delay(KEY_HOLD_MS);
  bleKeyboard.release(KEY_MEDIA_VOLUME_UP);

#if SHUTTER_SEND_ENTER
  // Fallback for camera apps that ignore Vol+. Note this goes out as a keyboard
  // report, so outside the camera app it lands in whatever field has focus.
  delay(ENTER_GAP_MS);
  bleKeyboard.press(KEY_RETURN);
  delay(KEY_HOLD_MS);
  bleKeyboard.release(KEY_RETURN);
#endif
}

static void shutterBurst() {
  // Defensive: releaseAll() zeroes the internal media-key state but only
  // notifies the *keyboard* report, so it cannot un-stick a Volume Up. An
  // explicit release is the only reliable way to clear one.
  bleKeyboard.release(KEY_MEDIA_VOLUME_UP);

  for (uint8_t i = 0; i < SHUTTER_BURST_COUNT; i++) {
    if (!bleKeyboard.isConnected()) {
      Serial.println("  link dropped mid-burst -- stopping.");
      break;
    }
    Serial.printf("  shot %u/%u\n", (unsigned)i + 1, (unsigned)SHUTTER_BURST_COUNT);
    sendShutterKey();
    if (i + 1 < SHUTTER_BURST_COUNT) {
      delay(SHUTTER_BURST_GAP_MS);
    }
  }

  ledPulseNow();
}

// ------------------------------------------------------------------- setup ---
void setup() {
  // Establish the ground reference FIRST. With the button bridging GPIO25 and
  // GPIO26, pin 25 floats until 26 is driven low and would read as permanently
  // released -- a silent failure that looks like a BLE fault.
#if BTN_RETURN_PIN >= 0
  pinMode(BTN_RETURN_PIN, OUTPUT);
  digitalWrite(BTN_RETURN_PIN, LOW);
#endif

  pinMode(PIN_BTN, INPUT_PULLUP);

  pinMode(PIN_LED, OUTPUT);
  ledWrite(false);  // avoid a stray flash during boot

  Serial.begin(115200);
  // Deliberately NOT while(!Serial) -- on the ESP32's USB-UART that can block
  // forever, and you would misdiagnose it as BleKeyboard::begin() failing.
  delay(200);

  Serial.println();
  Serial.println("ESP32 Bluetooth Camera Shutter");
  Serial.printf("  button : GPIO%u (pressed = LOW)\n", (unsigned)PIN_BTN);
#if BTN_RETURN_PIN >= 0
  Serial.printf("  ground : GPIO%u driven LOW\n", (unsigned)BTN_RETURN_PIN);
#else
  Serial.println("  ground : external GND wire");
#endif
  Serial.printf("  led    : GPIO%u\n", (unsigned)PIN_LED);
  Serial.printf("  burst  : %u shot(s), %u ms apart\n",
                (unsigned)SHUTTER_BURST_COUNT, (unsigned)SHUTTER_BURST_GAP_MS);

  // If this says PRESSED with nothing touching the button, the wiring is wrong.
  Serial.printf("  button reads %s at rest (expect RELEASED)\n",
                digitalRead(PIN_BTN) == LOW ? "PRESSED" : "RELEASED");

  bleKeyboard.begin();
  Serial.println("BLE advertising as \"ESP32 Shutter\" -- pair it from the phone.");

  buttonRaw = buttonStable = (digitalRead(PIN_BTN) == LOW);
}

// -------------------------------------------------------------------- loop ---
void loop() {
  const uint32_t now = millis();

  // -- link state ------------------------------------------------------------
  const bool connected = bleKeyboard.isConnected();
  if (connected != linkUp) {
    linkUp = connected;
    Serial.println(connected ? "Phone connected."
                             : "Phone disconnected -- advertising again.");
    if (connected) {
      // isConnected() can flip true a moment before the phone has subscribed to
      // the CCCDs, and that first keypress is sometimes swallowed. Settle, then
      // push a harmless release to force the subscription live.
      delay(CONNECT_SETTLE_MS);
      bleKeyboard.release(KEY_MEDIA_VOLUME_UP);
    }
  }

  // -- button debounce -------------------------------------------------------
  // Fire on the stable press edge only, never on release or on hold-repeat.
  const bool raw = (digitalRead(PIN_BTN) == LOW);
  if (raw != buttonRaw) {
    buttonRaw = raw;
    lastRawChangeMs = now;
  } else if (raw != buttonStable && (now - lastRawChangeMs) >= DEBOUNCE_MS) {
    buttonStable = raw;
    if (buttonStable) {
      Serial.println("Button pressed.");
      if (!bleKeyboard.isConnected()) {
        Serial.println("  not connected -- ignoring. Pair the phone first.");
      } else {
        shutterBurst();
      }
    }
  }

  // -- LED -------------------------------------------------------------------
  // Slow blink while advertising, solid when linked. A pulse inverts whatever
  // the base state is, so it stays visible in both cases.
  if (ledPulse && (int32_t)(now - ledPulseUntilMs) >= 0) {
    ledPulse = false;
  }

  bool baseOn;
  if (!linkUp) {
    if (now - ledToggleMs >= LED_BLINK_MS) {
      ledToggleMs = now;
      ledBlinkOn = !ledBlinkOn;
    }
    baseOn = ledBlinkOn;
  } else {
    baseOn = true;
  }
  ledWrite(ledPulse ? !baseOn : baseOn);
}
