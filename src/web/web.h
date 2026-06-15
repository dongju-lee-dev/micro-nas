#ifndef WEB_H
#define WEB_H

#include "esp_err.h"

esp_err_t web_init(char *wifi_ssid, char *wifi_password, char * password, uint32_t session, uint32_t session_time);

#endif