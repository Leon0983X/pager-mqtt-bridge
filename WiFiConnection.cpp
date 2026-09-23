#include <Arduino.h>
#include <WiFiNINA.h>
#include <spi_drv.h>  // SpiDrv; included the same way WiFiNINA itself does it
                       // (utility/wifi_drv.cpp: #include <spi_drv.h>), so it
                       // resolves via the Arduino_SpiNINA library's src root.

#include "WiFiConnection.h"
#include "SystemHealth.h"
#include "UserConfig.h"

/* wifiClient is defined here only (fixes G9; it used to be defined in
 * MqttConnection.cpp while declared extern in this header). */
WiFiClient wifiClient;

/* Set by wifiResetModule(), cleared by wifiTryConnect(). While true, the
 * NINA module is deliberately held in hardware reset (see wifiResetModule()
 * below) and wifiIsConnected() must not touch SPI -- see there for why. */
static bool ninaHeldInReset = false;

void wifiSetup() {
  /* WiFi.begin() internally loops for up to the timeout set with
   * setTimeout() (default 50 s!), calling the feed-watchdog callback every
   * ~10 ms (verified in WiFiNINA WiFi.cpp: WiFiClass::begin() calls
   * feedWatchdog() once per WL_DELAY_START_CONNECTION iteration). Wiring
   * that callback to healthKick() means a slow Wi-Fi connect cannot trip the
   * ~8 s hardware watchdog even though it can take several seconds (R12).
   * setTimeout() then bounds that same loop to WIFI_CONNECT_TIMEOUT_MS so a
   * single wifiTryConnect() attempt has a known worst case. */
  WiFi.setFeedWatchdogFunc(healthKick);
  WiFi.setTimeout(WIFI_CONNECT_TIMEOUT_MS);

  /* Bound the underlying TCP connect the MQTT client will later do through
   * wifiClient (used by mqttSetup()/mqttTryConnect() in MqttConnection.cpp;
   * set here too since this is where wifiClient is defined, and it must be
   * set before any connect() call). WiFiClient::_connTimeout defaults to 0,
   * which WiFiNINA treats as "use the NINA firmware's own long default". */
  wifiClient.setConnectionTimeout(TCP_CONNECT_TIMEOUT_MS);
}

bool wifiTryConnect() {
  healthKick();

  Serial.println(F("WiFi: connecting..."));

  /* If wifiResetModule() ran since the last attempt, SpiDrv::initialized is
   * false, so the very first NINA call inside WiFi.begin() (wifiSetPassphrase()
   * -> WAIT_FOR_SLAVE_SELECT() -> SpiDrv::begin()) re-runs the reset/boot
   * sequence documented in wifiResetModule() below: ~760 ms of delay(),
   * BEFORE WiFi.begin()'s own for-loop starts calling feedWatchdog() (that
   * loop only starts after wifiSetPassphrase() returns). That 760 ms is not
   * self-fed, but it is safe: it runs immediately after the healthKick()
   * two lines up, and 760 ms << the ~8 s watchdog timeout. After that,
   * WiFi.begin()'s own loop takes over feeding the watchdog every ~100 ms up
   * to WIFI_CONNECT_TIMEOUT_MS, as described in wifiSetup(). On a normal
   * attempt (NINA not held in reset), SpiDrv::begin() sees `initialized`
   * already true and returns immediately, so this delay does not occur. */
  int status = WiFi.begin(ssid, pass);

  /* WiFi.begin() always reaches SpiDrv::begin() first (see above), which
   * unconditionally sets `initialized = true` on the path taken when it was
   * false -- so by the time WiFi.begin() returns, SPI is initialized and the
   * module is out of reset, regardless of whether the connection itself
   * succeeded. Safe to clear here either way. */
  ninaHeldInReset = false;

  if (status == WL_CONNECTED) {
    Serial.println(F("WiFi: connected"));
    return true;
  }

  Serial.print(F("WiFi: connect failed, status="));
  Serial.println(status);
  return false;
}

bool wifiIsConnected() {
  /* While the NINA module is deliberately held in reset (see
   * wifiResetModule()), do NOT call WiFi.status(): WiFiDrv::getConnectionStatus()
   * starts with WAIT_FOR_SLAVE_SELECT(), which -- like every other NINA SPI
   * call -- lazily calls SpiDrv::begin() if `initialized` is false. That
   * would silently re-initialize (i.e. un-reset) the module the first time
   * anything calls wifiIsConnected() after a reset, which happens every
   * single connectionUpdate() pass (it's the first thing that function
   * checks) -- long before the backoff-scheduled wifiTryConnect() we
   * actually intended to do the re-init. Returning false here without
   * touching SPI keeps the module held in reset for the full backoff
   * interval, as intended, and also means nothing else in
   * ConnectionManager.cpp (mqttIsConnected(), mqttClient.poll(), ...) runs
   * either, since every caller in this codebase checks wifiIsConnected()
   * first and short-circuits on false. */
  if (ninaHeldInReset) {
    return false;
  }
  return WiFi.status() == WL_CONNECTED;
}

void wifiResetModule() {
  /* R11.2: reset the NINA module.
   *
   * WiFi.end() alone is NOT enough: it calls WiFiDrv::wifiDriverDeinit(),
   * which in this exact library version (WiFiNINA 2.1.1) is an EMPTY
   * function (verified by reading wifi_drv.cpp) -- it never touches the
   * NINA's hardware RESETN line or clears SpiDrv::initialized, so a
   * follow-up WiFi.begin() would see `initialized` still true and skip the
   * reset entirely (SpiDrv::begin(): `if (initialized && !force) return;`).
   * WiFi.end() is still called first, for whatever WiFiClass-level state it
   * does reset and to keep behaviour consistent if a future library version
   * implements it properly.
   *
   * The real reset is SpiDrv::end() (Arduino_SpiNINA 0.0.2, public API,
   * declared in spi_drv.h): it drives the NINA's reset line to its active
   * ("asserted") level, releases the SPI peripheral (SPIWIFI.end()), and
   * sets SpiDrv::initialized = false. Verified in spi_drv.cpp:
   *   void SpiDrv::end() {
   *     digitalWrite(SLAVERESET, inverted_reset ? HIGH : LOW);  // assert reset
   *     pinMode(SLAVESELECT, INPUT);
   *     SPIWIFI.end();
   *     initialized = false;
   *   }
   * The module is then held in that asserted reset state -- nothing pulses
   * it back -- until the next SpiDrv::begin() (reached lazily from inside
   * the next WiFi.begin(), see wifiTryConnect()), which does the real
   * reset/boot sequence (spi_drv.cpp):
   *   digitalWrite(SLAVERESET, ...);  delay(10);   // 10 ms reset pulse
   *   digitalWrite(SLAVERESET, ...);  delay(750);  // 750 ms NINA boot wait
   *   SPIWIFI.begin();
   *   initialized = true;
   * i.e. a genuine ~760 ms hardware reset-and-reboot cycle, not merely a
   * software state reset. See wifiTryConnect() for why that 760 ms is still
   * watchdog-safe, and wifiIsConnected() for why nothing may touch SPI while
   * `ninaHeldInReset` is set.
   */
  Serial.println(F("WiFi: resetting NINA module (WiFi.end() + SpiDrv::end())"));
  WiFi.end();
  SpiDrv::end();
  ninaHeldInReset = true;
}
