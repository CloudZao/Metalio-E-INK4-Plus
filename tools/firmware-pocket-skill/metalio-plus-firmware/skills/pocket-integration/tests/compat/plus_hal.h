#pragma once
#include <cstdint>
namespace plus_hal {
bool PauseForFlash(unsigned);void ResumeAfterFlash();bool DisplayHealthy();uint32_t DisplayedRevision();
}
