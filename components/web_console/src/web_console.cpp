/*
 * SPDX-FileCopyrightText: 2024 ESP-LEGO Team
 *
 * SPDX-License-Identifier: CC0-1.0
 *
 * ESP-LEGO V1.0 — Web Console main implementation.
 *
 * Implements:
 *   - SoftAP hotspot ("ESP-LEGO-Setup", no password)
 *   - HTTP server on 192.168.4.1 (port 80)
 *   - All API routes: /, /api/config, /api/config/wifi, /api/config/llm,
 *     /api/status, /api/scan, /api/ai, /api/script, /api/exec_log
 *   - Static HTML page (inline CSS/JS, no external dependencies)
 *   - NVS config persistence (namespace "web_console")
 *   - Inactivity timeout (CONFIG_WEB_CONSOLE_TIMEOUT_SEC)
 *
 * design.md §16.1–§16.4, §16.7–§16.10
 */

#include "sdkconfig.h"

#include "web_console/web_console.h"
#include "web_console/script_inject.h"
#include "web_console/wifi_scan.h"
#include "web_console/dns_server.h"
#include "web_console/llm_client.h"

// Interpreter print callback — defined in builtins.cpp as extern
extern void (*g_print_callback)(const char* str, int len);

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_event.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "cJSON.h"

#include "espnow_comm/peer_mgr.h"
#include "espnow_comm/comm.h"

// ====================================================================
// Log tag
// ====================================================================
static const char* TAG = "web_console";

// ====================================================================
// NVS namespace for web console config
// ====================================================================
#define NVS_NS "web_console"

#ifndef CONFIG_SCRIPT_MAX_LEN
#define CONFIG_SCRIPT_MAX_LEN 2048
#endif

// ====================================================================
// Global abort flag reference (defined in app_main.cpp)
// ====================================================================
extern volatile bool s_script_abort_requested;

// ====================================================================
// Static state
// ====================================================================
static httpd_handle_t      s_server        = NULL;
static esp_netif_t*        s_ap_netif      = NULL;
static TimerHandle_t       s_inactivity_timer = NULL;
static bool                s_initialised   = false;
static volatile bool       s_shutdown_requested = false;

// ====================================================================
// HTML page — embedded as a string literal (inlined CSS/JS)
// ====================================================================

static const char* s_html_page =
"<!DOCTYPE html>"
"<html lang=\"en\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
"<meta http-equiv=\"Cache-Control\" content=\"no-cache, no-store, must-revalidate\">"
"<meta http-equiv=\"Pragma\" content=\"no-cache\">"
"<meta http-equiv=\"Expires\" content=\"0\">"
"<title>ESP-LEGO V1.0</title>"
"<style>"
":root{"
"--bg:#0f1923;--card-bg:#1a2634;--card-border:#2a3a4a;"
"--primary:#4fc3f7;--success:#66bb6a;--warning:#ffa726;--error:#ef5350;"
"--text:#e0e0e0;--text-sec:#90a4ae;"
"--inp-bg:#0f1923;--inp-border:#2a3a4a;--log-bg:#0d1117"
"}"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{"
"font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Helvetica Neue',sans-serif;"
"background:var(--bg);color:var(--text);"
"padding:12px;max-width:480px;margin:0 auto;"
"-webkit-font-smoothing:antialiased;font-size:15px;line-height:1.5;"
"min-height:100vh"
"}"
"h2{"
"font-size:13px;font-weight:600;color:var(--primary);"
"text-transform:uppercase;letter-spacing:0.5px;margin-bottom:12px"
"}"
".card{"
"background:var(--card-bg);border:1px solid var(--card-border);"
"border-radius:12px;padding:16px;margin-bottom:12px"
"}"
"/* ---- Header ---- */"
".header-card{padding:12px 16px}"
".header-inner{display:flex;align-items:center;justify-content:space-between}"
".logo{display:flex;align-items:center;gap:10px}"
".logo-icon{"
"width:30px;height:30px;background:linear-gradient(135deg,var(--primary),#0288d1);"
"border-radius:7px;display:flex;align-items:center;justify-content:center;"
"font-size:14px;color:#fff;font-weight:700"
"}"
".logo-text{font-size:16px;font-weight:700;color:var(--text)}"
".ver{color:var(--text-sec);font-weight:400;font-size:11px;margin-left:2px}"
".status-badge{"
"font-size:11px;padding:4px 10px;border-radius:20px;"
"font-weight:500;white-space:nowrap"
"}"
"/* ---- Status bar / notification ---- */"
".status-ok{background:rgba(102,187,106,0.15);color:var(--success)}"
".status-warn{background:rgba(255,167,38,0.15);color:var(--warning)}"
".status-err{background:rgba(239,83,80,0.15);color:var(--error)}"
"#statusBar{"
"font-size:12px;padding:8px 12px;border-radius:8px;margin-bottom:12px;"
"transition:all .2s;min-height:32px;font-weight:500;text-align:center"
"}"
"#statusBar.status-ok{background:rgba(102,187,106,0.1);color:var(--success)}"
"#statusBar.status-warn{background:rgba(255,167,38,0.1);color:var(--warning)}"
"#statusBar.status-err{background:rgba(239,83,80,0.1);color:var(--error)}"
"/* ---- Labels ---- */"
"label{display:block;font-size:12px;color:var(--text-sec);margin:10px 0 4px;font-weight:500}"
"/* ---- Inputs ---- */"
"input,textarea,select{"
"width:100%;padding:10px 12px;"
"background:var(--inp-bg);border:1px solid var(--inp-border);"
"border-radius:8px;font-size:14px;color:var(--text);"
"outline:none;transition:border-color .15s;font-family:inherit"
"}"
"input:focus,textarea:focus{border-color:var(--primary)}"
"textarea{"
"font-family:'Cascadia Code','Fira Code','JetBrains Mono',monospace;"
"resize:vertical;line-height:1.4"
"}"
"/* ---- Input row (key + toggle) ---- */"
".input-row{display:flex;gap:8px}"
".input-row input{flex:1}"
"/* ---- Buttons ---- */"
"button{"
"padding:8px 16px;border:none;border-radius:8px;cursor:pointer;"
"font-size:13px;font-weight:600;transition:all .15s;font-family:inherit;"
"display:inline-flex;align-items:center;justify-content:center;gap:6px"
"}"
"button:active{transform:scale(0.97)}"
".btn-primary{background:var(--primary);color:#0f1923}"
".btn-primary:hover{background:#4dd0f8}"
".btn-secondary{background:var(--card-border);color:var(--text)}"
".btn-secondary:hover{background:#3a4a5a}"
".btn-success{background:var(--success);color:#0f1923}"
".btn-success:hover{background:#76c67a}"
".btn-danger{background:var(--error);color:#fff}"
".btn-danger:hover{background:#f05555}"
".btn-icon{"
"background:transparent;border:1px solid var(--inp-border);"
"color:var(--text-sec);padding:8px 12px;font-size:12px;border-radius:8px"
"}"
".btn-icon:hover{border-color:var(--primary);color:var(--primary)}"
".btn-row{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}"
"/* ---- Loading spinner ---- */"
".btn-loading{pointer-events:none;opacity:0.7}"
".btn-loading::before{"
"content:'';display:inline-block;width:14px;height:14px;"
"border:2px solid rgba(255,255,255,0.3);border-top-color:#fff;"
"border-radius:50%;animation:spin .6s linear infinite;vertical-align:middle;margin-right:6px"
"}"
"/* ---- Scan list ---- */"
".scan-list{margin-top:10px;border-top:1px solid var(--card-border);padding-top:10px}"
".scan-item{"
"display:flex;justify-content:space-between;align-items:center;"
"padding:10px 12px;border-radius:8px;cursor:pointer;"
"transition:background .15s;margin-bottom:4px;border:1px solid transparent"
"}"
".scan-item:hover{background:rgba(79,195,247,0.08);border-color:var(--primary)}"
".scan-loading,.scan-empty{"
"justify-content:center;color:var(--text-sec);cursor:default;font-size:13px"
"}"
".scan-loading:hover,.scan-empty:hover{background:transparent;border-color:transparent}"
".scan-name{font-size:14px;font-weight:500;color:var(--text)}"
".scan-rssi{font-size:12px;font-weight:500}"
".sig-strong{color:var(--success)}"
".sig-medium{color:var(--warning)}"
".sig-weak{color:var(--error)}"
"/* ---- Device list ---- */"
"#deviceList{"
"font-size:13px;line-height:1.8;"
"font-family:'Cascadia Code','Fira Code','JetBrains Mono',monospace"
"}"
"#deviceList .info{color:var(--text-sec)}"
"/* ---- Log box ---- */"
".log-box{"
"background:var(--log-bg);color:#a8d8a8;"
"font-family:'Cascadia Code','Fira Code','JetBrains Mono',monospace;"
"font-size:12px;padding:12px;border-radius:8px;"
"height:200px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;"
"line-height:1.5;border:1px solid var(--card-border)"
"}"
"/* ---- Info text ---- */"
".info{font-size:11px;color:var(--text-sec);margin-top:8px;line-height:1.4}"
"#aiResult{font-size:13px;margin-top:8px;padding:8px 10px;border-radius:6px}"
"/* ---- Scrollbar ---- */"
"::-webkit-scrollbar{width:5px}"
"::-webkit-scrollbar-track{background:transparent}"
"::-webkit-scrollbar-thumb{background:var(--card-border);border-radius:3px}"
"/* ---- Animation ---- */"
"@keyframes spin{to{transform:rotate(360deg)}}"
"</style>"
"</head>"
"<body>"
"<div class=\"card header-card\">"
"<div class=\"header-inner\">"
"<div class=\"logo\">"
"<div class=\"logo-icon\">&#9673;</div>"
"<div class=\"logo-text\">ESP-LEGO <span class=\"ver\">V1.0</span></div>"
"</div>"
"<div id=\"statusBadge\" class=\"status-badge status-ok\">Starting...</div>"
"</div>"
"</div>"
""
"<div id=\"statusBar\"></div>"
""
"<div class=\"card\">"
"<h2>Wi-Fi Config</h2>"
"<label>SSID</label>"
"<input id=\"wifi_ssid\" placeholder=\"Enter Wi-Fi SSID\">"
"<label>Password</label>"
"<input id=\"wifi_pass\" type=\"password\" placeholder=\"Wi-Fi password\">"
"<div class=\"btn-row\">"
"<button class=\"btn-primary\" onclick=\"saveWifiConfig()\">Save</button>"
"<button class=\"btn-secondary\" onclick=\"scanWifi()\" id=\"scanBtn\">Scan</button>"
"</div>"
"<div id=\"scanResults\" class=\"scan-list\" style=\"display:none\"></div>"
"<div class=\"info\">Station connects to this Wi-Fi for internet access.</div>"
"</div>"
""
"<div class=\"card\">"
"<h2>LLM Config</h2>"
"<label>Base URL</label>"
"<input id=\"llm_url\" placeholder=\"https://api.openai.com/v1\">"
"<label>API Key</label>"
"<div class=\"input-row\">"
"<input id=\"llm_key\" type=\"password\" placeholder=\"sk-...\">"
"<button class=\"btn-icon\" onclick=\"toggleKey()\" id=\"keyToggle\" type=\"button\">Show</button>"
"</div>"
"<label>Model</label>"
"<input id=\"llm_model\" placeholder=\"gpt-4o-mini\">"
"<div class=\"btn-row\">"
"<button class=\"btn-primary\" onclick=\"saveLlmConfig()\">Save</button>"
"</div>"
"<div class=\"info\">OpenAI-compatible endpoint. Key stored in device flash.</div>"
"</div>"
""
"<div class=\"card\">"
"<h2>AI Command</h2>"
"<textarea id=\"aiPrompt\" rows=\"3\" placeholder='e.g. \"Read temperature every 5s, over 30C turn on fan\"'></textarea>"
"<button class=\"btn-success\" onclick=\"callAI()\" id=\"aiBtn\">Generate &amp; Inject</button>"
"<div id=\"aiResult\" class=\"info\"></div>"
"</div>"
""
"<div class=\"card\">"
"<h2>Online Devices</h2>"
"<div id=\"deviceList\"><span class=\"info\">Loading...</span></div>"
"<div class=\"btn-row\">"
"<button class=\"btn-secondary\" onclick=\"refreshStatus()\">Refresh</button>"
"</div>"
"</div>"
""
"<div class=\"card\">"
"<h2>Direct Script Injection</h2>"
"<textarea id=\"scriptInput\" rows=\"4\" placeholder='e.g. while(true){print(remote_read(1));sleep(2000)}'></textarea>"
"<button class=\"btn-danger\" onclick=\"injectScript()\">Inject Script</button>"
"</div>"
""
"<div class=\"card\">"
"<h2>Execution Log</h2>"
"<div id=\"execLog\" class=\"log-box\">Waiting for output...</div>"
"<div class=\"btn-row\">"
"<button class=\"btn-secondary\" onclick=\"fetchLog()\">Refresh</button>"
"<button class=\"btn-secondary\" onclick=\"clearLog()\">Clear</button>"
"</div>"
"</div>"
""
"<div id=\"jstest\" style=\"font-size:0;height:1px\"></div>"

"<script>"
"try{document.getElementById('jstest').textContent='JS OK';}catch(e){}"
"const BASE='';"
"function $(id){return document.getElementById(id)}"
"function msg(text,type){"
" var bar=$('statusBar');"
" bar.textContent=text;"
" bar.className='status-'+type;"
" setTimeout(function(){bar.textContent='';bar.className=''},5000)"
"}"
"function toggleKey(){"
" var k=$('llm_key'),t=$('keyToggle');"
" if(k.type==='password'){k.type='text';t.textContent='Hide'}"
" else{k.type='password';t.textContent='Show'}"
"}"
"async function apiFetch(url,opts){"
" try{"
"  var r=await fetch(BASE+url,opts);"
"  if(!r.ok){var t=await r.text();msg('HTTP '+r.status+': '+t,'err');return null}"
"  return r.json()"
" }catch(e){msg('Fetch error: '+e.message,'err');return null}"
"}"
"async function saveWifiConfig(){"
" var body={"
"  wifi_ssid:$('wifi_ssid').value,"
"  wifi_pass:$('wifi_pass').value"
" };"
" var r=await apiFetch('/api/config/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
" if(r&&r.wifi_status)msg('WiFi saved, status: '+r.wifi_status,'ok');else if(r)msg('WiFi saved','ok')"
"}"
"async function saveLlmConfig(){"
" var body={"
"  llm_url:$('llm_url').value,"
"  llm_key:$('llm_key').value,"
"  llm_model:$('llm_model').value"
" };"
" var r=await apiFetch('/api/config/llm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
" if(r)msg('LLM config saved','ok')"
"}"
"async function loadConfig(){"
" var r=await apiFetch('/api/config');"
" if(!r)return;"
" $('wifi_ssid').value=r.wifi_ssid||'';"
" $('llm_url').value=r.llm_url||'';"
" $('llm_model').value=r.llm_model||'';"
"}"
"async function scanWifi(){"
" try{"
"  var btn=$('scanBtn');"
"  btn.disabled=true;btn.classList.add('btn-loading');"
"  msg('Scanning...','ok');"
"  var list=$('scanResults');"
"  list.style.display='block';"
"  list.innerHTML='<div class=\"scan-item scan-loading\"><span>Scanning...</span></div>';"
"  var ctl=new AbortController();"
"  setTimeout(function(){ctl.abort()},8000);"
"  var r=await apiFetch('/api/scan',{signal:ctl.signal});"
"  btn.disabled=false;btn.classList.remove('btn-loading');"
"  if(!r){msg('Scan failed','err');list.innerHTML='<div class=\"scan-item scan-empty\">Scan failed</div>';return}"
"  if(r.length===0){list.innerHTML='<div class=\"scan-item scan-empty\">No networks found</div>';msg('No Wi-Fi found','warn');return}"
"  list.innerHTML='';"
"  r.forEach(function(net){"
"   var item=document.createElement('div');"
"   item.className='scan-item';"
"   var ns=document.createElement('span');ns.className='scan-name';ns.textContent=net.ssid;"
"   var rs=document.createElement('span');"
"   var sig='sig-strong';if(net.rssi<-70)sig='sig-weak';else if(net.rssi<-50)sig='sig-medium';"
"   rs.className='scan-rssi '+sig;rs.textContent=net.rssi+' dBm';"
"   item.appendChild(ns);item.appendChild(rs);"
"   item.onclick=function(){$('wifi_ssid').value=net.ssid;msg('Selected: '+net.ssid,'ok')};"
"   list.appendChild(item)"
"  });"
"  msg('Found '+r.length+' networks','ok')"
" }catch(e){"
"  var btn=$('scanBtn');if(btn){btn.disabled=false;btn.classList.remove('btn-loading')}"
"  msg('scan error: '+e.message,'err');console.error(e)"
" }"
"}"
"async function refreshStatus(){"
" var r=await apiFetch('/api/status');"
" if(!r)return;"
" var html='';"
" if(r.peers&&r.peers.length>0){"
"  r.peers.forEach(function(p){"
"   html+='<b>'+p.name+'</b> (id='+p.id+')';"
"   if(p.capability){html+='<br><span style=font-size:11px;color:#90a4ae>'+p.capability+'</span>'}"
"   html+='<br>'"
"  })"
" }else{"
"  html='<span class=\"info\">No devices online</span>'"
" }"
" $('deviceList').innerHTML=html;"
" var bar=$('statusBar');"
" bar.textContent='Peers: '+r.peer_count+', Script: '+(r.script_running?'running':'idle');"
" bar.className=r.script_running?'status-ok':'status-warn';"
" var badge=$('statusBadge');"
" if(badge){"
"  badge.textContent=r.peer_count+' peer'+(r.peer_count===1?'':'s')+' \\u00b7 '+(r.script_running?'running':'idle');"
"  badge.className='status-badge '+(r.script_running?'status-ok':'status-warn')"
" }"
"}"
"async function callAI(){"
" var prompt=$('aiPrompt').value;"
" if(!prompt.trim()){msg('Enter a command first','warn');return}"
" $('aiResult').textContent='Calling LLM... (WiFi switching may take 10-15s)';"
" var btn=$('aiBtn');btn.disabled=true;btn.classList.add('btn-loading');"
" var r=await apiFetch('/api/ai',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt:prompt})});"
" btn.disabled=false;btn.classList.remove('btn-loading');"
" if(!r){$('aiResult').textContent='Error calling LLM';return}"
" if(r.status==='clarify'){showClarify(r);return}"
" $('aiResult').textContent=r.status==='ok'?'Script injected successfully':'Error: '+(r.error||'unknown');"
" if(r.script){$('scriptInput').value=r.script}"
" setTimeout(fetchLog,500)"
"}"
"function showClarify(r){"
" if(r.script){$('scriptInput').value=r.script}"
" window._clarify=r;"
" var opts=r.options||[];"
" var h='<div>'+(r.question||'Which device?')+'</div>';"
" for(var i=0;i<opts.length;i++){"
"  h+='<button class=\"btn-success\" onclick=\"pickDevice('+opts[i].id+')\">'+opts[i].name+' (id '+opts[i].id+')</button> ';"
" }"
" $('aiResult').innerHTML=h;"
"}"
"async function pickDevice(id){"
" var r=window._clarify;if(!r)return;"
" var script=(r.script||'').split(r.placeholder).join(String(id));"
" $('scriptInput').value=script;"
" $('aiResult').textContent='Injecting on device '+id+'...';"
" var res=await apiFetch('/api/script',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({script:script})});"
" $('aiResult').textContent=(res&&res.status==='ok')?('Script injected on device '+id):('Error: '+((res&&res.error)||'unknown'));"
" window._clarify=null;"
" setTimeout(fetchLog,500)"
"}"
"async function injectScript(){"
" var script=$('scriptInput').value;"
" if(!script.trim()){msg('Enter a script first','warn');return}"
" var r=await apiFetch('/api/script',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({script:script})});"
" if(r)msg(r.status==='ok'?'Script injected':'Error: '+r.error,'warn');"
" setTimeout(fetchLog,500)"
"}"
"async function fetchLog(){"
" var r=await apiFetch('/api/exec_log');"
" if(!r)return;"
" var el=$('execLog');"
" el.textContent=r.log||'(no output)';"
" el.scrollTop=el.scrollHeight"
"}"
"function clearLog(){"
" $('execLog').textContent=''"
"}"
"async function init(){"
" await loadConfig();"
" refreshStatus();"
" fetchLog();"
" setInterval(fetchLog,3000);"
" setInterval(refreshStatus,5000)"
"}"
"console.log('ESP-LEGO JS loaded');"
"init()"
"</script>"
"</body></html>";

// ====================================================================
// NVS helpers
// ====================================================================

static esp_err_t nvs_get_str_safe(const char* key, char* out, size_t max_len)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        out[0] = '\0';
        return err;
    }
    size_t len = max_len;
    err = nvs_get_str(h, key, out, &len);
    if (err != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_set_str_safe(const char* key, const char* val)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;

    if (val && strlen(val) > 0) {
        err = nvs_set_str(h, key, val);
    } else {
        err = nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    }

    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

// ====================================================================
// Inactivity timer callback
// ====================================================================

static void inactivity_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGI(TAG, "Inactivity timeout — stopping web console");
    s_shutdown_requested = true;

    // Stop the HTTP server
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    // Stop SoftAP HTTP only — keep Wi-Fi / ESP-NOW running (pure STA mode
    // breaks broadcast reception on ESP32-S3/C3, and esp_wifi_stop() kills
    // ESP-NOW entirely until the next reboot).
    if (s_ap_netif) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        espnow_comm_sync_rf();
        espnow_comm_send_discovery();
    }
}

// ====================================================================
// Reset inactivity timer
// ====================================================================

static void reset_inactivity_timer(void)
{
    if (s_inactivity_timer && CONFIG_WEB_CONSOLE_TIMEOUT_SEC > 0) {
        xTimerReset(s_inactivity_timer, 0);
    }
}

// ====================================================================
// HTTP Handlers
// ====================================================================

// ---- GET / — static HTML page ----

static esp_err_t root_get_handler(httpd_req_t* req)
{
    reset_inactivity_timer();
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// ---- GET /api/status ----

static esp_err_t status_get_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    int active_count = peer_mgr_active_count();

    // Get peer list
    cJSON* peers_json = cJSON_CreateArray();
    int count = 0;
    PeerEntry** list = peer_mgr_list(&count);
    for (int i = 0; i < count; i++) {
        cJSON* peer_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(peer_obj, "id", list[i]->module_id);
        cJSON_AddStringToObject(peer_obj, "name", list[i]->name);
        cJSON_AddStringToObject(peer_obj, "capability",
                                 list[i]->capability[0] ? list[i]->capability : "");
        cJSON_AddItemToArray(peers_json, peer_obj);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "peer_count", (double)active_count);
    cJSON_AddItemToObject(root, "peers", peers_json);

    // Check if Wi-Fi SSID is configured
    char wifi_ssid[128];
    nvs_get_str_safe("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    cJSON_AddBoolToObject(root, "wifi_configured",
                          (strlen(wifi_ssid) > 0) ? 1 : 0);

    // STA connection state (from llm_client module)
    extern bool wifi_sta_is_connected(void);
    cJSON_AddBoolToObject(root, "sta_connected", wifi_sta_is_connected() ? 1 : 0);

    char llm_url[256];
    nvs_get_str_safe("llm_url", llm_url, sizeof(llm_url));
    cJSON_AddBoolToObject(root, "llm_configured",
                          (strlen(llm_url) > 0) ? 1 : 0);

    cJSON_AddBoolToObject(root, "script_running",
                          s_script_abort_requested ? 0 : 0);
    // Note: true "script_running" check requires access to exec_task state.
    // This is a best-effort indicator.

    char* json = cJSON_PrintUnformatted(root);
    if (json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// ---- GET /api/config ----

static esp_err_t config_get_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    char wifi_ssid[128]  = "";
    char llm_url[256]    = "";
    char llm_model[128]  = "";

    nvs_get_str_safe("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    nvs_get_str_safe("llm_url", llm_url, sizeof(llm_url));
    nvs_get_str_safe("llm_model", llm_model, sizeof(llm_model));

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "wifi_ssid", wifi_ssid);
    cJSON_AddStringToObject(root, "wifi_pass", "***");   // masked
    cJSON_AddStringToObject(root, "llm_url", llm_url);
    cJSON_AddStringToObject(root, "llm_key", "***");     // masked
    cJSON_AddStringToObject(root, "llm_model", llm_model);

    char* json = cJSON_PrintUnformatted(root);
    if (json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
    }
    cJSON_Delete(root);
    return ESP_OK;
}

// ---- POST /api/config/wifi ----
// Accepts only wifi_ssid, wifi_pass. Ignores all unrelated fields (llm_key etc.).

static esp_err_t config_wifi_post_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    char buf[1024];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* item;

    item = cJSON_GetObjectItem(root, "wifi_ssid");
    if (item && item->valuestring) {
        nvs_set_str_safe("wifi_ssid", item->valuestring);
    }

    item = cJSON_GetObjectItem(root, "wifi_pass");
    if (item && item->valuestring) {
        nvs_set_str_safe("wifi_pass", item->valuestring);
    }

    // Deliberately ignore llm_key, llm_url, llm_model — domain isolation
    cJSON_Delete(root);

    // After saving WiFi credentials, trigger STA connection automatically
    char wifi_ssid[128] = "";
    char wifi_pass[128] = "";
    char llm_url[256] = "";
    nvs_get_str_safe("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    nvs_get_str_safe("wifi_pass", wifi_pass, sizeof(wifi_pass));
    nvs_get_str_safe("llm_url", llm_url, sizeof(llm_url));

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");

    if (strlen(wifi_ssid) > 0 && !llm_client_uses_local_proxy(llm_url)) {
        ESP_LOGI(TAG, "WiFi saved — connecting to %s", wifi_ssid);
        esp_err_t err = wifi_connect_sta(wifi_ssid, wifi_pass);
        if (err == ESP_OK) {
            cJSON_AddStringToObject(resp, "wifi_status", "connecting");
        } else {
            cJSON_AddStringToObject(resp, "wifi_status", "failed");
        }
    } else if (strlen(wifi_ssid) > 0) {
        cJSON_AddStringToObject(resp, "wifi_status", "saved_proxy_mode");
    }

    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    }
    return ESP_OK;
}

// ---- POST /api/config/llm ----
// Accepts only llm_url, llm_key, llm_model. Ignores wifi_ssid/wifi_pass.

static esp_err_t config_llm_post_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    char buf[1024];
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* item;
    bool llm_updated = false;

    item = cJSON_GetObjectItem(root, "llm_url");
    if (item && item->valuestring) {
        nvs_set_str_safe("llm_url", item->valuestring);
        llm_updated = true;
    }

    item = cJSON_GetObjectItem(root, "llm_key");
    if (item && item->valuestring) {
        nvs_set_str_safe("llm_key", item->valuestring);
        llm_updated = true;
    }

    item = cJSON_GetObjectItem(root, "llm_model");
    if (item && item->valuestring) {
        nvs_set_str_safe("llm_model", item->valuestring);
        llm_updated = true;
    }

    // Deliberately ignore wifi_ssid, wifi_pass — domain isolation
    cJSON_Delete(root);

    if (llm_updated) {
        ESP_LOGI(TAG, "LLM config saved to NVS (url/key/model)");
    } else {
        ESP_LOGW(TAG, "LLM config POST had no recognised fields");
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    }
    return ESP_OK;
}

// ---- POST /api/wifi/connect ----
// Read WiFi creds from NVS, attempt STA connection, report result.

static esp_err_t wifi_connect_post_handler(httpd_req_t* req)
{
    (void)req;
    reset_inactivity_timer();

    char wifi_ssid[128] = "";
    char wifi_pass[128] = "";
    nvs_get_str_safe("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    nvs_get_str_safe("wifi_pass", wifi_pass, sizeof(wifi_pass));

    cJSON* resp = cJSON_CreateObject();

    if (strlen(wifi_ssid) == 0) {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", "No WiFi SSID configured");
        char* json = cJSON_PrintUnformatted(resp);
        if (json) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, json);
            free(json);
        }
        cJSON_Delete(resp);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "WiFi connect request for %s", wifi_ssid);
    esp_err_t err = wifi_connect_sta(wifi_ssid, wifi_pass);

    if (err == ESP_OK) {
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "wifi_status", "connected");
    } else {
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "wifi_status", "failed");
    }

    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
    }
    return ESP_OK;
}

// ---- GET /api/scan ----

static esp_err_t scan_get_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    cJSON* results = wifi_scan_start();
    if (results == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan failed");
        return ESP_FAIL;
    }

    char* json = cJSON_PrintUnformatted(results);
    cJSON_Delete(results);

    if (json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
    }
    return ESP_OK;
}

static esp_err_t ai_post_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    // Read request body
    char* buf = (char*)malloc(1024);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, 1024 - 1);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == NULL) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* prompt_item = cJSON_GetObjectItem(root, "prompt");
    if (prompt_item == NULL || prompt_item->valuestring == NULL) {
        cJSON_Delete(root);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing prompt");
        return ESP_FAIL;
    }

    const char* user_prompt = prompt_item->valuestring;
    ESP_LOGI(TAG, "AI request: %s", user_prompt);

    // Copy prompt to local buffer before freeing the JSON (use-after-free
    // safety: user_prompt points into the cJSON tree)
    char* prompt_copy = (char*)malloc(1024);
    if (prompt_copy == NULL) {
        cJSON_Delete(root);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    strncpy(prompt_copy, user_prompt, 1024 - 1);
    prompt_copy[1024 - 1] = '\0';

    // Read NVS config
    char wifi_ssid[128]   = "";
    char wifi_pass[128]   = "";
    char llm_url[256]     = "";
    char llm_key[256]     = "";
    char llm_model[128]   = "";

    nvs_get_str_safe("wifi_ssid", wifi_ssid, sizeof(wifi_ssid));
    nvs_get_str_safe("wifi_pass", wifi_pass, sizeof(wifi_pass));
    nvs_get_str_safe("llm_url", llm_url, sizeof(llm_url));
    nvs_get_str_safe("llm_key", llm_key, sizeof(llm_key));
    nvs_get_str_safe("llm_model", llm_model, sizeof(llm_model));

    // Validate
    if (strlen(llm_url) == 0) {
        cJSON_Delete(root);
        free(prompt_copy);
        free(buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"LLM URL not configured\"}");
        return ESP_OK;
    }
    const bool local_proxy_mode = llm_client_uses_local_proxy(llm_url);
    if (strlen(wifi_ssid) == 0 && !local_proxy_mode) {
        cJSON_Delete(root);
        free(prompt_copy);
        free(buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"Wi-Fi not configured\"}");
        return ESP_OK;
    }

    cJSON_Delete(root);
    free(buf);
    buf = NULL;

    // Call LLM
    char* script = (char*)malloc(CONFIG_SCRIPT_MAX_LEN);
    if (script == NULL) {
        free(prompt_copy);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"Out of memory\"}");
        return ESP_OK;
    }
    script[0] = '\0';

    LlmClarify clarify;
    int ret = llm_client_call_ex(wifi_ssid, wifi_pass,
                                 llm_url, llm_key, llm_model,
                                 prompt_copy, script, CONFIG_SCRIPT_MAX_LEN,
                                 &clarify);
    free(prompt_copy);

    // ---- Send HTTP response BEFORE network teardown ----
    // llm_client_finish_network() restarts WiFi which kills the SoftAP;
    // the browser must receive the response first.
    if (ret != 0) {
        free(script);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"error\":\"LLM call failed\"}");
        llm_client_finish_network(local_proxy_mode);
        return ESP_OK;
    }

    if (strlen(script) == 0) {
        free(script);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req,
            "{\"status\":\"error\",\"error\":\"No script in response\"}");
        llm_client_finish_network(local_proxy_mode);
        return ESP_OK;
    }

    // A CLARIFY result means a device reference matched several online peers:
    // return the candidates for the UI to pick, and do NOT inject yet. The
    // script still holds the placeholder token; the UI fills it and re-injects
    // via /api/script (no extra LLM call).
    const bool need_clarify = (clarify.kind == LLM_RESULT_CLARIFY &&
                               clarify.option_count > 0 &&
                               clarify.placeholder[0] != '\0');

    // Build and send response before WiFi restart
    {
        cJSON* resp = cJSON_CreateObject();
        if (resp == NULL) {
            free(script);
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"JSON error\"}");
            llm_client_finish_network(local_proxy_mode);
            return ESP_OK;
        }
        if (need_clarify) {
            cJSON_AddStringToObject(resp, "status", "clarify");
            cJSON_AddStringToObject(resp, "question", clarify.question);
            cJSON_AddStringToObject(resp, "placeholder", clarify.placeholder);
            cJSON_AddStringToObject(resp, "script", script);
            cJSON* opts = cJSON_AddArrayToObject(resp, "options");
            if (opts) {
                for (int i = 0; i < clarify.option_count; i++) {
                    cJSON* o = cJSON_CreateObject();
                    if (o == NULL) break;
                    cJSON_AddNumberToObject(o, "id", clarify.options[i].id);
                    cJSON_AddStringToObject(o, "name", clarify.options[i].name);
                    cJSON_AddItemToArray(opts, o);
                }
            }
        } else {
            cJSON_AddStringToObject(resp, "status", "ok");
            cJSON_AddStringToObject(resp, "script", script);
        }

        char* json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (json) {
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, json);
            free(json);
        }
    }

    // ---- Network teardown: WiFi restart + task resume ----
    // Must happen AFTER the HTTP response so the browser doesn't lose its
    // connection mid-request.
    llm_client_finish_network(local_proxy_mode);

    // Inject script only when it is ready (WiFi is now on ESP-NOW channel).
    // For a clarify result we wait for the user's device choice.
    if (!need_clarify) {
        script_inject_enqueue(script, (int)strlen(script));
    }
    free(script);
    return ESP_OK;
}

// ---- POST /api/script ----

static esp_err_t script_post_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    // Read request body
    char* buf = (char*)malloc(4096);
    if (buf == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    int received = httpd_req_recv(req, buf, 4096 - 1);
    if (received <= 0) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    buf[received] = '\0';

    cJSON* root = cJSON_Parse(buf);
    if (root == NULL) {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON* script_item = cJSON_GetObjectItem(root, "script");
    if (script_item == NULL || script_item->valuestring == NULL) {
        cJSON_Delete(root);
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing script");
        return ESP_FAIL;
    }

    const char* script = script_item->valuestring;
    ESP_LOGI(TAG, "Script injection: %d bytes", (int)strlen(script));

    int inject_ret = script_inject_enqueue(script, (int)strlen(script));
    cJSON_Delete(root);
    free(buf);

    httpd_resp_set_type(req, "application/json");
    if (inject_ret == 0) {
        httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"Injection failed\"}");
    }

    return ESP_OK;
}

// ---- GET /api/exec_log ----

static esp_err_t exec_log_get_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    char* log_buf = (char*)malloc(CONFIG_EXEC_LOG_BUF_SIZE + 1);
    if (log_buf == NULL) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"log\":\"\"}");
        return ESP_OK;
    }

    int len = script_inject_read_log(log_buf, CONFIG_EXEC_LOG_BUF_SIZE);

    if (len <= 0) {
        free(log_buf);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"log\":\"\"}");
        return ESP_OK;
    }

    // Escape the log string for JSON embedding
    // Build response: {"log":"<escaped content>"}
    // Simple approach: use cJSON
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "log", log_buf);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (json) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, json);
        free(json);
    } else {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"log\":\"\"}");
    }
    free(log_buf);

    return ESP_OK;
}

// ====================================================================
// Captive portal error handlers.
// 404 → serve the web console HTML page (for unknown URIs like
//        /mmtls/*, /hotspot-detect.html, etc.)
// 405 → return 204 No Content (for probe URIs with unexpected
//        methods, e.g. POST to /mtuprobe)
// ====================================================================

static esp_err_t catchall_404_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, s_html_page);
    return ESP_OK;
}

static esp_err_t catchall_405_handler(httpd_req_t *req, httpd_err_code_t error)
{
    (void)error;
    // Method not allowed → redirect to root
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// ====================================================================
// Captive portal probe handlers — minimal 204 No Content responses.
// These are registered BEFORE the 404 error handler so they respond
// immediately without going through the error-handler code path.
// ====================================================================

static esp_err_t captive_200_handler(httpd_req_t* req)
{
    (void)req;
    // 302 redirect to the web console root page.
    // Tiny response (~80 bytes) sent instantly — no ECONNRESET.
    // OS opens browser at http://192.168.4.1/ → full console page.
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

// ====================================================================
// URI registration table
// ====================================================================

static const httpd_uri_t s_uris[] = {
    { .uri = "/",            .method = HTTP_GET,    .handler = root_get_handler,       .user_ctx = NULL },
    // Captive portal probe URLs (respond fast — tiny 204 No Content)
    { .uri = "/generate_204",.method = HTTP_GET,    .handler = captive_200_handler,    .user_ctx = NULL },
    { .uri = "/generate204", .method = HTTP_GET,    .handler = captive_200_handler,    .user_ctx = NULL },
    { .uri = "/mtuprobe",   .method = HTTP_GET,    .handler = captive_200_handler,    .user_ctx = NULL },
    { .uri = "/favicon.ico",.method = HTTP_GET,    .handler = captive_200_handler,    .user_ctx = NULL },
    { .uri = "/api/status",  .method = HTTP_GET,    .handler = status_get_handler,      .user_ctx = NULL },
    { .uri = "/api/config",      .method = HTTP_GET,    .handler = config_get_handler,          .user_ctx = NULL },
    { .uri = "/api/config/wifi", .method = HTTP_POST,   .handler = config_wifi_post_handler,    .user_ctx = NULL },
    { .uri = "/api/config/llm",  .method = HTTP_POST,   .handler = config_llm_post_handler,     .user_ctx = NULL },
    { .uri = "/api/wifi/connect", .method = HTTP_POST, .handler = wifi_connect_post_handler, .user_ctx = NULL },
    { .uri = "/api/scan",    .method = HTTP_GET,    .handler = scan_get_handler,        .user_ctx = NULL },
    { .uri = "/api/ai",      .method = HTTP_POST,   .handler = ai_post_handler,         .user_ctx = NULL },
    { .uri = "/api/script",  .method = HTTP_POST,   .handler = script_post_handler,     .user_ctx = NULL },
    { .uri = "/api/exec_log",.method = HTTP_GET,    .handler = exec_log_get_handler,    .user_ctx = NULL },
};

#define URI_COUNT (sizeof(s_uris) / sizeof(s_uris[0]))

// ====================================================================
// Start SoftAP
// ====================================================================

static esp_err_t start_softap(void)
{
    // Create AP netif. Wi-Fi was started by espnow_comm_init in
    // WIFI_MODE_STA — we transition to APSTA below.
    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create AP netif");
        return ESP_FAIL;
    }

    wifi_config_t ap_config = {};
    strcpy((char*)ap_config.ap.ssid, "ESP-LEGO-Setup");
    ap_config.ap.ssid_len = (uint8_t)strlen("ESP-LEGO-Setup");
    ap_config.ap.channel         = CONFIG_SOFTAP_CHANNEL;
    ap_config.ap.max_connection  = 4;
    ap_config.ap.authmode        = WIFI_AUTH_OPEN;
    ap_config.ap.beacon_interval = 100;

    // Wi-Fi was started by espnow_comm_init in WIFI_MODE_STA.
    // Transition the running instance to APSTA — this triggers the
    // AP VIF to start, firing WIFI_EVENT_AP_START.
    // Because we created the AP netif first (above), the event
    // handler is already registered and will call esp_netif_action_start
    // → lwIP netif added + DHCP server started.
    // This approach works reliably regardless of NVS-persisted config
    // because set_mode always starts the AP VIF fresh, unlike
    // set_config which may be a no-op with unchanged config.
    esp_err_t ret = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(APSTA) failed: %d", ret);
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        return ret;
    }

    ret = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config(AP) failed: %d", ret);
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SoftAP started: ESP-LEGO-Setup (channel %d)",
             CONFIG_SOFTAP_CHANNEL);

    // SoftAP start can change the primary channel; re-sync ESP-NOW and
    // prompt already-running sensors to announce immediately.
    espnow_comm_sync_rf();
    espnow_comm_send_discovery();

    return ESP_OK;
}

// ====================================================================
// Start HTTP server
// ====================================================================

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port    = 80;
    config.stack_size     = CONFIG_HTTP_SERVER_STACK_SIZE;
    config.max_uri_handlers = 16;
    config.max_open_sockets = 10;
    config.lru_purge_enable = true;

    esp_err_t ret = httpd_start(&s_server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %d", ret);
        return ret;
    }

    // Register all URI handlers (specific routes first)
    for (int i = 0; i < (int)URI_COUNT; i++) {
        ret = httpd_register_uri_handler(s_server, &s_uris[i]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI %s: %d",
                     s_uris[i].uri, ret);
        }
    }

    // Register 404 error handler for captive portal
    {
        ret = httpd_register_err_handler(s_server, HTTPD_404_NOT_FOUND,
                                          catchall_404_handler);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register 404 handler: %d", ret);
        } else {
            ESP_LOGI(TAG, "Captive portal 404 → HTML handler registered");
        }
    }

    // Register 405 error handler (wrong method on probe URLs)
    {
        ret = httpd_register_err_handler(s_server,
                                          HTTPD_405_METHOD_NOT_ALLOWED,
                                          catchall_405_handler);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register 405 handler: %d", ret);
        }
    }

    ESP_LOGI(TAG, "HTTP server running on 192.168.4.1:80");
    return ESP_OK;
}

// ====================================================================
// Public API
// ====================================================================

esp_err_t web_console_init(void)
{
#if !CONFIG_WEB_CONSOLE_ENABLED
    ESP_LOGI(TAG, "Web console disabled via Kconfig");
    return ESP_OK;
#endif

    if (s_initialised) {
        ESP_LOGW(TAG, "Web console already initialised");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initialising web console...");

    // Suppress noisy 404/405 WARNINGs from httpd_uri (normal during
    // captive portal operation — OS/app probes hit unknown URIs).
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);

    // ---- 1. Initialise script inject subsystem ----
    script_inject_init();

    // Register print callback so interpreter's print() writes to ring buffer
    g_print_callback = script_inject_write_print;

    // ---- 2. Create inactivity timer ----
    if (CONFIG_WEB_CONSOLE_TIMEOUT_SEC > 0) {
        s_inactivity_timer = xTimerCreate("wc_inactive",
            pdMS_TO_TICKS(CONFIG_WEB_CONSOLE_TIMEOUT_SEC * 1000),
            pdFALSE,  // one-shot
            NULL,
            inactivity_timer_cb);
        if (s_inactivity_timer == NULL) {
            ESP_LOGW(TAG, "Failed to create inactivity timer");
        }
    }

    // ---- 3. Start SoftAP ----
    esp_err_t ret = start_softap();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP failed — web console unavailable");
        // Fall back to UART-only mode
        return ESP_OK;  // Not a fatal error
    }

    // ---- 4. Start captive portal DNS server ----
    dns_server_start();

    // ---- 5. Start HTTP server ----
    ret = start_http_server();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server failed — web console unavailable");
        // Fall back: SoftAP still runs, but no server
        return ESP_OK;
    }

    // Reset inactivity timer
    if (s_inactivity_timer && CONFIG_WEB_CONSOLE_TIMEOUT_SEC > 0) {
        xTimerReset(s_inactivity_timer, 0);
    }

    s_initialised = true;
    ESP_LOGI(TAG, "Web console ready at http://192.168.4.1");
    return ESP_OK;
}

void web_console_deinit(void)
{
    if (!s_initialised) return;

    // Clear print callback
    g_print_callback = NULL;

    if (s_inactivity_timer) {
        xTimerDelete(s_inactivity_timer, 0);
        s_inactivity_timer = NULL;
    }

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    s_initialised = false;
    ESP_LOGI(TAG, "Web console deinitialised");
}
