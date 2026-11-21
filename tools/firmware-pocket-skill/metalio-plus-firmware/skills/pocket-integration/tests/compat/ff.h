#pragma once
struct FATFS{};
using DWORD=unsigned long;
constexpr int FR_OK=0;
int f_getfree(const char*,DWORD*,FATFS**);
