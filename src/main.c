#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "storage/storage.h"
#include "web/web.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_main()
{
	printf("Hello\n");

	esp_vfs_spiffs_conf_t conf = {
	    .base_path = "/flash",
	    .partition_label = "storage",
	    .max_files = 5,
	    .format_if_mount_failed = true,
	};

	ESP_ERROR_CHECK(esp_vfs_spiffs_register(&conf));

	FILE *f = fopen("/flash/setting.txt", "r");
	if (f == NULL) {
		while (1) {
			ESP_LOGI("main", "error not find setting.txt");
			vTaskDelay(pdMS_TO_TICKS(1000));
		}
	}

	gpio_num_t stop = GPIO_NUM_NC;
	gpio_num_t stop_led = GPIO_NUM_NC;
	gpio_num_t mosi = GPIO_NUM_NC;
	gpio_num_t miso = GPIO_NUM_NC;
	gpio_num_t sclk = GPIO_NUM_NC;
	gpio_num_t *cs = NULL;
	size_t cs_count = 0;
	char wifi_ssid[32] = {0};
	char wifi_password[64] = {0};
	char password[16] = {0};
	uint32_t session = 0;
	uint32_t session_time = 0;

	char line[64];
	while (fgets(line, sizeof(line), f)) {
		if (line[0] == '\n' || line[0] == '\r' || line[0] == ' ' || line[0] == '#')
			continue;

		line[strcspn(line, "\r\n")] = 0;
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		char *key = line;
		char *val = eq + 1;

		if (strcmp(key, "STOP") == 0)
			stop = atoi(val);
		else if (strcmp(key, "STOP_LED") == 0)
			stop_led = atoi(val);
		else if (strcmp(key, "SPI_MOSI") == 0)
			mosi = atoi(val);
		else if (strcmp(key, "SPI_MISO") == 0)
			miso = atoi(val);
		else if (strcmp(key, "SPI_SCLK") == 0)
			sclk = atoi(val);
		else if (strncmp(key, "SPI_SC_", 7) == 0) {
			gpio_num_t *temp = realloc(cs, sizeof(gpio_num_t) * ++cs_count);
			if (temp == NULL) {
				ESP_LOGI("main", "out of memory");
				return;
			}

			cs = temp;
			cs[cs_count - 1] = atoi(val);
		} else if (strcmp(key, "WIFI_SSID") == 0)
			strlcpy(wifi_ssid, val, sizeof(wifi_ssid));
		else if (strcmp(key, "WIFI_PASSWORD") == 0)
			strlcpy(wifi_password, val, sizeof(wifi_password));
		else if (strcmp(key, "PASSWORD") == 0)
			strlcpy(password, val, sizeof(password));
		else if (strcmp(key, "SESSION") == 0)
			session = atoi(val);
		else if (strcmp(key, "SESSION_TIME") == 0)
			session_time = atoi(val);
	}

	fclose(f);

	ESP_ERROR_CHECK(storage_init(stop, stop_led, mosi, miso, sclk, cs, cs_count));
	ESP_ERROR_CHECK(web_init(wifi_ssid, wifi_password, password, session, session_time));
}
