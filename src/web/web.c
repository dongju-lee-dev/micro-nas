#include "web.h"
#include "cJSON.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "storage/storage.h"
#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define MAX_ACTIVE_FILES 8

typedef struct stat stat_t;

typedef struct {
	char token[33];
	time_t last_activity;
} session_t;

static char s_password[16];
static session_t *s_sessions = NULL;
static uint32_t s_max_sessions = 0;
static uint32_t s_session_timeout = 1800;
static SemaphoreHandle_t s_session_mux = NULL;

static char s_file_lock[MAX_ACTIVE_FILES][256] = {0};
static SemaphoreHandle_t s_file_lock_mux = NULL;

static TimerHandle_t s_pmu_timer = NULL;
static int s_active_requests = 0;
static SemaphoreHandle_t s_pmu_mux = NULL;

static void pmu_relax_cb(TimerHandle_t xTimer)
{
	esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
}

static void pmu_activity_start()
{
	xSemaphoreTake(s_pmu_mux, portMAX_DELAY);
	xTimerStop(s_pmu_timer, 0);
	if (s_active_requests == 0) {
		esp_wifi_set_ps(WIFI_PS_NONE);
	}
	s_active_requests++;
	xSemaphoreGive(s_pmu_mux);
}

static void pmu_activity_end()
{
	xSemaphoreTake(s_pmu_mux, portMAX_DELAY);
	s_active_requests--;
	if (s_active_requests <= 0) {
		s_active_requests = 0;
		xTimerStart(s_pmu_timer, 0);
	}
	xSemaphoreGive(s_pmu_mux);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
	if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
		esp_wifi_connect();
	} else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
		mdns_init();
		mdns_hostname_set("micro-nas");
		mdns_instance_name_set("Micro NAS Storage");
		mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
	}
}

static void url_decode(char *dst, const char *src)
{
	char a, b;
	while (*src) {
		if ((*src == '%') && ((a = src[1]) && (b = src[2])) &&
		    (isxdigit((unsigned char)a) && isxdigit((unsigned char)b))) {
			if (a >= 'a')
				a -= 'a' - 'A';
			if (a >= 'A')
				a -= ('A' - 10);
			else
				a -= '0';

			if (b >= 'a')
				b -= 'a' - 'A';
			if (b >= 'A')
				b -= ('A' - 10);
			else
				b -= '0';

			*dst++ = 16 * a + b;
			src += 3;
		} else if (*src == '+') {
			*dst++ = ' ';
			src++;
		} else {
			*dst++ = *src++;
		}
	}
	*dst = '\0';
}

static bool file_lock(const char *path, uint32_t timeout_ms)
{
	if (!path)
		return false;
	uint32_t start_time = esp_timer_get_time() / 1000;

	while (1) {
		bool conflict = false;
		int free_slot = -1;

		xSemaphoreTake(s_file_lock_mux, portMAX_DELAY);
		for (int i = 0; i < MAX_ACTIVE_FILES; i++) {
			if (s_file_lock[i][0] != '\0' && strcasecmp(s_file_lock[i], path) == 0) {
				conflict = true;
				break;
			}
			if (s_file_lock[i][0] == '\0' && free_slot == -1) {
				free_slot = i;
			}
		}

		if (!conflict && free_slot != -1) {
			strncpy(s_file_lock[free_slot], path, 255);
			s_file_lock[free_slot][255] = '\0';
			xSemaphoreGive(s_file_lock_mux);
			return true;
		}
		xSemaphoreGive(s_file_lock_mux);

		if ((esp_timer_get_time() / 1000) - start_time >= timeout_ms) {
			return false;
		}

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

static void file_unlock(const char *path)
{
	if (!path)
		return;
	xSemaphoreTake(s_file_lock_mux, portMAX_DELAY);
	for (int i = 0; i < MAX_ACTIVE_FILES; i++) {
		if (s_file_lock[i][0] != '\0' && strcasecmp(s_file_lock[i], path) == 0) {
			s_file_lock[i][0] = '\0';
			break;
		}
	}
	xSemaphoreGive(s_file_lock_mux);
}

static bool is_valid_filename(const char *path)
{
	if (!path)
		return false;
	const char *invalid_chars = "<>:\"\\|?*";
	while (*path) {
		if (strchr(invalid_chars, *path))
			return false;
		path++;
	}
	return true;
}

static bool is_storage_path(const char *path)
{
	if (!path || strlen(path) >= 256)
		return false;

	if (strstr(path, "/../") != NULL || strncmp(path, "../", 3) == 0 ||
	    (strlen(path) >= 3 && strcmp(path + strlen(path) - 3, "/..") == 0) || strcmp(path, "..") == 0) {
		return false;
	}

	return strncmp(path, "/storage_", 9) == 0;
}

static void drain_body(httpd_req_t *req)
{
	if (req->content_len <= 0)
		return;

	char buf[128];
	int ret;
	int remaining = req->content_len;
	while (remaining > 0) {
		ret = httpd_req_recv(req, buf, remaining > sizeof(buf) ? sizeof(buf) : remaining);
		if (ret <= 0) {
			if (ret == HTTPD_SOCK_ERR_TIMEOUT)
				continue;
			break;
		}
		remaining -= ret;
	}
}

static void send_json_resp(httpd_req_t *req, cJSON *root)
{
	char *str = cJSON_PrintUnformatted(root);
	httpd_resp_set_type(req, "application/json");
	httpd_resp_send(req, str, strlen(str));
	free(str);
	cJSON_Delete(root);
}

static void send_err(httpd_req_t *req, const char *msg)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "status", "error");
	cJSON_AddStringToObject(root, "message", msg);
	send_json_resp(req, root);
}

static void send_ok(httpd_req_t *req)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "status", "success");
	send_json_resp(req, root);
}

static cJSON *read_json_body(httpd_req_t *req)
{
	if (req->content_len <= 0)
		return NULL;

	if (req->content_len > 4096) {
		drain_body(req);
		send_err(req, "Body too large");
		return NULL;
	}

	char *buf = malloc(req->content_len + 1);
	if (!buf) {
		drain_body(req);
		send_err(req, "Out of memory");
		return NULL;
	}

	int ret, received = 0;
	while (received < req->content_len) {
		ret = httpd_req_recv(req, buf + received, req->content_len - received);
		if (ret <= 0) {
			if (ret == HTTPD_SOCK_ERR_TIMEOUT)
				continue;
			free(buf);
			return NULL;
		}
		received += ret;
	}
	buf[received] = 0;
	cJSON *json = cJSON_Parse(buf);
	free(buf);
	if (!json)
		send_err(req, "Invalid JSON");
	return json;
}

static void handle_login(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *pw = cJSON_GetObjectItem(json, "password");
	if (cJSON_IsString(pw) && strcmp(pw->valuestring, s_password) == 0) {
		int slot = -1;
		time_t now = time(NULL);
		xSemaphoreTake(s_session_mux, portMAX_DELAY);
		for (int i = 0; i < s_max_sessions; i++) {
			if (s_sessions[i].token[0] == 0 || (now - s_sessions[i].last_activity) > s_session_timeout) {
				slot = i;
				break;
			}
		}
		if (slot != -1) {
			uint32_t r1 = esp_random(), r2 = esp_random();
			snprintf(s_sessions[slot].token, 33, "%08lx%08lx", r1, r2);
			s_sessions[slot].last_activity = now;
			cJSON *root = cJSON_CreateObject();
			cJSON_AddStringToObject(root, "status", "success");
			cJSON_AddStringToObject(root, "token", s_sessions[slot].token);
			xSemaphoreGive(s_session_mux);
			send_json_resp(req, root);
		} else {
			xSemaphoreGive(s_session_mux);
			send_err(req, "Session full");
		}
	} else {
		send_err(req, "Auth failed");
	}
	cJSON_Delete(json);
}

static void handle_status(httpd_req_t *req)
{
	cJSON *root = cJSON_CreateObject();
	cJSON_AddStringToObject(root, "status", "success");

	multi_heap_info_t info;
	heap_caps_get_info(&info, MALLOC_CAP_DEFAULT);
	cJSON *memory = cJSON_AddObjectToObject(root, "memory");
	cJSON_AddNumberToObject(memory, "total_heap", info.total_free_bytes + info.total_allocated_bytes);
	cJSON_AddNumberToObject(memory, "free_heap", info.total_free_bytes);
	cJSON_AddNumberToObject(memory, "min_free_heap", info.minimum_free_bytes);

	cJSON *cpu = cJSON_AddObjectToObject(root, "cpu");
	esp_chip_info_t chip_info;
	esp_chip_info(&chip_info);
	cJSON_AddNumberToObject(cpu, "cores", chip_info.cores);

	cJSON_AddNumberToObject(root, "uptime", esp_timer_get_time() / 1000000);
	cJSON_AddBoolToObject(root, "storage_mounted", storage_is_mount());

	cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
	wifi_ap_record_t ap_info;
	if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
		cJSON_AddStringToObject(wifi, "ssid", (char *)ap_info.ssid);
		cJSON_AddNumberToObject(wifi, "rssi", ap_info.rssi);
	} else {
		cJSON_AddStringToObject(wifi, "ssid", "Disconnected");
		cJSON_AddNumberToObject(wifi, "rssi", 0);
	}

	esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	if (netif) {
		esp_netif_ip_info_t ip_info;
		if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
			char ip_str[16];
			snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
			cJSON_AddStringToObject(wifi, "ip", ip_str);
		}
	}

	size_t count = storage_get_count();
	cJSON *cards = cJSON_AddArrayToObject(root, "storage_details");
	for (int i = 0; i < count; i++) {
		char p[32];
		snprintf(p, sizeof(p), "/storage_%d", i);

		uint64_t total_bytes = 0;
		uint64_t free_bytes = 0;

		esp_err_t ret = esp_vfs_fat_info(p, &total_bytes, &free_bytes);

		cJSON *card = cJSON_CreateObject();
		cJSON_AddNumberToObject(card, "id", i);
		cJSON_AddStringToObject(card, "path", p);

		if (ret == ESP_OK) {
			uint64_t used_bytes = total_bytes - free_bytes;

			cJSON_AddNumberToObject(card, "total", (double)total_bytes);
			cJSON_AddNumberToObject(card, "used", (double)used_bytes);
			cJSON_AddNumberToObject(card, "free", (double)free_bytes);
			cJSON_AddBoolToObject(card, "active", true);
		} else {
			ESP_LOGW("web", "Storage %d (%s) not ready or info failed", i, p);
			cJSON_AddNumberToObject(card, "total", 0);
			cJSON_AddNumberToObject(card, "used", 0);
			cJSON_AddNumberToObject(card, "free", 0);
			cJSON_AddBoolToObject(card, "active", false);
		}
		cJSON_AddItemToArray(cards, card);
	}
	send_json_resp(req, root);
}

static void handle_list(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *path_obj = cJSON_GetObjectItem(json, "path");
	const char *path = (path_obj && cJSON_IsString(path_obj)) ? path_obj->valuestring : NULL;

	if (is_storage_path(path)) {
		DIR *dir = opendir(path);
		if (dir) {
			cJSON *root = cJSON_CreateObject();
			cJSON_AddStringToObject(root, "status", "success");
			cJSON *items = cJSON_AddArrayToObject(root, "items");
			struct dirent *ent;
			while ((ent = readdir(dir)) != NULL) {
				cJSON *item = cJSON_CreateObject();
				cJSON_AddStringToObject(item, "name", ent->d_name);
				cJSON_AddBoolToObject(item, "is_dir", (ent->d_type == DT_DIR));
				cJSON_AddItemToArray(items, item);
			}
			closedir(dir);
			send_json_resp(req, root);
		} else {
			send_err(req, "Opendir failed");
		}
	} else {
		send_err(req, "Invalid path");
	}
	cJSON_Delete(json);
}

static void recursive_search(cJSON *results, const char *base_path, const char *query, int depth, int *count)
{
	if (depth > 3 || *count >= 50)
		return;

	DIR *dir = opendir(base_path);
	if (!dir)
		return;

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.')
			continue;

		char *fullpath = malloc(512);
		if (fullpath) {
			snprintf(fullpath, 512, "%s/%s", base_path, ent->d_name);
			if (strstr(ent->d_name, query)) {
				cJSON_AddItemToArray(results, cJSON_CreateString(fullpath));
				(*count)++;
			}
			if (ent->d_type == DT_DIR)
				recursive_search(results, fullpath, query, depth + 1, count);
			free(fullpath);
		}
	}
	closedir(dir);
}

static void handle_search(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *q = cJSON_GetObjectItem(json, "query");
	if (cJSON_IsString(q)) {
		cJSON *root = cJSON_CreateObject();
		cJSON_AddStringToObject(root, "status", "success");
		cJSON *res = cJSON_AddArrayToObject(root, "results");

		int count = 0;
		for (int i = 0; i < storage_get_count(); i++) {
			char p[32];
			snprintf(p, sizeof(p), "/storage_%d", i);
			recursive_search(res, p, q->valuestring, 0, &count);
		}
		send_json_resp(req, root);
	} else {
		send_err(req, "Invalid query");
	}
	cJSON_Delete(json);
}

static void handle_upload(httpd_req_t *req)
{
	char q[256], raw_path[256], path[256];
	if (httpd_req_get_url_query_str(req, q, 256) == ESP_OK &&
	    httpd_query_key_value(q, "path", raw_path, 256) == ESP_OK) {

		url_decode(path, raw_path);

		if (!is_storage_path(path)) {
			drain_body(req);
			send_err(req, "Invalid path");
			return;
		}

		if (!file_lock(path, 5000)) {
			drain_body(req);
			send_err(req, "File is busy");
			return;
		}

		FILE *f = fopen(path, "wb");
		if (f) {
			char *buf = malloc(4096);
			if (!buf) {
				fclose(f);
				drain_body(req);
				file_unlock(path);
				send_err(req, "Out of memory");
				return;
			}
			int ret, received = 0;
			bool ok = true;
			while (received < req->content_len) {
				ret = httpd_req_recv(req, buf, 4096);
				if (ret <= 0) {
					if (ret == HTTPD_SOCK_ERR_TIMEOUT)
						continue;
					ok = false;
					break;
				}

				if (fwrite(buf, 1, ret, f) != ret) {
					ok = false;
					break;
				}
				received += ret;
			}

			free(buf);
			fclose(f);
			if (!ok) {
				drain_body(req);
				remove(path);
			}

			file_unlock(path);
			ok ? send_ok(req) : send_err(req, "Upload failed");
		} else {
			drain_body(req);
			file_unlock(path);
			send_err(req, "File open failed");
		}
	} else {
		drain_body(req);
		send_err(req, "Invalid path");
	}
}

static void handle_download(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *path_obj = cJSON_GetObjectItem(json, "path");
	const char *raw_path = (path_obj && cJSON_IsString(path_obj)) ? path_obj->valuestring : NULL;

	if (is_storage_path(raw_path)) {
		char path[256];
		strncpy(path, raw_path, 255);
		path[255] = '\0';
		cJSON_Delete(json);

		if (!file_lock(path, 5000)) {
			send_err(req, "File is busy");
			return;
		}

		FILE *f = fopen(path, "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			size_t size = ftell(f);
			fseek(f, 0, SEEK_SET);

			httpd_resp_set_type(req, "application/octet-stream");

			char hdr[512], *fn = strrchr(path, '/') + 1;
			snprintf(hdr, 512, "attachment; filename=\"%s\"", fn);
			httpd_resp_set_hdr(req, "Content-Disposition", hdr);

			char size_str[16];
			snprintf(size_str, 16, "%u", size);
			httpd_resp_set_hdr(req, "Content-Length", size_str);

			char buf[1024];
			size_t n;
			while (1) {
				n = fread(buf, 1, 1024, f);
				if (n <= 0)
					break;
				if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
					break;
			}
			fclose(f);
			file_unlock(path);

			httpd_resp_send_chunk(req, NULL, 0);
		} else {
			file_unlock(path);
			send_err(req, "File not found");
		}
	} else {
		send_err(req, "Invalid path");
		cJSON_Delete(json);
	}
}

static void handle_new(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *path_obj = cJSON_GetObjectItem(json, "path");
	const char *path = (path_obj && cJSON_IsString(path_obj)) ? path_obj->valuestring : NULL;

	if (is_storage_path(path) && is_valid_filename(strrchr(path, '/') ? strrchr(path, '/') + 1 : path)) {
		if (!file_lock(path, 5000)) {
			send_err(req, "File is busy");
			cJSON_Delete(json);
			return;
		}
		bool ok = (mkdir(path, 0777) == 0);
		file_unlock(path);

		if (ok)
			send_ok(req);
		else
			send_err(req, "Mkdir failed");
	} else {
		send_err(req, "Invalid path or name");
	}
	cJSON_Delete(json);
}

static void handle_touch(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *path_obj = cJSON_GetObjectItem(json, "path");
	const char *path = (path_obj && cJSON_IsString(path_obj)) ? path_obj->valuestring : NULL;

	if (is_storage_path(path) && is_valid_filename(strrchr(path, '/') ? strrchr(path, '/') + 1 : path)) {
		if (!file_lock(path, 5000)) {
			send_err(req, "File is busy");
			cJSON_Delete(json);
			return;
		}
		FILE *f = fopen(path, "a");
		if (f) {
			fclose(f);
			file_unlock(path);
			send_ok(req);
		} else {
			file_unlock(path);
			send_err(req, "Touch failed");
		}
	} else {
		send_err(req, "Invalid path or name");
	}
	cJSON_Delete(json);
}

static bool delete_recursive(const char *path)
{
	struct stat st;
	if (stat(path, &st) != 0)
		return false;

	if (!S_ISDIR(st.st_mode))
		return (remove(path) == 0);

	DIR *dir = opendir(path);
	if (!dir)
		return false;

	struct dirent *ent;
	bool ok = true;
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		char *subpath = malloc(512);
		if (subpath) {
			snprintf(subpath, 512, "%s/%s", path, ent->d_name);
			if (!delete_recursive(subpath))
				ok = false;
			free(subpath);
		} else {
			ok = false;
		}
	}
	closedir(dir);
	if (ok)
		ok = (rmdir(path) == 0);
	return ok;
}

static void handle_delete(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *path_obj = cJSON_GetObjectItem(json, "path");
	const char *path = (path_obj && cJSON_IsString(path_obj)) ? path_obj->valuestring : NULL;

	if (is_storage_path(path)) {
		if (!file_lock(path, 5000)) {
			send_err(req, "File is busy");
			cJSON_Delete(json);
			return;
		}

		bool ok = delete_recursive(path);
		file_unlock(path);

		if (ok)
			send_ok(req);
		else
			send_err(req, "Delete failed");
	} else {
		send_err(req, "Invalid path");
	}
	cJSON_Delete(json);
}

static void handle_rename(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *path_obj = cJSON_GetObjectItem(json, "path");
	const char *path = (path_obj && cJSON_IsString(path_obj)) ? path_obj->valuestring : NULL;

	if (is_storage_path(path)) {
		cJSON *np = cJSON_GetObjectItem(json, "new");
		const char *new_path = (np && cJSON_IsString(np)) ? np->valuestring : NULL;

		if (new_path && is_storage_path(new_path)) {
			const char *old_name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
			const char *new_name = strrchr(new_path, '/') ? strrchr(new_path, '/') + 1 : new_path;

			if (!is_valid_filename(old_name) || !is_valid_filename(new_name)) {
				send_err(req, "Invalid filename");
				cJSON_Delete(json);
				return;
			}

			bool ok = false;
			const char *err_msg = "Rename failed";

			if (strcasecmp(path, new_path) == 0) {
				send_ok(req);
				cJSON_Delete(json);
				return;
			}

			const char *lock_first = path;
			const char *lock_second = new_path;
			if (strcasecmp(lock_first, lock_second) > 0) {
				lock_first = new_path;
				lock_second = path;
			}

			if (!file_lock(lock_first, 5000)) {
				send_err(req, "File is busy");
				cJSON_Delete(json);
				return;
			}
			if (!file_lock(lock_second, 5000)) {
				file_unlock(lock_first);
				send_err(req, "File is busy");
				cJSON_Delete(json);
				return;
			}

			if (access(new_path, F_OK) == 0) {
				err_msg = "Target exists";
			} else {
				ok = (rename(path, new_path) == 0);
			}

			file_unlock(lock_second);
			file_unlock(lock_first);

			if (ok)
				send_ok(req);
			else
				send_err(req, err_msg);
		} else {
			send_err(req, "Invalid new path");
		}
	} else {
		send_err(req, "Invalid path");
	}
	cJSON_Delete(json);
}

static void handle_info(httpd_req_t *req)
{
	cJSON *json = read_json_body(req);
	if (!json)
		return;

	cJSON *path_obj = cJSON_GetObjectItem(json, "path");
	const char *path = (path_obj && cJSON_IsString(path_obj)) ? path_obj->valuestring : NULL;

	if (is_storage_path(path)) {
		struct stat st;
		if (stat(path, &st) == 0) {
			cJSON *root = cJSON_CreateObject();
			cJSON_AddStringToObject(root, "status", "success");
			cJSON_AddStringToObject(root, "name", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
			cJSON_AddNumberToObject(root, "size", (double)st.st_size);
			cJSON_AddNumberToObject(root, "mtime", (double)st.st_mtime);
			cJSON_AddBoolToObject(root, "is_dir", S_ISDIR(st.st_mode));
			send_json_resp(req, root);
		} else {
			send_err(req, "Stat failed");
		}
	} else {
		send_err(req, "Invalid path");
	}
	cJSON_Delete(json);
}

esp_err_t static api_handle(httpd_req_t *req)
{
	pmu_activity_start();
	bool is_login = (strncmp(req->uri, "/api/login", 10) == 0);
	bool auth_ok = false;

	if (!is_login) {
		char token[33] = {0};
		if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", token, sizeof(token)) == ESP_OK) {
			time_t now = time(NULL);
			xSemaphoreTake(s_session_mux, portMAX_DELAY);
			for (int i = 0; i < s_max_sessions; i++) {
				if (s_sessions[i].token[0] != 0 && strcmp(token, s_sessions[i].token) == 0 &&
				    (now - s_sessions[i].last_activity) <= s_session_timeout) {
					s_sessions[i].last_activity = now;
					auth_ok = true;
					break;
				}
			}
			xSemaphoreGive(s_session_mux);
		}

		if (!auth_ok) {
			drain_body(req);
			send_err(req, "Unauthorized");
			pmu_activity_end();
			return ESP_OK;
		}
	}

	if (strncmp(req->uri, "/api/login", 10) == 0)
		handle_login(req);
	else if (strncmp(req->uri, "/api/status", 11) == 0)
		handle_status(req);
	else if (strncmp(req->uri, "/api/info", 9) == 0)
		handle_info(req);
	else if (strncmp(req->uri, "/api/upload", 11) == 0)
		handle_upload(req);
	else if (strncmp(req->uri, "/api/list", 9) == 0)
		handle_list(req);
	else if (strncmp(req->uri, "/api/search", 11) == 0)
		handle_search(req);
	else if (strncmp(req->uri, "/api/download", 13) == 0)
		handle_download(req);
	else if (strncmp(req->uri, "/api/new", 8) == 0)
		handle_new(req);
	else if (strncmp(req->uri, "/api/touch", 10) == 0)
		handle_touch(req);
	else if (strncmp(req->uri, "/api/delete", 11) == 0)
		handle_delete(req);
	else if (strncmp(req->uri, "/api/rename", 11) == 0)
		handle_rename(req);
	else {
		drain_body(req);
		send_err(req, "Unknown API endpoint");
	}

	pmu_activity_end();
	return ESP_OK;
}

static esp_err_t page_handle(httpd_req_t *req)
{
	pmu_activity_start();
	char uri_path[512];
	strncpy(uri_path, req->uri, sizeof(uri_path) - 1);
	uri_path[sizeof(uri_path) - 1] = '\0';

	char *query_ptr = strchr(uri_path, '?');
	if (query_ptr)
		*query_ptr = '\0';

	char *hash_ptr = strchr(uri_path, '#');
	if (hash_ptr)
		*hash_ptr = '\0';

	char path[512 + 11];
	if (strcmp(uri_path, "/") == 0)
		strcpy(path, "/flash/web/index.html");
	else
		snprintf(path, sizeof(path), "/flash/web%s", uri_path);

	if (strstr(path, "/../") != NULL || strncmp(path, "../", 3) == 0 ||
	    (strlen(path) >= 3 && strcmp(path + strlen(path) - 3, "/..") == 0) || strcmp(path, "..") == 0 ||
	    strncmp(path, "/flash/web/", 11) != 0) {
		httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Forbidden");
		pmu_activity_end();
		return ESP_FAIL;
	}

	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
		httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not Found");
		pmu_activity_end();
		return ESP_FAIL;
	}

	FILE *f = fopen(path, "r");

	if (!f) {
		httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Error");
		pmu_activity_end();
		return ESP_FAIL;
	}

	const char *ext = strrchr(path, '.');
	if (ext) {
		if (strcasecmp(ext, ".html") == 0)
			httpd_resp_set_type(req, "text/html");
		else if (strcasecmp(ext, ".css") == 0)
			httpd_resp_set_type(req, "text/css");
		else if (strcasecmp(ext, ".js") == 0)
			httpd_resp_set_type(req, "application/javascript");
		else if (strcasecmp(ext, ".ico") == 0)
			httpd_resp_set_type(req, "image/x-icon");
		else
			httpd_resp_set_type(req, "text/plain");
	} else {
		httpd_resp_set_type(req, "application/octet-stream");
	}

	char buf[1024];
	size_t n;
	while (1) {
		n = fread(buf, 1, 1024, f);
		if (n <= 0)
			break;
		if (httpd_resp_send_chunk(req, buf, n) != ESP_OK)
			break;
	}

	fclose(f);

	httpd_resp_send_chunk(req, NULL, 0);
	pmu_activity_end();
	return ESP_OK;
}

esp_err_t web_init(char *wifi_ssid, char *wifi_password, char *password, uint32_t session, uint32_t session_time)
{
	strncpy(s_password, password, 15);
	s_password[15] = '\0';

	s_max_sessions = (session > 0) ? session : 1;
	s_session_timeout = (session_time > 0) ? session_time : 1800;
	s_sessions = calloc(s_max_sessions, sizeof(session_t));

	s_session_mux = xSemaphoreCreateMutex();
	s_file_lock_mux = xSemaphoreCreateMutex();
	s_pmu_mux = xSemaphoreCreateMutex();
	s_pmu_timer = xTimerCreate("pmu_timer", pdMS_TO_TICKS(10000), pdFALSE, NULL, pmu_relax_cb);

	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);

	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

	wifi_config_t wifi_cfg = {.sta = {}};
	strncpy((char *)wifi_cfg.sta.ssid, wifi_ssid, 31);
	strncpy((char *)wifi_cfg.sta.password, wifi_password, 63);
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
	ESP_ERROR_CHECK(esp_wifi_start());

	httpd_handle_t server = NULL;
	httpd_config_t config = HTTPD_DEFAULT_CONFIG();
	config.max_uri_handlers = 20;
	config.uri_match_fn = httpd_uri_match_wildcard;
	ESP_ERROR_CHECK(httpd_start(&server, &config));

	httpd_uri_t page = {.uri = "/*", .method = HTTP_GET, .handler = page_handle};
	httpd_uri_t api = {.uri = "/api/*", .method = HTTP_POST, .handler = api_handle};
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &page));
	ESP_ERROR_CHECK(httpd_register_uri_handler(server, &api));

	return ESP_OK;
}