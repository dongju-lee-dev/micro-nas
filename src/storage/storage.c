#include "storage.h"
#include "driver/gpio.h"
#include "driver/sdspi_host.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include <stdbool.h>

static gpio_num_t s_stop;
static gpio_num_t s_stop_led;
static gpio_num_t *s_cs;
static size_t s_cs_count;
static sdmmc_host_t s_host;
static sdmmc_card_t **s_card;
static SemaphoreHandle_t s_stop_semaphore;
static SemaphoreHandle_t s_storage_mux;
static bool s_is_mount;

static void storage_stop_task(void *pvParameters)
{
	int last = 1;

	while (1) {
		if (xSemaphoreTake(s_stop_semaphore, portMAX_DELAY) == pdTRUE) {
			vTaskDelay(pdMS_TO_TICKS(500));
			while (xSemaphoreTake(s_stop_semaphore, 0) == pdTRUE)
				;

			int now = gpio_get_level(s_stop);
			if (now != last) {
				if (now == 0) {
					gpio_set_level(s_stop_led, 1);
					storage_unmount();
				} else {
					gpio_set_level(s_stop_led, 0);
					storage_mount();
				}
				last = now;
			}
		}
	}
}

static void IRAM_ATTR storage_stop_button(void *arg)
{
	BaseType_t xHigherPriorityTaskWoken = pdFALSE;
	xSemaphoreGiveFromISR(s_stop_semaphore, &xHigherPriorityTaskWoken);
	if (xHigherPriorityTaskWoken) {
		portYIELD_FROM_ISR();
	}
}

esp_err_t storage_init(gpio_num_t stop, gpio_num_t stop_led, gpio_num_t mosi, gpio_num_t miso, gpio_num_t sclk,
		       gpio_num_t *cs, size_t cs_count)
{
	s_stop = stop;
	s_stop_led = stop_led;
	s_cs = cs;
	s_cs_count = cs_count;
	s_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
	s_card = calloc(cs_count, sizeof(sdmmc_card_t *));
	s_stop_semaphore = xSemaphoreCreateBinary();
	s_storage_mux = xSemaphoreCreateMutex();
	s_is_mount = false;

	for (size_t i = 0; i < cs_count; i++) {
		gpio_config_t cs_cfg = {
		    .pin_bit_mask = (1ULL << cs[i]),
		    .mode = GPIO_MODE_OUTPUT,
		    .pull_up_en = GPIO_PULLUP_ENABLE,
		};
		gpio_config(&cs_cfg);
		gpio_set_level(cs[i], 1);
	}

	spi_bus_config_t config = {
	    .mosi_io_num = mosi,
	    .miso_io_num = miso,
	    .sclk_io_num = sclk,
	    .quadwp_io_num = GPIO_NUM_NC,
	    .quadhd_io_num = GPIO_NUM_NC,
	    .max_transfer_sz = 16384,
	    .intr_flags = ESP_INTR_FLAG_IRAM,
	};

	esp_err_t err = spi_bus_initialize(s_host.slot, &config, SDSPI_DEFAULT_DMA);
	if (err != ESP_OK) {
		return err;
	}

	gpio_config_t stop_io_cfg = {
	    .pin_bit_mask = (1ULL << stop),
	    .mode = GPIO_MODE_INPUT,
	    .pull_up_en = GPIO_PULLUP_ENABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	    .intr_type = GPIO_INTR_ANYEDGE,
	};
	gpio_config(&stop_io_cfg);

	gpio_config_t stop_led_is_cfg = {
	    .pin_bit_mask = (1ULL << stop_led),
	    .mode = GPIO_MODE_OUTPUT,
	    .pull_up_en = GPIO_PULLUP_DISABLE,
	    .pull_down_en = GPIO_PULLDOWN_DISABLE,
	    .intr_type = GPIO_INTR_DISABLE,
	};
	gpio_config(&stop_led_is_cfg);

	err = gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
	if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
		return err;
	}
	gpio_isr_handler_add(stop, storage_stop_button, NULL);

	xTaskCreate(storage_stop_task, "storage_stop_task", 4096, NULL, 10, NULL);

	if (gpio_get_level(s_stop) != 0) {
		gpio_set_level(s_stop_led, 0);
		storage_mount();
	} else {
		gpio_set_level(s_stop_led, 1);
	}

	ESP_LOGI("storage", "init end");

	return ESP_OK;
}

void storage_mount()
{
	xSemaphoreTake(s_storage_mux, portMAX_DELAY);
	for (size_t i = 0; i < s_cs_count; i++) {
		if (s_card[i] != NULL) {
			continue;
		}

		char base_path[16];
		snprintf(base_path, sizeof(base_path), "/storage_%d", i);

		sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
		slot_config.host_id = s_host.slot;
		slot_config.gpio_cs = s_cs[i];

		esp_vfs_fat_mount_config_t mount_config = {
		    .format_if_mount_failed = false,
		    .max_files = 5,
		    .allocation_unit_size = 0,
		    .disk_status_check_enable = false,
		    .use_one_fat = false,
		};

		esp_vfs_fat_sdspi_mount(base_path, &s_host, &slot_config, &mount_config, &s_card[i]);
	}

	s_is_mount = true;
	xSemaphoreGive(s_storage_mux);
}

void storage_unmount()
{
	xSemaphoreTake(s_storage_mux, portMAX_DELAY);
	for (size_t i = 0; i < s_cs_count; i++) {
		if (s_card[i] == NULL)
			continue;

		char base_path[16];
		snprintf(base_path, sizeof(base_path), "/storage_%d", i);
		esp_vfs_fat_sdcard_unmount(base_path, s_card[i]);
		s_card[i] = NULL;
	}

	s_is_mount = false;
	xSemaphoreGive(s_storage_mux);
}

bool storage_is_mount()
{
	return s_is_mount;
}

size_t storage_get_count()
{
	return s_cs_count;
}
