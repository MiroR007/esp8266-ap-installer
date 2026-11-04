#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event_loop.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "tcpip_adapter.h"

#define MAX_TRACKED_STATIONS 8
#define MAC_LEN 6

static const char *TAG = "ap-control";

typedef struct {
    bool used;
    uint8_t mac[MAC_LEN];
    uint16_t aid;
} station_slot_t;

static station_slot_t s_stations[MAX_TRACKED_STATIONS];
static bool s_block_enabled;
static uint8_t s_blocked_mac[MAC_LEN];
static SemaphoreHandle_t s_lock;

static const char INDEX_HTML[] =
"<!doctype html><html lang='ar' dir='rtl'><head>"
"<meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>إدارة نقطة ESP8266</title><style>"
":root{color-scheme:dark}*{box-sizing:border-box}body{margin:0;background:#0b1220;color:#e5edf8;font-family:system-ui,sans-serif}"
"main{max-width:720px;margin:auto;padding:20px}.card{background:#141f31;border:1px solid #293851;border-radius:18px;padding:18px;margin:14px 0}"
"h1{font-size:1.55rem;margin:.2rem 0}.muted{color:#9fb0c9;line-height:1.6}.station{display:grid;grid-template-columns:auto 1fr;gap:12px;align-items:center;padding:13px;border:1px solid #32435e;border-radius:13px;margin:10px 0}"
"input{width:22px;height:22px}.mac{font-family:ui-monospace,monospace;direction:ltr;text-align:left}.ip{color:#9fb0c9;direction:ltr;text-align:left}"
"button{width:100%;border:0;border-radius:12px;padding:13px;margin-top:9px;font-size:1rem;font-weight:700;background:#3b82f6;color:white}"
"button.warn{background:#ef4444}button.stop{background:#22c55e;color:#07130b}button:disabled{opacity:.45}.status{min-height:1.6em;margin-top:10px}"
".pill{display:inline-block;border-radius:99px;padding:4px 10px;background:#24334b;color:#bcd0eb;font-size:.85rem}"
"</style></head><body><main><h1>إدارة عملاء نقطة الاختبار</h1>"
"<p class='muted'>هذه الصفحة تتحكم فقط بالأجهزة المتصلة مباشرةً بنقطة الوصول التي ينشئها هذا ESP8266.</p>"
"<section class='card'><span id='mode' class='pill'>السماح بإعادة الاتصال</span><div id='list'><p class='muted'>جارٍ التحميل…</p></div>"
"<button id='once' class='warn' disabled>فصل الجهاز المحدد مرة واحدة</button>"
"<button id='block' class='warn' disabled>حظر محلي وفصل</button>"
"<button id='restore' class='stop'>إيقاف الحظر / استعادة الاتصال</button><div id='status' class='status muted'></div></section>"
"<section class='card muted'>تنبيه: إذا فصلت الهاتف الذي يعرض هذه الصفحة، ستنقطع الصفحة أيضًا. الفصل لمرة واحدة لا يمنع الجهاز من إعادة الاتصال تلقائيًا.</section>"
"</main><script>"
"let selected='';const $=s=>document.querySelector(s);"
"function esc(s){return String(s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]))}"
"async function refresh(){try{let r=await fetch('/api/stations',{cache:'no-store'}),d=await r.json();"
"$('#mode').textContent=d.blocked?('محظور: '+d.blocked):'السماح بإعادة الاتصال';"
"$('#list').innerHTML=d.stations.length?d.stations.map(x=>`<label class='station'><input type='radio' name='s' value='${esc(x.mac)}'><span><div class='mac'>${esc(x.mac)}</div><div class='ip'>${esc(x.ip||'لا يوجد IP بعد')}</div></span></label>`).join(''):`<p class='muted'>لا توجد أجهزة متصلة حاليًا.</p>`;"
"document.querySelectorAll('input[name=s]').forEach(x=>x.onchange=()=>{selected=x.value;$('#once').disabled=$('#block').disabled=false});"
"}catch(e){$('#status').textContent='تعذر تحديث القائمة.'}}"
"async function act(path,mac){$('#status').textContent='جارٍ التنفيذ…';try{let r=await fetch(path,{method:'POST',headers:{'Content-Type':'text/plain'},body:mac||''});let d=await r.json();$('#status').textContent=d.message||'تم';selected='';setTimeout(refresh,700)}catch(e){$('#status').textContent='انقطع الاتصال أو تعذر تنفيذ الطلب.'}}"
"$('#once').onclick=()=>act('/api/disconnect',selected);$('#block').onclick=()=>act('/api/block',selected);$('#restore').onclick=()=>act('/api/restore','');"
"refresh();setInterval(refresh,3000);</script></body></html>";

static bool mac_equal(const uint8_t a[MAC_LEN], const uint8_t b[MAC_LEN])
{
    return memcmp(a, b, MAC_LEN) == 0;
}

static void mac_to_string(const uint8_t mac[MAC_LEN], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static bool parse_mac(const char *text, uint8_t mac[MAC_LEN])
{
    unsigned int v[MAC_LEN];
    char tail;
    if (sscanf(text, "%2x:%2x:%2x:%2x:%2x:%2x%c",
               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) {
        return false;
    }
    for (int i = 0; i < MAC_LEN; ++i) mac[i] = (uint8_t)v[i];
    return true;
}

static void remember_station(const uint8_t mac[MAC_LEN], uint16_t aid)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int free_index = -1;
    for (int i = 0; i < MAX_TRACKED_STATIONS; ++i) {
        if (s_stations[i].used && mac_equal(s_stations[i].mac, mac)) {
            s_stations[i].aid = aid;
            xSemaphoreGive(s_lock);
            return;
        }
        if (!s_stations[i].used && free_index < 0) free_index = i;
    }
    if (free_index >= 0) {
        s_stations[free_index].used = true;
        memcpy(s_stations[free_index].mac, mac, MAC_LEN);
        s_stations[free_index].aid = aid;
    }
    xSemaphoreGive(s_lock);
}

static void forget_station(const uint8_t mac[MAC_LEN])
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_TRACKED_STATIONS; ++i) {
        if (s_stations[i].used && mac_equal(s_stations[i].mac, mac)) {
            memset(&s_stations[i], 0, sizeof(s_stations[i]));
            break;
        }
    }
    xSemaphoreGive(s_lock);
}

static bool aid_for_mac(const uint8_t mac[MAC_LEN], uint16_t *aid)
{
    bool found = false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_TRACKED_STATIONS; ++i) {
        if (s_stations[i].used && mac_equal(s_stations[i].mac, mac)) {
            *aid = s_stations[i].aid;
            found = true;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return found;
}

static esp_err_t event_handler(void *ctx, system_event_t *event)
{
    (void)ctx;
    if (event->event_id == SYSTEM_EVENT_AP_STACONNECTED) {
        const system_event_ap_staconnected_t *e = &event->event_info.sta_connected;
        remember_station(e->mac, e->aid);

        bool reject;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        reject = s_block_enabled && mac_equal(s_blocked_mac, e->mac);
        xSemaphoreGive(s_lock);
        if (reject) {
            ESP_LOGI(TAG, "Rejecting blocked station, AID=%u", e->aid);
            esp_wifi_deauth_sta(e->aid);
        }
    } else if (event->event_id == SYSTEM_EVENT_AP_STADISCONNECTED) {
        forget_station(event->event_info.sta_disconnected.mac);
    }
    return ESP_OK;
}

static esp_err_t send_json(httpd_req_t *req, const char *json)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t stations_get(httpd_req_t *req)
{
    wifi_sta_list_t wifi_list = {0};
    tcpip_adapter_sta_list_t ip_list = {0};
    char json[1024];
    size_t used = 0;

    if (esp_wifi_ap_get_sta_list(&wifi_list) != ESP_OK ||
        tcpip_adapter_get_sta_list(&wifi_list, &ip_list) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"error\":\"station list unavailable\"}");
    }

    char blocked[18] = {0};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_block_enabled) mac_to_string(s_blocked_mac, blocked);
    xSemaphoreGive(s_lock);

    used = blocked[0]
        ? (size_t)snprintf(json, sizeof(json), "{\"blocked\":\"%s\",\"stations\":[", blocked)
        : (size_t)snprintf(json, sizeof(json), "{\"blocked\":null,\"stations\":[");

    for (int i = 0; i < wifi_list.num; ++i) {
        char mac[18];
        char ip[16] = {0};
        mac_to_string(wifi_list.sta[i].mac, mac);
        for (int j = 0; j < ip_list.num; ++j) {
            if (mac_equal(wifi_list.sta[i].mac, ip_list.sta[j].mac)) {
                snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ip_list.sta[j].ip));
                break;
            }
        }
        used += snprintf(json + used, sizeof(json) - used,
                         "%s{\"mac\":\"%s\",\"ip\":\"%s\"}",
                         i ? "," : "", mac, ip);
    }
    snprintf(json + used, sizeof(json) - used, "]}");
    return send_json(req, json);
}

static esp_err_t read_mac_body(httpd_req_t *req, uint8_t mac[MAC_LEN])
{
    char body[32];
    if (req->content_len <= 0 || req->content_len >= (int)sizeof(body)) return ESP_ERR_INVALID_ARG;
    int got = httpd_req_recv(req, body, req->content_len);
    if (got != req->content_len) return ESP_FAIL;
    body[got] = '\0';
    return parse_mac(body, mac) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

static esp_err_t disconnect_post(httpd_req_t *req)
{
    uint8_t mac[MAC_LEN];
    uint16_t aid;
    if (read_mac_body(req, mac) != ESP_OK || !aid_for_mac(mac, &aid)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"message\":\"الجهاز غير متصل أو العنوان غير صالح\"}");
    }
    esp_err_t err = esp_wifi_deauth_sta(aid);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"message\":\"تعذر فصل الجهاز\"}");
    }
    return send_json(req, "{\"message\":\"تم طلب الفصل مرة واحدة؛ قد يعيد الجهاز الاتصال تلقائيًا\"}");
}

static esp_err_t block_post(httpd_req_t *req)
{
    uint8_t mac[MAC_LEN];
    uint16_t aid;
    if (read_mac_body(req, mac) != ESP_OK || !aid_for_mac(mac, &aid)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_json(req, "{\"message\":\"الجهاز غير متصل أو العنوان غير صالح\"}");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(s_blocked_mac, mac, MAC_LEN);
    s_block_enabled = true;
    xSemaphoreGive(s_lock);

    esp_err_t err = esp_wifi_deauth_sta(aid);
    if (err != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_block_enabled = false;
        xSemaphoreGive(s_lock);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_json(req, "{\"message\":\"تعذر تفعيل الحظر\"}");
    }
    return send_json(req, "{\"message\":\"تم الحظر المحلي والفصل. استخدم الاستعادة للسماح مجددًا\"}");
}

static esp_err_t restore_post(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_block_enabled = false;
    memset(s_blocked_mac, 0, sizeof(s_blocked_mac));
    xSemaphoreGive(s_lock);
    return send_json(req, "{\"message\":\"تم إيقاف الحظر؛ يمكن للجهاز إعادة الاتصال الآن\"}");
}

static void start_web_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&server, &config));

    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_get},
        {.uri = "/api/stations", .method = HTTP_GET, .handler = stations_get},
        {.uri = "/api/disconnect", .method = HTTP_POST, .handler = disconnect_post},
        {.uri = "/api/block", .method = HTTP_POST, .handler = block_post},
        {.uri = "/api/restore", .method = HTTP_POST, .handler = restore_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
    }
}

static void start_access_point(void)
{
    const size_t ssid_len = strlen(CONFIG_APP_AP_SSID);
    const size_t password_len = strlen(CONFIG_APP_AP_PASSWORD);
    if (ssid_len == 0 || ssid_len > 32 || password_len < 8 || password_len > 63) {
        ESP_LOGE(TAG, "SSID must be 1-32 bytes and password must be 8-63 bytes");
        abort();
    }

    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_init(event_handler, NULL));

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, CONFIG_APP_AP_SSID, sizeof(ap.ap.ssid));
    strncpy((char *)ap.ap.password, CONFIG_APP_AP_PASSWORD, sizeof(ap.ap.password));
    ap.ap.ssid_len = ssid_len;
    ap.ap.channel = CONFIG_APP_AP_CHANNEL;
    ap.ap.max_connection = 8;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP ready: SSID=%s, UI=http://192.168.4.1", CONFIG_APP_AP_SSID);
}

void app_main(void)
{
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs);
    }

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) abort();
    start_access_point();
    start_web_server();
}

