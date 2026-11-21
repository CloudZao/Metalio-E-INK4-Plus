#include "pocket_build.h"
#include "pocket_image.h"
#include "pocket_layout.h"

#if defined(POCKET_DISPLAY_NAME) && defined(POCKET_DESCRIPTION)
#define COMPACT_NAME POCKET_DISPLAY_NAME
#define COMPACT_DESCRIPTION POCKET_DESCRIPTION
#elif POCKET_ROLE_MANAGER
#define COMPACT_NAME "固件口袋"
#define COMPACT_DESCRIPTION "单 BIN 切换，独立字体与资源"
#else
#error "An application must supply its display name and description"
#endif

#if POCKET_ROLE_MANAGER
#define ABI_ROLE pocket::kPocketManagerFlag
#define ABI_STORAGE 0
#define ABI_EXIT 0
#define ABI_TF 0
#define ABI_SCHEMA 0
#define ABI_EXIT_HINT ""
#else
// Future application builds must deliberately supply the integration contract.
// Do not silently give old applications an unimplemented return declaration.
#if !defined(POCKET_STORAGE_MODE) || !defined(POCKET_EXIT_METHOD) || !defined(POCKET_EXIT_HINT) || !defined(POCKET_TF_REQUIRED_STANDALONE)
#error "Application profile must declare storage, exit and standalone TF requirements"
#endif
#define ABI_ROLE pocket::kPocketApplicationFlag
#define ABI_STORAGE POCKET_STORAGE_MODE
#define ABI_EXIT POCKET_EXIT_METHOD
#define ABI_TF POCKET_TF_REQUIRED_STANDALONE
#define ABI_SCHEMA POCKET_STATE_SCHEMA
#define ABI_EXIT_HINT POCKET_EXIT_HINT
#endif

// ESP-IDF retains this section immediately after its standard app descriptor.
// The packager verifies the final offset and identity, not just this source.
extern "C" const __attribute__((section(".rodata_custom_desc"), used))
pocket::CompactMetadata pocket_compact_metadata = {
    {'F','P','O','C','K','E','T','-','A','B','I','-','0','0','0','1'},
    1, 0, 512, ABI_ROLE, ABI_STORAGE, ABI_EXIT, ABI_TF, 0, ABI_SCHEMA,
    POCKET_TARGET_ID, POCKET_LAYOUT_ID, POCKET_APP_ID, POCKET_VERSION,
    COMPACT_NAME, COMPACT_DESCRIPTION, ABI_EXIT_HINT, {}
};
