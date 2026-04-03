#include "http_api.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alert_service.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ota_service.h"
#include "sdkconfig.h"
#include "servo_control.h"
#include "status_store.h"
#include "wifi_service.h"

#ifndef CONFIG_RPC_WIFI_FALLBACK_AP_SSID
#define CONFIG_RPC_WIFI_FALLBACK_AP_SSID ""
#endif

static const char *TAG = "http_api";

static httpd_handle_t s_server;
static SemaphoreHandle_t s_press_lock;
static bool s_press_in_progress;

typedef struct {
    bool use_mode;
    servo_control_press_mode_t mode;
    int press_angle;
    uint32_t press_ms;
} press_request_t;

static const char *s_control_page_html =
    "<!doctype html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 杩滅▼鐢垫簮鎺у埗鍣?</title>"
    "<style>"
    "body{font-family:Segoe UI,sans-serif;margin:0;background:#eef4f2;color:#17343a}"
    ".wrap{max-width:980px;margin:0 auto;padding:18px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(300px,1fr));gap:14px}"
    ".card{background:#fff;border-radius:16px;padding:16px;box-shadow:0 8px 22px rgba(0,0,0,.08)}"
    "h1,h2{margin:0 0 10px}label{display:block;font-size:12px;margin:10px 0 4px;color:#4b6871}"
    "input,button{width:100%;padding:10px 12px;border-radius:10px;border:1px solid #c9d7d2;font-size:14px;box-sizing:border-box}"
    "button{background:#17343a;color:#fff;font-weight:700;cursor:pointer}button.alt{background:#4c766f}button.ghost{background:#fff;color:#17343a}"
    ".row{display:flex;gap:8px}.row>*{flex:1}.pill{display:inline-block;padding:5px 9px;border-radius:999px;font-size:12px;font-weight:700;margin-right:8px}"
    ".ok{background:#dff6e8;color:#12653a}.warn{background:#ffe9c7;color:#925400}.bad{background:#ffd8d8;color:#a12626}"
    ".mono{font-family:Consolas,monospace}.toast{display:none;position:sticky;top:10px;padding:10px 12px;background:#17343a;color:#fff;border-radius:12px;margin-bottom:12px}"
    ".slot{border:1px solid #dbe6e1;border-radius:12px;padding:10px;margin-top:10px;background:#f7fbf9}.hint{font-size:12px;color:#637e85;margin-top:8px;line-height:1.4}.quick{font-size:12px;color:#5f7a80;margin:0 0 10px}"
    "</style></head><body><div class='wrap'><div id='toast' class='toast'></div><h1>ESP32 杩滅▼鐢垫簮鎺у埗鍣?</h1><div class='grid'>"
    "<section class='card'><h2>璁惧鐘舵€?</h2><div><span id='wifiBadge' class='pill warn'>Wi-Fi 绂荤嚎</span><span id='hostBadge' class='pill bad'>涓绘満绂荤嚎</span></div>"
    "<p>璁惧鍚? <strong id='deviceName'>-</strong><br>IP 鍦板潃: <strong id='ipAddress'>-</strong><br>鑸垫満瑙掑害: <strong id='servoAngle'>-</strong><br>"
    "褰撳墠 Wi-Fi: <strong id='activeWifi'>-</strong><br>蹇冭烦闂撮殧: <strong id='heartbeatAge'>-</strong><br>鏈€杩戝憡璀? <strong id='lastAlert'>-</strong><br>"
    "鏈€杩戜笂鎶? <span id='lastReport' class='mono'>-</span></p>"
    "<button id='refreshBtn' class='ghost'>鍒锋柊鐘舵€?</button></section>"
    "<section class='card'><h2>蹇嵎鎺у埗</h2><div class='quick'>甯哥敤鎿嶄綔灏介噺鐩磋揪锛屽厛璋冭搴︼紝鍐嶆墽琛屾寜鍘嬨€?</div><label>鐩爣瑙掑害</label><input id='servoAngleInput' type='number' value='0'>"
    "<label>鑷畾涔夋寜鍘嬫椂闀?ms)</label><input id='customPressMs' type='number' value='350'>"
    "<div class='row'><button id='moveBtn'>杞埌鐩爣瑙掑害</button><button id='restBtn' class='ghost'>鍥炲埌寰呮満瑙?</button></div>"
    "<div class='row'><button id='shortBtn' class='alt'>鐭寜鐢垫簮閿?</button><button id='longBtn'>闀挎寜鐢垫簮閿?</button></div>"
    "<button id='customBtn' class='ghost'>鎸夊綋鍓嶈搴︽墽琛岃嚜瀹氫箟鎸夊帇</button><div id='servoSummary' class='hint'>姝ｅ湪鍔犺浇鑸垫満閰嶇疆...</div></section>"
    "<section class='card'><h2>鑸垫満鍙傛暟</h2>"
    "<div class='row'><div><label>寰呮満瑙掑害</label><input id='restAngleInput' type='number'></div><div><label>棰勫瑙掑害</label><input id='readyAngleInput' type='number'></div></div>"
    "<div class='row'><div><label>鐭寜瑙掑害</label><input id='shortAngleInput' type='number'></div><div><label>闀挎寜瑙掑害</label><input id='longAngleInput' type='number'></div></div>"
    "<div class='row'><div><label>鐭寜鏃堕暱(ms)</label><input id='shortMsInput' type='number'></div><div><label>闀挎寜鏃堕暱(ms)</label><input id='longMsInput' type='number'></div></div>"
    "<div class='row'><div><label>棰勫鍋滈】(ms)</label><input id='prepareMsInput' type='number'></div><div><label>鍥炰綅鍋滈】(ms)</label><input id='releaseMsInput' type='number'></div></div>"
    "<div class='row'><button id='saveServoBtn'>淇濆瓨鍙傛暟</button><button id='applyServoBtn' class='ghost'>淇濆瓨骞跺洖寰呮満瑙?</button></div>"
    "<div class='hint'>鎸夊帇娴佺▼: 棰勫瑙掑害 -> 绛夊緟 -> 鎸夊帇瑙掑害 -> 绛夊緟 -> 鍥炲埌寰呮満瑙掋€?</div></section>"
    "<section class='card'><h2>Wi-Fi 閰嶇疆</h2><div id='wifiProfiles'></div><button id='saveWifiBtn'>淇濆瓨 Wi-Fi 閰嶇疆</button><button id='clearWifiBtn' class='ghost'>娓呴櫎杩愯鏃?Wi-Fi</button><div id='fallbackWifi' class='hint'>澶囩敤 Wi-Fi: -</div></section>"
    "<section class='card'><h2>璁块棶鎺у埗</h2><label>API 浠ょ墝</label><input id='tokenInput' type='password' autocomplete='off'><div class='row'><button id='saveTokenBtn' class='ghost'>淇濆瓨浠ょ墝</button><button id='alertBtn' class='alt'>鍙戦€佹祴璇曞憡璀?</button></div></section>"
    "</div><script>"
    "const state={token:'',busy:0,loading:false,statusLoaded:false,lastStatus:null};const wifiSlots=[0,1,2];const $=id=>document.getElementById(id);"
    "function showToast(t,e){const x=$(\'toast\');x.textContent=t;x.style.display=\'block\';x.style.background=e?\'#9f2f2f\':\'#17343a\';clearTimeout(showToast.t);showToast.t=setTimeout(()=>x.style.display=\'none\',2500)}"
    "function getToken(){return state.token||localStorage.getItem(\'rpcToken\')||\'\'}function setToken(v){state.token=v||\'\';localStorage.setItem(\'rpcToken\',state.token);$(\'tokenInput\').value=state.token}"
    "function buildUrl(p){const t=getToken();return !t?p:p+(p.includes(\'?\')?\'&\':\'?\')+\'token=\'+encodeURIComponent(t)}"
    "async function api(p,o){const opts=Object.assign({headers:{}},o||{});const t=getToken();if(t){opts.headers[\'X-Auth-Token\']=t}const r=await fetch(buildUrl(p),opts);if(!r.ok){throw new Error(await r.text()||(\'HTTP \'+r.status))}const c=r.headers.get(\'content-type\')||\'\';return c.includes(\'application/json\')?r.json():r.text()}"
    "function applyEnglishUi(){document.title=\'ESP32 Power Controller\';const h1=document.querySelector(\'h1\');if(h1){h1.textContent=\'ESP32 Power Controller\'}const cards=[...document.querySelectorAll(\'.card\')];if($(\'wifiBadge\')){$(\'wifiBadge\').textContent=\'Wi-Fi offline\'}if($(\'hostBadge\')){$(\'hostBadge\').textContent=\'Host offline\'}if(cards[0]){const h2=cards[0].querySelector(\'h2\');const p=cards[0].querySelector(\'p\');if(h2){h2.textContent=\'Device Status\'}if(p){p.innerHTML=`Device: <strong id='deviceName'>-</strong><br>IP Address: <strong id='ipAddress'>-</strong><br>Servo Angle: <strong id='servoAngle'>-</strong><br>Current Wi-Fi: <strong id='activeWifi'>-</strong><br>Setup Hotspot: <strong id='fallbackAp'>-</strong><br>Heartbeat Age: <strong id='heartbeatAge'>-</strong><br>Last Alert: <strong id='lastAlert'>-</strong><br>Last Report: <span id='lastReport' class='mono'>-</span>`}if($(\'refreshBtn\')){$(\'refreshBtn\').textContent=\'Refresh Status\'}}if(cards[1]){const h2=cards[1].querySelector(\'h2\');const quick=cards[1].querySelector(\'.quick\');const labels=cards[1].querySelectorAll(\'label\');if(h2){h2.textContent=\'Quick Control\'}if(quick){quick.textContent=\'Adjust the target angle first, then run a short, long, or custom press.\'}if(labels[0]){labels[0].textContent=\'Target Angle\'}if(labels[1]){labels[1].textContent=\'Custom Press Duration (ms)\'}if($(\'moveBtn\')){$(\'moveBtn\').textContent=\'Move To Angle\'}if($(\'restBtn\')){$(\'restBtn\').textContent=\'Move To Rest\'}if($(\'shortBtn\')){$(\'shortBtn\').textContent=\'Short Press\'}if($(\'longBtn\')){$(\'longBtn\').textContent=\'Long Press\'}if($(\'customBtn\')){$(\'customBtn\').textContent=\'Run Custom Press\'}if($(\'servoSummary\')){$(\'servoSummary\').textContent=\'Loading servo configuration...\'} }if(cards[2]){const h2=cards[2].querySelector(\'h2\');const labels=cards[2].querySelectorAll(\'label\');if(h2){h2.textContent=\'Servo Config\'}if(labels[0]){labels[0].textContent=\'Rest Angle\'}if(labels[1]){labels[1].textContent=\'Ready Angle\'}if(labels[2]){labels[2].textContent=\'Short Press Angle\'}if(labels[3]){labels[3].textContent=\'Long Press Angle\'}if(labels[4]){labels[4].textContent=\'Short Press (ms)\'}if(labels[5]){labels[5].textContent=\'Long Press (ms)\'}if(labels[6]){labels[6].textContent=\'Prepare Delay (ms)\'}if(labels[7]){labels[7].textContent=\'Release Delay (ms)\'}if($(\'saveServoBtn\')){$(\'saveServoBtn\').textContent=\'Save Config\'}if($(\'applyServoBtn\')){$(\'applyServoBtn\').textContent=\'Save And Move To Rest\'}const hint=cards[2].querySelector(\'.hint\');if(hint){hint.textContent=\'Press flow: ready angle, wait, press angle, wait, then return to rest angle.\'}}if(cards[3]){const h2=cards[3].querySelector(\'h2\');if(h2){h2.textContent=\'Wi-Fi Config\'}if($(\'saveWifiBtn\')){$(\'saveWifiBtn\').textContent=\'Save Wi-Fi Config\'}if($(\'clearWifiBtn\')){$(\'clearWifiBtn\').textContent=\'Clear Runtime Wi-Fi\'}if($(\'fallbackWifi\')){$(\'fallbackWifi\').textContent=\'Fallback Wi-Fi: -\'}}if(cards[4]){const h2=cards[4].querySelector(\'h2\');const label=cards[4].querySelector(\'label\');if(h2){h2.textContent=\'Access Control\'}if(label){label.textContent=\'API Token\'}if($(\'saveTokenBtn\')){$(\'saveTokenBtn\').textContent=\'Save Token\'}if($(\'alertBtn\')){$(\'alertBtn\').textContent=\'Send Test Alert\'}}}"
    "function updateActionButtons(){const b=state.busy>0;const pressBusy=!!(state.lastStatus&&state.lastStatus.press_in_progress);[\'moveBtn\',\'restBtn\',\'saveServoBtn\',\'applyServoBtn\',\'saveWifiBtn\',\'clearWifiBtn\'].forEach(id=>{const el=$(id);if(el){el.disabled=b}});[\'shortBtn\',\'longBtn\',\'customBtn\'].forEach(id=>{const el=$(id);if(el){el.disabled=b||pressBusy}});$(\'refreshBtn\').disabled=state.loading;const ps=$(\'pressState\');if(ps){ps.textContent=pressBusy?\'Press task running\':\'Press task idle\'}}"
    "function setBusy(on){state.busy=Math.max(0,state.busy+(on?1:-1));updateActionButtons()}function renderStatus(d){state.lastStatus=d;$(\'deviceName\').textContent=d.device||\'-\';$(\'ipAddress\').textContent=d.ip_address||\'-\';$(\'servoAngle\').textContent=((d.servo_angle===undefined||d.servo_angle===null)?\'-\':d.servo_angle)+\' deg\';$(\'activeWifi\').textContent=d.configured_ssid||\'-\';if($(\'fallbackAp\')){$(\'fallbackAp\').textContent=d.fallback_ap_active?((d.fallback_ap_ssid||\'setup hotspot\')+\' @ \'+(d.fallback_ap_ip||\'192.168.4.1\')):\'off\'}$(\'heartbeatAge\').textContent=d.last_heartbeat_age_ms>=0?(Math.round(d.last_heartbeat_age_ms/100)/10+\' s\'):\'-\';$(\'lastAlert\').textContent=d.last_alert_reason||\'-\';$(\'lastReport\').textContent=d.last_report||\'-\';$(\'wifiBadge\').textContent=d.wifi_connected?(\'Wi-Fi connected \'+(d.ip_address||\'\')):(d.fallback_ap_active?(\'Setup hotspot active \'+(d.fallback_ap_ip||\'\')):\'Wi-Fi offline\');$(\'wifiBadge\').className=\'pill \'+((d.wifi_connected||d.fallback_ap_active)?\'ok\':\'warn\');$(\'hostBadge\').textContent=d.host_online?(\'Host online \'+(d.host_name||\'\')):\'Host offline\';$(\'hostBadge\').className=\'pill \'+(d.host_online?\'ok\':\'bad\');$(\'servoSummary\').textContent=\'Range \'+d.servo_min_angle+\'..\'+d.servo_max_angle+\' | Rest \'+d.servo_rest_angle+\' | Ready \'+d.servo_ready_angle+\' | Short \'+d.short_press_angle+\'/\'+d.short_press_ms+\'ms | Long \'+d.long_press_angle+\'/\'+d.long_press_ms+\'ms | Prepare \'+d.prepare_settle_ms+\'ms | Release \'+d.release_settle_ms+\'ms\';if(!state.statusLoaded){$(\'servoAngleInput\').value=d.servo_angle;$(\'customPressMs\').value=d.short_press_ms;$(\'restAngleInput\').value=d.servo_rest_angle;$(\'readyAngleInput\').value=d.servo_ready_angle;$(\'shortAngleInput\').value=d.short_press_angle;$(\'longAngleInput\').value=d.long_press_angle;$(\'shortMsInput\').value=d.short_press_ms;$(\'longMsInput\').value=d.long_press_ms;$(\'prepareMsInput\').value=d.prepare_settle_ms;$(\'releaseMsInput\').value=d.release_settle_ms;state.statusLoaded=true}updateActionButtons()}"
    "function wifiEditorActive(){const active=document.activeElement;return !!(active&&active.id&&(active.id.startsWith(\'wifiSsid\')||active.id.startsWith(\'wifiPassword\')))}function collectWifiDrafts(){const drafts={};wifiSlots.forEach(i=>{const s=$(\'wifiSsid\'+i);const p=$(\'wifiPassword\'+i);if(s||p){drafts[i]={ssid:s?s.value:\'\',password:p?p.value:\'\'}}});return drafts}function renderWifi(d){const fb=(d.profiles||[]).find(p=>p.slot<0);$(\'fallbackWifi\').textContent=fb&&fb.configured?(\'Fallback Wi-Fi: \'+fb.ssid):\'Fallback Wi-Fi: not configured\';if(wifiEditorActive()){return}const drafts=collectWifiDrafts();const box=$(\'wifiProfiles\');box.innerHTML=\'\';(d.profiles||[]).filter(p=>p.slot>=0).sort((a,b)=>a.slot-b.slot).forEach(p=>{const el=document.createElement(\'div\');el.className=\'slot\';el.innerHTML=`<strong>Slot ${p.slot+1}</strong><div class='hint'>${p.active?'Active':(p.configured?'Saved':'Empty')}</div><label>SSID</label><input id='wifiSsid${p.slot}' type='text'><label>Password</label><input id='wifiPassword${p.slot}' type='password' autocomplete='new-password'>`;box.appendChild(el);const draft=drafts[p.slot]||{};$(\'wifiSsid\'+p.slot).value=(draft.ssid!==undefined&&draft.ssid!==\'\')?draft.ssid:(p.configured?(p.ssid||\'\'):\'\');$(\'wifiPassword\'+p.slot).value=draft.password||\'\'})}"
    "async function loadAll(silent){if(state.loading){return}state.loading=true;updateActionButtons();try{const [s,w]=await Promise.all([api(\'/api/status\'),api(\'/api/wifi\')]);renderStatus(s);renderWifi(w);if(!silent)showToast(\'Refreshed\',false)}catch(e){if(!silent)showToast(e.message,true)}finally{state.loading=false;updateActionButtons()}}async function runAction(task,msg){setBusy(true);try{await task();await loadAll(true);showToast(msg,false)}catch(e){showToast(e.message||String(e),true)}finally{setBusy(false)}}"
    "$(\'refreshBtn\').onclick=()=>loadAll(false);$(\'moveBtn\').onclick=()=>runAction(()=>api(\'/api/servo?angle=\'+encodeURIComponent(parseInt($(\'servoAngleInput\').value||\'0\',10)),{method:\'POST\'}),\'Servo moved\');$(\'restBtn\').onclick=()=>runAction(()=>api(\'/api/servo?angle=\'+encodeURIComponent(parseInt($(\'restAngleInput\').value||\'0\',10)),{method:\'POST\'}),\'Moved to rest angle\');$(\'shortBtn\').onclick=()=>runAction(()=>api(\'/api/press?mode=short\',{method:\'POST\'}),\'Short press queued\');$(\'longBtn\').onclick=()=>runAction(()=>api(\'/api/press?mode=long\',{method:\'POST\'}),\'Long press queued\');$(\'customBtn\').onclick=()=>runAction(()=>api(\'/api/press?angle=\'+encodeURIComponent(parseInt($(\'servoAngleInput\').value||\'0\',10))+\'&ms=\'+encodeURIComponent(parseInt($(\'customPressMs\').value||\'0\',10)),{method:\'POST\'}),\'Custom press queued\');"
    "$(\'saveServoBtn\').onclick=()=>{const body=new URLSearchParams({rest_angle:$(\'restAngleInput\').value,ready_angle:$(\'readyAngleInput\').value,short_press_angle:$(\'shortAngleInput\').value,long_press_angle:$(\'longAngleInput\').value,short_press_ms:$(\'shortMsInput\').value,long_press_ms:$(\'longMsInput\').value,prepare_settle_ms:$(\'prepareMsInput\').value,release_settle_ms:$(\'releaseMsInput\').value}).toString();return runAction(()=>api(\'/api/servo/config\',{method:\'POST\',headers:{\'Content-Type\':\'application/x-www-form-urlencoded\'},body}),\'Servo config saved\')};$(\'applyServoBtn\').onclick=()=>{const body=new URLSearchParams({rest_angle:$(\'restAngleInput\').value,ready_angle:$(\'readyAngleInput\').value,short_press_angle:$(\'shortAngleInput\').value,long_press_angle:$(\'longAngleInput\').value,short_press_ms:$(\'shortMsInput\').value,long_press_ms:$(\'longMsInput\').value,prepare_settle_ms:$(\'prepareMsInput\').value,release_settle_ms:$(\'releaseMsInput\').value,move_to_rest:\'1\'}).toString();return runAction(()=>api(\'/api/servo/config\',{method:\'POST\',headers:{\'Content-Type\':\'application/x-www-form-urlencoded\'},body}),\'Servo config applied\')};"
    "$(\'saveWifiBtn\').onclick=async()=>{const body=new URLSearchParams();for(let i=0;i<3;i++){const s=$(\'wifiSsid\'+i);const p=$(\'wifiPassword\'+i);if(s){body.set(\'ssid\'+i,s.value.trim());if(p&&p.value!==\'\'){body.set(\'password\'+i,p.value)}}}await runAction(()=>api(\'/api/wifi/config\',{method:\'POST\',headers:{\'Content-Type\':\'application/x-www-form-urlencoded\'},body:body.toString()}),\'Wi-Fi config saved\');for(let i=0;i<3;i++){const p=$(\'wifiPassword\'+i);if(p){p.value=\'\'}}};$(\'clearWifiBtn\').onclick=()=>runAction(()=>api(\'/api/wifi/clear\',{method:\'POST\'}),\'Runtime Wi-Fi cleared\');$(\'saveTokenBtn\').onclick=()=>{setToken($(\'tokenInput\').value.trim());showToast(\'Token saved\',false);loadAll(true)};$(\'alertBtn\').onclick=()=>runAction(()=>api(\'/api/alert/test\',{method:\'POST\'}),\'Alert test sent\');const q=new URLSearchParams(location.search).get(\'token\');if(q){setToken(q)}else{$(\'tokenInput\').value=getToken()}applyEnglishUi();updateActionButtons();loadAll(true);setInterval(()=>loadAll(true),10000)"
    "</script></body></html>";

static int hex_value(char ch);
static void url_decode_copy(char *dst, size_t dst_len, const char *src, size_t src_len);
static void json_escape_copy(char *dst, size_t dst_len, const char *src);
static bool extract_form_value(const char *encoded, const char *key, char *value, size_t value_len);
static bool get_query_value(httpd_req_t *req, const char *key, char *value, size_t value_len);
static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_len);
static bool get_form_or_query_value(httpd_req_t *req, const char *body, const char *key, char *value, size_t value_len);
static size_t appendf(char *buffer, size_t buffer_len, size_t offset, const char *fmt, ...);
static bool press_request_is_active(void);
static bool request_is_authorized(httpd_req_t *req);
static esp_err_t respond_with_status_json(httpd_req_t *req);
static esp_err_t respond_with_servo_config_json(httpd_req_t *req);
static esp_err_t respond_with_wifi_json(httpd_req_t *req);
static esp_err_t respond_with_ota_json(httpd_req_t *req);
static esp_err_t queue_press_request(const press_request_t *request);
static void press_worker_task(void *arg);

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static void url_decode_copy(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t dst_index = 0;

    if (dst_len == 0) {
        return;
    }

    for (size_t src_index = 0; src_index < src_len && dst_index + 1 < dst_len; src_index++) {
        if (src[src_index] == '%' && src_index + 2 < src_len) {
            const int upper = hex_value(src[src_index + 1]);
            const int lower = hex_value(src[src_index + 2]);
            if (upper >= 0 && lower >= 0) {
                dst[dst_index++] = (char)((upper << 4) | lower);
                src_index += 2;
                continue;
            }
        }

        dst[dst_index++] = (src[src_index] == '+') ? ' ' : src[src_index];
    }

    dst[dst_index] = '\0';
}

static void json_escape_copy(char *dst, size_t dst_len, const char *src)
{
    size_t dst_index = 0;

    if (dst_len == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (size_t src_index = 0; src[src_index] != '\0' && dst_index + 1 < dst_len; src_index++) {
        const char ch = src[src_index];
        if ((ch == '"' || ch == '\\') && dst_index + 2 < dst_len) {
            dst[dst_index++] = '\\';
            dst[dst_index++] = ch;
        } else if ((unsigned char)ch < 0x20) {
            if (dst_index + 6 < dst_len) {
                dst_index += (size_t)snprintf(dst + dst_index, dst_len - dst_index, "\\u%04x", (unsigned char)ch);
            } else {
                break;
            }
        } else {
            dst[dst_index++] = ch;
        }
    }

    dst[dst_index] = '\0';
}

static bool extract_form_value(const char *encoded, const char *key, char *value, size_t value_len)
{
    const size_t key_len = strlen(key);
    const char *cursor = encoded;

    if (encoded == NULL || value_len == 0) {
        return false;
    }

    while (*cursor != '\0') {
        const char *pair_end = strchr(cursor, '&');
        if (pair_end == NULL) {
            pair_end = cursor + strlen(cursor);
        }

        const char *equals = memchr(cursor, '=', (size_t)(pair_end - cursor));
        if (equals != NULL && (size_t)(equals - cursor) == key_len &&
            strncmp(cursor, key, key_len) == 0) {
            url_decode_copy(value, value_len, equals + 1, (size_t)(pair_end - equals - 1));
            return true;
        }

        cursor = (*pair_end == '&') ? pair_end + 1 : pair_end;
    }

    return false;
}

static bool get_query_value(httpd_req_t *req, const char *key, char *value, size_t value_len)
{
    const size_t query_len = httpd_req_get_url_query_len(req) + 1;
    if (query_len <= 1) {
        return false;
    }

    char *query = calloc(query_len, sizeof(char));
    if (query == NULL) {
        return false;
    }

    bool found = false;
    if (httpd_req_get_url_query_str(req, query, query_len) == ESP_OK) {
        found = extract_form_value(query, key, value, value_len);
    }

    free(query);
    return found;
}

static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_len)
{
    if (buffer == NULL || buffer_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    buffer[0] = '\0';
    if (req->content_len <= 0) {
        return ESP_OK;
    }

    if ((size_t)req->content_len >= buffer_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    int received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return ESP_FAIL;
        }
        received += ret;
    }

    buffer[received] = '\0';
    return ESP_OK;
}

static bool get_form_or_query_value(httpd_req_t *req,
                                    const char *body,
                                    const char *key,
                                    char *value,
                                    size_t value_len)
{
    if (get_query_value(req, key, value, value_len)) {
        return true;
    }

    if (body != NULL && body[0] != '\0') {
        return extract_form_value(body, key, value, value_len);
    }

    return false;
}

static size_t appendf(char *buffer, size_t buffer_len, size_t offset, const char *fmt, ...)
{
    if (offset >= buffer_len) {
        return buffer_len;
    }

    va_list args;
    va_start(args, fmt);
    const int written = vsnprintf(buffer + offset, buffer_len - offset, fmt, args);
    va_end(args);

    if (written < 0) {
        return offset;
    }

    if ((size_t)written >= buffer_len - offset) {
        return buffer_len - 1;
    }

    return offset + (size_t)written;
}

static bool press_request_is_active(void)
{
    bool active = false;

    if (s_press_lock == NULL) {
        return false;
    }

    if (xSemaphoreTake(s_press_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        active = s_press_in_progress;
        xSemaphoreGive(s_press_lock);
    }

    return active;
}

static bool request_is_authorized(httpd_req_t *req)
{
    if (strlen(CONFIG_RPC_API_AUTH_TOKEN) == 0) {
        return true;
    }

    char token[96];
    if (get_query_value(req, "token", token, sizeof(token)) &&
        strcmp(token, CONFIG_RPC_API_AUTH_TOKEN) == 0) {
        return true;
    }

    if (httpd_req_get_hdr_value_str(req, "X-Auth-Token", token, sizeof(token)) == ESP_OK &&
        strcmp(token, CONFIG_RPC_API_AUTH_TOKEN) == 0) {
        return true;
    }

    httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "missing or invalid token");
    return false;
}

static esp_err_t respond_with_status_json(httpd_req_t *req)
{
    app_status_snapshot_t snapshot;
    wifi_service_credentials_t credentials;
    servo_control_config_t servo_config;
    const bool fallback_ap_active = wifi_service_is_fallback_ap_active();

    status_store_snapshot(&snapshot);
    if (wifi_service_get_credentials(&credentials) != ESP_OK || servo_control_get_config(&servo_config) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status unavailable");
        return ESP_FAIL;
    }

    char device[64];
    char ip[24];
    char ssid[64];
    char host[48];
    char user[48];
    char alert[96];
    char last_report[256];
    char fallback_ap_ssid[64];

    json_escape_copy(device, sizeof(device), CONFIG_RPC_DEVICE_NAME);
    json_escape_copy(ip, sizeof(ip), snapshot.ip_address);
    json_escape_copy(ssid, sizeof(ssid), credentials.ssid);
    json_escape_copy(host, sizeof(host), snapshot.host_name);
    json_escape_copy(user, sizeof(user), snapshot.user_name);
    json_escape_copy(alert, sizeof(alert), snapshot.last_alert_reason);
    json_escape_copy(last_report, sizeof(last_report), snapshot.last_report);
    json_escape_copy(fallback_ap_ssid, sizeof(fallback_ap_ssid), CONFIG_RPC_WIFI_FALLBACK_AP_SSID);

    char *json = calloc(1, 1792);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status buffer unavailable");
        return ESP_ERR_NO_MEM;
    }

    snprintf(json,
             1792,
             "{\"device\":\"%s\",\"wifi_connected\":%s,\"ip_address\":\"%s\",\"configured_ssid\":\"%s\","
             "\"fallback_ap_active\":%s,\"fallback_ap_ssid\":\"%s\",\"fallback_ap_ip\":\"%s\","
             "\"wifi_active_slot\":%d,\"servo_min_angle\":%d,\"servo_max_angle\":%d,"
             "\"servo_rest_angle\":%d,\"servo_ready_angle\":%d,\"short_press_angle\":%d,\"long_press_angle\":%d,"
             "\"short_press_ms\":%lu,\"long_press_ms\":%lu,\"prepare_settle_ms\":%lu,\"release_settle_ms\":%lu,"
             "\"servo_angle\":%d,\"press_in_progress\":%s,\"host_online\":%s,\"press_count\":%lu,\"last_press_ms\":%lu,"
             "\"last_heartbeat_age_ms\":%lld,\"host_name\":\"%s\",\"user_name\":\"%s\","
             "\"cpu_pct\":%.1f,\"memory_pct\":%.1f,\"host_uptime_s\":%lu,\"last_report\":\"%s\","
             "\"last_alert_age_ms\":%lld,\"last_alert_reason\":\"%s\"}",
             device,
             snapshot.wifi_connected ? "true" : "false",
             ip,
             ssid,
             fallback_ap_active ? "true" : "false",
             fallback_ap_ssid,
             WIFI_SERVICE_FALLBACK_AP_IP,
             credentials.active_slot,
             servo_config.min_angle,
             servo_config.max_angle,
             servo_config.rest_angle,
             servo_config.ready_angle,
             servo_config.short_press_angle,
             servo_config.long_press_angle,
             (unsigned long)servo_config.short_press_ms,
             (unsigned long)servo_config.long_press_ms,
             (unsigned long)servo_config.prepare_settle_ms,
             (unsigned long)servo_config.release_settle_ms,
             snapshot.servo_angle,
             press_request_is_active() ? "true" : "false",
             snapshot.host_online ? "true" : "false",
             (unsigned long)snapshot.press_count,
             (unsigned long)snapshot.last_press_ms,
             snapshot.last_heartbeat_age_ms,
             host,
             user,
             (double)snapshot.cpu_pct,
             (double)snapshot.memory_pct,
             (unsigned long)snapshot.host_uptime_s,
             last_report,
             snapshot.last_alert_age_ms,
             alert);

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

static esp_err_t respond_with_servo_config_json(httpd_req_t *req)
{
    servo_control_config_t config;
    if (servo_control_get_config(&config) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "servo config unavailable");
        return ESP_FAIL;
    }

    char json[768];
    snprintf(json,
             sizeof(json),
             "{\"servo_min_angle\":%d,\"servo_max_angle\":%d,\"servo_rest_angle\":%d,\"servo_ready_angle\":%d,"
             "\"short_press_angle\":%d,\"long_press_angle\":%d,\"short_press_ms\":%lu,\"long_press_ms\":%lu,"
             "\"prepare_settle_ms\":%lu,\"release_settle_ms\":%lu}",
             config.min_angle,
             config.max_angle,
             config.rest_angle,
             config.ready_angle,
             config.short_press_angle,
             config.long_press_angle,
             (unsigned long)config.short_press_ms,
             (unsigned long)config.long_press_ms,
             (unsigned long)config.prepare_settle_ms,
             (unsigned long)config.release_settle_ms);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t respond_with_wifi_json(httpd_req_t *req)
{
    wifi_service_profiles_snapshot_t snapshot;
    if (wifi_service_get_profiles_snapshot(&snapshot) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi status unavailable");
        return ESP_FAIL;
    }

    char json[2048];
    size_t offset = 0;
    offset = appendf(json,
                     sizeof(json),
                     offset,
                     "{\"wifi_connected\":%s,\"active_slot\":%d,\"profiles\":[",
                     wifi_service_is_connected() ? "true" : "false",
                     snapshot.active_slot);

    for (size_t index = 0; index < snapshot.count; index++) {
        char ssid[96];
        json_escape_copy(ssid, sizeof(ssid), snapshot.profiles[index].ssid);
        offset = appendf(json,
                         sizeof(json),
                         offset,
                         "%s{\"slot\":%d,\"configured\":%s,\"active\":%s,\"loaded_from_nvs\":%s,\"ssid\":\"%s\"}",
                         index == 0 ? "" : ",",
                         snapshot.profiles[index].slot,
                         snapshot.profiles[index].configured ? "true" : "false",
                         snapshot.profiles[index].active ? "true" : "false",
                         snapshot.profiles[index].loaded_from_nvs ? "true" : "false",
                         ssid);
    }

    appendf(json, sizeof(json), offset, "]}");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t respond_with_ota_json(httpd_req_t *req)
{
    ota_service_status_t status;
    ota_service_get_status(&status);

    char ota_status[48];
    char detail[144];
    char url[256];
    json_escape_copy(ota_status, sizeof(ota_status), status.status);
    json_escape_copy(detail, sizeof(detail), status.detail);
    json_escape_copy(url, sizeof(url), status.url);

    char json[640];
    snprintf(json,
             sizeof(json),
             "{\"in_progress\":%s,\"rollback_pending_verify\":%s,\"last_success\":%s,"
             "\"last_error\":%d,\"status\":\"%s\",\"detail\":\"%s\",\"url\":\"%s\"}",
             status.in_progress ? "true" : "false",
             status.rollback_pending_verify ? "true" : "false",
             status.last_success ? "true" : "false",
             status.last_error,
             ota_status,
             detail,
             url);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t queue_press_request(const press_request_t *request)
{
    if (request == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_press_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    press_request_t *request_copy = calloc(1, sizeof(*request_copy));
    if (request_copy == NULL) {
        return ESP_ERR_NO_MEM;
    }
    *request_copy = *request;

    xSemaphoreTake(s_press_lock, portMAX_DELAY);
    if (s_press_in_progress) {
        xSemaphoreGive(s_press_lock);
        free(request_copy);
        return ESP_ERR_INVALID_STATE;
    }
    s_press_in_progress = true;
    xSemaphoreGive(s_press_lock);

    if (xTaskCreate(press_worker_task, "press_worker", 4096, request_copy, 5, NULL) != pdPASS) {
        xSemaphoreTake(s_press_lock, portMAX_DELAY);
        s_press_in_progress = false;
        xSemaphoreGive(s_press_lock);
        free(request_copy);
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void press_worker_task(void *arg)
{
    press_request_t request = *(press_request_t *)arg;
    free(arg);

    esp_err_t err;
    if (request.use_mode) {
        err = servo_control_press_mode(request.mode);
    } else {
        err = servo_control_press_custom(request.press_angle, request.press_ms);
    }

    if (err == ESP_OK) {
        status_store_set_servo_angle(servo_control_get_angle());
        status_store_record_press(request.press_ms);
        ESP_LOGI(TAG,
                 "Servo press finished angle=%d ms=%lu mode=%s",
                 request.press_angle,
                 (unsigned long)request.press_ms,
                 request.use_mode
                     ? (request.mode == SERVO_CONTROL_PRESS_MODE_LONG ? "long" : "short")
                     : "custom");
    } else {
        ESP_LOGE(TAG, "Servo press failed in background task: %s", esp_err_to_name(err));
    }

    xSemaphoreTake(s_press_lock, portMAX_DELAY);
    s_press_in_progress = false;
    xSemaphoreGive(s_press_lock);
    vTaskDelete(NULL);
}

static esp_err_t root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, s_control_page_html);
}

static esp_err_t api_help_handler(httpd_req_t *req)
{
    static const char *help_text =
        "remote_power_controller\n"
        "GET  /\n"
        "GET  /api/status\n"
        "GET  /api/help\n"
        "GET  /api/wifi\n"
        "GET  /api/ota\n"
        "GET  /api/servo/config\n"
        "GET  /api/report\n"
        "POST /api/wifi/config\n"
        "POST /api/wifi/clear\n"
        "POST /api/servo?angle=45\n"
        "POST /api/servo/config\n"
        "POST /api/press?mode=short|long\n"
        "POST /api/press?angle=45&ms=350\n"
        "POST /api/ota?url=https://example.com/firmware.bin\n"
        "POST /api/report?source=NAME&payload=JSON_STRING\n"
        "POST /api/heartbeat?host=NB01&user=alice&cpu=22.4&mem=51.8&uptime=86400\n"
        "POST /api/alert/test\n";

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, help_text);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }
    return respond_with_status_json(req);
}

static esp_err_t wifi_status_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }
    return respond_with_wifi_json(req);
}

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }
    return respond_with_ota_json(req);
}

static esp_err_t ota_start_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    char body[1024];
    char url[192] = "";
    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body");
        return err;
    }

    if (!get_form_or_query_value(req, body, "url", url, sizeof(url))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing url");
        return ESP_FAIL;
    }

    err = ota_service_start(url);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "url must be a valid https URL");
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "OTA already running or service unavailable");
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota start failed");
        return err;
    }

    httpd_resp_set_status(req, "202 Accepted");
    return respond_with_ota_json(req);
}

static esp_err_t wifi_config_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    char body[1024];
    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body");
        return err;
    }

    wifi_service_profile_t profiles[WIFI_SERVICE_MAX_PROFILES];
    err = wifi_service_get_saved_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi profile load failed");
        return err;
    }

    bool saw_multi = false;
    for (int slot = 0; slot < WIFI_SERVICE_MAX_PROFILES; slot++) {
        char key[16];
        char requested_ssid[WIFI_SERVICE_SSID_MAX_LEN + 1] = "";
        char requested_password[WIFI_SERVICE_PASSWORD_MAX_LEN + 1] = "";

        snprintf(key, sizeof(key), "ssid%d", slot);
        const bool has_ssid = get_form_or_query_value(req, body, key, requested_ssid, sizeof(requested_ssid));
        snprintf(key, sizeof(key), "password%d", slot);
        const bool has_password =
            get_form_or_query_value(req, body, key, requested_password, sizeof(requested_password));

        if (!has_ssid && !has_password) {
            continue;
        }

        saw_multi = true;

        if (has_ssid) {
            if (requested_ssid[0] == '\0') {
                memset(&profiles[slot], 0, sizeof(profiles[slot]));
                continue;
            }

            const bool same_ssid = strcmp(profiles[slot].ssid, requested_ssid) == 0;
            strlcpy(profiles[slot].ssid, requested_ssid, sizeof(profiles[slot].ssid));

            if (has_password) {
                strlcpy(profiles[slot].password, requested_password, sizeof(profiles[slot].password));
            } else if (!same_ssid) {
                profiles[slot].password[0] = '\0';
            }
        } else if (profiles[slot].ssid[0] != '\0') {
            strlcpy(profiles[slot].password, requested_password, sizeof(profiles[slot].password));
        }
    }

    if (saw_multi) {
        err = wifi_service_set_profiles(profiles, WIFI_SERVICE_MAX_PROFILES);
        if (err == ESP_ERR_INVALID_ARG) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid wifi profile data");
            return err;
        }
        if (err != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi reconfigure failed");
            return err;
        }

        ESP_LOGI(TAG, "Wi-Fi runtime profiles updated over HTTP");
        return respond_with_wifi_json(req);
    }

    char ssid[WIFI_SERVICE_SSID_MAX_LEN + 1] = "";
    char password[WIFI_SERVICE_PASSWORD_MAX_LEN + 1] = "";
    if (!get_form_or_query_value(req, body, "ssid", ssid, sizeof(ssid))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing ssid");
        return ESP_FAIL;
    }
    (void)get_form_or_query_value(req, body, "password", password, sizeof(password));

    err = wifi_service_update_credentials(ssid, password);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid ssid or password length");
        return err;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi reconfigure failed");
        return err;
    }

    ESP_LOGI(TAG, "Wi-Fi primary slot updated over HTTP for SSID=%s", ssid);
    return respond_with_wifi_json(req);
}

static esp_err_t wifi_clear_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    esp_err_t err = wifi_service_clear_credentials();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "wifi clear failed");
        return err;
    }

    ESP_LOGI(TAG, "Stored runtime Wi-Fi profiles cleared over HTTP");
    return respond_with_wifi_json(req);
}

static esp_err_t wifi_ap_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    esp_err_t err = wifi_service_force_fallback_ap();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "fallback ap enable failed");
        return err;
    }

    ESP_LOGI(TAG, "Fallback setup hotspot enabled over HTTP");
    return respond_with_status_json(req);
}

static esp_err_t wifi_sta_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    esp_err_t err = wifi_service_force_station_retry();
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "station retry failed");
        return err;
    }

    ESP_LOGI(TAG, "Station retry triggered over HTTP");
    return respond_with_status_json(req);
}

static esp_err_t servo_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    char angle_text[16];
    if (!get_query_value(req, "angle", angle_text, sizeof(angle_text))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing angle");
        return ESP_FAIL;
    }

    esp_err_t err = servo_control_set_angle((int)strtol(angle_text, NULL, 10));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "servo set failed");
        return err;
    }

    status_store_set_servo_angle(servo_control_get_angle());
    return respond_with_status_json(req);
}

static esp_err_t servo_config_get_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }
    return respond_with_servo_config_json(req);
}

static esp_err_t servo_config_post_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    char body[1024];
    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body");
        return err;
    }

    servo_control_config_t config;
    if (servo_control_get_config(&config) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "servo config unavailable");
        return ESP_FAIL;
    }

    bool has_update = false;
    char value[24];
    if (get_form_or_query_value(req, body, "rest_angle", value, sizeof(value))) {
        config.rest_angle = (int)strtol(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "ready_angle", value, sizeof(value))) {
        config.ready_angle = (int)strtol(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "short_press_angle", value, sizeof(value))) {
        config.short_press_angle = (int)strtol(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "long_press_angle", value, sizeof(value))) {
        config.long_press_angle = (int)strtol(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "press_angle", value, sizeof(value))) {
        config.short_press_angle = (int)strtol(value, NULL, 10);
        config.long_press_angle = (int)strtol(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "short_press_ms", value, sizeof(value))) {
        config.short_press_ms = (uint32_t)strtoul(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "long_press_ms", value, sizeof(value))) {
        config.long_press_ms = (uint32_t)strtoul(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "prepare_settle_ms", value, sizeof(value))) {
        config.prepare_settle_ms = (uint32_t)strtoul(value, NULL, 10);
        has_update = true;
    }
    if (get_form_or_query_value(req, body, "release_settle_ms", value, sizeof(value))) {
        config.release_settle_ms = (uint32_t)strtoul(value, NULL, 10);
        has_update = true;
    }

    if (!has_update) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing servo config values");
        return ESP_FAIL;
    }

    bool move_to_rest = false;
    if (get_form_or_query_value(req, body, "move_to_rest", value, sizeof(value))) {
        move_to_rest = (strcmp(value, "1") == 0 || strcmp(value, "true") == 0);
    }

    err = servo_control_update_config(&config, true, move_to_rest);
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid servo config values");
        return err;
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "servo config update failed");
        return err;
    }

    status_store_set_servo_angle(servo_control_get_angle());
    return respond_with_status_json(req);
}

static esp_err_t press_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    servo_control_config_t config;
    if (servo_control_get_config(&config) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "servo config unavailable");
        return ESP_FAIL;
    }

    int press_angle = config.short_press_angle;
    uint32_t press_ms = config.short_press_ms;
    bool use_long_mode = false;
    bool has_mode = false;
    bool has_override = false;
    char value[24];

    if (get_query_value(req, "mode", value, sizeof(value))) {
        has_mode = true;
        if (strcmp(value, "long") == 0) {
            press_angle = config.long_press_angle;
            press_ms = config.long_press_ms;
            use_long_mode = true;
        } else if (strcmp(value, "short") == 0) {
            press_angle = config.short_press_angle;
            press_ms = config.short_press_ms;
        } else {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mode must be short or long");
            return ESP_FAIL;
        }
    }

    if (get_query_value(req, "angle", value, sizeof(value))) {
        press_angle = (int)strtol(value, NULL, 10);
        has_override = true;
    }
    if (get_query_value(req, "ms", value, sizeof(value))) {
        press_ms = (uint32_t)strtoul(value, NULL, 10);
        has_override = true;
    }

    if (press_ms == 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "press duration must be greater than zero");
        return ESP_FAIL;
    }

    press_request_t request = {
        .use_mode = has_mode && !has_override,
        .mode = use_long_mode ? SERVO_CONTROL_PRESS_MODE_LONG : SERVO_CONTROL_PRESS_MODE_SHORT,
        .press_angle = press_angle,
        .press_ms = press_ms,
    };

    esp_err_t err = queue_press_request(&request);

    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid press arguments");
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        return httpd_resp_sendstr(req, "servo press already in progress");
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "servo press queue failed");
        return err;
    }

    char json[160];
    snprintf(json,
             sizeof(json),
             "{\"queued\":true,\"mode\":\"%s\",\"press_angle\":%d,\"press_ms\":%lu}",
             request.use_mode ? (use_long_mode ? "long" : "short") : "custom",
             press_angle,
             (unsigned long)press_ms);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t report_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    char body[2048];
    char source[32] = "external_script";
    char payload[1024] = "";

    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body");
        return err;
    }

    (void)get_form_or_query_value(req, body, "source", source, sizeof(source));
    if (!get_form_or_query_value(req, body, "payload", payload, sizeof(payload))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing payload");
        return ESP_FAIL;
    }

    status_store_record_report(source, payload);
    return respond_with_status_json(req);
}

static esp_err_t report_status_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    app_status_snapshot_t snapshot;
    status_store_snapshot(&snapshot);

    char source[64];
    char payload[640];
    json_escape_copy(source, sizeof(source), snapshot.report_source);
    json_escape_copy(payload, sizeof(payload), snapshot.last_report);

    char json[1400];
    snprintf(json,
             sizeof(json),
             "{\"host_online\":%s,\"last_heartbeat_age_ms\":%lld,"
             "\"report_source\":\"%s\",\"payload\":\"%s\"}",
             snapshot.host_online ? "true" : "false",
             snapshot.last_heartbeat_age_ms,
             source,
             payload);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t heartbeat_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    char body[2048];
    char payload[1024] = "";
    char source[32] = "heartbeat_script";

    esp_err_t err = read_request_body(req, body, sizeof(body));
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body");
        return err;
    }

    if (get_form_or_query_value(req, body, "payload", payload, sizeof(payload))) {
        (void)get_form_or_query_value(req, body, "source", source, sizeof(source));
        status_store_record_report(source, payload);
        return respond_with_status_json(req);
    }

    char host_name[32] = "unknown";
    char user_name[32] = "";
    char cpu_string[16] = "0";
    char mem_string[16] = "0";
    char uptime_string[16] = "0";

    (void)get_form_or_query_value(req, body, "host", host_name, sizeof(host_name));
    (void)get_form_or_query_value(req, body, "user", user_name, sizeof(user_name));
    (void)get_form_or_query_value(req, body, "cpu", cpu_string, sizeof(cpu_string));
    (void)get_form_or_query_value(req, body, "mem", mem_string, sizeof(mem_string));
    (void)get_form_or_query_value(req, body, "uptime", uptime_string, sizeof(uptime_string));

    status_store_record_heartbeat(host_name,
                                  user_name,
                                  strtof(cpu_string, NULL),
                                  strtof(mem_string, NULL),
                                  (uint32_t)strtoul(uptime_string, NULL, 10));
    return respond_with_status_json(req);
}

static esp_err_t alert_test_handler(httpd_req_t *req)
{
    if (!request_is_authorized(req)) {
        return ESP_FAIL;
    }

    esp_err_t err = alert_service_send_webhook("manual_test", "Test alert triggered from HTTP API");
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "alert send failed");
        return err;
    }
    return respond_with_status_json(req);
}

esp_err_t http_api_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    if (s_press_lock == NULL) {
        s_press_lock = xSemaphoreCreateMutex();
        if (s_press_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 20;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "http server start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t root_uri = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
    const httpd_uri_t status_uri = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
    const httpd_uri_t help_uri = { .uri = "/api/help", .method = HTTP_GET, .handler = api_help_handler };
    const httpd_uri_t wifi_uri = { .uri = "/api/wifi", .method = HTTP_GET, .handler = wifi_status_handler };
    const httpd_uri_t ota_get_uri = { .uri = "/api/ota", .method = HTTP_GET, .handler = ota_status_handler };
    const httpd_uri_t ota_post_uri = { .uri = "/api/ota", .method = HTTP_POST, .handler = ota_start_handler };
    const httpd_uri_t wifi_config_uri = { .uri = "/api/wifi/config", .method = HTTP_POST, .handler = wifi_config_handler };
    const httpd_uri_t wifi_clear_uri = { .uri = "/api/wifi/clear", .method = HTTP_POST, .handler = wifi_clear_handler };
    const httpd_uri_t wifi_ap_uri = { .uri = "/api/wifi/ap", .method = HTTP_POST, .handler = wifi_ap_handler };
    const httpd_uri_t wifi_sta_uri = { .uri = "/api/wifi/sta", .method = HTTP_POST, .handler = wifi_sta_handler };
    const httpd_uri_t servo_uri = { .uri = "/api/servo", .method = HTTP_POST, .handler = servo_handler };
    const httpd_uri_t servo_config_get_uri = { .uri = "/api/servo/config", .method = HTTP_GET, .handler = servo_config_get_handler };
    const httpd_uri_t servo_config_post_uri = { .uri = "/api/servo/config", .method = HTTP_POST, .handler = servo_config_post_handler };
    const httpd_uri_t press_uri = { .uri = "/api/press", .method = HTTP_POST, .handler = press_handler };
    const httpd_uri_t report_get_uri = { .uri = "/api/report", .method = HTTP_GET, .handler = report_status_handler };
    const httpd_uri_t report_post_uri = { .uri = "/api/report", .method = HTTP_POST, .handler = report_handler };
    const httpd_uri_t heartbeat_uri = { .uri = "/api/heartbeat", .method = HTTP_POST, .handler = heartbeat_handler };
    const httpd_uri_t alert_uri = { .uri = "/api/alert/test", .method = HTTP_POST, .handler = alert_test_handler };

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &help_uri);
    httpd_register_uri_handler(s_server, &wifi_uri);
    httpd_register_uri_handler(s_server, &ota_get_uri);
    httpd_register_uri_handler(s_server, &ota_post_uri);
    httpd_register_uri_handler(s_server, &wifi_config_uri);
    httpd_register_uri_handler(s_server, &wifi_clear_uri);
    httpd_register_uri_handler(s_server, &wifi_ap_uri);
    httpd_register_uri_handler(s_server, &wifi_sta_uri);
    httpd_register_uri_handler(s_server, &servo_uri);
    httpd_register_uri_handler(s_server, &servo_config_get_uri);
    httpd_register_uri_handler(s_server, &servo_config_post_uri);
    httpd_register_uri_handler(s_server, &press_uri);
    httpd_register_uri_handler(s_server, &report_get_uri);
    httpd_register_uri_handler(s_server, &report_post_uri);
    httpd_register_uri_handler(s_server, &heartbeat_uri);
    httpd_register_uri_handler(s_server, &alert_uri);

    ESP_LOGI(TAG, "HTTP API started");
    return ESP_OK;
}
