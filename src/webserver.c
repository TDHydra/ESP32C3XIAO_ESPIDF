/**
 * @file webserver.c
 * @brief HTTP server: dashboard HTML + SSE live-log stream + JSON status.
 *
 * Endpoints
 * ─────────
 *   GET /        — Monitoring dashboard (HTML; auto-refreshes via SSE)
 *   GET /events  — Server-Sent Events: "log" and "status" event types
 *   GET /status  — JSON snapshot (battery, motion, audio RMS)
 *
 * SSE protocol
 * ────────────
 *   The /events handler blocks indefinitely in the HTTP worker thread,
 *   polling the app_log ring buffer every CFG_SSE_POLL_MS ms and sending
 *   any new entries to the connected browser.  A keepalive comment (":\n\n")
 *   is emitted every ~5 s so proxies do not close idle connections.
 *   A "status" event is also sent every ~5 s carrying a JSON payload with
 *   the current battery, PIR and audio state.
 *
 *   Only one SSE client is served at a time; a second connect request will
 *   block behind the HTTP server's queue until the first disconnects.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "webserver.h"
#include "app_log.h"
#include "battery.h"
#include "pir.h"
#include "microphone.h"
#include "app_config.h"

static const char *TAG = "WEBSERVER";

/* ══════════════════════════════════════════════════════════════════════
 * Embedded HTML dashboard
 * Everything between the markers is served verbatim to the browser.
 * ══════════════════════════════════════════════════════════════════════ */
static const char INDEX_HTML[] =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>ESP32-C3 Monitor</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0d1117;color:#c9d1d9;font-family:'Courier New',monospace;font-size:13px}"
".hdr{background:#161b22;padding:12px 18px;border-bottom:1px solid #21262d;display:flex;align-items:center;gap:12px}"
".hdr h1{color:#58a6ff;font-size:17px;flex:1}"
".hdr p{color:#8b949e;font-size:11px}"
".dot{display:inline-block;width:9px;height:9px;border-radius:50%;margin-right:5px;vertical-align:middle}"
".dot.ok{background:#3fb950}.dot.warn{background:#d29922}.dot.err{background:#f85149}.dot.spin{background:#d29922;animation:blink 1s infinite}"
"@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}"
".cards{display:flex;flex-wrap:wrap;gap:8px;padding:10px 18px;background:#010409}"
".card{background:#161b22;border:1px solid #21262d;border-radius:6px;padding:9px 14px;min-width:130px}"
".card .lbl{color:#8b949e;font-size:10px;text-transform:uppercase;letter-spacing:.8px}"
".card .val{font-size:20px;font-weight:700;margin-top:3px}"
".card .sub{color:#8b949e;font-size:10px;margin-top:2px}"
".val.ok{color:#3fb950}.val.warn{color:#d29922}.val.err{color:#f85149}.val.neu{color:#58a6ff}"
".toolbar{display:flex;align-items:center;justify-content:space-between;padding:6px 18px;"
"background:#0d1117;border-bottom:1px solid #21262d}"
".toolbar span{color:#8b949e;font-size:11px;max-width:60%;overflow:hidden;white-space:nowrap;text-overflow:ellipsis}"
".btn{background:#21262d;color:#c9d1d9;border:1px solid #30363d;border-radius:4px;padding:3px 9px;"
"cursor:pointer;font-size:11px;font-family:inherit}"
".btn:hover{background:#30363d}.btn.on{background:#238636;border-color:#2ea043;color:#fff}"
"#logs{height:calc(100vh - 168px);overflow-y:auto;padding:6px 18px}"
".le{display:flex;gap:8px;padding:2px 0;border-bottom:1px solid #161b22;font-size:12px;line-height:1.5}"
".le .ts{color:#484f58;min-width:88px;flex-shrink:0}"
".le .lv{min-width:64px;flex-shrink:0;font-weight:700}"
".le .tg{min-width:80px;flex-shrink:0;color:#388bfd}"
".le.V .lv{color:#484f58}.le.D .lv{color:#8b949e}.le.I .lv{color:#3fb950}"
".le.W .lv{color:#d29922}.le.E .lv{color:#f85149}.le.C .lv{color:#ff7b72;font-size:13px}"
".le.V .msg{color:#484f58}.le.D .msg{color:#8b949e}.le.I .msg{color:#c9d1d9}"
".le.W .msg{color:#e3b341}.le.E .msg{color:#ffa198}.le.C .msg{color:#ff7b72;font-weight:700}"
"</style>"
"</head>"
"<body>"
"<div class=\"hdr\">"
"<h1>&#9675; ESP32-C3 System Monitor</h1>"
"<p><span id=\"dot\" class=\"dot spin\"></span><span id=\"cst\">Connecting...</span>"
"&nbsp;&bull;&nbsp;<span id=\"upt\">--:--:--</span></p>"
"</div>"
"<div class=\"cards\">"
"<div class=\"card\"><div class=\"lbl\">Battery</div>"
"<div class=\"val neu\" id=\"bpct\">--%</div>"
"<div class=\"sub\" id=\"bmv\">--- mV</div></div>"
"<div class=\"card\"><div class=\"lbl\">Battery State</div>"
"<div class=\"val neu\" id=\"bst\">--</div></div>"
"<div class=\"card\"><div class=\"lbl\">Motion (PIR)</div>"
"<div class=\"val neu\" id=\"mot\">--</div></div>"
"<div class=\"card\"><div class=\"lbl\">Audio RMS</div>"
"<div class=\"val neu\" id=\"rms\">--</div></div>"
"<div class=\"card\"><div class=\"lbl\">Log Entries</div>"
"<div class=\"val neu\" id=\"cnt\">0</div></div>"
"</div>"
"<div class=\"toolbar\">"
"<span id=\"last\">No events received</span>"
"<div style=\"display:flex;gap:6px\">"
"<button class=\"btn on\" id=\"asbtn\" onclick=\"toggleAS()\">Auto-scroll</button>"
"<button class=\"btn\" onclick=\"clearLogs()\">Clear</button>"
"</div>"
"</div>"
"<div id=\"logs\"></div>"
"<script>"
"var as=true,cnt=0,es=null;"
"var LV={0:'V',1:'D',2:'I',3:'W',4:'E',5:'C'};"
"var LC={0:'V',1:'D',2:'I',3:'W',4:'E',5:'C'};"
"function h(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/\"/g,'&quot;');}"
"function pad(n){return String(n).padStart(2,'0');}"
"function fmtTs(s){var h2=Math.floor(s/3600),m=Math.floor((s%3600)/60),s2=s%60;"
"return pad(h2)+':'+pad(m)+':'+pad(s2);}"
"function setConn(st,txt){"
"var d=document.getElementById('dot'),c=document.getElementById('cst');"
"d.className='dot '+st;c.textContent=txt;}"
"function toggleAS(){"
"as=!as;"
"var b=document.getElementById('asbtn');"
"b.textContent=as?'Auto-scroll':'Auto-scroll OFF';"
"b.className=as?'btn on':'btn';}"
"function clearLogs(){document.getElementById('logs').innerHTML='';cnt=0;document.getElementById('cnt').textContent='0';}"
"function addLog(d){"
"var box=document.getElementById('logs');"
"var le=document.createElement('div');"
"var lv=LV[d.level]||'I';"
"le.className='le '+lv;"
"var ts=d.ts_s!==undefined?fmtTs(d.ts_s):'--:--:--';"
"le.innerHTML='<span class=\"ts\">'+h(ts)+'</span>'"
"+'<span class=\"lv\">['+lv+']</span>'"
"+'<span class=\"tg\">'+h(d.tag||'')+'</span>'"
"+'<span class=\"msg\">'+h(d.msg||'')+'</span>';"
"box.appendChild(le);"
"cnt++;document.getElementById('cnt').textContent=cnt;"
"var m=String(d.msg||'').substring(0,70);"
"document.getElementById('last').textContent='Last: '+m;"
"if(as)box.scrollTop=box.scrollHeight;}"
"function updateStatus(d){"
"if(d.battery_mv!==undefined){"
"document.getElementById('bmv').textContent=d.battery_mv+' mV';"
"document.getElementById('bpct').textContent=d.battery_pct+'%';"
"var bs=d.battery_state||'OK';"
"var be=document.getElementById('bst');"
"be.textContent=bs;"
"be.className='val '+(bs==='FULL'||bs==='HIGH'||bs==='MEDIUM'?'ok':bs==='LOW'?'warn':'err');"
"var pe=document.getElementById('bpct');"
"pe.className='val '+(d.battery_pct>40?'ok':d.battery_pct>20?'warn':'err');}"
"if(d.motion!==undefined){"
"var me=document.getElementById('mot');"
"me.textContent=d.motion?'DETECTED':'Clear';"
"me.className='val '+(d.motion?'warn':'ok');}"
"if(d.audio_rms!==undefined){document.getElementById('rms').textContent=d.audio_rms;}"
"if(d.uptime_s!==undefined){document.getElementById('upt').textContent=fmtTs(d.uptime_s);}}"
"function connect(){"
"setConn('spin','Connecting...');"
"es=new EventSource('/events');"
"es.onopen=function(){setConn('ok','Connected');};"
"es.addEventListener('log',function(e){"
"try{addLog(JSON.parse(e.data));}catch(x){console.error(x);}});"
"es.addEventListener('status',function(e){"
"try{updateStatus(JSON.parse(e.data));}catch(x){console.error(x);}});"
"es.onerror=function(){setConn('err','Reconnecting...');es.close();setTimeout(connect,3000);};"
"}"
"connect();"
"</script>"
"</body>"
"</html>";

/* ══════════════════════════════════════════════════════════════════════
 * JSON helpers
 * ══════════════════════════════════════════════════════════════════════ */

/**
 * Escape a string for safe embedding inside a JSON string value.
 * Handles ", \, and control characters.
 */
static int json_escape(const char *in, char *out, size_t out_len)
{
    size_t o = 0;
    for (size_t i = 0; in[i] && o + 4 < out_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[o++] = '\\'; out[o++] = (char)c;
        } else if (c < 0x20) {
            /* skip control characters */
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return (int)o;
}

static const char *level_name(app_log_level_t lv)
{
    switch (lv) {
        case APP_LOG_VERBOSE:  return "VERBOSE";
        case APP_LOG_DEBUG:    return "DEBUG";
        case APP_LOG_INFO:     return "INFO";
        case APP_LOG_WARN:     return "WARN";
        case APP_LOG_ERROR:    return "ERROR";
        case APP_LOG_CRITICAL: return "CRITICAL";
        default:               return "INFO";
    }
}

/** Build a JSON status payload into buf. Returns bytes written. */
static int build_status_json(char *buf, size_t buf_len)
{
    battery_info_t batt  = battery_get_info();
    bool           motion = pir_motion_detected();
    uint32_t       rms   = microphone_get_rms();
    uint32_t       up_s  = (uint32_t)(esp_timer_get_time() / 1000000ULL);

    return snprintf(buf, buf_len,
        "{"
        "\"battery_mv\":%lu,"
        "\"battery_pct\":%u,"
        "\"battery_state\":\"%s\","
        "\"motion\":%s,"
        "\"audio_rms\":%lu,"
        "\"uptime_s\":%lu"
        "}",
        (unsigned long)batt.voltage_mv,
        batt.percent,
        batt.state_str ? batt.state_str : "UNKNOWN",
        motion ? "true" : "false",
        (unsigned long)rms,
        (unsigned long)up_s);
}

/* ══════════════════════════════════════════════════════════════════════
 * HTTP handlers
 * ══════════════════════════════════════════════════════════════════════ */

/* GET / — serve the dashboard HTML */
static esp_err_t index_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "GET / from client");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, (ssize_t)strlen(INDEX_HTML));
}

/* GET /status — JSON status snapshot */
static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buf[256];
    build_status_json(buf, sizeof(buf));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, buf, (ssize_t)strlen(buf));
}

/* GET /events — SSE live stream (blocks until client disconnects) */
static esp_err_t events_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "SSE client connected");

    httpd_resp_set_type(req, "text/event-stream");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    /* Chunked transfer so we can send incrementally */
    httpd_resp_set_hdr(req, "Transfer-Encoding", "chunked");

    /* Start replay from the oldest buffered entry */
    uint32_t cursor = app_log_oldest_id();
    int idle_polls  = 0;    /* polls since last log entry was sent */

    /* Allocate these outside the loop to avoid repeated large stack frames */
    char chunk[512];
    char escaped_tag[CFG_LOG_TAG_MAX * 2];
    char escaped_msg[CFG_LOG_MSG_MAX * 2];

    while (true) {
        app_log_entry_t entry;
        bool got = app_log_next(&cursor, &entry);

        if (got) {
            /* Encode log entry as SSE "log" event with JSON data */
            json_escape(entry.tag, escaped_tag, sizeof(escaped_tag));
            json_escape(entry.msg, escaped_msg, sizeof(escaped_msg));

            int len = snprintf(chunk, sizeof(chunk),
                "event: log\n"
                "data: {"
                "\"id\":%lu,"
                "\"ts_s\":%lu,"
                "\"level\":%d,"
                "\"tag\":\"%s\","
                "\"msg\":\"%s\""
                "}\n\n",
                (unsigned long)entry.id,
                (unsigned long)entry.ts_s,
                (int)entry.level,
                escaped_tag,
                escaped_msg);

            if (httpd_resp_send_chunk(req, chunk, len) != ESP_OK) {
                ESP_LOGD(TAG, "SSE send failed — client disconnected");
                break;
            }
            idle_polls = 0;

        } else {
            /* No new log entries — sleep briefly */
            vTaskDelay(pdMS_TO_TICKS(CFG_SSE_POLL_MS));
            idle_polls++;

            /* Every ~5 s: send a status event (doubles as SSE keepalive) */
            if (idle_polls >= CFG_SSE_KEEPALIVE_POLLS) {
                char status_json[256];
                build_status_json(status_json, sizeof(status_json));
                int slen = snprintf(chunk, sizeof(chunk),
                    "event: status\ndata: %s\n\n", status_json);
                if (httpd_resp_send_chunk(req, chunk, slen) != ESP_OK) {
                    ESP_LOGD(TAG, "SSE status/keepalive send failed — client disconnected");
                    break;
                }
                idle_polls = 0;
            }
        }
    }

    /* Signal end of chunked body */
    httpd_resp_send_chunk(req, NULL, 0);
    ESP_LOGI(TAG, "SSE client disconnected");
    return ESP_OK;
}

/* ══════════════════════════════════════════════════════════════════════
 * URI registration table
 * ══════════════════════════════════════════════════════════════════════ */

static const httpd_uri_t uri_index = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = index_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_status = {
    .uri     = "/status",
    .method  = HTTP_GET,
    .handler = status_get_handler,
    .user_ctx = NULL,
};

static const httpd_uri_t uri_events = {
    .uri     = "/events",
    .method  = HTTP_GET,
    .handler = events_get_handler,
    .user_ctx = NULL,
};

/* ══════════════════════════════════════════════════════════════════════
 * Public API
 * ══════════════════════════════════════════════════════════════════════ */

esp_err_t webserver_start(void)
{
    ESP_LOGI(TAG, "Starting HTTP server on port %d", CFG_WEBSERVER_PORT);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port      = CFG_WEBSERVER_PORT;
    config.max_open_sockets = 5;
    /* Allow the SSE handler to hold a socket open indefinitely */
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;
    config.lru_purge_enable  = true;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %d", ret);
        return ret;
    }

    httpd_register_uri_handler(server, &uri_index);
    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_events);

    app_log_write(APP_LOG_INFO, TAG,
        "HTTP server ready on http://192.168.4.1:%d", CFG_WEBSERVER_PORT);
    app_log_write(APP_LOG_INFO, TAG,
        "Connect to WiFi \"%s\" then open the URL above", CFG_WIFI_AP_SSID);

    return ESP_OK;
}
