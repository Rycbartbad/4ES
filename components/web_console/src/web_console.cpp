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
#include "web_console/script_normalizer.h"

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
#include "hw_drivers/drivers.h"

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
"<html lang=\"zh-CN\">"
"<head>"
"<meta charset=\"UTF-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no\">"
"<meta http-equiv=\"Cache-Control\" content=\"no-cache, no-store, must-revalidate\">"
"<meta http-equiv=\"Pragma\" content=\"no-cache\">"
"<meta http-equiv=\"Expires\" content=\"0\">"
"<title>ESP-LEGO｜一句话驱动硬件</title>"
"<style>"
":root{"
"--bg:#07090d;--surface:#0d1118;--surface-2:#111722;--line:#222c39;"
"--cyan:#72f2d0;--blue:#6aa7ff;--violet:#a48aff;--success:#72f2d0;"
"--warning:#f6bd67;--error:#ff7d76;--text:#f3f4f1;--muted:#929ba8;--log:#080b10"
"}"
"*{box-sizing:border-box;margin:0;padding:0}"
"html{background:var(--bg);scroll-behavior:smooth}"
"body{"
"font-family:-apple-system,BlinkMacSystemFont,'Segoe UI','Helvetica Neue',sans-serif;"
"background:radial-gradient(circle at 50% -15%,rgba(83,123,180,.2),transparent 38%),var(--bg);"
"color:var(--text);padding:0 14px 30px;max-width:980px;margin:0 auto;"
"-webkit-font-smoothing:antialiased;font-size:15px;line-height:1.5;min-height:100vh;overflow-x:hidden"
"}"
"body:before{content:'';position:fixed;inset:0;pointer-events:none;opacity:.22;"
"background-image:linear-gradient(rgba(114,242,208,.055) 1px,transparent 1px),linear-gradient(90deg,rgba(114,242,208,.055) 1px,transparent 1px);"
"background-size:42px 42px;mask-image:linear-gradient(to bottom,#000,transparent 72%)}"
"h2{font-size:12px;font-weight:750;color:var(--cyan);letter-spacing:1.1px;text-transform:uppercase;margin-bottom:14px}"
".card{"
"position:relative;background:linear-gradient(145deg,rgba(17,23,34,.96),rgba(11,15,22,.96));"
"border:1px solid var(--line);border-radius:20px;padding:18px;margin-bottom:14px;overflow:hidden"
"}"
".topbar{position:relative;z-index:10;min-height:68px;display:flex;align-items:center;justify-content:space-between;gap:10px}"
".logo{display:flex;align-items:center;gap:10px}"
".logo-icon{"
"width:31px;height:31px;border:1px solid rgba(114,242,208,.55);border-radius:50%;"
"display:grid;place-items:center;color:var(--cyan);box-shadow:inset 0 0 14px rgba(114,242,208,.15),0 0 18px rgba(114,242,208,.08)"
"}"
".logo-icon:after{content:'';width:8px;height:8px;border-radius:50%;background:var(--cyan);box-shadow:0 0 10px var(--cyan)}"
".logo-text{font-size:14px;font-weight:760;letter-spacing:.4px}.ver{color:var(--muted);font-weight:450;font-size:10px;margin-left:3px}"
".top-actions{display:flex;align-items:center;gap:7px}"
".status-badge{"
"display:none;font-size:10px;padding:5px 9px;border-radius:999px;font-weight:650;white-space:nowrap"
"}"
".status-ok{background:rgba(102,187,106,0.15);color:var(--success)}"
".status-warn{background:rgba(246,189,103,.13);color:var(--warning)}"
".status-err{background:rgba(255,125,118,.13);color:var(--error)}"
"#statusBar{"
"position:fixed;z-index:30;left:50%;top:70px;transform:translate(-50%,-8px);"
"width:min(90vw,520px);font-size:12px;padding:0 14px;border-radius:999px;max-height:0;overflow:hidden;"
"opacity:0;transition:.24s;min-height:0;font-weight:650;text-align:center;backdrop-filter:blur(12px)"
"}"
"#statusBar.status-ok,#statusBar.status-warn,#statusBar.status-err{max-height:80px;padding:9px 14px;opacity:1;transform:translate(-50%,0);border:1px solid currentColor}"
"#statusBar.status-ok{background:rgba(14,42,38,.94)}#statusBar.status-warn{background:rgba(47,36,19,.94)}#statusBar.status-err{background:rgba(49,22,25,.94)}"
"label{display:block;font-size:12px;color:var(--muted);margin:11px 0 5px;font-weight:600}"
"input,textarea,select{"
"width:100%;padding:11px 13px;background:#090d13;border:1px solid #293443;"
"border-radius:11px;font-size:14px;color:var(--text);outline:none;transition:.2s;font-family:inherit"
"}"
"input:focus,textarea:focus,select:focus{border-color:var(--cyan);box-shadow:0 0 0 3px rgba(114,242,208,.08)}"
"textarea{resize:vertical;line-height:1.45}.code-input{font-family:ui-monospace,'Cascadia Code',monospace}"
".input-row{display:flex;gap:8px}"
".input-row input{flex:1}"
"button{"
"padding:9px 15px;border:0;border-radius:10px;cursor:pointer;font-size:13px;font-weight:700;transition:.18s;font-family:inherit;"
"display:inline-flex;align-items:center;justify-content:center;gap:6px"
"}"
"button:hover{filter:brightness(1.08)}button:active{transform:scale(.975)}"
"button:focus-visible,input:focus-visible,textarea:focus-visible,select:focus-visible,summary:focus-visible{outline:2px solid var(--cyan);outline-offset:3px}"
"button:disabled{cursor:not-allowed;opacity:.55}"
".btn-primary,.btn-success{background:linear-gradient(115deg,var(--cyan),#91e6ff);color:#05110f}"
".btn-secondary{background:#17202c;color:var(--text);border:1px solid #2b3746}"
".btn-danger{background:rgba(255,125,118,.15);color:#ffaaa5;border:1px solid rgba(255,125,118,.35)}"
".btn-icon{background:transparent;border:1px solid #2b3746;color:var(--muted);padding:8px 11px;font-size:11px}"
".btn-row{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}"
".btn-wide{width:100%;padding:11px 16px}"
".top-btn{padding:7px 10px;font-size:11px;background:rgba(17,23,34,.78);color:var(--muted);border:1px solid var(--line)}"
".hero{min-height:560px;padding:24px 18px 18px;text-align:center;border-color:rgba(114,242,208,.22);isolation:isolate}"
".hero:before,.hero:after{content:'';position:absolute;z-index:-1;border-radius:50%;filter:blur(45px);pointer-events:none}"
".hero:before{width:260px;height:220px;background:rgba(76,233,205,.13);top:70px;left:calc(50% - 190px);animation:aurora 9s ease-in-out infinite alternate}"
".hero:after{width:250px;height:240px;background:rgba(146,112,255,.15);top:90px;right:calc(50% - 200px);animation:aurora 11s ease-in-out infinite alternate-reverse}"
".eyebrow{font-size:10px;color:var(--cyan);font-weight:800;letter-spacing:1.8px;text-transform:uppercase}"
".hero h1{font-size:31px;line-height:1.08;margin:9px auto 10px;letter-spacing:-1.2px;max-width:620px}"
".hero-copy{color:var(--muted);font-size:13px;max-width:550px;margin:0 auto}"
".nova-stage{height:205px;display:grid;place-items:center;position:relative;margin:2px 0 -2px}"
".nova-shell{position:relative;width:160px;height:160px;border-radius:50%;background:transparent;padding:0;filter:none!important}"
".nova-shell:focus-visible{outline-offset:8px}"
".nova-orbit{position:absolute;inset:15px;border:1px solid rgba(114,242,208,.38);border-radius:50%;animation:orbit 12s linear infinite}"
".nova-orbit:before,.nova-orbit:after{content:'';position:absolute;width:7px;height:7px;border-radius:50%;background:var(--cyan);box-shadow:0 0 12px var(--cyan)}"
".nova-orbit:before{top:8px;left:23px}.nova-orbit:after{right:1px;bottom:42px;width:4px;height:4px;background:var(--violet)}"
".nova-orbit.two{inset:31px;border-color:rgba(164,138,255,.35);animation-direction:reverse;animation-duration:8s}"
".nova-core{position:absolute;inset:45px;border-radius:46% 54% 52% 48%;background:radial-gradient(circle at 38% 32%,#fff 0 4%,#bdfdf0 12%,#56d9c1 38%,#315e72 68%,#16212d 100%);"
"box-shadow:0 0 18px rgba(114,242,208,.65),0 0 55px rgba(114,242,208,.22);animation:float 5s ease-in-out infinite,breathe 3.8s ease-in-out infinite}"
".nova-eye{position:absolute;width:11px;height:16px;left:50%;top:50%;transform:translate(-50%,-50%);border-radius:50%;background:#f7ffff;box-shadow:0 0 12px #fff}"
".nova-pulse{position:absolute;inset:37px;border:1px solid rgba(114,242,208,.3);border-radius:50%;opacity:0}"
".nova-shell.searching .nova-orbit{animation-duration:3.2s}.nova-shell.searching .nova-pulse{animation:signal 2.2s ease-out infinite}"
".nova-shell.setup .nova-core,.nova-shell.warning .nova-core{filter:hue-rotate(295deg);box-shadow:0 0 24px rgba(246,189,103,.5)}"
".nova-shell.thinking .nova-core{filter:hue-rotate(50deg);box-shadow:0 0 28px rgba(164,138,255,.75),0 0 70px rgba(164,138,255,.28)}"
".nova-shell.thinking .nova-orbit{animation-duration:1.8s;border-color:rgba(164,138,255,.7)}"
".nova-shell.running .nova-orbit{animation-duration:3s}.nova-shell.listening{transform:scale(1.03)}"
".nova-shell.listening .nova-pulse{animation:listen 1s ease-out infinite}"
".nova-shell.success .nova-pulse{animation:signal 1.1s ease-out 2}.nova-shell.success .nova-core{box-shadow:0 0 35px rgba(114,242,208,.9),0 0 90px rgba(114,242,208,.35)}"
".nova-shell.error .nova-core,.nova-shell.offline .nova-core{filter:hue-rotate(125deg) saturate(.7);animation:none;opacity:.65}"
".nova-shell.error .nova-orbit,.nova-shell.offline .nova-orbit{animation-play-state:paused;border-color:rgba(255,125,118,.45)}"
".nova-copy{min-height:72px}.nova-name{font-size:10px;letter-spacing:1.8px;color:var(--muted);text-transform:uppercase}"
".nova-copy h3{font-size:17px;margin:3px 0}.nova-copy p{font-size:12px;color:var(--muted);min-height:18px}"
".nova-action{margin-top:8px;padding:6px 11px;border-radius:999px;background:transparent;color:var(--cyan);border:1px solid rgba(114,242,208,.28);font-size:11px}"
".composer{max-width:720px;margin:13px auto 0;text-align:left;padding:8px;background:rgba(7,10,15,.78);border:1px solid #2a3847;border-radius:18px;box-shadow:0 22px 55px rgba(0,0,0,.28)}"
".composer:focus-within{border-color:rgba(114,242,208,.7);box-shadow:0 0 0 3px rgba(114,242,208,.07),0 22px 55px rgba(0,0,0,.32)}"
".composer textarea{border:0;background:transparent;min-height:76px;resize:none;box-shadow:none!important;font-family:inherit;font-size:15px}"
".composer-bottom{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:4px}"
".composer-bottom .quick-actions{min-width:0;flex:1;margin:0}.composer-submit{white-space:nowrap;border-radius:12px;padding:10px 15px}"
".ready-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}"
".ready-item{background:#090d13;border:1px solid var(--line);border-radius:13px;padding:11px 8px;text-align:center;font-size:10px;color:var(--muted)}"
".ready-item b{display:block;color:var(--text);font-size:13px;margin-top:3px}"
".ready-dot{display:inline-block;width:6px;height:6px;border-radius:50%;background:var(--warning);margin-right:4px}"
".ready-item.ok .ready-dot{background:var(--success);box-shadow:0 0 8px rgba(114,242,208,.65)}"
".quick-actions{display:flex;gap:7px;overflow-x:auto;padding:2px 0 5px;margin:9px 0}"
".chip{white-space:nowrap;cursor:pointer;color:var(--muted);font-family:inherit;border:1px solid var(--line);background:#111722;border-radius:999px;padding:6px 10px;font-size:10px}"
".chip:hover{border-color:var(--cyan);color:var(--cyan)}"
".two-col{display:grid;grid-template-columns:1fr;gap:12px}"
".section-head{display:flex;justify-content:space-between;align-items:center;gap:10px;margin-bottom:4px}.section-head h2{margin:0}"
".device-row{display:flex;align-items:flex-start;justify-content:space-between;gap:10px;padding:11px 0;border-bottom:1px solid var(--line)}"
".device-row:last-child{border-bottom:0}"
".device-name{font-weight:700;font-size:13px}.device-cap{font-size:11px;color:var(--muted);margin-top:2px}"
".device-state{font-size:10px;padding:3px 7px;border-radius:999px;background:rgba(114,242,208,.1);color:var(--success)}"
".device-state.offline{background:rgba(146,155,168,.1);color:var(--muted)}"
"details.panel{margin-bottom:14px;border:1px solid var(--line);border-radius:20px;background:rgba(13,17,24,.9);overflow:hidden}"
"details.panel>summary{cursor:pointer;padding:16px 18px;font-weight:750;list-style:none}"
"details.panel>summary::-webkit-details-marker{display:none}"
"details.panel>summary:after{content:'+';float:right;color:var(--cyan)}"
"details.panel[open]>summary:after{content:'−'}"
".panel-body{padding:0 12px 12px}.lab-grid{display:grid;grid-template-columns:1fr;gap:12px}.lab-grid .card{margin:0}"
".footer{text-align:center;color:var(--muted);font-size:10px;padding:10px 0 20px}"
".btn-loading{pointer-events:none;opacity:0.7}"
".btn-loading::before{"
"content:'';display:inline-block;width:14px;height:14px;"
"border:2px solid rgba(255,255,255,0.3);border-top-color:#fff;"
"border-radius:50%;animation:spin .6s linear infinite;vertical-align:middle;margin-right:6px"
"}"
".scan-list{margin-top:10px;border-top:1px solid var(--line);padding-top:10px}"
".scan-item{"
"display:flex;justify-content:space-between;align-items:center;"
"padding:10px 12px;border-radius:9px;cursor:pointer;"
"transition:background .15s;margin-bottom:4px;border:1px solid transparent"
"}"
".scan-item:hover{background:rgba(114,242,208,.06);border-color:var(--cyan)}"
".scan-loading,.scan-empty{"
"justify-content:center;color:var(--muted);cursor:default;font-size:13px"
"}"
".scan-loading:hover,.scan-empty:hover{background:transparent;border-color:transparent}"
".scan-name{font-size:14px;font-weight:500;color:var(--text)}"
".scan-rssi{font-size:12px;font-weight:500}"
".sig-strong{color:var(--success)}"
".sig-medium{color:var(--warning)}"
".sig-weak{color:var(--error)}"
"#deviceList{font-size:13px;line-height:1.8}#deviceList .info{color:var(--muted)}"
".mic-visual{display:flex;align-items:center;gap:8px;margin:9px 0}.mic-bars{display:flex;align-items:center;gap:3px;height:30px}"
".mic-bars i{display:block;width:3px;height:8px;border-radius:2px;background:var(--cyan);opacity:.45}.mic-bars i:nth-child(2){height:16px}.mic-bars i:nth-child(3){height:24px}.mic-bars i:nth-child(4){height:14px}.mic-bars i:nth-child(5){height:7px}"
".mic-active .mic-bars i{animation:micBars .65s ease-in-out infinite alternate}.mic-active .mic-bars i:nth-child(2){animation-delay:.12s}.mic-active .mic-bars i:nth-child(4){animation-delay:.25s}"
".mic-meter{height:9px;background:#080c12;border:1px solid var(--line);border-radius:6px;overflow:hidden;margin:10px 0 8px}"
"#micMeter{height:100%;width:0;background:linear-gradient(90deg,var(--success),var(--warning),var(--error));transition:width .12s}"
"#micLevel{font-family:ui-monospace,'Cascadia Code',monospace;font-size:12px}"
".log-box{"
"background:var(--log);color:#a8d8a8;font-family:ui-monospace,'Cascadia Code',monospace;"
"font-size:12px;padding:12px;border-radius:8px;"
"height:200px;overflow-y:auto;white-space:pre-wrap;word-break:break-all;"
"line-height:1.5;border:1px solid var(--line)"
"}"
".info{font-size:11px;color:var(--muted);margin-top:8px;line-height:1.45}"
"#aiResult{font-size:13px;margin-top:8px;padding:8px 10px;border-radius:6px}"
"::-webkit-scrollbar{width:5px}"
"::-webkit-scrollbar-track{background:transparent}"
"::-webkit-scrollbar-thumb{background:var(--line);border-radius:3px}"
"@keyframes spin{to{transform:rotate(360deg)}}"
"@keyframes orbit{to{transform:rotate(360deg)}}"
"@keyframes float{50%{transform:translateY(-5px) rotate(3deg)}}"
"@keyframes breathe{50%{opacity:.82;scale:.96}}"
"@keyframes signal{0%{transform:scale(.75);opacity:.8}100%{transform:scale(1.75);opacity:0}}"
"@keyframes listen{0%{transform:scale(.85);opacity:.7}100%{transform:scale(1.35);opacity:0}}"
"@keyframes aurora{to{transform:translate(30px,20px) scale(1.12)}}"
"@keyframes micBars{to{transform:scaleY(1.65);opacity:1}}"
"@media(min-width:520px){.status-badge{display:inline-flex}.hero{padding-left:30px;padding-right:30px}.hero h1{font-size:40px}}"
"@media(min-width:720px){.two-col,.lab-grid{grid-template-columns:1fr 1fr}.hero{min-height:570px}.nova-stage{height:215px}.composer-bottom{gap:14px}}"
"@media(max-width:560px){.composer-bottom{align-items:stretch;flex-direction:column}.composer-submit{width:100%}.hero{min-height:635px}.ready-item b{font-size:11px}.top-btn .wide-label{display:none}}"
"@media(prefers-reduced-motion:reduce){html{scroll-behavior:auto}*,*:before,*:after{animation-duration:.001ms!important;animation-iteration-count:1!important;transition-duration:.001ms!important}.nova-core{transform:none!important}}"
"</style>"
"</head>"
"<body>"
"<header class=\"topbar\">"
"<div class=\"logo\">"
"<div class=\"logo-icon\" aria-hidden=\"true\"></div>"
"<div class=\"logo-text\">ESP-LEGO <span class=\"ver\">V1.0</span></div>"
"</div>"
"<div class=\"top-actions\"><span id=\"statusBadge\" class=\"status-badge status-warn\" data-i18n=\"status.connecting\">正在连接</span><button class=\"top-btn\" onclick=\"toggleLanguage()\" id=\"langBtn\" aria-label=\"切换语言\">EN</button><button class=\"top-btn\" onclick=\"openLab()\" data-i18n-aria=\"nav.lab\" aria-label=\"实验室\"><span aria-hidden=\"true\">◇</span><span class=\"wide-label\" data-i18n=\"nav.lab\">实验室</span></button></div>"
"</header>"
"<div id=\"statusBar\" role=\"status\" aria-live=\"polite\"></div>"
"<main>"
"<section class=\"card hero\">"
"<div class=\"eyebrow\" data-i18n=\"hero.eyebrow\">AI × 模块化硬件</div><h1 data-i18n=\"hero.title\">把想法变成真实动作</h1><p class=\"hero-copy\" data-i18n=\"hero.subtitle\">连接模块，描述目标。NOVA 会把自然语言转化为可运行的硬件逻辑。</p>"
"<div class=\"nova-stage\"><button id=\"nova\" class=\"nova-shell searching\" onclick=\"novaAction()\" data-i18n-aria=\"nova.aria\" aria-label=\"打开 NOVA 状态\"><span class=\"nova-orbit\"></span><span class=\"nova-orbit two\"></span><span class=\"nova-pulse\"></span><span class=\"nova-core\"><span class=\"nova-eye\"></span></span></button></div>"
"<div class=\"nova-copy\" aria-live=\"polite\"><div class=\"nova-name\">NOVA · <span data-i18n=\"nova.role\">星灵伙伴</span></div><h3 id=\"novaTitle\">正在唤醒系统</h3><p id=\"novaDetail\">我正在寻找附近的模块...</p><button id=\"novaActionBtn\" class=\"nova-action\" onclick=\"novaAction()\">查看状态</button></div>"
"<div class=\"composer\"><textarea id=\"aiPrompt\" rows=\"3\" data-i18n-placeholder=\"composer.placeholder\" placeholder=\"例如：每 5 秒读取温度，超过 30 度就启动风扇\"></textarea><div class=\"composer-bottom\"><div class=\"quick-actions\"><button class=\"chip\" data-example=\"example.temp.prompt\" data-i18n=\"example.temp\" onclick=\"useExampleKey(this.dataset.example)\">温度监测</button><button class=\"chip\" data-example=\"example.door.prompt\" data-i18n=\"example.door\" onclick=\"useExampleKey(this.dataset.example)\">智能门铃</button><button class=\"chip\" data-example=\"example.servo.prompt\" data-i18n=\"example.servo\" onclick=\"useExampleKey(this.dataset.example)\">舵机控制</button><button class=\"chip\" data-example=\"example.sound.prompt\" data-i18n=\"example.sound\" onclick=\"useExampleKey(this.dataset.example)\">声音触发</button></div><button class=\"btn-success composer-submit\" onclick=\"callAI()\" id=\"aiBtn\" data-i18n=\"composer.run\">生成并运行</button></div><div id=\"aiResult\" class=\"info\" aria-live=\"polite\"></div></div>"
"</section>"
"<section class=\"card\"><div class=\"section-head\"><h2 data-i18n=\"ready.title\">系统状态</h2><button class=\"btn-icon\" onclick=\"refreshStatus()\" data-i18n=\"common.refresh\">刷新</button></div><div class=\"ready-grid\"><div id=\"deviceReady\" class=\"ready-item\"><span class=\"ready-dot\"></span><span data-i18n=\"ready.modules\">模块</span><b data-i18n=\"common.checking\">检测中</b></div><div id=\"aiReady\" class=\"ready-item\"><span class=\"ready-dot\"></span><span data-i18n=\"ready.ai\">AI 服务</span><b data-i18n=\"common.checking\">检测中</b></div><div id=\"runReady\" class=\"ready-item ok\"><span class=\"ready-dot\"></span><span data-i18n=\"ready.engine\">运行引擎</span><b data-i18n=\"common.standby\">待命</b></div></div></section>"
"<div class=\"two-col\"><section class=\"card\"><div class=\"section-head\"><h2 data-i18n=\"modules.title\">模块轨道</h2></div><div id=\"deviceList\"><span class=\"info\" data-i18n=\"modules.discovering\">正在发现模块...</span></div></section>"
"<section id=\"micCard\" class=\"card\"><h2 data-i18n=\"mic.title\">声音感知</h2><div class=\"mic-visual\"><div class=\"mic-bars\" aria-hidden=\"true\"><i></i><i></i><i></i><i></i><i></i></div><div id=\"micLevel\" data-i18n=\"mic.notTested\">尚未检测</div></div><div class=\"mic-meter\"><div id=\"micMeter\"></div></div><button id=\"micBtn\" class=\"btn-secondary\" onclick=\"testMic()\" data-i18n=\"mic.start\">开始 5 秒检测</button><div class=\"info\" data-i18n=\"mic.privacy\">用于拍手、敲击和噪声触发；只感知音量，不录音、不上传。</div></section></div>"
"<details id=\"labPanel\" class=\"panel\"><summary><span data-i18n=\"lab.title\">NOVA 实验室</span> <span class=\"info\" data-i18n=\"lab.subtitle\">连接、AI 与工程工具</span></summary><div class=\"panel-body\"><div class=\"lab-grid\">"
"<section id=\"wifiSetup\" class=\"card\"><h2 data-i18n=\"wifi.title\">网络接入</h2><label data-i18n=\"wifi.ssid\">2.4 GHz Wi-Fi 名称</label><input id=\"wifi_ssid\" data-i18n-placeholder=\"wifi.ssidPlaceholder\" placeholder=\"输入 Wi-Fi SSID\"><label data-i18n=\"wifi.password\">Wi-Fi 密码</label><input id=\"wifi_pass\" type=\"password\" data-i18n-placeholder=\"wifi.passwordPlaceholder\" placeholder=\"输入密码\"><div class=\"btn-row\"><button class=\"btn-primary\" onclick=\"saveWifiConfig()\" data-i18n=\"wifi.save\">保存网络</button><button class=\"btn-secondary\" onclick=\"scanWifi()\" id=\"scanBtn\" data-i18n=\"wifi.scan\">扫描附近网络</button></div><div id=\"scanResults\" class=\"scan-list\" style=\"display:none\"></div><div class=\"info\" data-i18n=\"wifi.help\">仅在选择 ESP32 联网生成时使用；只支持 2.4 GHz 网络。</div></section>"
"<section id=\"aiSetup\" class=\"card\"><h2 data-i18n=\"ai.title\">AI 能力</h2><label data-i18n=\"ai.url\">兼容 OpenAI 的 API 地址</label><input id=\"llm_url\" placeholder=\"https://api.openai.com/v1\"><label>API Key</label><div class=\"input-row\"><input id=\"llm_key\" type=\"password\" placeholder=\"sk-...\"><button class=\"btn-icon\" onclick=\"toggleKey()\" id=\"keyToggle\" type=\"button\" data-i18n=\"common.show\">显示</button></div><label data-i18n=\"ai.model\">模型</label><input id=\"llm_model\" placeholder=\"gpt-4o-mini\"><label data-i18n=\"ai.route\">生成方式</label><select id=\"llm_route\"><option value=\"phone\" data-i18n=\"ai.phone\">手机移动数据（推荐）</option><option value=\"device\" data-i18n=\"ai.device\">ESP32 连接路由器</option></select><div class=\"btn-row\"><button class=\"btn-primary\" onclick=\"saveLlmConfig()\" data-i18n=\"ai.save\">保存 AI 设置</button></div><div class=\"info\" data-i18n=\"ai.help\">手机模式：保持页面打开，关闭 Wi-Fi 后用移动数据生成，再重新连接主控热点运行。</div></section>"
"<section class=\"card\"><h2 data-i18n=\"dev.scriptTitle\">直接注入脚本</h2><textarea class=\"code-input\" id=\"scriptInput\" rows=\"4\" data-i18n-placeholder=\"dev.scriptPlaceholder\" placeholder=\"例如：while(true){print(remote_read(1));sleep(2000)}\"></textarea><button class=\"btn-danger\" onclick=\"injectScript()\" data-i18n=\"dev.run\">运行此脚本</button></section>"
"<section class=\"card\"><h2 data-i18n=\"dev.logTitle\">运行日志</h2><div id=\"execLog\" class=\"log-box\" data-i18n=\"dev.waiting\">等待输出...</div><div class=\"btn-row\"><button class=\"btn-secondary\" onclick=\"fetchLog()\" data-i18n=\"common.refresh\">刷新</button><button class=\"btn-secondary\" onclick=\"clearLog()\" data-i18n=\"common.clear\">清空</button></div></section>"
"</div></div></details>"
"</main><div class=\"footer\" data-i18n=\"footer\">ESP-LEGO · 把编程门槛留给系统，把创造力还给用户</div>"
""
"<div id=\"jstest\" style=\"font-size:0;height:1px\"></div>"

"<script>"
"try{document.getElementById('jstest').textContent='JS OK';}catch(e){}"
"const BASE='';"
"var phoneContext=null;"
"var pendingPhoneScript='';"
"var phonePhase='idle';"
"var currentLang='zh-CN';"
"try{if(localStorage.getItem('espUiLanguage')==='en')currentLang='en'}catch(e){}"
"var I18N={"
"'zh-CN':{"
"'page.title':'ESP-LEGO｜把想法变成真实动作','status.connecting':'正在连接','nav.lab':'实验室','language.switch':'切换为英文',"
"'hero.eyebrow':'AI × 模块化硬件','hero.title':'把想法变成真实动作','hero.subtitle':'连接模块，描述目标。NOVA 会把自然语言转化为可运行的硬件逻辑。',"
"'nova.aria':'打开 NOVA 状态','nova.role':'星灵伙伴','nova.waking.title':'正在唤醒系统','nova.waking.detail':'我正在寻找附近的模块...',"
"'nova.searching.title':'正在寻找模块','nova.searching.detail':'给模块供电，我会自动发现它们。','nova.setup.title':'还差一步：接入 AI 能力','nova.setup.detail':'打开实验室，保存 API 地址、Key 和模型。',"
"'nova.ready.title':'一切就绪','nova.ready.detail':'告诉我你想创造什么。','nova.running.title':'想法正在行动','nova.running.detail':'自动化脚本正在主控上运行。',"
"'nova.thinking.title':'正在理解你的想法','nova.thinking.detail':'我正在把自然语言变成硬件规则。','nova.listening.title':'正在感知声音','nova.listening.detail':'只读取音量，不录音、不上传。',"
"'nova.success.title':'完成了','nova.success.detail':'模块已经开始行动。','nova.warning.title':'有一项需要处理','nova.error.title':'遇到一点问题','nova.offline.title':'与主控失去连接','nova.offline.detail':'确认仍连接 ESP-LEGO-Setup，然后重试。',"
"'nova.action.status':'查看状态','nova.action.setup':'接入 AI','nova.action.refresh':'重新连接','nova.action.create':'描述想法','nova.action.lab':'打开实验室',"
"'composer.placeholder':'例如：每 5 秒读取温度，超过 30 度就启动风扇','composer.run':'生成并运行','composer.empty':'请先描述你想让硬件做什么',"
"'example.temp':'温度监测','example.temp.prompt':'每 5 秒读取一次温度并打印','example.door':'智能门铃','example.door.prompt':'有人按门铃时让蜂鸣器响两次','example.servo':'舵机控制','example.servo.prompt':'把舵机转到 90 度','example.sound':'声音触发','example.sound.prompt':'声音变大时打印提醒',"
"'ready.title':'系统状态','ready.modules':'模块','ready.ai':'AI 服务','ready.engine':'运行引擎','ready.devices':'{online} 在线 / {known} 已知','ready.configured':'已配置','ready.needsSetup':'待配置','ready.running':'正在运行',"
"'common.checking':'检测中','common.standby':'待命','common.refresh':'刷新','common.clear':'清空','common.show':'显示','common.hide':'隐藏','common.online':'在线','common.offline':'离线','common.unknown':'未知错误',"
"'modules.title':'模块轨道','modules.discovering':'正在发现模块...','modules.empty':'尚未发现模块，请确认从机已供电','modules.unnamed':'未命名模块',"
"'mic.title':'声音感知','mic.notTested':'尚未检测','mic.start':'开始 5 秒检测','mic.listening':'正在聆听...','mic.level':'{level}% 满量程','mic.peak':'峰值 {level}% 满量程','mic.privacy':'用于拍手、敲击和噪声触发；只感知音量，不录音、不上传。','mic.failed':'读取失败：{error}','mic.done':'声音检测完成；若电平不变，请检查供电、共地、L/R 与 I2S 接线。','mic.error':'声音检测失败，请查看串口中的 I2S 错误。',"
"'lab.title':'NOVA 实验室','lab.subtitle':'连接、AI 与工程工具','wifi.title':'网络接入','wifi.ssid':'2.4 GHz Wi-Fi 名称','wifi.ssidPlaceholder':'输入 Wi-Fi SSID','wifi.password':'Wi-Fi 密码','wifi.passwordPlaceholder':'输入密码','wifi.save':'保存网络','wifi.scan':'扫描附近网络','wifi.help':'仅在选择 ESP32 联网生成时使用；只支持 2.4 GHz 网络。','wifi.saved':'网络设置已保存','wifi.savedStatus':'网络已保存：{status}','wifi.scanning':'正在扫描附近网络...','wifi.scanFailed':'扫描失败','wifi.none':'未发现网络','wifi.none24':'没有发现 2.4 GHz Wi-Fi','wifi.found':'发现 {count} 个网络','wifi.selected':'已选择：{ssid}',"
"'ai.title':'AI 能力','ai.url':'兼容 OpenAI 的 API 地址','ai.model':'模型','ai.route':'生成方式','ai.phone':'手机移动数据（推荐）','ai.device':'ESP32 连接路由器','ai.save':'保存 AI 设置','ai.saved':'AI 设置已保存','ai.help':'手机模式：保持页面打开，关闭 Wi-Fi 后用移动数据生成，再重新连接主控热点运行。','ai.needConfig':'AI 尚未配置，请先保存 API 地址和 Key','ai.needWifi':'尚未配置路由器 Wi-Fi，请先保存 SSID 和密码','ai.preparingPhone':'正在准备手机移动数据流程...','ai.connecting':'ESP32 正在连接 AI 服务...','ai.failed':'AI 服务调用失败','ai.ok':'脚本已提交，正在校验并运行','ai.error':'错误：{error}',"
"'phone.key':'手机模式需要重新输入 API Key','phone.retryMessage':'尚未连接到主控。请重新连接 ESP-LEGO-Setup 后再点击。','phone.retryButton':'第 3 步：重试运行','phone.context':'无法从 ESP 读取 AI 上下文','phone.https':'手机模式需要 HTTPS API 地址','phone.noScript':'AI 响应中没有脚本','phone.emptyScript':'AI 返回了空脚本','phone.invalidScript':'AI 返回的脚本格式无效，请重新生成','phone.missingScript':'AI 返回的 JSON 中没有 script 字段','phone.unsafeLoop':'不允许无限循环，请改用带计数器的有限循环','phone.generated':'脚本已生成。请重新打开 Wi-Fi、连接 ESP-LEGO-Setup，然后再次点击运行。','phone.runButton':'第 3 步：重连后运行','phone.mobile':'手机仍在使用 ESP 热点。请保持页面打开，关闭 Wi-Fi 并保留移动数据，然后再次点击。','phone.mobileButton':'第 2 步：使用移动数据生成','phone.restored':'已恢复生成的脚本。请重新连接 ESP-LEGO-Setup，然后点击运行。',"
"'dev.scriptTitle':'直接注入脚本','dev.scriptPlaceholder':'例如：var i=0;while(i<10){print(remote_read(1));sleep(2000);i=i+1;}','dev.run':'运行此脚本','dev.logTitle':'运行日志','dev.waiting':'等待输出...','dev.empty':'(暂无输出)','dev.needScript':'请先输入脚本','dev.started':'脚本已提交，正在校验并运行','dev.reconnectLog':'；如未显示输出，请重新连接 ESP-LEGO-Setup',"
"'clarify.question':'请选择要使用的模块','clarify.injecting':'正在向设备 {id} 注入...','clarify.done':'脚本已在设备 {id} 上运行','footer':'ESP-LEGO · 把编程门槛留给系统，把创造力还给用户','fetch.error':'连接失败：{error}'"
"},"
"'en':{"
"'page.title':'ESP-LEGO | Turn ideas into real action','status.connecting':'Connecting','nav.lab':'Lab','language.switch':'Switch to Chinese',"
"'hero.eyebrow':'AI × MODULAR HARDWARE','hero.title':'Turn ideas into real action','hero.subtitle':'Connect modules and describe a goal. NOVA turns natural language into hardware logic that runs locally.',"
"'nova.aria':'Open NOVA status','nova.role':'SPIRIT COMPANION','nova.waking.title':'Waking the system','nova.waking.detail':'I am looking for nearby modules...',"
"'nova.searching.title':'Looking for modules','nova.searching.detail':'Power on a module and I will discover it automatically.','nova.setup.title':'One more step: connect AI','nova.setup.detail':'Open the Lab and save the API URL, key, and model.',"
"'nova.ready.title':'Everything is ready','nova.ready.detail':'Tell me what you want to create.','nova.running.title':'Your idea is in motion','nova.running.detail':'The automation script is running on the controller.',"
"'nova.thinking.title':'Understanding your idea','nova.thinking.detail':'I am turning natural language into hardware rules.','nova.listening.title':'Sensing sound','nova.listening.detail':'I only read volume. Nothing is recorded or uploaded.',"
"'nova.success.title':'Done','nova.success.detail':'Your modules have started moving.','nova.warning.title':'One item needs attention','nova.error.title':'I ran into a problem','nova.offline.title':'Controller connection lost','nova.offline.detail':'Reconnect to ESP-LEGO-Setup, then try again.',"
"'nova.action.status':'View status','nova.action.setup':'Connect AI','nova.action.refresh':'Reconnect','nova.action.create':'Describe an idea','nova.action.lab':'Open Lab',"
"'composer.placeholder':'Example: read temperature every 5 seconds and start the fan above 30°C','composer.run':'Generate & run','composer.empty':'Describe what you want the hardware to do first',"
"'example.temp':'Temperature','example.temp.prompt':'Read the temperature every 5 seconds and print it','example.door':'Smart doorbell','example.door.prompt':'Make the buzzer sound twice when the doorbell is pressed','example.servo':'Servo control','example.servo.prompt':'Turn the servo to 90 degrees','example.sound':'Sound trigger','example.sound.prompt':'Print an alert when the sound level increases',"
"'ready.title':'System status','ready.modules':'Modules','ready.ai':'AI service','ready.engine':'Runtime','ready.devices':'{online} online / {known} known','ready.configured':'Configured','ready.needsSetup':'Setup needed','ready.running':'Running',"
"'common.checking':'Checking','common.standby':'Standby','common.refresh':'Refresh','common.clear':'Clear','common.show':'Show','common.hide':'Hide','common.online':'Online','common.offline':'Offline','common.unknown':'Unknown error',"
"'modules.title':'Module orbit','modules.discovering':'Discovering modules...','modules.empty':'No modules found. Make sure a module is powered on.','modules.unnamed':'Unnamed module',"
"'mic.title':'Sound sensing','mic.notTested':'Not tested','mic.start':'Start 5-second test','mic.listening':'Listening...','mic.level':'{level}% full scale','mic.peak':'Peak {level}% full scale','mic.privacy':'For claps, knocks, and noise triggers. Volume only; no recording or upload.','mic.failed':'Read failed: {error}','mic.done':'Sound test complete. If the level never moves, check power, ground, L/R, and I2S wiring.','mic.error':'Sound test failed. Check the serial log for I2S errors.',"
"'lab.title':'NOVA Lab','lab.subtitle':'Connectivity, AI, and engineering tools','wifi.title':'Network access','wifi.ssid':'2.4 GHz Wi-Fi name','wifi.ssidPlaceholder':'Enter Wi-Fi SSID','wifi.password':'Wi-Fi password','wifi.passwordPlaceholder':'Enter password','wifi.save':'Save network','wifi.scan':'Scan nearby networks','wifi.help':'Only required for ESP32 online generation. 2.4 GHz networks only.','wifi.saved':'Network settings saved','wifi.savedStatus':'Network saved: {status}','wifi.scanning':'Scanning nearby networks...','wifi.scanFailed':'Scan failed','wifi.none':'No networks found','wifi.none24':'No 2.4 GHz Wi-Fi found','wifi.found':'Found {count} networks','wifi.selected':'Selected: {ssid}',"
"'ai.title':'AI capability','ai.url':'OpenAI-compatible API URL','ai.model':'Model','ai.route':'Generation route','ai.phone':'Phone mobile data (recommended)','ai.device':'ESP32 via router','ai.save':'Save AI settings','ai.saved':'AI settings saved','ai.help':'Phone mode: keep this page open, disable Wi-Fi to generate over mobile data, then reconnect to the controller hotspot to run.','ai.needConfig':'AI is not configured. Save the API URL and key first.','ai.needWifi':'Router Wi-Fi is not configured. Save the SSID and password first.','ai.preparingPhone':'Preparing the phone mobile-data flow...','ai.connecting':'ESP32 is connecting to the AI service...','ai.failed':'AI service call failed','ai.ok':'Script submitted for validation and execution','ai.error':'Error: {error}',"
"'phone.key':'Phone mode needs the API key again','phone.retryMessage':'The controller is not connected yet. Reconnect to ESP-LEGO-Setup and tap again.','phone.retryButton':'Step 3: retry run','phone.context':'Could not read AI context from ESP','phone.https':'Phone mode requires an HTTPS API URL','phone.noScript':'The AI response contained no script','phone.emptyScript':'The AI returned an empty script','phone.invalidScript':'The AI returned an invalid script format. Generate it again.','phone.missingScript':'The AI JSON response has no script field','phone.unsafeLoop':'Infinite loops are not allowed. Use a bounded counter loop.','phone.generated':'Script generated. Turn Wi-Fi back on, reconnect to ESP-LEGO-Setup, then tap run again.','phone.runButton':'Step 3: run after reconnecting','phone.mobile':'The phone is still using the ESP hotspot. Keep this page open, disable Wi-Fi, and leave mobile data on.','phone.mobileButton':'Step 2: generate with mobile data','phone.restored':'The generated script was restored. Reconnect to ESP-LEGO-Setup, then tap run.',"
"'dev.scriptTitle':'Direct script injection','dev.scriptPlaceholder':'Example: var i=0;while(i<10){print(remote_read(1));sleep(2000);i=i+1;}','dev.run':'Run this script','dev.logTitle':'Runtime log','dev.waiting':'Waiting for output...','dev.empty':'(No output yet)','dev.needScript':'Enter a script first','dev.started':'Script submitted for validation and execution','dev.reconnectLog':'; reconnect to ESP-LEGO-Setup if output is not visible',"
"'clarify.question':'Choose a module','clarify.injecting':'Injecting on device {id}...','clarify.done':'Script is running on device {id}','footer':'ESP-LEGO · Let the system handle complexity so you can create','fetch.error':'Connection failed: {error}'"
"}"
"};"
"function $(id){return document.getElementById(id)}"
"function t(key,vars){var s=(I18N[currentLang]&&I18N[currentLang][key])||(I18N['zh-CN'][key])||key;if(vars)Object.keys(vars).forEach(function(k){s=s.split('{'+k+'}').join(vars[k])});return s}"
"function applyLanguage(){"
" document.documentElement.lang=currentLang;document.title=t('page.title');"
" document.querySelectorAll('[data-i18n]').forEach(function(el){if(el.id!=='execLog'||el.dataset.loaded!=='1')el.textContent=t(el.getAttribute('data-i18n'))});"
" document.querySelectorAll('[data-i18n-placeholder]').forEach(function(el){el.placeholder=t(el.getAttribute('data-i18n-placeholder'))});"
" document.querySelectorAll('[data-i18n-aria]').forEach(function(el){el.setAttribute('aria-label',t(el.getAttribute('data-i18n-aria')))});"
" $('langBtn').textContent=currentLang==='zh-CN'?'EN':'中文';$('langBtn').setAttribute('aria-label',t('language.switch'));"
" $('keyToggle').textContent=t($('llm_key').type==='password'?'common.show':'common.hide');"
" if(phonePhase==='reconnect'){$('aiBtn').textContent=t('phone.runButton');$('aiResult').textContent=t('phone.restored')}else if(phonePhase==='mobile'){$('aiBtn').textContent=t('phone.mobileButton');$('aiResult').textContent=t('phone.mobile')}"
" if($('micBtn').disabled)$('micBtn').textContent=t('mic.listening');"
" renderStatus();renderNova()"
"}"
"function toggleLanguage(){currentLang=currentLang==='zh-CN'?'en':'zh-CN';try{localStorage.setItem('espUiLanguage',currentLang)}catch(e){}applyLanguage()}"
"var lastStatus=null,statusFailures=0,novaOverride=null,novaTimer=null,currentNovaState='searching';"
"function baseNovaState(){if(statusFailures>=2)return'offline';if(!lastStatus)return'searching';if(!lastStatus.llm_configured)return'setup';if(lastStatus.script_running)return'running';if(!(lastStatus.peer_count>0))return'searching';return'ready'}"
"function novaText(state){var detailKey='nova.'+state+'.detail',action='nova.action.status';if(state==='setup')action='nova.action.setup';else if(state==='offline'||state==='searching')action='nova.action.refresh';else if(state==='ready'||state==='running')action='nova.action.create';else if(state==='error'||state==='warning')action='nova.action.lab';return{title:t('nova.'+state+'.title'),detail:t(detailKey),action:t(action)}}"
"function renderNova(){"
" var state=statusFailures>=2?'offline':(novaOverride?novaOverride.state:baseNovaState());var copy=novaText(state);"
" if(novaOverride&&statusFailures<2){if(novaOverride.title)copy.title=novaOverride.title;if(novaOverride.detail)copy.detail=novaOverride.detail}"
" currentNovaState=state;$('nova').className='nova-shell '+state;$('novaTitle').textContent=copy.title;$('novaDetail').textContent=copy.detail;$('novaActionBtn').textContent=copy.action;"
" if(state==='offline'){var badge=$('statusBadge');badge.textContent=t('common.offline');badge.className='status-badge status-err';['deviceReady','aiReady','runReady'].forEach(function(id){var el=$(id);el.className='ready-item';el.querySelector('b').textContent=t('common.offline')})}"
"}"
"function setNova(state,title,detail,duration){novaOverride={state:state,title:title||'',detail:detail||''};if(novaTimer)clearTimeout(novaTimer);novaTimer=null;renderNova();if(duration){novaTimer=setTimeout(function(){novaOverride=null;renderNova()},duration)}}"
"function clearNova(){novaOverride=null;if(novaTimer)clearTimeout(novaTimer);novaTimer=null;renderNova()}"
"function novaAction(){if(currentNovaState==='setup')openLab('ai');else if(currentNovaState==='offline'||currentNovaState==='searching')refreshStatus();else if(currentNovaState==='error'||currentNovaState==='warning')openLab();else{$('aiPrompt').focus();$('aiPrompt').scrollIntoView({behavior:'smooth',block:'center'})}}"
"function msg(text,type){"
" var bar=$('statusBar');"
" bar.textContent=text;"
" bar.className='status-'+type;"
" if(type==='err')setNova('error','',text,5000);else if(type==='warn')setNova('warning','',text,5000);else if(type==='ok')setNova('success','',text,1800);"
" setTimeout(function(){bar.textContent='';bar.className=''},5000)"
"}"
"function toggleKey(){"
" var k=$('llm_key'),btn=$('keyToggle');"
" if(k.type==='password'){k.type='text';btn.textContent=t('common.hide')}"
" else{k.type='password';btn.textContent=t('common.show')}"
"}"
"function openLab(section){var panel=$('labPanel');panel.open=true;var target=section==='ai'?$('aiSetup'):section==='wifi'?$('wifiSetup'):panel;target.scrollIntoView({behavior:'smooth',block:'start'});if(section==='ai')setTimeout(function(){$('llm_url').focus()},450)}"
"function openSetup(){openLab()}"
"function useExampleKey(key){$('aiPrompt').value=t(key);$('aiPrompt').focus()}"
"async function apiFetch(url,opts,quiet){"
" try{"
"  var r=await fetch(BASE+url,opts);"
"  if(!r.ok){var detail=await r.text();if(!quiet)msg('HTTP '+r.status+': '+detail,'err');return null}"
"  return r.json()"
" }catch(e){if(!quiet)msg(t('fetch.error',{error:e.message}),'err');return null}"
"}"
"async function saveWifiConfig(){"
" var body={"
"  wifi_ssid:$('wifi_ssid').value,"
"  wifi_pass:$('wifi_pass').value"
" };"
" var r=await apiFetch('/api/config/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
" if(r&&r.wifi_status)msg(t('wifi.savedStatus',{status:r.wifi_status}),'ok');else if(r)msg(t('wifi.saved'),'ok')"
"}"
"async function saveLlmConfig(){"
" var body={"
"  llm_url:$('llm_url').value,"
"  llm_key:$('llm_key').value,"
"  llm_model:$('llm_model').value"
" };"
" var r=await apiFetch('/api/config/llm',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});"
" if(r){"
"  try{localStorage.setItem('espLlmKey',body.llm_key);localStorage.setItem('espLlmRoute',$('llm_route').value)}catch(e){}"
"  msg(t('ai.saved'),'ok');refreshStatus()"
" }"
"}"
"async function loadConfig(){"
" var r=await apiFetch('/api/config');"
" if(!r)return;"
" $('wifi_ssid').value=r.wifi_ssid||'';"
" $('llm_url').value=r.llm_url||'';"
" $('llm_model').value=r.llm_model||'';"
" try{$('llm_key').value=localStorage.getItem('espLlmKey')||'';$('llm_route').value=localStorage.getItem('espLlmRoute')||'phone'}catch(e){}"
" try{pendingPhoneScript=localStorage.getItem('espPendingScript')||''}catch(e){}"
" if(pendingPhoneScript){$('scriptInput').value=pendingPhoneScript;phonePhase='reconnect';$('aiBtn').textContent=t('phone.runButton');$('aiResult').textContent=t('phone.restored')}"
"}"
"async function scanWifi(){"
" try{"
"  var btn=$('scanBtn');"
"  btn.disabled=true;btn.classList.add('btn-loading');"
"  msg(t('wifi.scanning'),'ok');"
"  var list=$('scanResults');"
"  list.style.display='block';"
"  list.textContent='';var loading=document.createElement('div');loading.className='scan-item scan-loading';loading.textContent=t('wifi.scanning');list.appendChild(loading);"
"  var ctl=new AbortController();"
"  setTimeout(function(){ctl.abort()},8000);"
"  var r=await apiFetch('/api/scan',{signal:ctl.signal});"
"  btn.disabled=false;btn.classList.remove('btn-loading');"
"  if(!r){msg(t('wifi.scanFailed'),'err');list.textContent='';var failed=document.createElement('div');failed.className='scan-item scan-empty';failed.textContent=t('wifi.scanFailed');list.appendChild(failed);return}"
"  if(r.length===0){list.textContent='';var empty=document.createElement('div');empty.className='scan-item scan-empty';empty.textContent=t('wifi.none');list.appendChild(empty);msg(t('wifi.none24'),'warn');return}"
"  list.textContent='';"
"  r.forEach(function(net){"
"   var item=document.createElement('div');"
"   item.className='scan-item';"
"   var ns=document.createElement('span');ns.className='scan-name';ns.textContent=net.ssid;"
"   var rs=document.createElement('span');"
"   var sig='sig-strong';if(net.rssi<-70)sig='sig-weak';else if(net.rssi<-50)sig='sig-medium';"
"   rs.className='scan-rssi '+sig;rs.textContent=net.rssi+' dBm';"
"   item.appendChild(ns);item.appendChild(rs);"
"   item.onclick=function(){$('wifi_ssid').value=net.ssid;msg(t('wifi.selected',{ssid:net.ssid}),'ok')};"
"   list.appendChild(item)"
"  });"
"  msg(t('wifi.found',{count:r.length}),'ok')"
" }catch(e){"
"  var btn=$('scanBtn');if(btn){btn.disabled=false;btn.classList.remove('btn-loading')}"
"  msg(t('fetch.error',{error:e.message}),'err');console.error(e)"
" }"
"}"
"function renderStatus(){"
" var r=lastStatus;if(!r)return;var listEl=$('deviceList');listEl.textContent='';"
" if(r.peers&&r.peers.length>0){r.peers.forEach(function(p){"
"  var row=document.createElement('div');row.className='device-row';var meta=document.createElement('div');"
"  var name=document.createElement('div');name.className='device-name';name.textContent=(p.name||t('modules.unnamed'))+' · ID '+p.id;meta.appendChild(name);"
"  if(p.capability){var cap=document.createElement('div');cap.className='device-cap';cap.textContent=p.capability;meta.appendChild(cap)}"
"  var state=document.createElement('span');state.className='device-state'+(p.online?'':' offline');state.textContent=t(p.online?'common.online':'common.offline');"
"  row.appendChild(meta);row.appendChild(state);listEl.appendChild(row)"
" })}else{var empty=document.createElement('span');empty.className='info';empty.textContent=t('modules.empty');listEl.appendChild(empty)}"
" var online=Number(r.peer_count)||0,known=Number(r.known_count)||online;var dr=$('deviceReady');dr.className='ready-item'+(online>0?' ok':'');dr.querySelector('b').textContent=t('ready.devices',{online:online,known:known});"
" var ar=$('aiReady');ar.className='ready-item'+(r.llm_configured?' ok':'');ar.querySelector('b').textContent=t(r.llm_configured?'ready.configured':'ready.needsSetup');"
" var rr=$('runReady');rr.className='ready-item ok';rr.querySelector('b').textContent=t(r.script_running?'ready.running':'common.standby');"
" var badge=$('statusBadge');if(badge){badge.textContent=online+' '+t('common.online')+' · '+t(r.script_running?'ready.running':'common.standby');badge.className='status-badge '+(online>0?'status-ok':'status-warn')}"
"}"
"async function refreshStatus(){"
" if(phonePhase==='mobile'||phonePhase==='reconnect')return;"
" var r=await apiFetch('/api/status',null,true);"
" if(!r){statusFailures++;renderNova();return false}"
" statusFailures=0;lastStatus=r;renderStatus();renderNova();return true"
"}"
""
"function formatMicLevel(level){return level<1?level.toFixed(3):level.toFixed(1)}"
"async function readMicLevel(){"
" var r=await apiFetch('/api/mic',null,true);"
" if(!r)return null;"
" if(r.status!=='ok'){$('micLevel').textContent=t('mic.failed',{error:r.error||t('common.unknown')});$('micMeter').style.width='0';return null}"
" var level=Math.max(0,Math.min(100,Number(r.level_percent)||0));"
" $('micLevel').textContent=t('mic.level',{level:formatMicLevel(level)});"
" $('micMeter').style.width=(level>0?Math.max(2,level):0)+'%';"
" return level"
"}"
"async function testMic(){"
" var btn=$('micBtn');btn.disabled=true;btn.classList.add('btn-loading');btn.textContent=t('mic.listening');$('micCard').classList.add('mic-active');setNova('listening');"
" var peak=0,reads=0;"
" for(var i=0;i<24;i++){"
"  var level=await readMicLevel();"
"  if(level!==null){peak=Math.max(peak,level);reads++}"
"  await new Promise(function(resolve){setTimeout(resolve,200)})"
" }"
" btn.disabled=false;btn.classList.remove('btn-loading');btn.textContent=t('mic.start');$('micCard').classList.remove('mic-active');"
" if(reads){$('micLevel').textContent=t('mic.peak',{level:formatMicLevel(peak)});msg(t('mic.done'),'ok')}"
" else{msg(t('mic.error'),'err')}"
"}"
""
"/* Polling uses exponential back-off and pauses during AI requests. */"
"var _poll={"
" logTimer:null,statusTimer:null,"
" logDelay:3000,statusDelay:5000,"
" maxDelay:30000,paused:false"
"};"
"function _scheduleLog(){"
" if(_poll.logTimer)clearTimeout(_poll.logTimer);"
" if(_poll.paused)return;"
" _poll.logTimer=setTimeout(async function(){"
"  var ok=await fetchLog();"
"  _poll.logDelay=ok?3000:Math.min(_poll.logDelay*2,_poll.maxDelay);"
"  _scheduleLog()"
" },_poll.logDelay)"
"}"
"function _scheduleStatus(){"
" if(_poll.statusTimer)clearTimeout(_poll.statusTimer);"
" if(_poll.paused)return;"
" _poll.statusTimer=setTimeout(async function(){"
"  await refreshStatus();"
"  _scheduleStatus()"
" },_poll.statusDelay)"
"}"
"function _pausePolling(){"
" _poll.paused=true;"
" if(_poll.logTimer){clearTimeout(_poll.logTimer);_poll.logTimer=null}"
" if(_poll.statusTimer){clearTimeout(_poll.statusTimer);_poll.statusTimer=null}"
"}"
"function _resumePolling(){"
" _poll.paused=false;"
" _poll.logDelay=3000;"
" _scheduleLog();_scheduleStatus()"
"}"
""
"async function callAI(){"
" var prompt=$('aiPrompt').value;"
" if(!prompt.trim()){msg(t('composer.empty'),'warn');return}"
" var phoneMode=$('llm_route').value==='phone';"
" if(!phoneMode)phonePhase='idle';"
" if(!phoneMode){"
"  var st=await apiFetch('/api/status');"
"  if(!st)return;"
"  if(!st.llm_configured){"
"   msg(t('ai.needConfig'),'warn');openLab('ai');return"
"  }"
"  if(!st.wifi_configured&&!st.local_proxy){"
"   msg(t('ai.needWifi'),'warn');openLab('wifi');return"
"  }"
" }"
" $('aiResult').textContent=t(phoneMode?'ai.preparingPhone':'ai.connecting');setNova('thinking');"
" var btn=$('aiBtn');btn.disabled=true;btn.classList.add('btn-loading');"
" _pausePolling();"
" var r=phoneMode?await callAIFromPhoneFlow(prompt):await apiFetch('/api/ai',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt:prompt})});"
" btn.disabled=false;btn.classList.remove('btn-loading');"
" if(!r){_resumePolling();$('aiResult').textContent=t('ai.failed');setNova('error','',t('ai.failed'),5000);return}"
" if(r.status==='pending'){$('aiResult').textContent=r.message;btn.textContent=r.button;setNova('setup','',r.message);return}"
" _resumePolling();"
" if(r.error&&r.error in{'Wi-Fi not configured':1,'LLM URL not configured':1,'LLM call failed':1,'No script in response':1}){"
"  var errMap=currentLang==='en'?{'Wi-Fi not configured':'Router Wi-Fi is not saved. Configure it in the Lab first.','LLM URL not configured':'The AI URL is not saved. Configure it in the Lab first.','LLM call failed':'AI call failed. Check the router connection and API key.','No script in response':'The AI returned no script. Try rephrasing the command.'}:{'Wi-Fi not configured':'尚未保存路由器 Wi-Fi，请先在实验室中配置。','LLM URL not configured':'尚未保存 AI 地址，请先在实验室中配置。','LLM call failed':'AI 调用失败，请检查路由器连接与 API Key。','No script in response':'AI 没有返回脚本，请换一种方式描述。'};"
"  msg(errMap[r.error]||r.error,'err');"
"  $('aiResult').textContent=t('ai.error',{error:errMap[r.error]||r.error});btn.textContent=t('composer.run');return"
" }"
" if(r.status==='clarify'){showClarify(r);return}"
" $('aiResult').textContent=r.status==='ok'?t('ai.ok'):t('ai.error',{error:r.error||t('common.unknown')});"
" btn.textContent=t('composer.run');"
" if(r.script){$('scriptInput').value=r.script}"
" if(r.status==='ok'){setNova('success','',t('ai.ok'),1800);fetchLogAfterExecution()}else setNova('error','',$('aiResult').textContent,5000)"
"}"
"function extractMobileScript(content){"
" var text=String(content||'').trim();"
" var m=text.match(/```(?:[A-Za-z0-9_+-]+)?[\\t ]*[\\r\\n]+([\\s\\S]*?)```/);if(m)text=m[1].trim();"
" if(text.charAt(0)==='{'){var parsed;try{parsed=JSON.parse(text)}catch(e){throw new Error(t('phone.invalidScript'))}if(!parsed||typeof parsed.script!=='string')throw new Error(t('phone.missingScript'));text=parsed.script.trim();m=text.match(/```(?:[A-Za-z0-9_+-]+)?[\\t ]*[\\r\\n]+([\\s\\S]*?)```/);if(m)text=m[1].trim()}"
" if(!text)throw new Error(t('phone.emptyScript'));"
" if(text.charAt(0)==='{'||text.charAt(0)==='['||text.indexOf('```')>=0)throw new Error(t('phone.invalidScript'));"
" if(/while\\s*\\(\\s*(?:true|1)\\s*\\)/i.test(text))throw new Error(t('phone.unsafeLoop'));"
" return text"
"}"
"async function callAIFromPhoneFlow(prompt){"
" var key=$('llm_key').value.trim();"
" if(!key)return {status:'error',error:t('phone.key')};"
" if(pendingPhoneScript){"
"  var injected=await apiFetch('/api/script',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({script:pendingPhoneScript})});"
"  if(!injected)return {status:'pending',message:t('phone.retryMessage'),button:t('phone.retryButton')};"
"  if(injected.status!=='ok')return {status:'error',error:injected.error||t('phone.invalidScript')};"
"  var completed=pendingPhoneScript;pendingPhoneScript='';phoneContext=null;phonePhase='idle';"
"  try{localStorage.removeItem('espPendingScript')}catch(e){}"
"  return {status:'ok',script:completed};"
" }"
" if(!phoneContext||phoneContext.user_prompt!==prompt){"
"  var ctx=await apiFetch('/api/ai/context');"
"  if(!ctx)return {status:'error',error:t('phone.context')};"
"  phoneContext={llm_url:ctx.llm_url,llm_model:ctx.llm_model,system_prompt:ctx.system_prompt,user_prompt:prompt};"
" }"
" var url=(phoneContext.llm_url||'').replace(/\\/+$/,'');"
" if(url.indexOf('https://')!==0)return {status:'error',error:t('phone.https')};"
" if(url.indexOf('/chat/completions')<0)url+='/chat/completions';"
" var body={model:phoneContext.llm_model,messages:[{role:'system',content:phoneContext.system_prompt},{role:'user',content:prompt}],temperature:0,max_tokens:512};"
" if((phoneContext.llm_model||'').indexOf('deepseek-v4')>=0)body.thinking={type:'disabled'};"
" var resp;try{resp=await fetch(url,{method:'POST',headers:{'Content-Type':'application/json','Authorization':'Bearer '+key},body:JSON.stringify(body)})}catch(e){phonePhase='mobile';return {status:'pending',message:t('phone.mobile'),button:t('phone.mobileButton')}}"
" if(!resp.ok){var detail=await resp.text();return {status:'error',error:'LLM HTTP '+resp.status+': '+detail.slice(0,160)}}"
" var data;try{data=await resp.json()}catch(e){return {status:'error',error:t('phone.invalidScript')}}"
" var content=data&&data.choices&&data.choices[0]&&data.choices[0].message&&data.choices[0].message.content;"
" if(!content)return {status:'error',error:t('phone.noScript')};"
" var script;try{script=extractMobileScript(content)}catch(e){return {status:'error',error:e.message||t('phone.invalidScript')}}"
" pendingPhoneScript=script;phonePhase='reconnect';$('scriptInput').value=script;"
" try{localStorage.setItem('espPendingScript',script)}catch(e){}"
" return {status:'pending',message:t('phone.generated'),button:t('phone.runButton')};"
"}"
"function showClarify(r){"
" if(r.script){$('scriptInput').value=r.script}"
" window._clarify=r;"
" var opts=r.options||[];"
" var root=$('aiResult');root.textContent='';var question=document.createElement('div');question.textContent=r.question||t('clarify.question');root.appendChild(question);"
" var actions=document.createElement('div');actions.className='btn-row';"
" opts.forEach(function(opt){var btn=document.createElement('button');btn.className='btn-success';btn.textContent=(opt.name||t('modules.unnamed'))+' (ID '+opt.id+')';btn.onclick=function(){pickDevice(opt.id)};actions.appendChild(btn)});root.appendChild(actions);clearNova()"
"}"
"async function pickDevice(id){"
" var r=window._clarify;if(!r)return;"
" var script=(r.script||'').split(r.placeholder).join(String(id));"
" $('scriptInput').value=script;"
" $('aiResult').textContent=t('clarify.injecting',{id:id});setNova('thinking');"
" var res=await apiFetch('/api/script',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({script:script})});"
" $('aiResult').textContent=(res&&res.status==='ok')?t('clarify.done',{id:id}):t('ai.error',{error:(res&&res.error)||t('common.unknown')});"
" if(res&&res.status==='ok')setNova('success','',$('aiResult').textContent,1800);else setNova('error','',$('aiResult').textContent,5000);"
" window._clarify=null;"
" setTimeout(fetchLog,500)"
"}"
"async function injectScript(){"
" var script=$('scriptInput').value;"
" if(!script.trim()){msg(t('dev.needScript'),'warn');return}"
" setNova('thinking');"
" var r=await apiFetch('/api/script',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({script:script})});"
" if(r&&r.script)$('scriptInput').value=r.script;"
" if(r&&r.status==='ok'&&script===pendingPhoneScript){pendingPhoneScript='';phoneContext=null;phonePhase='idle';try{localStorage.removeItem('espPendingScript')}catch(e){}}"
" if(r)msg(r.status==='ok'?t('dev.started'):t('ai.error',{error:r.error||t('common.unknown')}),r.status==='ok'?'ok':'err');"
" fetchLogAfterExecution()"
"}"
"async function fetchLog(){"
" if(phonePhase==='mobile'||phonePhase==='reconnect')return;"
" var r=await apiFetch('/api/exec_log',null,true);"
" if(!r)return false;"
" var el=$('execLog');"
" el.textContent=r.log||t('dev.empty');el.dataset.loaded='1';"
" el.scrollTop=el.scrollHeight;"
" return true"
"}"
"async function fetchLogAfterExecution(){"
" var lastResult=null;"
" for(var i=0;i<4;i++){"
"  await new Promise(function(resolve){setTimeout(resolve,1000)});"
"  var r=await apiFetch('/api/exec_log',null,true);"
"  if(r){lastResult=r;if(r.log){var el=$('execLog');el.textContent=r.log;el.dataset.loaded='1';el.scrollTop=el.scrollHeight;return}}"
" }"
" if(lastResult){var el=$('execLog');el.textContent=t('dev.empty');el.dataset.loaded='1';el.scrollTop=el.scrollHeight}"
" else {$('aiResult').textContent+=t('dev.reconnectLog')}"
"}"
"function clearLog(){"
" $('execLog').textContent='';$('execLog').dataset.loaded='1'"
"}"
"async function init(){"
" applyLanguage();"
" await loadConfig();"
" await refreshStatus();"
" await fetchLog();"
" _scheduleLog();_scheduleStatus()"
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

// httpd_req_recv() may return a partial body.  Read the declared body in a
// loop so fragmented browser requests are not misreported as invalid JSON.
static esp_err_t recv_json_body(httpd_req_t* req, char* buf, size_t buf_size)
{
    if (req == NULL || buf == NULL || buf_size < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    if ((size_t)req->content_len >= buf_size) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "Request body too large");
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received,
                                 req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                "Incomplete request body");
            return ESP_FAIL;
        }
        received += ret;
    }

    buf[received] = '\0';
    return ESP_OK;
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
    int known_count = peer_mgr_total_count();

    // Get peer list
    cJSON* peers_json = cJSON_CreateArray();
    int count = 0;
    PeerEntry** list = peer_mgr_list_all(&count);
    for (int i = 0; i < count; i++) {
        cJSON* peer_obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(peer_obj, "id", list[i]->module_id);
        cJSON_AddStringToObject(peer_obj, "name", list[i]->name);
        cJSON_AddStringToObject(peer_obj, "capability",
                                 list[i]->capability[0] ? list[i]->capability : "");
        cJSON_AddBoolToObject(peer_obj, "online",
                              list[i]->state == PEER_ACTIVE ? 1 : 0);
        cJSON_AddItemToArray(peers_json, peer_obj);
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "peer_count", (double)active_count);
    cJSON_AddNumberToObject(root, "known_count", (double)known_count);
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

    cJSON_AddBoolToObject(root, "local_proxy",
                          llm_client_uses_local_proxy(llm_url) ? 1 : 0);

    cJSON_AddBoolToObject(root, "script_running",
                          script_inject_is_running() ? 1 : 0);

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

// ---- GET /api/mic ----

static esp_err_t mic_get_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    double level_percent = 0.0;
    const esp_err_t ret = hw_mic_read_level(&level_percent);
    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_ERR_NO_MEM;
    }

    cJSON_AddStringToObject(root, "status", ret == ESP_OK ? "ok" : "error");
    if (ret == ESP_OK) {
        cJSON_AddNumberToObject(root, "level_percent", level_percent);
    } else {
        cJSON_AddStringToObject(root, "error", esp_err_to_name(ret));
    }

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "JSON error");
        return ESP_ERR_NO_MEM;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
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
    if (recv_json_body(req, buf, sizeof(buf)) != ESP_OK) {
        return ESP_FAIL;
    }

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
    if (recv_json_body(req, buf, sizeof(buf)) != ESP_OK) {
        return ESP_FAIL;
    }

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

// ---- GET /api/ai/context ----
// Return the non-secret dynamic prompt and endpoint metadata so the phone
// browser can call the LLM over mobile data. The API key deliberately stays
// in the browser/device stores and is never returned by this endpoint.

static esp_err_t ai_context_get_handler(httpd_req_t* req)
{
    reset_inactivity_timer();

    char llm_url[256] = "";
    char llm_model[128] = "";
    nvs_get_str_safe("llm_url", llm_url, sizeof(llm_url));
    nvs_get_str_safe("llm_model", llm_model, sizeof(llm_model));

    if (llm_url[0] == '\0' || llm_model[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "LLM URL/model not configured");
        return ESP_FAIL;
    }

    constexpr int prompt_size = 6144;
    char* system_prompt = (char*)malloc(prompt_size);
    if (system_prompt == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "Out of memory");
        return ESP_FAIL;
    }
    llm_client_build_system_prompt(system_prompt, prompt_size);

    cJSON* root = cJSON_CreateObject();
    if (root == NULL) {
        free(system_prompt);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "JSON error");
        return ESP_FAIL;
    }
    cJSON_AddStringToObject(root, "llm_url", llm_url);
    cJSON_AddStringToObject(root, "llm_model", llm_model);
    cJSON_AddStringToObject(root, "system_prompt", system_prompt);
    free(system_prompt);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "JSON error");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_sendstr(req, json);
    free(json);
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

    if (recv_json_body(req, buf, 1024) != ESP_OK) {
        free(buf);
        return ESP_FAIL;
    }

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
    // connection mid-request. A short delay lets lwIP flush the TCP send
    // buffer before esp_wifi_stop() tears down the interface.
    vTaskDelay(pdMS_TO_TICKS(100));
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

    if (recv_json_body(req, buf, 4096) != ESP_OK) {
        free(buf);
        return ESP_FAIL;
    }

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

    char* script = (char*)malloc(CONFIG_SCRIPT_MAX_LEN);
    if (script == NULL) {
        cJSON_Delete(root);
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    const int normalize_ret = script_normalize_response(
        script_item->valuestring, script, CONFIG_SCRIPT_MAX_LEN);
    if (normalize_ret != SCRIPT_NORMALIZE_OK) {
        ESP_LOGW(TAG, "Script rejected before execution: %s",
                 script_normalize_error(normalize_ret));
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "error");
        cJSON_AddStringToObject(resp, "error", script_normalize_error(normalize_ret));
        char* json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        free(script);
        cJSON_Delete(root);
        free(buf);
        httpd_resp_set_type(req, "application/json");
        if (json != NULL) {
            httpd_resp_sendstr(req, json);
            free(json);
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"Script validation failed\"}");
        }
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Validated script injection: %d bytes", (int)strlen(script));
    int inject_ret = script_inject_enqueue(script, (int)strlen(script));
    cJSON_Delete(root);
    free(buf);

    httpd_resp_set_type(req, "application/json");
    if (inject_ret == 0) {
        cJSON* resp = cJSON_CreateObject();
        cJSON_AddStringToObject(resp, "status", "ok");
        cJSON_AddStringToObject(resp, "script", script);
        char* json = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (json != NULL) {
            httpd_resp_sendstr(req, json);
            free(json);
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
        }
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"error\":\"Injection failed\"}");
    }
    free(script);

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
// Captive portal probe handlers
//
// Goal: make the phone OS believe the AP has internet connectivity so
// it does NOT auto-switch back to the user's home WiFi while the web
// console is open.  Trade-off: the OS will NOT show an automatic
// "Sign in to network" popup — users must open http://192.168.4.1
// manually (acceptable since they already know the address).
//
// Three variants cover the major OS probe families:
//   captive_204_handler  — Android & Chrome OS: expects 204 No Content
//   captive_ios_handler  — Apple iOS/macOS:      expects 200 + "Success" body
//   captive_ncsi_handler — Windows NCSI:         expects 200 + "Microsoft NCSI"
// ====================================================================

static esp_err_t captive_204_handler(httpd_req_t* req)
{
    // Android /generate_204 and related probes expect exactly HTTP 204
    // with an empty body to conclude "internet is available".
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_sendstr(req, "");
    return ESP_OK;
}

static esp_err_t captive_ios_handler(httpd_req_t* req)
{
    // Apple CNA (Captive Network Assistant) probe.
    // iOS/macOS GETs /hotspot-detect.html and checks for this exact body.
    // Returning it causes the OS to mark the network as "internet available".
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req,
        "<HTML><HEAD><TITLE>Success</TITLE></HEAD>"
        "<BODY>Success</BODY></HTML>");
    return ESP_OK;
}

static esp_err_t captive_ncsi_handler(httpd_req_t* req)
{
    // Windows Network Connectivity Status Indicator probe.
    // Expects HTTP 200 + body "Microsoft NCSI".
    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Microsoft NCSI");
    return ESP_OK;
}

// ====================================================================
// URI registration table
// ====================================================================

static const httpd_uri_t s_uris[] = {
    { .uri = "/",            .method = HTTP_GET,    .handler = root_get_handler,       .user_ctx = NULL },
    // Android / Chrome OS connectivity probes — must return 204 No Content
    { .uri = "/generate_204",            .method = HTTP_GET, .handler = captive_204_handler, .user_ctx = NULL },
    { .uri = "/generate204",             .method = HTTP_GET, .handler = captive_204_handler, .user_ctx = NULL },
    { .uri = "/mtuprobe",                .method = HTTP_GET, .handler = captive_204_handler, .user_ctx = NULL },

    // Apple iOS / macOS CNA probes — must return 200 + "Success" body
    { .uri = "/hotspot-detect.html",     .method = HTTP_GET, .handler = captive_ios_handler, .user_ctx = NULL },
    { .uri = "/library/test/success.html",.method = HTTP_GET,.handler = captive_ios_handler, .user_ctx = NULL },
    { .uri = "/success.txt",             .method = HTTP_GET, .handler = captive_ios_handler, .user_ctx = NULL },

    // Windows NCSI probes — must return 200 + "Microsoft NCSI"
    { .uri = "/ncsi.txt",                .method = HTTP_GET, .handler = captive_ncsi_handler,.user_ctx = NULL },
    { .uri = "/connecttest.txt",         .method = HTTP_GET, .handler = captive_ncsi_handler,.user_ctx = NULL },

    // Favicon — return 204 to avoid browser error (no icon served)
    { .uri = "/favicon.ico",             .method = HTTP_GET, .handler = captive_204_handler, .user_ctx = NULL },

    // Application API
    { .uri = "/api/status",      .method = HTTP_GET,  .handler = status_get_handler,          .user_ctx = NULL },
    { .uri = "/api/mic",         .method = HTTP_GET,  .handler = mic_get_handler,             .user_ctx = NULL },
    { .uri = "/api/config",      .method = HTTP_GET,  .handler = config_get_handler,          .user_ctx = NULL },
    { .uri = "/api/config/wifi", .method = HTTP_POST, .handler = config_wifi_post_handler,    .user_ctx = NULL },
    { .uri = "/api/config/llm",  .method = HTTP_POST, .handler = config_llm_post_handler,     .user_ctx = NULL },
    { .uri = "/api/wifi/connect",.method = HTTP_POST, .handler = wifi_connect_post_handler,   .user_ctx = NULL },
    { .uri = "/api/scan",        .method = HTTP_GET,  .handler = scan_get_handler,            .user_ctx = NULL },
    { .uri = "/api/ai/context",  .method = HTTP_GET,  .handler = ai_context_get_handler,      .user_ctx = NULL },
    { .uri = "/api/ai",          .method = HTTP_POST, .handler = ai_post_handler,             .user_ctx = NULL },
    { .uri = "/api/script",      .method = HTTP_POST, .handler = script_post_handler,         .user_ctx = NULL },
    { .uri = "/api/exec_log",    .method = HTTP_GET,  .handler = exec_log_get_handler,        .user_ctx = NULL },
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
    config.max_uri_handlers = 24;
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
