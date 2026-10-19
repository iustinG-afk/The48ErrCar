#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_camera.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_timer.h"
#include "rom/ets_sys.h"

// PIN CONNECTIONS
#define PIN_SERVO      4    // IO4
#define PIN_DHT        5    // IO5
#define PIN_MOTOR_FWD  6    // IO6
#define PIN_MOTOR_REV  7    // IO7
#define PIN_US_TRIG    15   // IO15
#define PIN_US_ECHO    16   // IO16
#define PIN_PUMP      17   // IO17
#define PIN_BUZZER     18   // IO18
#define PIN_BACKLIGHTS      40   // IO40
#define PIN_SIGNAL_R   41   // IO41
#define PIN_HEADLIGHTS     42   // IO42
#define PIN_SIGNAL_L   2    // IO2

// PIN MICROPHONE
#define MIC_CHANNEL ADC_CHANNEL_0 // IO1

// PIN MAPPING CAMERA
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_SIOD    8    // IO8
#define CAM_PIN_SIOC    3    // IO3
#define CAM_PIN_HREF    9    // IO9
#define CAM_PIN_D7      10   // IO10
#define CAM_PIN_XCLK    11   // IO11
#define CAM_PIN_D6      12   // IO12
#define CAM_PIN_D5      13   // IO13
#define CAM_PIN_PCLK    14   // IO14
#define CAM_PIN_VSYNC   46   // IO46
#define CAM_PIN_D4      47   // IO47
#define CAM_PIN_D0      48   // IO48
#define CAM_PIN_D3      45   // IO45
#define CAM_PIN_D1      38   // IO38
#define CAM_PIN_D2      39   // IO39

// BUZZER LOGIC
#define BUZZER_MUTE 0
#define BUZZER_RING 1

static const char *TAG = "MASINA_ULTRA_TUNED";

// Shared state between the web handlers and the background tasks.
// 'volatile' because they're written and read from different tasks.
volatile bool signal_l_active = false;
volatile bool signal_r_active = false;
volatile bool backwards = false;
volatile bool stationary = true;
volatile bool manual_stop = false;        // brake lights forced on from the app
volatile bool back_stop = false;          // reverse blocked by the rear sensor
volatile bool manual_horn = false; 
volatile bool back_command_active = false; // the user is currently asking to reverse

volatile bool microphone_active = false;
volatile int64_t auto_off_temp = 0;       // time (us) when mic-triggered signals turn off; 0 = no timeout

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "WiFi AP PORNIT - Conectati la: The48ErrCar");
    } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client conectat!");
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        ESP_LOGI(TAG, "Client deconectat!");
    }
}

// HTML PAGE / CSS / JS
// The whole control UI is served from flash as one string, so no filesystem is needed.
const char *html_page = 
"<!DOCTYPE html><html lang='ro'><head><meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1,user-scalable=no'>"
"<style>"
"body,html{margin:0;padding:0;height:100%;width:100%;background:radial-gradient(circle at center,#1a0836 0%,#080310 100%);overflow:hidden;font-family:Arial,sans-serif;user-select:none;-webkit-user-select:none}"
"#fpv-box{display:none;position:absolute;top:46%;left:50%;transform:translate(-50%,-50%);width:55vw;max-width:420px;height:55vh;max-height:280px;border:2px solid #b026ff;border-radius:16px;overflow:hidden;box-shadow:0 0 25px rgba(176,38,255,.5),inset 0 0 10px #b026ff;background:#000;z-index:2;pointer-events:none}"
"#cam{width:100%;height:100%;object-fit:cover}"
"#ui{position:absolute;top:0;left:0;width:100%;height:100%;z-index:10;display:flex;flex-direction:column;justify-content:space-between;pointer-events:none}"
".top-bar{display:flex;justify-content:space-between;padding:12px;background:linear-gradient(to bottom,rgba(10,3,20,.9),transparent);pointer-events:auto}"
".btn-icon{--glow:#b026ff;--fg:#d8b4fe;background:rgba(120,40,200,.15);border:2px solid var(--glow);border-radius:12px;margin:2px;padding:8px;cursor:pointer;box-shadow:0 0 10px color-mix(in srgb,var(--glow) 45%,transparent);display:flex;justify-content:center;align-items:center;width:46px;height:46px;transition:.15s ease;color:var(--fg)}"
".btn-icon svg{width:24px;height:24px;fill:currentColor;filter:drop-shadow(0 0 3px var(--glow))}"
".btn-icon.act{background:var(--glow);color:#fff;box-shadow:0 0 22px var(--glow);transform:scale(1.06)}"
".btn-icon:active{transform:scale(.9)}"
".b-far{--glow:#fff;--fg:#f5f5ff}.b-stop{--glow:#ff2b4d;--fg:#ffb3bd}.b-semnal{--glow:#ffb020;--fg:#ffd580}.b-claxon{--glow:#ff4444;--fg:#ffb3b3}.b-apa{--glow:#38bdf8;--fg:#bde8fd}.b-cam{--glow:#b026ff;--fg:#d8b4fe}.b-mic{--glow:#39ff6a;--fg:#bdfbcc}"
".controls{display:flex;justify-content:space-between;align-items:flex-end;padding:12px 20px;flex-grow:1;pointer-events:auto}"
".joy{width:130px;height:130px;background:rgba(120,40,200,.05);border:3px solid rgba(176,38,255,.3);border-radius:50%;position:relative;display:flex;justify-content:center;align-items:center;touch-action:none;box-shadow:inset 0 0 20px rgba(176,38,255,.2)}"
".knob{width:48px;height:48px;background:rgba(176,38,255,.7);border-radius:50%;position:absolute;pointer-events:none;box-shadow:0 0 20px #b026ff;transition:transform .05s linear}"
".hud{display:flex;flex-direction:column;align-items:center;background:rgba(10,3,20,.85);padding:6px 10px;border-radius:12px;border:1px solid rgba(176,38,255,.6);box-shadow:0 0 15px rgba(176,38,255,.25)}"
".sl-cont{color:#d8b4fe;font-size:10px;text-align:center;font-weight:700;margin-bottom:2px}"
".speed-slider-container{display:flex;flex-direction:column;align-items:center;width:110px}"
"input[type=range]{-webkit-appearance:none;width:100%;height:4px;background:rgba(176,38,255,.3);border-radius:2px;outline:0;accent-color:#b026ff;margin:3px 0}"
"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:14px;height:14px;border-radius:50%;background:#d8b4fe;cursor:pointer;box-shadow:0 0 8px #b026ff;transition:transform .1s}"
"input[type=range]::-webkit-slider-thumb:hover{transform:scale(1.2)}"
".speed-marks{display:flex;justify-content:space-between;width:100%;font-size:9px;color:#b892ff;font-weight:700;padding:0 2px}"
".hud-row{display:flex;gap:4px;margin-top:5px}"
".dht-box{display:flex;align-items:center;gap:4px;background:rgba(57,255,106,.12);padding:3px 8px;border-radius:6px;border:1px solid rgba(57,255,106,.5);box-shadow:0 0 8px rgba(57,255,106,.15);color:#39ff6a;font-weight:700;font-size:10px;cursor:pointer}"
".dht-box svg{width:12px;height:12px;fill:#39ff6a;filter:drop-shadow(0 0 3px #39ff6a)}"
"#conn{display:flex;align-items:center;gap:5px;background:rgba(120,40,200,.12);padding:3px 8px;border-radius:6px;border:1px solid rgba(176,38,255,.4);box-shadow:0 0 6px rgba(176,38,255,.1);color:#d8b4fe;font-weight:700;font-size:10px;cursor:default}"
"#conn .dot{width:8px;height:8px;border-radius:50%;background:#555;box-shadow:0 0 4px #555;transition:.3s}"
"#conn.ok .dot{background:#39ff6a;box-shadow:0 0 8px #39ff6a}#conn.bad .dot{background:#ff3b3b;box-shadow:0 0 8px #ff3b3b}"
"</style></head><body>"
"<div id='fpv-box'><img id='cam' src=''></div>"
"<div id='ui'>"
  "<div class='top-bar'>"
    "<div style='display:flex;flex-direction:column;gap:4px'>"
      "<div style='display:flex;gap:4px'>"
        "<button class='btn-icon b-far' id='lf' onclick=\"tg('lf',this)\" title='Faruri'><svg viewBox='0 0 24 24'><path d='M12 2a7 7 0 0 0-7 7c0 2.38 1.19 4.47 3 5.74V17a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1v-2.26c1.81-1.27 3-3.36 3-5.74a7 7 0 0 0-7-7zm2 17h-4v1a1 1 0 0 0 1 1h2a1 1 0 0 0 1-1v-1z'/></svg></button>"
        "<button class='btn-icon b-stop' id='lr' onclick=\"tg('lr',this)\" title='Stopuri'><svg viewBox='0 0 24 24'><path d='M12 2a7 7 0 0 0-7 7c0 2.38 1.19 4.47 3 5.74V17a1 1 0 0 0 1 1h6a1 1 0 0 0 1-1v-2.26c1.81-1.27 3-3.36 3-5.74a7 7 0 0 0-7-7zm2 17h-4v1a1 1 0 0 0 1 1h2a1 1 0 0 0 1-1v-1z'/></svg></button>"
      "</div>"
      "<div style='display:flex;gap:4px'>"
        "<button class='btn-icon b-semnal' id='sl' onclick=\"tg('sl',this)\" title='Stânga'><svg viewBox='0 0 24 24'><path d='M9 7v3h7v4H9v3l-5-5 5-5z'/></svg></button>"
        "<button class='btn-icon b-semnal' id='sr' onclick=\"tg('sr',this)\" title='Dreapta'><svg viewBox='0 0 24 24'><path d='M15 7v3H8v4h7v3l5-5-5-5z'/></svg></button>"
      "</div>"
    "</div>"
    "<div style='display:flex;flex-direction:column;align-items:flex-end;gap:4px'>"
      "<div style='display:flex;gap:4px'>"
        "<button class='btn-icon b-mic' id='m' onclick=\"tg('m',this)\" title='Microfon ON/OFF'><svg viewBox='0 0 24 24'><path d='M12 14c1.66 0 3-1.34 3-3V5c0-1.66-1.34-3-3-3S9 3.34 9 5v6c0 1.66 1.34 3 3 3zm5-3c0 2.76-2.24 5-5 5s-5-2.24-5-5H5c0 3.53 2.61 6.43 6 6.92V21h2v-3.08c3.39-.49 6-3.39 6-6.92h-2z'/></svg></button>"
        "<button class='btn-icon b-cam' id='btnCam' onclick='tgCam(this)' title='FPV'><svg viewBox='0 0 24 24'><path d='M17 10.5V7c0-.55-.45-1-1-1H4c-.55 0-1 .45-1 1v10c0 .55.45 1 1 1h12c.55 0 1-.45 1-1v-3.5l4 4v-11l-4 4z'/></svg></button>"
      "</div>"
      "<div style='display:flex;gap:4px'>"
        "<button class='btn-icon b-claxon' oncontextmenu=\"event.preventDefault();\" ontouchstart=\"event.preventDefault(); cmd('h1');\" ontouchend=\"event.preventDefault(); cmd('h0');\" onmousedown=\"cmd('h1')\" onmouseup=\"cmd('h0')\" onmouseleave=\"cmd('h0')\" title='Claxon'><svg viewBox='0 0 24 24'><path d='M3 9v6h4l5 5V4L7 9H3zm13.5 3c0-1.77-1.02-3.29-2.5-4.03v8.05c1.48-.73 2.5-2.25 2.5-4.02z'/></svg></button>"
        "<button class='btn-icon b-apa' id='p' onclick=\"tg('p',this)\" title='Apă'><svg viewBox='0 0 24 24'><path d='M12 2.69l5.66 5.66a8 8 0 1 1-11.31 0z'/></svg></button>"
      "</div>"
    "</div>"
  "</div>"
  "<div class='controls'>"
    "<div class='joy' id='jL'><div class='knob' id='kL'></div></div>"
    "<div class='hud'>"
      "<div class='sl-cont'>Viteză: <span id='p_val'>50</span>%</div>"
      "<div class='speed-slider-container'>"
        "<input type='range' id='pwr' min='50' max='100' step='25' value='50' oninput=\"updateSpeed(this.value)\">"
        "<div class='speed-marks'><span>50</span><span>75</span><span>100</span></div>"
      "</div>"
      "<div class='hud-row'>"
        "<div class='dht-box' onclick='readD()'>"
          "<svg viewBox='0 0 24 24'><path d='M15 13V5a3 3 0 0 0-6 0v8a5 5 0 1 0 6 0zm-3-2a2 2 0 1 1 2-2 2 2 0 0 1-2 2z'/></svg>"
          "<span id='dht_d'>--°C|--%</span>"
        "</div>"
        "<div id='conn'><div class='dot'></div><span id='conn_t'>...</span></div>"
      "</div>"
    "</div>"
    "<div class='joy' id='jR'><div class='knob' id='kR'></div></div>"
  "</div>"
"</div>"
"<script>"
  // toggle button states (lights, signals, pump, mic)
  "let st={lf:0,lr:0,sl:0,sr:0,p:0,m:0};"
  "function updateSpeed(v){document.getElementById('p_val').innerText=v}"
  "function cmd(c){fetch('/cmd?c='+c,{cache:'no-store',keepalive:true}).then(()=>setConn(1)).catch(()=>setConn(0))}"
  "function tg(k,b){st[k]=!st[k];cmd(k+(st[k]?'1':'0'));b.classList.toggle('act',st[k])}"
  
  "let connEl=document.getElementById('conn'),connT=document.getElementById('conn_t');"
  "function setConn(o){connEl.classList.remove('ok','bad');connEl.classList.add(o?'ok':'bad');connT.innerText=o?'Conectat':'Fără semnal'}"
  
  // the camera stream runs on its own server (port 8080); auto-retry if it drops
  "let camOn=!1,camRetry=null;"
  "function tgCam(b){camOn=!camOn;let f=document.getElementById('fpv-box');if(camOn){f.style.display='block';startCam();b.classList.add('act')}else{if(camRetry)clearTimeout(camRetry);camRetry=null;f.style.display='none';document.getElementById('cam').src='';b.classList.remove('act')}}"
  "function startCam(){document.getElementById('cam').src='http://'+window.location.hostname+':8080/stream?t='+Date.now()}"
  "document.getElementById('cam').addEventListener('error',()=>{if(camOn){if(camRetry)clearTimeout(camRetry);camRetry=setTimeout(startCam,1500)}});"
  
  "function readD(){document.getElementById('dht_d').innerText='...';fetch('/dht').then(r=>r.text()).then(t=>{document.getElementById('dht_d').innerText=t;setConn(1)}).catch(()=>setConn(0))}"
  
  // Drive loop: sends joystick values every 65ms, but only if they changed
  // (or as a 400ms heartbeat). isBusy stops requests from piling up on a slow link.
  "let targetF=0,targetR=0,targetSt=614,lastF=-1,lastR=-1,lastSt=-1,isBusy=!1,lastT=0,pwr=document.getElementById('pwr');"
  "setInterval(()=>{"
    "if(isBusy)return;"
    "let ch=(targetF!==lastF||targetR!==lastR||targetSt!==lastSt);"
    "if(!ch&&(Date.now()-lastT<400))return;"
    "isBusy=!0;lastF=targetF;lastR=targetR;lastSt=targetSt;lastT=Date.now();"
    "fetch(`/cmd?c=ctrl&f=${targetF}&r=${targetR}&st=${targetSt}`,{cache:'no-store',keepalive:true})"
    ".then(()=>{isBusy=!1;setConn(1)}).catch(()=>{isBusy=!1;setConn(0)});"
  "},65);"

  // Generic joystick: constrained to one axis, returns a value from -1 to 1 via cb
  "function sJoy(j,k,ax,cb){"
    "let act=!1,cx,cy,r;"
    "j.addEventListener('pointerdown',e=>{act=!0;let b=j.getBoundingClientRect();cx=b.left+b.width/2;cy=b.top+b.height/2;r=b.width/2;j.setPointerCapture(e.pointerId);mv(e)});"
    "j.addEventListener('pointermove',e=>{if(act)mv(e)});"
    "const end=e=>{act=!1;k.style.transform='translate(0,0)';cb(0)};"
    "j.addEventListener('pointerup',end);j.addEventListener('pointercancel',end);"
    "function mv(e){let dx=e.clientX-cx,dy=e.clientY-cy,d=Math.sqrt(dx*dx+dy*dy);if(d>r){dx=dx*r/d;dy=dy*r/d}if(ax==='y')dx=0;if(ax==='x')dy=0;k.style.transform=`translate(${dx}px,${dy}px)`;cb(ax==='y'?-dy/r:dx/r)}"
  "}"
  // left joystick = throttle: up -> forward PWM (0-255), down -> reverse PWM, scaled by the speed slider
  "sJoy(document.getElementById('jL'),document.getElementById('kL'),'y',v=>{let mp=(parseInt(pwr.value)/100)*255,val=Math.round(v*mp);targetF=0;targetR=0;if(val>0)targetF=val;else if(val<0)targetR=-val});"
  // right joystick = steering: servo duty 614 (center) +/- 180
  "sJoy(document.getElementById('jR'),document.getElementById('kR'),'x',v=>{targetSt=Math.round(614+(v*180))});"
"</script></body></html>";

// VIDEO STREAMING
// MJPEG: one long HTTP response made of JPEG frames separated by a boundary string
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
    camera_fb_t * fb = NULL;
    esp_err_t res = ESP_OK;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if(res != ESP_OK) return res;
    
    while(true) {
        fb = esp_camera_fb_get();
        if (!fb) { res = ESP_FAIL; break; }
        
        char part_buf[64];
        size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, fb->len);
        
        if(res == ESP_OK) res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        if(res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        if(res == ESP_OK) res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(60));  // caps the frame rate (~16 fps) to leave WiFi bandwidth for controls
        if(res != ESP_OK) break;        // client disconnected
    }
    return res;
}

// TASK MICROPHONE
// Clap detection: 2 claps = left signal, 3 = right, 4+ = hazards (all auto-off after 2s)
void microfon_task(void *pvParameters) {
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = {
        .unit_id = ADC_UNIT_1,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config1, &adc1_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12, 
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1_handle, MIC_CHANNEL, &config));

    int batai = 0;                  // clap count in the current sequence
    int64_t timp_ultima_bataie = 0; // time of last clap (ms)

    while(1) {
        if (!microphone_active) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        int val_max = 0;
        int val_min = 4095;

        // Sample for ~6ms and take peak-to-peak as the loudness
        for(int i = 0; i < 30; i++) {
            int current_val = 0;
            adc_oneshot_read(adc1_handle, MIC_CHANNEL, &current_val);
            
            // ignore saturated/zero readings
            if (current_val > 0 && current_val < 4095) {
                if(current_val > val_max) val_max = current_val;
                if(current_val < val_min) val_min = current_val;
            }
            esp_rom_delay_us(200); 
        }
        
        int amplitude = 0;
        if (val_max >= val_min) {
            amplitude = val_max - val_min;
        }

        int64_t timp_curent_ms = esp_timer_get_time() / 1000; 
        int64_t timp_curent_us = esp_timer_get_time();

        // Turn off mic-triggered signals once their timeout expires
        if (auto_off_temp > 0 && timp_curent_us > auto_off_temp) {
            signal_l_active = false;
            signal_r_active = false;
            auto_off_temp = 0; 
        }

        // Count a clap if it's loud enough and at least 250ms after the previous one (debounce)
        if (amplitude > 2000) {
            if (timp_curent_ms - timp_ultima_bataie > 250) { 
                batai++;
                timp_ultima_bataie = timp_curent_ms;
                ESP_LOGI(TAG, "👏 %d Cought! (Amplitude: %d)", batai, amplitude);
            }
        }

        // 1.2s of silence ends the sequence; act on the clap count
        if (batai > 0 && (timp_curent_ms - timp_ultima_bataie > 1200)) { 
            // A signal turned on from the app (no timeout set) wins over claps
            bool prioritate_web_activa = (signal_l_active || signal_r_active) && (auto_off_temp == 0);

            if (!prioritate_web_activa) {
                if (batai == 2) {
                    signal_l_active = true; signal_r_active = false;
                    auto_off_temp = timp_curent_us + 2000000; 
                } else if (batai == 3) {
                    signal_r_active = true; signal_l_active = false;
                    auto_off_temp = timp_curent_us + 2000000;
                } else if (batai >= 4) {
                    signal_l_active = true; signal_r_active = true;
                    auto_off_temp = timp_curent_us + 2000000;
                }
            } 
            batai = 0; 
        }
        
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

// ULTRASONIC SENSOR
// HC-SR04 style: 10us trigger pulse, then the echo pulse width is proportional to distance
float distance_cm() {
    gpio_set_level(PIN_US_TRIG, 0);
    esp_rom_delay_us(2);
    gpio_set_level(PIN_US_TRIG, 1);
    esp_rom_delay_us(10);
    gpio_set_level(PIN_US_TRIG, 0);

    // Wait for echo to go high; 30ms timeout means nothing was detected
    int64_t start_time = esp_timer_get_time();
    while (gpio_get_level(PIN_US_ECHO) == 0) {
        if ((esp_timer_get_time() - start_time) > 30000) return 999.0f; 
    }

    int64_t pulse_start = esp_timer_get_time();
    while (gpio_get_level(PIN_US_ECHO) == 1) {
        if ((esp_timer_get_time() - pulse_start) > 30000) break;
    }
    int64_t pulse_end = esp_timer_get_time();

    int64_t duration = pulse_end - pulse_start;
    float distance = (duration * 0.0343f) / 2.0f;  // speed of sound 0.0343 cm/us, halved for the round trip
    return distance;
}

// DRIVING BACKWARDS OBSTACLE DETECTION
// Stops the reverse motor if something is within 5cm while the user is reversing,
// and also drives the buzzer (horn, or a 1s beep on obstacle).
void safety_spate_task(void *pvParameters) {
    int64_t buzzer_start_time = 0;
    bool buzzer_active_limit = false;
    bool obstacle_beep_done = false; 

    while(1) {
        float dist = distance_cm();
        bool buzzer_req_sens = false;
        
        if (back_command_active && dist > 0 && dist <= 5.0f) { 
            back_stop = true;
            
            // cut reverse power immediately instead of waiting for the next app command
            ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
            ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);

            // beep only once (1s) per obstacle, not continuously
            if (!obstacle_beep_done) {
                if (!buzzer_active_limit) {
                    buzzer_start_time = esp_timer_get_time();
                    buzzer_active_limit = true;
                }
                
                if ((esp_timer_get_time() - buzzer_start_time) < 1000000) { 
                    buzzer_req_sens = true;
                } else {
                    obstacle_beep_done = true; 
                }
            }
        } else {
            back_stop = false;
            buzzer_active_limit = false;
            obstacle_beep_done = false;
        }
        
        if (manual_horn || buzzer_req_sens) {
            gpio_set_level(PIN_BUZZER, BUZZER_RING);
        } else {
            gpio_set_level(PIN_BUZZER, BUZZER_MUTE);
        }
        
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// TASK BLINKING SIGNALS
// Also handles the rear lights: on while reversing/blocked, off when stationary
// (unless the user forced them on with manual_stop).
void blinker_task(void *pvParameters) {
    bool stare_bec = false;
    while(1) {
        if (!manual_stop) {
            if (backwards || back_stop) {
                gpio_set_level(PIN_BACKLIGHTS, 1);
            } else if (stationary) {
                gpio_set_level(PIN_BACKLIGHTS, 0);
            }
        }

        if(signal_l_active || signal_r_active) {
            stare_bec = !stare_bec;  // toggles every 500ms
            
            if(signal_l_active) gpio_set_level(PIN_SIGNAL_L, stare_bec ? 1 : 0);
            else gpio_set_level(PIN_SIGNAL_L, 0);

            if(signal_r_active) gpio_set_level(PIN_SIGNAL_R, stare_bec ? 1 : 0);
            else gpio_set_level(PIN_SIGNAL_R, 0);
            
            vTaskDelay(pdMS_TO_TICKS(500)); 
        } else {
            gpio_set_level(PIN_SIGNAL_L, 0);
            gpio_set_level(PIN_SIGNAL_R, 0);
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
    }
}

// HUMIDITY & TEMPERATURE SENSOR
// Bit-banged DHT11 read. Returns 0 on success, -1 on timeout, -2 on checksum error.
int citeste_dht11(int *temp, int *humidity) {
    uint8_t date[5] = {0};  // 2 humidity bytes, 2 temperature bytes, 1 checksum
    // Start signal: pull low for 20ms, then release and listen
    gpio_reset_pin(PIN_DHT);
    gpio_set_direction(PIN_DHT, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_DHT, 0);
    vTaskDelay(pdMS_TO_TICKS(20)); 
    gpio_set_level(PIN_DHT, 1);
    esp_rom_delay_us(30); 
    gpio_set_direction(PIN_DHT, GPIO_MODE_INPUT);
    gpio_set_pull_mode(PIN_DHT, GPIO_PULLUP_ONLY); 
    
    // Timing is in microseconds, so interrupts are disabled while reading
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    portENTER_CRITICAL(&mux); 
    
    // Sensor's response handshake (low -> high -> low)
    int timeout = 0;
    while(gpio_get_level(PIN_DHT) == 1) { if(++timeout > 1000) { portEXIT_CRITICAL(&mux); return -1; } esp_rom_delay_us(1); }
    timeout = 0;
    while(gpio_get_level(PIN_DHT) == 0) { if(++timeout > 1000) { portEXIT_CRITICAL(&mux); return -1; } esp_rom_delay_us(1); }
    timeout = 0;
    while(gpio_get_level(PIN_DHT) == 1) { if(++timeout > 1000) { portEXIT_CRITICAL(&mux); return -1; } esp_rom_delay_us(1); }
    // 40 data bits: the length of the high pulse decides 0 vs 1 (~26us = 0, ~70us = 1)
    for (int i = 0; i < 40; i++) {
        timeout = 0;
        while(gpio_get_level(PIN_DHT) == 0) { if(++timeout > 1000) { portEXIT_CRITICAL(&mux); return -1; } esp_rom_delay_us(1); }
        uint32_t durata = 0;
        while(gpio_get_level(PIN_DHT) == 1) { durata++; if(durata > 1000) { portEXIT_CRITICAL(&mux); return -1; } esp_rom_delay_us(1); }
        if (durata > 40) date[i/8] |= (1 << (7 - (i%8)));
    }
    portEXIT_CRITICAL(&mux); 

    // Checksum = low 8 bits of the sum of the first 4 bytes
    if (date[4] == ((date[0] + date[1] + date[2] + date[3]) & 0xFF)) {
        *humidity = date[0]; *temp = date[2];
        return 0; 
    }
    return -2; 
}

// WEB HANDLER
static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Connection", "close"); 
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Browsers always request this; answer with an empty 204 to avoid a 404 in the log
static esp_err_t favicon_handler(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t dht_handler(httpd_req_t *req) {
    int temp = 0, umid = 0;
    char resp[64];
    int err = -1;
    // DHT11 reads fail occasionally, so retry up to 3 times
    for (int incercare = 0; incercare < 3; incercare++) {
        err = citeste_dht11(&temp, &umid);
        if (err == 0) break;
        vTaskDelay(pdMS_TO_TICKS(25)); 
    }
    if (err == 0) {
        snprintf(resp, sizeof(resp), "%d°C|%d%%", temp, umid);
    } else {
        snprintf(resp, sizeof(resp), "Eroare!");
    }
    httpd_resp_set_hdr(req, "Connection", "close"); 
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// COMMAND HANDLER (APP PRIORITY)
// Single endpoint: /cmd?c=<command>. Toggles use "<name>1"/"<name>0";
// "ctrl" carries drive values: f = forward PWM, r = reverse PWM, st = steering servo duty.
static esp_err_t cmd_handler(httpd_req_t *req) {
    char buf[128];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char cmd[16];
        if (httpd_query_key_value(buf, "c", cmd, sizeof(cmd)) == ESP_OK) {
            if(strcmp(cmd, "ping")==0) { }
            else if(strcmp(cmd, "ctrl")==0) {
                char vf[8], vr[8], vst[8];
                if (httpd_query_key_value(buf, "f", vf, sizeof(vf)) == ESP_OK &&
                    httpd_query_key_value(buf, "r", vr, sizeof(vr)) == ESP_OK &&
                    httpd_query_key_value(buf, "st", vst, sizeof(vst)) == ESP_OK) {
                    
                    int df = atoi(vf);
                    int dr = atoi(vr);
                    int duty = atoi(vst);

                    back_command_active = (dr > 0);  // tells the safety task the user wants to reverse

                    // obstacle detected behind: ignore the reverse request
                    if (back_stop) {
                        dr = 0; 
                    }

                    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, df);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1);
                    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, dr);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);

                    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
                    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);

                    // update the motion state used by the rear lights
                    if (dr > 0) {
                        backwards = true;
                        stationary = false;
                    } else if (df > 0) {
                        backwards = false;
                        stationary = false;
                    } else {
                        backwards = false;
                        stationary = true;
                    }
                }
            }
            else if(strcmp(cmd, "p1")==0) { gpio_set_level(PIN_PUMP, 1); }
            else if(strcmp(cmd, "p0")==0) { gpio_set_level(PIN_PUMP, 0); }
            
            else if(strcmp(cmd, "lf1")==0) { gpio_set_level(PIN_HEADLIGHTS, 1); }
            else if(strcmp(cmd, "lf0")==0) { gpio_set_level(PIN_HEADLIGHTS, 0); }
            else if(strcmp(cmd, "lr1")==0) { manual_stop = true; gpio_set_level(PIN_BACKLIGHTS, 1); }
            else if(strcmp(cmd, "lr0")==0) { manual_stop = false; gpio_set_level(PIN_BACKLIGHTS, 0); }
            
            else if(strcmp(cmd, "h1")==0) { manual_horn = true; }
            else if(strcmp(cmd, "h0")==0) { manual_horn = false; }
            
            else if(strcmp(cmd, "m1")==0) { microphone_active = true; ESP_LOGI(TAG, "Microfon ACTIVAT din aplicatie!"); }
            else if(strcmp(cmd, "m0")==0) { microphone_active = false; ESP_LOGI(TAG, "Microfon OPRIT din aplicatie!"); }

            // Signals from the app clear auto_off_temp so they stay on until switched off,
            // and left/right are mutually exclusive
            else if(strcmp(cmd, "sl1")==0) { signal_l_active = true; signal_r_active = false; auto_off_temp = 0; }
            else if(strcmp(cmd, "sl0")==0) { signal_l_active = false; auto_off_temp = 0; }
            else if(strcmp(cmd, "sr1")==0) { signal_r_active = true; signal_l_active = false; auto_off_temp = 0; }
            else if(strcmp(cmd, "sr0")==0) { signal_r_active = false; auto_off_temp = 0; }
        }
    }
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    httpd_resp_send(req, "OK", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// SERVER ON
// Control server on port 80: UI page and commands
static httpd_handle_t start_webserver(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.server_port = 80; 
    config.max_open_sockets = 7;
    config.backlog_conn = 8;
    config.lru_purge_enable = true;  // drop the oldest idle socket when full instead of refusing new ones
    config.recv_wait_timeout = 5; 
    config.send_wait_timeout = 5;
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_root);
        httpd_uri_t uri_cmd = { .uri = "/cmd", .method = HTTP_GET, .handler = cmd_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_cmd);
        httpd_uri_t uri_dht = { .uri = "/dht", .method = HTTP_GET, .handler = dht_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_dht); 
        httpd_uri_t uri_fav = { .uri = "/favicon.ico", .method = HTTP_GET, .handler = favicon_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_fav);
    }
    return server;
}

// Separate server (port 8080) for video so the endless stream
// never blocks the control commands. Needs its own ctrl_port to avoid a clash.
static httpd_handle_t start_stream_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;   
    config.ctrl_port = 32769;  
    config.lru_purge_enable = true;
    config.max_open_sockets = 2; 
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    
    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };
        httpd_register_uri_handler(server, &uri_stream);
    }
    return server;
}

// HARDWARE INITIALISATION
static esp_err_t init_camera() {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pin_bit_mask = (1ULL << 48); 
    gpio_config(&io_conf);

    camera_config_t config;
    // LEDC channel/timer used to generate the camera XCLK; kept apart from the motor/servo ones (0-2 / 0-1)
    config.ledc_channel = LEDC_CHANNEL_3; 
    config.ledc_timer = LEDC_TIMER_2;
    config.pin_d0 = CAM_PIN_D0; config.pin_d1 = CAM_PIN_D1;
    config.pin_d2 = CAM_PIN_D2; config.pin_d3 = CAM_PIN_D3;
    config.pin_d4 = CAM_PIN_D4; config.pin_d5 = CAM_PIN_D5;
    config.pin_d6 = CAM_PIN_D6; config.pin_d7 = CAM_PIN_D7;
    config.pin_xclk = CAM_PIN_XCLK; config.pin_pclk = CAM_PIN_PCLK;
    config.pin_vsync = CAM_PIN_VSYNC; config.pin_href = CAM_PIN_HREF;
    config.pin_sccb_sda = CAM_PIN_SIOD; config.pin_sccb_scl = CAM_PIN_SIOC;
    config.pin_pwdn = CAM_PIN_PWDN; config.pin_reset = CAM_PIN_RESET;
    
    config.xclk_freq_hz = 10000000;      // lower XCLK = more stable image on long/jumper wires
    config.frame_size = FRAMESIZE_VGA; 
    config.pixel_format = PIXFORMAT_JPEG; 
    config.grab_mode = CAMERA_GRAB_LATEST; // always return the newest frame, drop stale ones (low latency)
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.jpeg_quality = 12;            // 0-63, lower = better quality/bigger frames
    config.fb_count = 1;
    
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) return err;
    return ESP_OK;
}

void wifi_init_softap(void) {
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_netif_create_default_wifi_ap();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    esp_event_handler_instance_t instance_any_id;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                                        &wifi_event_handler, NULL, &instance_any_id));
    
    // The car creates its own network; connect the phone to it and open 192.168.4.1
    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "The48ErrCar",
            .ssid_len = strlen("The48ErrCar"),
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .channel = 1,                    
            .beacon_interval = 200,          
            .ssid_hidden = 0                 
        },
    };
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));  // disable power save for lower latency
    
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(84));  // units are 0.25 dBm, so 84 = 21 dBm
    
    vTaskDelay(pdMS_TO_TICKS(500)); 
    ESP_LOGI(TAG, "✅ WiFi AP PORNIT pe Canal 1 cu 20dBm!");
}

void configure_pins() {
    gpio_reset_pin(PIN_BUZZER);
    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_BUZZER, BUZZER_MUTE); 

    // Simple on/off outputs, all start LOW
    int out_pins[] = {PIN_HEADLIGHTS, PIN_BACKLIGHTS, PIN_SIGNAL_L, PIN_SIGNAL_R, PIN_PUMP, PIN_US_TRIG};
    for(int i = 0; i < 6; i++) {
        gpio_reset_pin(out_pins[i]);
        gpio_set_direction(out_pins[i], GPIO_MODE_OUTPUT);
        gpio_set_level(out_pins[i], 0);
    }
    
    gpio_reset_pin(PIN_US_ECHO);
    gpio_set_direction(PIN_US_ECHO, GPIO_MODE_INPUT);

    // Servo: 50Hz (20ms period), 13-bit resolution = 8192 steps.
    // Duty 614 is ~1.5ms = centered; the UI sweeps 614 +/- 180.
    ledc_timer_config_t ledc_timer0 = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT,
        .freq_hz          = 50, 
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer0);

    ledc_channel_config_t ledc_channel0 = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_SERVO,
        .duty           = 614, 
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel0);

    // Motor driver: 1kHz PWM, 8-bit (duty 0-255 matches what the web UI sends)
    ledc_timer_config_t ledc_timer1 = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_1,
        .duty_resolution  = LEDC_TIMER_8_BIT,
        .freq_hz          = 1000,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer1);

    ledc_channel_config_t ch_motor_fwd = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_1,
        .timer_sel      = LEDC_TIMER_1,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_MOTOR_FWD,
        .duty           = 0, 
        .hpoint         = 0
    };
    ledc_channel_config(&ch_motor_fwd);

    ledc_channel_config_t ch_motor_rev = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_2,
        .timer_sel      = LEDC_TIMER_1,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = PIN_MOTOR_REV,
        .duty           = 0, 
        .hpoint         = 0
    };
    ledc_channel_config(&ch_motor_rev);
}

void app_main(void) {
    // NVS is required by the WiFi driver; erase and retry if the partition is full/outdated
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    configure_pins();
    
    wifi_init_softap();
    start_webserver();
    start_stream_server();

    xTaskCreate(blinker_task, "semnalizare", 2048, NULL, 5, NULL);
    xTaskCreate(safety_spate_task, "safety_spate", 2048, NULL, 5, NULL);
    xTaskCreate(microfon_task, "microfon", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Server ON!");

    // Camera starts last; the car stays drivable even if it fails
    if(init_camera() == ESP_OK) {
        ESP_LOGI(TAG, "Camera OV3660 started successfully!");
    } else {
        ESP_LOGE(TAG, "Warning: Camera didn't start!");
    }
}