#pragma once
// Deterministic host SDK seam. Production .cc is compiled unchanged.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <vector>
using esp_err_t = int;
using esp_event_base_t = const char*;
using esp_event_handler_instance_t = void*;
using TickType_t = unsigned;
using QueueHandle_t = void*;
using wifi_mode_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_STATE=1, ESP_ERR_WIFI_NOT_INIT=2,
 ESP_ERR_WIFI_NOT_STARTED=3, ESP_ERR_WIFI_NOT_CONNECT=4, ESP_ERR_WIFI_CONN=5;
constexpr int WIFI_MODE_NULL=0,WIFI_MODE_STA=1,WIFI_STORAGE_RAM=0,WIFI_PS_MIN_MODEM=0,
 WIFI_ALL_CHANNEL_SCAN=0,WIFI_AUTH_OPEN=0,WIFI_IF_STA=0,WIFI_SCAN_TYPE_ACTIVE=0;
constexpr int ESP_EVENT_ANY_ID=-1,WIFI_EVENT_SCAN_DONE=1,WIFI_EVENT_STA_CONNECTED=2,
 WIFI_EVENT_STA_DISCONNECTED=3,WIFI_EVENT_STA_STOP=4,IP_EVENT_STA_GOT_IP=1,IP_EVENT_STA_LOST_IP=2;
constexpr int pdTRUE=1,pdPASS=1,MALLOC_CAP_INTERNAL=1,MALLOC_CAP_8BIT=2;
inline const char WIFI_EVENT[]="wifi";
inline const char IP_EVENT[]="ip";
#define ESP_EVENT_DEFINE_BASE(name) const char name[] = #name
#define ESP_LOGI(...) ((void)0)
#define pdMS_TO_TICKS(ms) (ms)
struct esp_ip4_addr_t { uint32_t addr=0; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip; };
struct esp_netif_t { bool up=false; esp_netif_ip_info_t info; };
struct ip_event_got_ip_t { esp_netif_t* esp_netif; esp_netif_ip_info_t ip_info; };
struct wifi_event_sta_connected_t { uint8_t ssid[32]{}; uint8_t ssid_len=0; uint8_t bssid[6]{}; };
struct wifi_event_sta_disconnected_t { uint8_t ssid[32]{}; uint8_t ssid_len=0; uint8_t bssid[6]{}; uint8_t reason=0; };
struct wifi_ap_record_t { uint8_t ssid[33]{}; int8_t rssi=-50; uint8_t primary=1; int authmode=0; };
struct wifi_init_config_t { bool nvs_enable=true; };
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
struct wifi_config_t { struct {
 uint8_t ssid[32]{},password[64]{}; int scan_method=0;
 struct { int authmode=0; } threshold;
 struct { bool capable=false,required=false; } pmf_cfg;
} sta; };
struct wifi_scan_config_t { bool show_hidden=false; int scan_type=0;
 struct { struct { int min=0,max=0; } active; } scan_time; };
namespace fake {
using Handler = void(*)(void*,esp_event_base_t,int32_t,void*);
inline Handler handler=nullptr;
inline esp_event_base_t control=nullptr;
inline esp_netif_t sta, other;
inline wifi_config_t config;
inline int64_t time=0;
inline bool running=true,initialized=true,stop_event=true,stop_down=true,
 barrier=true,post_ok=true,barrier_registered=true,associated=false,async_barrier=false;
inline int stop_state=ESP_ERR_WIFI_CONN,connect_calls=0,cleanup_calls=0,queue_spaces=4;
inline bool send_ok=true;
enum class Outcome { Success, Auth, NoIp, NoAssociation, StaleIp, WrongNetif, OtherPeer };
inline Outcome outcome=Outcome::Success;
inline std::deque<std::function<void()>> delays;
inline std::vector<uint8_t> configured_password;
inline void Emit(esp_event_base_t base,int id,void* data=nullptr) { if(handler) handler(nullptr,base,id,data); }
inline wifi_event_sta_connected_t Peer() {
 wifi_event_sta_connected_t event{};
 std::copy_n(config.sta.ssid,32,event.ssid);
 event.ssid_len=static_cast<uint8_t>(strnlen(reinterpret_cast<char*>(config.sta.ssid),32));
 event.bssid[0]=42; return event;
}
inline void Disconnect(int reason,bool foreign=false) {
 const auto peer=Peer(); wifi_event_sta_disconnected_t event{};
 std::copy_n(peer.ssid,32,event.ssid); event.ssid_len=peer.ssid_len;
 std::copy_n(peer.bssid,6,event.bssid); event.reason=static_cast<uint8_t>(reason);
 if(foreign) { event.ssid[0]^=1; event.bssid[0]^=1; }
 Emit(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&event);
}
inline void Reset() {
 sta={};other={};config={};time=0;running=true;initialized=true;stop_event=true;stop_down=true;
 barrier=true;post_ok=true;barrier_registered=true;associated=false;async_barrier=false;stop_state=ESP_ERR_WIFI_CONN;
 connect_calls=0;cleanup_calls=0;queue_spaces=4;send_ok=true;outcome=Outcome::Success;
 delays.clear();configured_password.clear();
}
}
inline int64_t esp_timer_get_time(){return fake::time;}
inline void vTaskDelay(TickType_t ticks){fake::time+=static_cast<int64_t>(ticks)*1000;
 if(!fake::delays.empty()){auto f=std::move(fake::delays.front());fake::delays.pop_front();f();}}
inline esp_err_t esp_netif_init(){return ESP_OK;}
inline esp_err_t esp_event_loop_create_default(){return ESP_OK;}
inline esp_netif_t* esp_netif_get_handle_from_ifkey(const char*){return &fake::sta;}
inline esp_netif_t* esp_netif_create_default_wifi_sta(){return &fake::sta;}
inline bool esp_netif_is_netif_up(esp_netif_t* n){return n->up;}
inline esp_err_t esp_netif_get_ip_info(esp_netif_t* n,esp_netif_ip_info_t* ip){*ip=n->info;return ESP_OK;}
inline void esp_ip4addr_ntoa(const esp_ip4_addr_t*,char* out,size_t size){std::snprintf(out,size,"192.0.2.1");}
inline esp_err_t esp_wifi_get_mode(wifi_mode_t* mode){*mode=WIFI_MODE_STA;return fake::initialized?ESP_OK:ESP_ERR_WIFI_NOT_INIT;}
inline esp_err_t esp_wifi_init(wifi_init_config_t*){fake::initialized=true;return ESP_OK;}
inline esp_err_t esp_event_handler_instance_register(esp_event_base_t base,int, fake::Handler callback,void*,esp_event_handler_instance_t* out){
 fake::handler=callback;*out=reinterpret_cast<void*>(1);
 if(base!=WIFI_EVENT&&base!=IP_EVENT)fake::control=base;return ESP_OK;}
inline esp_err_t esp_event_post(esp_event_base_t base,int id,const void* data,size_t,unsigned){
 if(!fake::post_ok)return ESP_ERR_INVALID_STATE;
 if(fake::barrier) {
  if(fake::async_barrier) {
   const uint32_t ticket=*static_cast<const uint32_t*>(data);
   fake::delays.push_back([base,id,ticket]{auto copy=ticket;fake::Emit(base,id,&copy);});
  } else fake::Emit(base,id,const_cast<void*>(data));
 } return ESP_OK;}
inline esp_err_t esp_wifi_set_storage(int){return ESP_OK;}
inline esp_err_t esp_wifi_set_mode(int){return ESP_OK;}
inline esp_err_t esp_wifi_set_ps(int){return ESP_OK;}
inline esp_err_t esp_wifi_start(){fake::running=true;return ESP_OK;}
inline esp_err_t esp_wifi_stop(){fake::running=false;fake::associated=false;
 if(fake::stop_down)fake::sta.up=false;
 if(fake::stop_event)fake::Emit(WIFI_EVENT,WIFI_EVENT_STA_STOP);return ESP_OK;}
inline esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap){
 if(!fake::initialized)return ESP_ERR_WIFI_NOT_INIT;
 if(!fake::running)return fake::stop_state;
 if(!fake::associated)return ESP_ERR_WIFI_NOT_CONNECT;
 std::copy_n(fake::config.sta.ssid,32,ap->ssid);return ESP_OK;}
inline esp_err_t esp_wifi_set_config(int,wifi_config_t* config){fake::config=*config;
 fake::configured_password.assign(config->sta.password,config->sta.password+64);return ESP_OK;}
inline esp_err_t esp_wifi_connect(){
 ++fake::connect_calls;
 if(fake::outcome==fake::Outcome::Auth){fake::Disconnect(202);return ESP_OK;}
 if(fake::outcome==fake::Outcome::NoAssociation)return ESP_OK;
 auto event=fake::Peer();fake::associated=true;fake::sta.up=true;fake::sta.info.ip.addr=0x010200c0;
 fake::Emit(WIFI_EVENT,WIFI_EVENT_STA_CONNECTED,&event);
 if(fake::outcome==fake::Outcome::OtherPeer)fake::Disconnect(202,true);
 if(fake::outcome!=fake::Outcome::NoIp&&fake::outcome!=fake::Outcome::StaleIp){
 ip_event_got_ip_t ip{fake::outcome==fake::Outcome::WrongNetif?&fake::other:&fake::sta,fake::sta.info};
 fake::Emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&ip);}
 return ESP_OK;}
inline esp_err_t esp_wifi_disconnect(){++fake::cleanup_calls;fake::associated=false;
 fake::Disconnect(8);return ESP_OK;}
inline esp_err_t esp_wifi_scan_start(wifi_scan_config_t*,bool){fake::Emit(WIFI_EVENT,WIFI_EVENT_SCAN_DONE);return ESP_OK;}
inline esp_err_t esp_wifi_scan_get_ap_records(uint16_t* count,wifi_ap_record_t*){*count=0;return ESP_OK;}
inline esp_err_t esp_wifi_scan_stop(){return ESP_OK;}
inline esp_err_t esp_wifi_clear_ap_list(){return ESP_OK;}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t){return 0;}
inline unsigned uxQueueSpacesAvailable(QueueHandle_t){return static_cast<unsigned>(fake::queue_spaces);}
inline int xQueueSend(QueueHandle_t,const void*,unsigned){return fake::send_ok?pdTRUE:0;}
inline int xQueueReceive(QueueHandle_t,void*,unsigned){return 0;}
inline void xQueueReset(QueueHandle_t){}
inline QueueHandle_t xQueueCreate(unsigned,size_t){return reinterpret_cast<void*>(1);}
inline void vQueueDelete(QueueHandle_t){}
inline int xTaskCreatePinnedToCoreWithCaps(void(*)(void*),const char*,unsigned,void*,int,void*,int,int){return pdPASS;}
