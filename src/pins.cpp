#include <Arduino.h>
#include <board_config.h>
#include <config.h>
#ifndef BOARD_TRMNL_X
#endif

void pins_init(void) {
  gpio_config_t io_conf = {};
  io_conf.intr_type = GPIO_INTR_DISABLE;
  io_conf.mode = GPIO_MODE_INPUT;
#if defined(BOARD_XIAO_EPAPER_DISPLAY) || defined(BOARD_XIAO_EPAPER_DISPLAY_3CLR)
  // The XIAO boards wake through any of their three keys; the EXT1 wake
  // mask in goToSleep() must list the same pins. KEY3 is the interrupt
  // pin that the device table holds.
  io_conf.pin_bit_mask = (1ULL << PIN_KEY1) | (1ULL << PIN_KEY2) | (1ULL << PIN_KEY3);
#elif defined(BOARD_TRMNL_X)
  io_conf.pin_bit_mask = (1ULL << PIN_INTERRUPT);
#else
  io_conf.pin_bit_mask = (1ULL << pDevice->interrupt_pin);
#endif
  io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io_conf);
}
