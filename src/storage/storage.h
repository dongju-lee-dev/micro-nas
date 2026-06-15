#ifndef STORAGE_H
#define STORAGE_H

#include "driver/gpio.h"
#include "esp_err.h"

esp_err_t storage_init(gpio_num_t stop, gpio_num_t stop_led, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk, gpio_num_t *cs,
		       size_t cs_count);
void storage_mount();
void storage_unmount();
bool storage_is_mount();
size_t storage_get_count();

#endif