/*
 * ╔══════════════════════════════════════════════════════════╗
 * ║          ESP32 · BLUE LED DASHBOARD CONTROLLER          ║
 * ║          Controlled via Supabase REST API               ║
 * ╚══════════════════════════════════════════════════════════╝
 *
 * PIN   : D2 (GPIO2) — Built-in Blue LED on most ESP32 boards
 * DEPS  : WiFi.h, HTTPClient.h, ArduinoJson (v6+)
 *
 * FIX LOG:
 *   - Supabase fetch moved to a separate FreeRTOS task (core 0)
 *     so the main loop (core 1) handles LED without any stall.
 *   - blinkState no longer reset on every state change.
 *   - Solid ON now uses digitalWrite once and skips the blink timer.
 *   - Added mutex so state variables are thread-safe.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ─── USER CONFIG ──────────────────────────────────────────────
const char* WIFI_SSID     = "deku";
const char* WIFI_PASS     = "12taman9";

const char* SUPABASE_URL  = "https://kjfumdyatvvthmbzrhvm.supabase.co";
const char* SUPABASE_KEY  = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImtqZnVtZHlhdHZ2dGhtYnpyaHZtIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NzQ4MDE4ODksImV4cCI6MjA5MDM3Nzg4OX0.yVpYIQgYEm1W2W2pzsY5vphnACShkJA_mBRJK2soAPc";
const char* TABLE_NAME    = "led_control";

const int   ROW_ID        = 1;
const int   POLL_INTERVAL = 2000;   // ms between Supabase polls (runs on core 0)
// ──────────────────────────────────────────────────────────────

#define LED_PIN 2

// ─── Shared state (protected by mutex) ───────────────────────
SemaphoreHandle_t stateMutex;

volatile bool ledOn   = false;
volatile bool blinkEn = false;
volatile int  speedMs = 500;

// ─── Blink state (only touched by main loop / core 1) ────────
unsigned long lastBlinkToggle = 0;
bool          blinkPhase      = false;

// ─── Previous state for change detection ─────────────────────
bool lastLedOn   = false;
bool lastBlinkEn = false;
int  lastSpeedMs = -1;

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);   // start with LED off, clean state

  Serial.println();
  Serial.println("╔═══════════════════════════════╗");
  Serial.println("║   ESP32 LED Dashboard Ready   ║");
  Serial.println("╚═══════════════════════════════╝");

  // Create mutex before starting tasks
  stateMutex = xSemaphoreCreateMutex();

  connectWiFi();

  // ── Launch fetch task on core 0 ──────────────────────────
  // This keeps HTTP calls OFF the main loop so LED never stalls.
  xTaskCreatePinnedToCore(
    fetchTask,    // task function
    "fetchTask",  // name
    8192,         // stack size
    NULL,         // params
    1,            // priority
    NULL,         // handle
    0             // core 0
  );
}

// ═════════════════════════════════════════════════════════════
//  MAIN LOOP — core 1 only, never blocks
// ═════════════════════════════════════════════════════════════
void loop() {
  // Snapshot shared state safely
  bool  _ledOn, _blink;
  int   _speed;

  if (xSemaphoreTake(stateMutex, 0) == pdTRUE) {  // non-blocking take
    _ledOn = ledOn;
    _blink = blinkEn;
    _speed = speedMs;
    xSemaphoreGive(stateMutex);
  } else {
    // Mutex busy (fetch task writing) — keep last known state, don't stall
    return;
  }

  handleLED(_ledOn, _blink, _speed);
  // No delay here — loop runs as fast as possible for smooth blinking
}

// ═════════════════════════════════════════════════════════════
//  LED HANDLER — called every loop iteration
// ═════════════════════════════════════════════════════════════
void handleLED(bool _ledOn, bool _blink, int _speed) {

  if (!_ledOn) {
    // ── OFF: drive LOW and hold ───────────────────────────
    digitalWrite(LED_PIN, LOW);
    blinkPhase = false;              // reset so next ON starts clean
    return;
  }

  if (!_blink) {
    // ── SOLID ON: drive HIGH and hold ────────────────────
    // Write every iteration — this actively fights any noise/glitch.
    digitalWrite(LED_PIN, HIGH);
    blinkPhase = false;
    return;
  }

  // ── BLINK: non-blocking toggle ───────────────────────────
  unsigned long now = millis();
  if (now - lastBlinkToggle >= (unsigned long)_speed) {
    blinkPhase      = !blinkPhase;
    lastBlinkToggle = now;
    digitalWrite(LED_PIN, blinkPhase ? HIGH : LOW);
  }
}

// ═════════════════════════════════════════════════════════════
//  FETCH TASK — runs on core 0, polls Supabase
// ═════════════════════════════════════════════════════════════
void fetchTask(void* param) {
  while (true) {
    ensureWiFi();

    if (WiFi.status() == WL_CONNECTED) {
      fetchState();
    }

    vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL));  // yield — don't hog core 0
  }
}

// ═════════════════════════════════════════════════════════════
//  WIFI
// ═════════════════════════════════════════════════════════════
void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(WIFI_SSID);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
    // Blink during connect — direct writes, no state machine needed here
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }

  digitalWrite(LED_PIN, LOW);   // always go LOW after connect attempt

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.println("✔ WiFi Connected!");
    Serial.print("  IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n✘ WiFi failed — will retry in fetch loop");
  }
}

void ensureWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Lost connection, reconnecting...");
    WiFi.reconnect();
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
}

// ═════════════════════════════════════════════════════════════
//  SUPABASE FETCH
// ═════════════════════════════════════════════════════════════
void fetchState() {
  HTTPClient http;

  String url = String(SUPABASE_URL)
             + "/rest/v1/" + TABLE_NAME
             + "?id=eq." + ROW_ID
             + "&select=led_on,blink,speed_ms";

  http.begin(url);
  http.addHeader("apikey",        SUPABASE_KEY);
  http.addHeader("Authorization", String("Bearer ") + SUPABASE_KEY);
  http.addHeader("Content-Type",  "application/json");

  int code = http.GET();

  if (code == 200) {
    parseResponse(http.getString());
  } else {
    Serial.printf("[Supabase] HTTP error: %d\n", code);
  }

  http.end();
}

// ═════════════════════════════════════════════════════════════
//  PARSE + WRITE STATE
// ═════════════════════════════════════════════════════════════
void parseResponse(const String& body) {
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc.is<JsonArray>() || doc.as<JsonArray>().size() == 0) {
    Serial.println("[JSON] Parse error or empty response");
    return;
  }

  JsonObject obj     = doc[0];
  bool newLedOn      = obj["led_on"]   | false;
  bool newBlink      = obj["blink"]    | false;    // dashboard guarantees this is false when solid ON
  int  newSpeedMs    = obj["speed_ms"] | 500;

  // ── Only update + log when something actually changed ────
  bool changed = (newLedOn   != lastLedOn)
              || (newBlink   != lastBlinkEn)
              || (newSpeedMs != lastSpeedMs);

  if (!changed) return;

  // ── Write to shared state under mutex ────────────────────
  if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    ledOn   = newLedOn;
    blinkEn = newBlink;
    speedMs = newSpeedMs;
    xSemaphoreGive(stateMutex);
  } else {
    Serial.println("[Mutex] Timeout — skipping update");
    return;
  }

  // Update last-known values AFTER successful write
  lastLedOn   = newLedOn;
  lastBlinkEn = newBlink;
  lastSpeedMs = newSpeedMs;

  // ── Log new state ─────────────────────────────────────────
  Serial.println("─── State Updated ───────────────");
  if (!newLedOn) {
    Serial.println("  → LED OFF (holding LOW)");
  } else if (!newBlink) {
    Serial.println("  → LED SOLID ON (holding HIGH)");  // no blinking
  } else {
    Serial.printf("  → LED BLINKING @ %d ms\n", newSpeedMs);
  }
  Serial.println("─────────────────────────────────");
}
