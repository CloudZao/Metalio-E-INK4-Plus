#pragma once

#include <string_view>

// CMake EMBED_FILES：main/assets/factory-test/*.ogg
// 符号名按文件名：_binary_<name>_ogg_{start,end}

extern const char ogg_factory_test_audio_start[] asm("_binary_factory_test_audio_ogg_start");
extern const char ogg_factory_test_audio_end[] asm("_binary_factory_test_audio_ogg_end");

inline const std::string_view FactoryTestAssets_OGG_FACTORY_TEST_AUDIO{
    static_cast<const char*>(ogg_factory_test_audio_start),
    static_cast<size_t>(ogg_factory_test_audio_end - ogg_factory_test_audio_start),
};
