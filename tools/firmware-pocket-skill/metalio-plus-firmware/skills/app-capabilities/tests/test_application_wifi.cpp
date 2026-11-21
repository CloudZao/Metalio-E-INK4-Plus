#include "pocket_wifi.h"
#include "pocket_core.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>
namespace fs = std::filesystem;
using namespace pocket;
std::string PublicPayload(const WifiNetwork& n) {
    std::string out;
    out.push_back(static_cast<char>(n.ssid.size()));
    out.push_back(static_cast<char>(n.password.size()));
    return out+n.ssid+n.password;
}
std::map<std::string,std::string> Snapshot(const fs::path& directory) {
    std::map<std::string,std::string> out;
    for(const auto& file:fs::directory_iterator(directory)) {
        std::ifstream stream(file.path(),std::ios::binary);
        out[file.path().filename().string()]={std::istreambuf_iterator<char>(stream),std::istreambuf_iterator<char>()};
    }
    return out;
}
int main(int argc,char**argv) {
    assert(argc==2);
    fs::path base(argv[1]), shared=base/"shared", priv=base/"weather", note=base/"notepad";
    fs::create_directories(shared);fs::create_directories(priv);fs::create_directories(note);
    const std::string run1(32,'1'),run2(32,'2'),run3(32,'3');
    std::string error; std::vector<WifiNetwork> list; WifiNetwork offer;bool available=true;
    assert(!ReadApplicationWifi(&list,&error));
    assert(InitializeApplicationWifi(shared.string(),priv.string(),false,"",&error));
    assert(ReadApplicationWifi(&list,&error)&&list.empty());
    assert(SaveApplicationWifi("same","private-old",&error));
    assert(SaveApplicationWifi("local-only","local-pass",&error));
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&!available);
    assert(fs::is_empty(shared)); // No global record is ever created by app save.
    assert(RecordStore(shared.string()).Save("wifi_current",1,PublicPayload({"same","public-new"})).ok());
    const auto authoritative=Snapshot(shared);
    assert(InitializeApplicationWifi(shared.string(),priv.string(),true,run1,&error));
    assert(ReadApplicationWifi(&list,&error)&&list.size()==2&&list.back().ssid=="same");
    assert(list[1].ssid=="same"&&list[1].password=="public-new");
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&!available); // Reading creates no offer.
    assert(SaveApplicationWifi("same","  manual raw bytes  ",&error));
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&available&&offer.password=="  manual raw bytes  ");
    assert(InitializeApplicationWifi(shared.string(),priv.string(),true,run1,&error));
    assert(ReadApplicationWifi(&list,&error)&&list.back().ssid=="same"&&list.back().password=="  manual raw bytes  ");
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&available); // Same run survives reboot.
    assert(ClearPendingWifiOffer(&error));
    assert(InitializeApplicationWifi(shared.string(),priv.string(),true,run1,&error));
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&!available);
    assert(ReadApplicationWifi(&list,&error)&&list.back().password=="  manual raw bytes  "); // Clearing doesn't undo manual choice.
    assert(SaveApplicationWifi("same","pending-of-run1",&error));
    assert(InitializeApplicationWifi(shared.string(),priv.string(),true,run2,&error));
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&!available);
    assert(ReadApplicationWifi(&list,&error)&&list.back().ssid=="same"&&list[1].password=="public-new");
    assert(InitializeApplicationWifi(shared.string(),priv.string(),false,"",&error));
    assert(ReadApplicationWifi(&list,&error)&&list.size()==2&&list.back().password=="pending-of-run1");
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&!available);
    assert(InitializeApplicationWifi(shared.string(),priv.string(),true,run3,&error));
    assert(SaveApplicationWifi("same","current",&error));
    assert(ForgetApplicationWifi("same",&error));
    assert(ReadPendingWifiOffer(&offer,&available,&error)&&!available);
    assert(ReadApplicationWifi(&list,&error));
    for(const auto& n:list)assert(n.ssid!="same");
    assert(Snapshot(shared)==authoritative); // Save, forget and clear didn't touch shared authority.
    assert(InitializeApplicationWifi(shared.string(),note.string(),false,"",&error));
    assert(ReadApplicationWifi(&list,&error)&&list.empty()); // Separate app / standalone cannot inherit the other app.
    for(int i=0;i<10;++i)assert(SaveApplicationWifi("n"+std::to_string(i),"pw",&error));
    assert(!SaveApplicationWifi("eleventh","pw",&error));
    assert(SaveApplicationWifi("n0","updated",&error));
    assert(!SaveApplicationWifi(std::string(33,'s'),"pw",&error));
    assert(!SaveApplicationWifi("nul",std::string("a\0b",3),&error));
    const auto offline=note.string()+"-offline";
    fs::rename(note,offline);
    assert(ReadApplicationWifi(&list,&error)&&list.size()==10&&list.back().password=="updated"); // Radio only reads RAM.
    assert(!SaveApplicationWifi("n0","must-not-stick",&error));
    fs::rename(offline,note);
    assert(InitializeApplicationWifi(shared.string(),note.string(),false,"",&error));
    assert(ReadApplicationWifi(&list,&error)&&list.back().password=="updated");
    assert(!InitializeApplicationWifi(shared.string(),(base/"missing").string(),true,run1,&error));
    assert(ReadApplicationWifi(&list,&error)&&list.size()==1); // Local failure cannot block public/cache reads or scanning.
    assert(!SaveApplicationWifi("ssid","pw",&error)&&!fs::exists(base/"missing")); // Never relocate or create implicit directory.
    const std::string binary_ssid("raw\xff\x80",5), binary_pass(" pw\t\xff ",6);
    assert(RecordStore(shared.string()).Save("wifi_current",1,PublicPayload({binary_ssid,binary_pass})).ok());
    assert(InitializeApplicationWifi(shared.string(),priv.string(),true,run2,&error));
    assert(ReadApplicationWifi(&list,&error)&&list.back().ssid==binary_ssid&&list.back().password==binary_pass);
    // Independent invalid byte vectors: trailing bytes, wrong length, NUL, legacy PKWIFI.
    for(const auto& invalid:std::vector<std::string>{PublicPayload({"same","x"})+"tail",
            std::string("\x21\0",2)+std::string(33,'s'), std::string("\x02\0" "a\0",4),
            std::string("PKWIFI\1\0",8)}) {
        assert(RecordStore(shared.string()).Save("wifi_current",1,invalid).ok());
        assert(InitializeApplicationWifi(shared.string(),priv.string(),true,run2,&error));
        assert(ReadApplicationWifi(&list,&error));
        for(const auto& n:list) assert(n.ssid!=binary_ssid);
    }
    std::ofstream(shared/"wifi_current.a.pkr",std::ios::binary|std::ios::trunc)<<"bad";
    std::ofstream(shared/"wifi_current.b.pkr",std::ios::binary|std::ios::trunc)<<"bad";
    const auto corrupt_public=Snapshot(shared);
    assert(InitializeApplicationWifi(shared.string(),note.string(),true,run1,&error)); // Public corruption is optional.
    assert(ReadApplicationWifi(&list,&error)&&list.size()==10);
    assert(SaveApplicationWifi("n0","still-local",&error));
    assert(Snapshot(shared)==corrupt_public);
    std::ofstream(note/"wifi_local.a.pkr",std::ios::binary|std::ios::trunc)<<"broken";
    std::ofstream(note/"wifi_local.b.pkr",std::ios::binary|std::ios::trunc)<<"broken";
    const auto corrupt_local=Snapshot(note);
    assert(!InitializeApplicationWifi(shared.string(),note.string(),true,run1,&error));
    assert(ReadApplicationWifi(&list,&error)&&list.empty());
    assert(!SaveApplicationWifi("ssid","pw",&error));
    assert(Snapshot(note)==corrupt_local); // No silent repair of an unknown user's data.
    DisableApplicationWifiStorage();
    assert(!ReadApplicationWifi(&list,&error)&&!SaveApplicationWifi("ssid","pw",&error));
    assert(!InitializeApplicationWifi(shared.string(),priv.string(),true,"bad-run",&error));
    std::cout<<"PASS app-private persistence; public authority read-only; missing/corrupt public fallback; raw credential bytes; same-run offer persistence; new-run/standalone offer isolation; public latest preference; private manual preference; local forget; cache-only network reads; missing/corrupt storage protection; bounded records\n";
}
