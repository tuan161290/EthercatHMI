/*
 * SOEM OSAL for ESP-IDF (FreeRTOS SMP).
 *
 * Time base is esp_timer (1 us, monotonic since boot). Sub-tick sleeps in
 * osal_monotonic_sleep() use a GPTimer alarm so the calling task blocks instead
 * of spinning; the last few microseconds are spun to hit the deadline.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file SOEM/LICENSE.md distributed with this software for
 * full license information.
 */

#include <stdlib.h>
#include <sys/time.h>
#include "osal.h"
#include "osal_esp.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "driver/gptimer.h"

static const char *TAG = "soem_osal";

/* Remaining time (us) below which we spin instead of arming the alarm. */
#define OSAL_SPIN_MARGIN_US 30

static gptimer_handle_t s_sleep_timer;
static SemaphoreHandle_t s_sleep_done;
static SemaphoreHandle_t s_sleep_lock;

static bool IRAM_ATTR sleep_alarm_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
   BaseType_t woken = pdFALSE;
   xSemaphoreGiveFromISR(s_sleep_done, &woken);
   return woken == pdTRUE;
}

esp_err_t osal_esp_init(void)
{
   if (s_sleep_timer)
   {
      return ESP_OK;
   }
   s_sleep_done = xSemaphoreCreateBinary();
   s_sleep_lock = xSemaphoreCreateMutex();
   if (!s_sleep_done || !s_sleep_lock)
   {
      return ESP_ERR_NO_MEM;
   }

   /* The alarm interrupt is allocated on the calling core: call this from the RT core. */
   const gptimer_config_t cfg = {
       .clk_src = GPTIMER_CLK_SRC_DEFAULT,
       .direction = GPTIMER_COUNT_UP,
       .resolution_hz = 1000000,
   };
   ESP_RETURN_ON_ERROR(gptimer_new_timer(&cfg, &s_sleep_timer), TAG, "gptimer_new_timer");
   const gptimer_event_callbacks_t cbs = {
       .on_alarm = sleep_alarm_cb,
   };
   ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_sleep_timer, &cbs, NULL), TAG, "register cb");
   ESP_RETURN_ON_ERROR(gptimer_enable(s_sleep_timer), TAG, "gptimer_enable");
   ESP_RETURN_ON_ERROR(gptimer_start(s_sleep_timer), TAG, "gptimer_start");
   return ESP_OK;
}

static inline int64_t timespec_to_us(const ec_timet *ts)
{
   return (int64_t)ts->tv_sec * 1000000 + ts->tv_nsec / 1000;
}

static inline void us_to_timespec(int64_t usec, ec_timet *ts)
{
   ts->tv_sec = usec / 1000000;
   ts->tv_nsec = (usec % 1000000) * 1000;
}

void osal_get_monotonic_time(ec_timet *ts)
{
   us_to_timespec(esp_timer_get_time(), ts);
}

ec_timet osal_current_time(void)
{
   struct timeval tv;
   ec_timet ts;

   gettimeofday(&tv, NULL);
   ts.tv_sec = tv.tv_sec;
   ts.tv_nsec = tv.tv_usec * 1000;
   return ts;
}

void osal_time_diff(ec_timet *start, ec_timet *end, ec_timet *diff)
{
   osal_timespecsub(end, start, diff);
}

void osal_timer_start(osal_timert *self, uint32 timeout_usec)
{
   us_to_timespec(esp_timer_get_time() + timeout_usec, &self->stop_time);
}

boolean osal_timer_is_expired(osal_timert *self)
{
   return esp_timer_get_time() >= timespec_to_us(&self->stop_time);
}

int osal_usleep(uint32 usec)
{
   const uint32_t tick_us = portTICK_PERIOD_MS * 1000;

   if (usec >= tick_us)
   {
      /* Round up so we never sleep less than requested. */
      vTaskDelay((usec + tick_us - 1) / tick_us);
   }
   else if (usec > 0)
   {
      esp_rom_delay_us(usec);
   }
   return 0;
}

static void spin_until(int64_t deadline)
{
   while (esp_timer_get_time() < deadline)
   {
   }
}

int osal_monotonic_sleep(ec_timet *ts)
{
   const int64_t deadline = timespec_to_us(ts);
   const int64_t tick_us = portTICK_PERIOD_MS * 1000;
   int64_t remaining = deadline - esp_timer_get_time();

   if (remaining <= 0)
   {
      return 0;
   }

   /* Coarse part: whole ticks, leaving at least one tick for the precise part. */
   if (remaining > 2 * tick_us)
   {
      vTaskDelay((remaining - tick_us) / tick_us);
      remaining = deadline - esp_timer_get_time();
   }

   if (remaining > OSAL_SPIN_MARGIN_US && s_sleep_timer && xSemaphoreTake(s_sleep_lock, 0) == pdTRUE)
   {
      uint64_t now_cnt = 0;
      xSemaphoreTake(s_sleep_done, 0); /* drop a stale wake-up */
      gptimer_get_raw_count(s_sleep_timer, &now_cnt);
      const gptimer_alarm_config_t alarm = {
          .alarm_count = now_cnt + (uint64_t)(remaining - OSAL_SPIN_MARGIN_US),
      };
      if (gptimer_set_alarm_action(s_sleep_timer, &alarm) == ESP_OK)
      {
         /* Timeout is only a safety net in case the alarm is missed. */
         xSemaphoreTake(s_sleep_done, pdMS_TO_TICKS(remaining / 1000) + 2);
      }
      xSemaphoreGive(s_sleep_lock);
   }

   spin_until(deadline);
   return 0;
}

void *osal_malloc(size_t size)
{
   return malloc(size);
}

void osal_free(void *ptr)
{
   free(ptr);
}

typedef struct
{
   void (*func)(void *);
   void *param;
} osal_thread_start_t;

/* FreeRTOS tasks must not return; SOEM thread functions may. */
static void osal_thread_trampoline(void *arg)
{
   osal_thread_start_t start = *(osal_thread_start_t *)arg;
   free(arg);
   start.func(start.param);
   vTaskDelete(NULL);
}

static int osal_thread_spawn(void *thandle, const char *name, int stacksize, void *func,
                             void *param, UBaseType_t prio, BaseType_t core)
{
   osal_thread_start_t *start = malloc(sizeof(*start));
   if (!start)
   {
      return 0;
   }
   start->func = (void (*)(void *))func;
   start->param = param;
   if (xTaskCreatePinnedToCore(osal_thread_trampoline, name, stacksize, start, prio,
                               (TaskHandle_t *)thandle, core) != pdPASS)
   {
      free(start);
      return 0;
   }
   return 1;
}

int osal_thread_create(void *thandle, int stacksize, void *func, void *param)
{
   return osal_thread_spawn(thandle, "soem_wk", stacksize, func, param,
                            CONFIG_SOEM_WORKER_TASK_PRIO, tskNO_AFFINITY);
}

int osal_thread_create_rt(void *thandle, int stacksize, void *func, void *param)
{
   return osal_thread_spawn(thandle, "soem_rt", stacksize, func, param,
                            CONFIG_SOEM_RT_TASK_PRIO, CONFIG_SOEM_RT_CORE);
}

void *osal_mutex_create(void)
{
   return xSemaphoreCreateMutex();
}

void osal_mutex_destroy(void *mutex)
{
   if (mutex)
   {
      vSemaphoreDelete((SemaphoreHandle_t)mutex);
   }
}

void osal_mutex_lock(void *mutex)
{
   xSemaphoreTake((SemaphoreHandle_t)mutex, portMAX_DELAY);
}

void osal_mutex_unlock(void *mutex)
{
   xSemaphoreGive((SemaphoreHandle_t)mutex);
}
