#include "settings_bluetooth_tab.h"

#include "bluetooth_screen/bluetooth_screen.h"

void SettingsBluetoothTab_Build(lv_obj_t* page) {
    BluetoothScreen::BuildInto(page);
}

void SettingsBluetoothTab_Reset() {
    // OnDeactivated 由设置壳在离 Tab 时先调；此处只清 UI 状态
    BluetoothScreen::ResetUi();
}

void SettingsBluetoothTab_OnActivated() {
    BluetoothScreen::OnActivated();
}

void SettingsBluetoothTab_OnDeactivated() {
    BluetoothScreen::OnDeactivated();
}
