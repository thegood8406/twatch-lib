cmake_minimum_required(VERSION 3.13)

idf_component_register(
  SRCS
  "drivers/i2c.c"
  "drivers/uart.c"
  "drivers/axp20x.c"
  "drivers/ft6236.c"
  "drivers/bma423/bma.c"
  "drivers/bma423/bma4.c"
  "drivers/bma423/bma423.c"
  "drivers/st7789.c"
  "drivers/pcf8563.c"
  "drivers/drv2605.c"
  "drivers/ir/esp32_rmt_common.c"
  "drivers/ir/rmtlib_raw.c"
  
  "hal/hal.c"
  "hal/audio.c"
  "hal/pmu.c"
  "hal/touch.c"
  "hal/vibrate.c"
  "hal/screen.c"
  "hal/rtc.c"
  "hal/gps.c"

  "img/img.c"

  INCLUDE_DIRS "inc"
  REQUIRES driver esp_timer

)
