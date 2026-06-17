#include <Arduino.h>
#include <pins.h>
#include <config.h>

void pins_init(void)
{
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
#if defined(BOARD_XIAO_EPAPER_DISPLAY) || defined(BOARD_XIAO_EPAPER_DISPLAY_3CLR)
    io_conf.pin_bit_mask = (1ULL << PIN_KEY1) | (1ULL << PIN_KEY2) | (1ULL << PIN_KEY3);
#else
    io_conf.pin_bit_mask = (1ULL << PIN_INTERRUPT);
#endif
    gpio_config(&io_conf);
}
