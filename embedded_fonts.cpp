#include "panorama_ui.h"

#include "noto_regular.inc"
#include "noto_bold.inc"
#include "stratum_regular.inc"
#include "stratum_bold.inc"

namespace panorama
{
bool LoadEmbeddedFonts(ImFontAtlas& atlas, Fonts& fonts)
{
    static constexpr ImWchar glyph_ranges[] = {
        0x0020, 0x024F,
        0x2000, 0x206F,
        0x20AC, 0x20AC,
        0x2122, 0x2122,
        0
    };
    fonts.noto_regular = atlas.AddFontFromMemoryCompressedBase85TTF(NotoRegular_compressed_data_base85, 18.0f, nullptr, glyph_ranges);
    fonts.noto_bold = atlas.AddFontFromMemoryCompressedBase85TTF(NotoBold_compressed_data_base85, 18.0f, nullptr, glyph_ranges);
    fonts.stratum_regular = atlas.AddFontFromMemoryCompressedBase85TTF(StratumRegular_compressed_data_base85, 18.0f, nullptr, glyph_ranges);
    fonts.stratum_bold = atlas.AddFontFromMemoryCompressedBase85TTF(StratumBold_compressed_data_base85, 18.0f, nullptr, glyph_ranges);
    return fonts.noto_regular && fonts.noto_bold && fonts.stratum_regular && fonts.stratum_bold;
}
}
