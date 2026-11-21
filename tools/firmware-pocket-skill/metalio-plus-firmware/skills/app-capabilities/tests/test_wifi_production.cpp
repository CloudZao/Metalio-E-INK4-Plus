// Compile the ACTUAL shipped connection source with a deterministic SDK seam.
// No copied connection algorithm and no helper-only assertion can pass this suite.
#include "pocket_wifi_connection.cc"
#ifndef WIFI_BASELINE
#include "network_device_setup.h"
#endif
#include <cassert>
#include <filesystem>
#include <iostream>
namespace fs=std::filesystem;
using namespace pocket;
void Reset() {
    fake::Reset();
    view={}; generation.store(1); link_generation.store(0);
    associated.store(false); fresh_ip.store(false); disconnect_reason.store(0);
    accept_link_events.store(false); radio_started.store(true);
    suspend_requested.store(false); suspended.store(false); scan_pending.store(false);
    barrier_next.store(0); barrier_done.store(0);
    queue.store(reinterpret_cast<void*>(1));
    netif=&fake::sta; wifi_handler=nullptr; ip_handler=nullptr;
    barrier_handler=reinterpret_cast<void*>(1);
    fake::handler=OnEvent;fake::control=PLUS_WIFI_CONTROL;
#ifndef WIFI_BASELINE
    stop_observed.store(false); active_ssid.clear(); active_bssid={}; bssid_known=false;
#endif
}
bool Connect(const std::string& ssid="test") { return ConnectSaved(ssid,generation.load(),esp_timer_get_time()+1000000); }
void Pass(const char* name) { std::cout<<"PASS "<<name<<"\n"; }
int main(int argc,char** argv) {
    assert(argc==2);fs::path base(argv[1]);fs::create_directories(base);
    std::string error;
    assert(InitializeApplicationWifi("",base.string(),false,"",&error));
    assert(SaveApplicationWifi("test","bad-password",&error));
    Reset();
#ifdef WIFI_BASELINE
    assert(!Connect());assert(fake::connect_calls==0);
    Pass("baseline reproduces STOP + netif down + ESP_ERR_WIFI_CONN rejected before connect");
    Reset();fake::stop_state=ESP_ERR_WIFI_NOT_CONNECT;fake::outcome=fake::Outcome::Auth;
    assert(!Connect());assert(disconnect_reason.load()==8);
    Pass("baseline reproduces cleanup reason 8 overwriting authentication 202");
    return 0;
#else
    assert(Connect()&&fake::connect_calls==1&&view.connected&&view.connection_id==1);
    Pass("production accepts real STOP + down + ESP_ERR_WIFI_CONN, connects and requires fresh IP");
    for(const int code:{ESP_ERR_WIFI_NOT_CONNECT,ESP_ERR_WIFI_NOT_STARTED,ESP_ERR_WIFI_NOT_INIT}) {
        Reset();fake::stop_state=code;assert(Connect()&&fake::connect_calls==1);
    }
    Pass("all supported SDK stopped AP states");
    Reset();fake::stop_event=false;
    fake::delays.push_back([] { fake::Emit(WIFI_EVENT,WIFI_EVENT_STA_STOP); });
    assert(Connect()&&fake::connect_calls==1);
    Pass("delayed real STOP acknowledgement is awaited");
    Reset();fake::async_barrier=true;
    fake::delays.push_back([] {
        fake::Disconnect(202);
        ip_event_got_ip_t old_ip{&fake::sta,{{123}}};
        fake::Emit(IP_EVENT,IP_EVENT_STA_GOT_IP,&old_ip);
    });
    assert(Connect()&&disconnect_reason.load()==0&&view.connection_id==1);
    Pass("queued old auth/IP drained before asynchronous fence and new session");
    Reset();fake::stop_event=false;assert(!Connect()&&fake::connect_calls==0&&view.failure==WifiFailure::Reset);
    Pass("missing STOP rejected");
    Reset();fake::sta.up=true;fake::stop_down=false;assert(!Connect()&&fake::connect_calls==0);
    Pass("netif still up rejected");
    Reset();fake::stop_state=12345;assert(!Connect()&&fake::connect_calls==0);
    Pass("unknown AP error rejected");
    Reset();fake::barrier=false;assert(!Connect()&&fake::connect_calls==0);
    Pass("missing event fence acknowledgement rejected");
    Reset();fake::post_ok=false;assert(!Connect()&&fake::connect_calls==0);
    Pass("full event queue / fence post failure rejected");
    Reset();barrier_handler=nullptr;assert(!Connect()&&fake::connect_calls==0);
    Pass("missing fence registration is not bypassed");
    Reset();fake::initialized=false;radio_started.store(false);barrier_handler=nullptr;
    assert(StopRadioAndDrain(1000000,1,true));
    Pass("only exact double NOT_INIT and down permits early startup shutdown");
    Reset();fake::outcome=fake::Outcome::Auth;assert(!Connect());
    assert(view.failure==WifiFailure::Authentication&&view.reason_code==202&&disconnect_reason.load()==202);
    assert(view.message.find("202")!=std::string::npos&&fake::cleanup_calls==1);
    Pass("auth 202 preserved across actual cleanup DISCONNECTED 8 callback");
    assert(SaveApplicationWifi("test","correct-password",&error));
    fake::outcome=fake::Outcome::Success;generation.fetch_add(1);
    assert(Connect()&&view.failure==WifiFailure::None&&view.reason_code==0);
    assert(std::string(fake::configured_password.begin(),fake::configured_password.begin()+16)=="correct-password");
    Pass("wrong password then edited password reconnect uses saved new bytes");
    Reset();fake::outcome=fake::Outcome::NoIp;assert(!Connect()&&view.failure==WifiFailure::NoIp&&!view.connected);
    Pass("associated without DHCP has separate no-IP failure");
    Reset();fake::outcome=fake::Outcome::StaleIp;assert(!Connect()&&!view.connected);
    Pass("cached nonzero IP without fresh event cannot indicate success");
    Reset();fake::outcome=fake::Outcome::WrongNetif;assert(!Connect()&&!view.connected);
    Pass("IP from other netif rejected");
    Reset();fake::outcome=fake::Outcome::NoAssociation;assert(!Connect()&&view.failure==WifiFailure::Timeout);
    Pass("no association has separate timeout failure");
    Reset();fake::outcome=fake::Outcome::OtherPeer;assert(Connect()&&disconnect_reason.load()==0);
    Pass("foreign peer disconnect cannot poison current attempt");
    auto same_ssid=fake::Peer();wifi_event_sta_disconnected_t other_bssid{};
    std::copy_n(same_ssid.ssid,32,other_bssid.ssid);other_bssid.ssid_len=same_ssid.ssid_len;
    std::copy_n(same_ssid.bssid,6,other_bssid.bssid);other_bssid.bssid[0]^=1;other_bssid.reason=202;
    fake::Emit(WIFI_EVENT,WIFI_EVENT_STA_DISCONNECTED,&other_bssid);
    assert(disconnect_reason.load()==0&&view.connected);
    Pass("same SSID different BSSID disconnect rejected after association");
    const auto id=view.connection_id;
    generation.fetch_add(1);fake::Disconnect(202);
    assert(disconnect_reason.load()==0&&view.connection_id==id);
    Pass("old request disconnect ignored after request generation changes");
    Reset();fake::outcome=fake::Outcome::NoAssociation;
    fake::delays.push_back([] { generation.fetch_add(1); fake::Disconnect(202); });
    assert(!Connect()&&disconnect_reason.load()==0);
    Pass("attempt cancellation rejects late auth event");
    Reset();assert(Connect());
    fake::delays.push_back([] { fake::Disconnect(202); });
    // Stop calls a synchronous STOP; fence drains via SDK queue in real firmware.
    // Independently exercise old disconnection while admission is closed.
    accept_link_events.store(false);fake::Disconnect(202);assert(disconnect_reason.load()==0);
    Pass("closed admission rejects cleanup and old-session disconnects");
    Reset();fake::queue_spaces=0;
    assert(!RequestWifiConnect("test")&&view.failure==WifiFailure::QueueBusy);
    plus_examples::WifiPageStatus page;
    page.Submit("test","latest");assert(!page.action_error.empty());
    assert(page.Message(WifiConnectionSnapshot()).find("队列忙")!=std::string::npos);
    Publish([](auto& v){v.message="Wi-Fi 已就绪";});assert(page.Message(view)==page.action_error);
    Pass("saved but queue-busy remains visible offline on every page");
    DisableApplicationWifiStorage();page.Submit("test","latest");
    assert(page.Message(view).find("保存失败")!=std::string::npos);
    Pass("save failure independent of radio ready and offline status");
    assert(InitializeApplicationWifi("",base.string(),false,"",&error));
    const std::string binary("raw\xff",4);assert(SaveApplicationWifi(binary," raw ",&error));
    Reset();assert(Connect(binary));
    assert(WifiSsidLabel(binary)=="raw\\xFF");
    assert(std::string(reinterpret_cast<char*>(fake::config.sta.ssid),4)==binary);
    Pass("non-UTF8 raw SSID connects unchanged; safe display is separate");
    std::cout<<"All production SDK-stub cases passed; no real access point or device acceptance claimed.\n";
#endif
}
