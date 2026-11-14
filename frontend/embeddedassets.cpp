#include "frontend/embeddedassets.h"

#ifdef OSHEXED_ASSETS_ZIP

// The assets zip (ui/, assets/), built by CMake, pulled into the executable by the assembler. .incbin keeps the bytes
// away from the compiler: a C array of a few MB costs it far more time and memory. The asm labels on the declarations
// fix the symbol names, so a platform's underscore prefix can't get in the way. CMake rebuilds this file whenever the
// zip changes. MinGW's assembler has no .pushsection, so the block switches to read-only data and ends back in .text,
// where the compiler expects to be (as incbin.h does).
#if defined(_WIN32)
#define OSHEXED_RODATA ".rdata,\"dr\""
#else
#define OSHEXED_RODATA ".rodata"
#endif

__asm__(".section " OSHEXED_RODATA "\n"
        ".balign 16\n"
        ".globl oshexed_assets_begin\n"
        "oshexed_assets_begin:\n"
        ".incbin \"" OSHEXED_ASSETS_ZIP "\"\n"
        ".globl oshexed_assets_end\n"
        "oshexed_assets_end:\n"
        ".byte 0\n"
        ".text\n");

extern "C" const unsigned char oshexed_assets_begin[] __asm__("oshexed_assets_begin");
extern "C" const unsigned char oshexed_assets_end[] __asm__("oshexed_assets_end");

namespace editor {

EmbeddedAssets embeddedAssets() {
    return {oshexed_assets_begin, std::size_t(oshexed_assets_end - oshexed_assets_begin)};
}

} // namespace editor

#else

namespace editor {

EmbeddedAssets embeddedAssets() { return {}; }

} // namespace editor

#endif
