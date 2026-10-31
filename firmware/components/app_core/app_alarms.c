/*
 * Alarm supervisor (app_alarms.h).
 *
 * Codes:
 *   A001 no Ethernet link       A002 bus held in PRE-OP     A003 working-counter errors
 *   A004 slave held after recovery
 *   A101..A116 drive fault of axis 1..16 (cable order, as on the Manual page)
 *   A201..A216 axis stopped by the axis logic (enable/handshake time-out, homing error, bus, ...)
 *   A301..A316 axis not usable (PDO layout)
 *   A401..A404 remote IO coupler fault (R1EC Device Status != 0)
 *   W001 clock not set
 * Machine-sequence alarms will get their own codes once the sequence exists.
 */

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "app_alarms.h"
#include "ecat_axis.h"
#include "ecat_io.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "machine_alarm.h"
#include "machine_clock.h"

#define POLL_MS          200
#define LINK_GRACE_US    8000000 /* the PHY needs a few seconds after reset */
#define WKC_HOLD_POLLS   10      /* A003 stays active 2 s after the last error */

/* Snapshot copies, in PSRAM (allocated by the task): internal RAM is kept for DMA and EtherCAT. */
static ecat_axes_status_t *s_ax;
static ecat_io_layout_t *s_io_l;
static ecat_io_image_t *s_io;

static void report(const char *code, uint8_t sev, const char *title, const char *source, const char *cause,
                   const char *remedy, bool active)
{
    const machine_alarm_spec_t spec = {
        .code = code, .sev = sev, .title = title, .source = source, .cause = cause, .remedy = remedy,
    };
    machine_alarm_update(&spec, active);
}

static void bus_alarms(const app_bus_info_t *b, int64_t now)
{
    static uint32_t last_wkc;
    static int wkc_hold;
    char src[64], cause[128];

    report("A001", MACHINE_ALARM_ERROR, "EtherCAT cable not connected", "HMI RJ45 \xC2\xB7 IN port of the first slave",
           "There is no Ethernet link to the first EtherCAT slave.",
           "Check the cable from the HMI to the IN port of the first slave, and that the slave has power.",
           !b->link_up && now > LINK_GRACE_US);

    report("A002", MACHINE_ALARM_ERROR, "EtherCAT not running", "Settings \xE2\x80\xBA EtherCAT",
           "The bus is held in PRE-OP: no topology is accepted yet, or the bus differs from the accepted one.",
           "Open Settings \xE2\x80\xBA EtherCAT, check the differences, then Re-check, or Scan and Accept.",
           b->holding && b->link_up);

    if (!b->in_op) {
        last_wkc = 0;
        wkc_hold = 0;
    } else if (b->wkc_errors != last_wkc) {
        last_wkc = b->wkc_errors;
        wkc_hold = WKC_HOLD_POLLS;
    } else if (wkc_hold > 0) {
        wkc_hold--;
    }
    snprintf(src, sizeof(src), "EtherCAT \xC2\xB7 %" PRIu32 " frame(s) with errors since OP", b->wkc_errors);
    report("A003", MACHINE_ALARM_ERROR, "EtherCAT communication error", src,
           "Frames came back with a wrong working counter: a slave did not answer (cable, power or noise).",
           "Check the cables and the 24 V of every slave. Axes switch off after 3 bad cycles in a row.",
           wkc_hold > 0);

    snprintf(cause, sizeof(cause), "%u slave(s) came back after a fault but are not the accepted device.",
             (unsigned)b->n_blocked);
    report("A004", MACHINE_ALARM_ERROR, "EtherCAT slave held in PRE-OP", "EtherCAT \xC2\xB7 slave recovery", cause,
           "Stop the machine, then Re-check in Settings \xE2\x80\xBA EtherCAT.", b->in_op && b->n_blocked > 0);
}

static bool stop_error(uint8_t err)
{
    switch (err) {
    case ECAT_AXERR_BUS:
    case ECAT_AXERR_ENABLE_TIMEOUT:
    case ECAT_AXERR_MODE_TIMEOUT:
    case ECAT_AXERR_SETPOINT_TIMEOUT:
    case ECAT_AXERR_HOMING:
    case ECAT_AXERR_HOMING_TIMEOUT:
    case ECAT_AXERR_RESET_FAILED:
    case ECAT_AXERR_DROPPED:
        return true;
    default:
        return false; /* operator mistakes (not enabled, busy) and the jog dead-man are not alarms */
    }
}

static void axis_alarms(void)
{
    char code[8], title[48] = "", src[64] = "", cause[128] = "";
    ecat_axes_read(s_ax);
    for (int i = 0; i < ECAT_MAX_AXES; i++) {
        const bool present = s_ax->in_op && i < s_ax->n_axes;
        const ecat_axis_status_t *a = &s_ax->axis[i];

        bool fault = present && a->available &&
                     (a->cia_state == ECAT_CIA_FAULT || a->cia_state == ECAT_CIA_FAULT_REACTION);
        snprintf(code, sizeof(code), "A1%02d", i + 1);
        if (fault) {
            snprintf(title, sizeof(title), "Servo %s drive fault", a->name);
            snprintf(src, sizeof(src), "Slave %u \xC2\xB7 %s \xC2\xB7 drive error 0x%04X", a->slave, a->name, a->error_code);
            snprintf(cause, sizeof(cause), "The drive reports error 0x%04X (vendor code 0x%04X).", a->error_code,
                     a->vendor_error);
        }
        report(code, MACHINE_ALARM_ERROR, title, src, cause,
               "Look up the code in the drive manual and remove the cause. Then press Reset and home the axis.",
               fault);

        const bool stopped = present && a->available && stop_error(a->error);
        snprintf(code, sizeof(code), "A2%02d", i + 1);
        if (stopped) {
            snprintf(title, sizeof(title), "Servo %s stopped", a->name);
            snprintf(src, sizeof(src), "Slave %u \xC2\xB7 %s \xC2\xB7 %s", a->slave, a->name,
                     ecat_cia_state_text(a->cia_state));
            snprintf(cause, sizeof(cause), "%s.", ecat_axis_error_text(a->error));
        }
        report(code, MACHINE_ALARM_ERROR, title, src, cause,
               "Check STO, the 24 V and main power of the drive. Reset, switch the servo on again, and home if asked.",
               stopped);

        const bool unusable = present && !a->available;
        snprintf(code, sizeof(code), "A3%02d", i + 1);
        if (unusable) {
            snprintf(title, sizeof(title), "Servo %s not usable", a->name);
            snprintf(src, sizeof(src), "Slave %u \xC2\xB7 %s", a->slave, a->name);
            strlcpy(cause, "The drive did not take the expected PDO layout, so it is not controlled.", sizeof(cause));
        }
        report(code, MACHINE_ALARM_ERROR, title, src, cause,
               "See the ecat_drv lines of the start-up log, then Re-check in Settings \xE2\x80\xBA EtherCAT.", unusable);
    }
}

static void io_alarms(void)
{
    char code[8], title[48] = "", src[64] = "", cause[128] = "";
    ecat_io_layout_read(s_io_l);
    ecat_io_image_read(s_io);
    for (int k = 0; k < ECAT_IO_MAX_COUPLERS; k++) {
        const bool present = s_io_l->in_op && k < s_io_l->n && s_io->gen == s_io_l->gen;
        const uint32_t st = present ? s_io->device_status[k] : 0;
        snprintf(code, sizeof(code), "A4%02d", k + 1);
        if (st) {
            snprintf(title, sizeof(title), "Remote IO %s fault", s_io_l->c[k].name);
            snprintf(src, sizeof(src), "Slave %u \xC2\xB7 %s \xC2\xB7 Device Status 0x%08" PRIX32, s_io_l->c[k].slave,
                     s_io_l->c[k].name, st);
            snprintf(cause, sizeof(cause),
                     "The coupler reports Device Status 0x%08" PRIX32 ": a module fault or missing field supply. "
                     "Its inputs are not trusted.", st);
        }
        report(code, MACHINE_ALARM_ERROR, title, src, cause,
               "Check the module LEDs and the 24 V field supply of the rack, then Re-check the bus.", st != 0);
    }
}

static void clock_alarm(void)
{
    const bool rtc = machine_clock_rtc_present();
    report("W001", MACHINE_ALARM_WARNING, "Clock not set", "RTC RX8025T",
           rtc ? "The RTC lost its time (backup cell empty or removed). Time stamps count from start-up."
               : "The RTC does not answer on I2C. Time stamps count from start-up.",
           "Set the date and time in Settings \xE2\x80\xBA Time, or connect Wi-Fi with internet time on "
           "(Settings \xE2\x80\xBA Network): the clock is then set by itself.",
           !machine_clock_valid());
}

static void alarms_task(void *arg)
{
    s_ax = heap_caps_calloc(1, sizeof(*s_ax), MALLOC_CAP_SPIRAM);
    s_io_l = heap_caps_calloc(1, sizeof(*s_io_l), MALLOC_CAP_SPIRAM);
    s_io = heap_caps_calloc(1, sizeof(*s_io), MALLOC_CAP_SPIRAM);
    configASSERT(s_ax && s_io_l && s_io);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
        app_bus_info_t b;
        app_bus_info(&b);
        bus_alarms(&b, esp_timer_get_time());
        axis_alarms();
        io_alarms();
        clock_alarm();
    }
}

void app_alarms_start(void)
{
    xTaskCreatePinnedToCore(alarms_task, "alarms", 4096, NULL, 2, NULL, 0);
}
