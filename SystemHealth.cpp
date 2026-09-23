#include <Arduino.h>
#include <mbed.h>

#include "SystemHealth.h"
#include "UserConfig.h"

/* Hardware watchdog (R12) and restart reason (R14).
 *
 * mbed::Watchdog (mbed/drivers/include/drivers/Watchdog.h, pulled in by
 * mbed.h) directly maps to the RP2040 hardware watchdog on this target:
 * DEVICE_WATCHDOG=1 is set in variants/NANO_RP2040_CONNECT/defines.txt, and
 * the hal_watchdog_* implementation is linked in from the board's prebuilt
 * libmbed.a. get_max_timeout() reports the RP2040 hardware limit, about
 * 8.3 s (0x7fffff watchdog ticks at ~1 MHz) -- WATCHDOG_TIMEOUT_MS is
 * clamped to it below.
 *
 * Restart reason (R14): the pico-sdk function watchdog_caused_reboot()
 * (declared in hardware/watchdog.h) is also linked into this board's
 * libmbed.a -- confirmed with arm-none-eabi-nm on
 * variants/NANO_RP2040_CONNECT/libs/libmbed.a, which lists it as a defined
 * (T) symbol. Its own header lives under a pico-sdk subtree of this core
 * that is not on the normal include path and pulls in a lot of unrelated
 * register definitions, so it is simpler and cleaner to declare the one
 * function we need ourselves and let the linker resolve it.
 *
 * CAVEAT verified against the RP2040 reset architecture, and how it is
 * resolved: the RP2040 has a single chip-level reset controller, and the ARM
 * core's AIRCR.SYSRESETREQ (which is exactly what NVIC_SystemReset()
 * triggers, see healthReboot() below) is wired into the *same* watchdog
 * block that a real watchdog timeout uses -- one sets the watchdog "FORCE"
 * reason bit, the other the "TIMER" reason bit, and watchdog_caused_reboot()
 * (and mbed::ResetReason, which reads the same registers on this target)
 * report both simply as "the watchdog fired". So on their own, neither can
 * tell a deliberate reboot from connectionRebootDevice() (R11.3) apart from
 * an actual 8 s watchdog timeout (F1).
 *
 * To resolve that, healthReboot() writes a magic marker to the RP2040
 * watchdog's own SCRATCH0 register right before NVIC_SystemReset(), and
 * healthBegin() checks for it first, before even asking
 * watchdog_caused_reboot(). SCRATCH0 lives at WATCHDOG_BASE (0x40058000,
 * see pico-sdk/rp2040/hardware_regs/include/hardware/regs/addressmap.h in
 * this core) + WATCHDOG_SCRATCH0_OFFSET (0x0c, see .../regs/watchdog.h),
 * i.e. address 0x4005800c. Verified free for this use: grepping this whole
 * core/pico-sdk tree for "SCRATCH0" turns up only its register definition
 * (regs/watchdog.h, structs/watchdog.h, the SVD), never a write from any
 * shipped source; the platform's own use of watchdog scratch registers is
 * SCRATCH4 (pico-sdk's watchdog_enable() writes its own "I was deliberately
 * enabled" marker there, per hardware/watchdog.h's doc comment for
 * watchdog_enable_caused_reboot()) and SCRATCH4-7 (watchdog_reboot()'s
 * post-reset jump vector, per the same header) -- SCRATCH0-3 are untouched
 * by anything in this core. These scratch registers are specifically
 * designed to survive a watchdog/software reset (WATCHDOG_SCRATCH0_RESET in
 * the regs header is 0, meaning they reset to 0 only on power-on) -- exactly
 * the two reset kinds this marker needs to survive, and gone again after a
 * real power cycle, which is what makes it work as a "was this reboot
 * deliberate" flag across exactly a watchdog-style reset and nothing else.
 */

#define WATCHDOG_SCRATCH0 (*(volatile uint32_t *)0x4005800cUL)
static const uint32_t SOFT_REBOOT_MAGIC = 0x5AFEB007UL;

extern "C" bool watchdog_caused_reboot(void);

static const char *bootReason = "unknown";

void healthBegin() {
  /* Read the reset reason as early as possible, before anything else that
   * could touch the same registers (R14). Check the deliberate-reboot
   * marker first (see the block comment above): it resolves the
   * watchdog-vs-deliberate-reboot ambiguity that watchdog_caused_reboot()
   * alone cannot. */
  if (WATCHDOG_SCRATCH0 == SOFT_REBOOT_MAGIC) {
    bootReason = "software-reboot";
    WATCHDOG_SCRATCH0 = 0;  // consume it so a later real watchdog timeout
                             // isn't misreported as a deliberate reboot
  } else if (watchdog_caused_reboot()) {
    /* Not our marker, but the watchdog block still fired: a real timeout. */
    bootReason = "watchdog";
  } else {
    switch (mbed::ResetReason::get()) {
      case RESET_REASON_POWER_ON:
        bootReason = "power-on";
        break;
      case RESET_REASON_PIN_RESET:
        /* Verified on hardware 2026-09-23: on this board, both a power loss
         * and a press of the reset button report RESET_REASON_PIN_RESET,
         * because the RUN pin is held low during power-up, so the chip
         * itself records a RUN-pin reset either way. The two causes are
         * indistinguishable here; label it generically rather than
         * implying it was the button. */
        bootReason = "power-on-or-reset";
        break;
      case RESET_REASON_SOFTWARE:
        bootReason = "software-reset";
        break;
      case RESET_REASON_BROWN_OUT:
        bootReason = "brown-out";
        break;
      case RESET_REASON_WATCHDOG:
        bootReason = "watchdog";
        break;
      case RESET_REASON_MULTIPLE:
        bootReason = "multiple";
        break;
      default:
        bootReason = "unknown";
        break;
    }
  }
  Serial.print(F("Restart reason: "));
  Serial.println(bootReason);

  /* R12: start the hardware watchdog, clamped to the platform maximum. */
  mbed::Watchdog &watchdog = mbed::Watchdog::get_instance();
  uint32_t maxTimeout = watchdog.get_max_timeout();
  uint32_t timeout = WATCHDOG_TIMEOUT_MS;
  if (timeout == 0 || timeout > maxTimeout) {
    timeout = maxTimeout;
  }
  bool started = watchdog.start(timeout);

  Serial.print(F("Watchdog: requested "));
  Serial.print(WATCHDOG_TIMEOUT_MS);
  Serial.print(F(" ms, platform max "));
  Serial.print(maxTimeout);
  Serial.print(F(" ms, started with "));
  Serial.print(timeout);
  Serial.println(started ? F(" ms") : F(" ms FAILED"));
}

void healthKick() {
  mbed::Watchdog::get_instance().kick();
}

const char *healthBootReason() {
  return bootReason;
}

void healthReboot() {
  /* R8/R11: callers are expected to have already published "offline" and
   * stopped MQTT before calling this; it does not return. */
  Serial.println(F("Rebooting now (NVIC_SystemReset)"));
  Serial.flush();

  /* R14: mark this as a deliberate reboot so the next healthBegin() reports
   * "software-reboot" instead of "watchdog" -- see the block comment near
   * the top of this file. */
  WATCHDOG_SCRATCH0 = SOFT_REBOOT_MAGIC;

  NVIC_SystemReset();
}
