#include <shared/system.h>
#include <beeb/TVOutput.h>
#include <beeb/OutputData.h>
#include <shared/debug.h>
#include <string.h>
#include <stdlib.h>
#include <beeb/video.h>
//#include <SDL.h>
//#include "misc.h"
//#include "conf.h"
#include <shared/log.h>
#include <math.h>

#include <shared/enum_def.h>
#include <beeb/TVOutput.inl>
#include <shared/enum_end.h>

//LOG_EXTERN(OUTPUT);

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

TVOutput::TVOutput() {
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

TVOutput::~TVOutput() {
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void TVOutput::Init(uint32_t r_shift,uint32_t g_shift,uint32_t b_shift) {
    m_r_shift=r_shift;
    m_g_shift=g_shift;
    m_b_shift=b_shift;

    ASSERT((0xffu<<m_r_shift&0xffu<<m_g_shift&0xffu<<m_b_shift)==0);

    m_texture_pixels.resize(TV_TEXTURE_WIDTH*TV_TEXTURE_HEIGHT);
#if VIDEO_TRACK_METADATA
    m_texture_units.resize(m_texture_pixels.size());
#endif

    this->InitPalette();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static const uint8_t DIGITS[10][13]={
    {0x00,0x00,0x04,0x0A,0x11,0x11,0x11,0x11,0x11,0x0A,0x04,0x00,0x00,},// 48 (0x30) '0'
    {0x00,0x00,0x04,0x06,0x05,0x04,0x04,0x04,0x04,0x04,0x1F,0x00,0x00,},// 49 (0x31) '1'
    {0x00,0x00,0x0E,0x11,0x11,0x10,0x08,0x04,0x02,0x01,0x1F,0x00,0x00,},// 50 (0x32) '2'
    {0x00,0x00,0x1F,0x10,0x08,0x04,0x0E,0x10,0x10,0x11,0x0E,0x00,0x00,},// 51 (0x33) '3'
    {0x00,0x00,0x08,0x08,0x0C,0x0A,0x0A,0x09,0x1F,0x08,0x08,0x00,0x00,},// 52 (0x34) '4'
    {0x00,0x00,0x1F,0x01,0x01,0x0D,0x13,0x10,0x10,0x11,0x0E,0x00,0x00,},// 53 (0x35) '5'
    {0x00,0x00,0x0E,0x11,0x01,0x01,0x0F,0x11,0x11,0x11,0x0E,0x00,0x00,},// 54 (0x36) '6'
    {0x00,0x00,0x1F,0x10,0x08,0x08,0x04,0x04,0x02,0x02,0x02,0x00,0x00,},// 55 (0x37) '7'
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E,0x11,0x11,0x11,0x0E,0x00,0x00,},// 56 (0x38) '8'
    {0x00,0x00,0x0E,0x11,0x11,0x11,0x1E,0x10,0x10,0x11,0x0E,0x00,0x00,},// 57 (0x39) '9'
};

// (272 scanned lines + 40 retrace lines + 0.5 interlace lines) * (52+4+8=64)us = 20000us = 20ms

static const int HORIZONTAL_RETRACE_CYCLES=2*4;
static const int BACK_PORCH_CYCLES=2*8;
static const int SCAN_OUT_CYCLES=2*52;
static const int SCANLINE_CYCLES=HORIZONTAL_RETRACE_CYCLES+BACK_PORCH_CYCLES+SCAN_OUT_CYCLES;
static_assert(SCANLINE_CYCLES==128,"one scanline must be 64us");
static const int VERTICAL_RETRACE_SCANLINES=12;

// HEIGHT_SCALE will go away, when I get a moment to do it.
static const int HEIGHT_SCALE=2;
static_assert(HEIGHT_SCALE==2,"");

// If this many lines are scanned without a vertical retrace, the TV
// retraces anyway.
//
// Originally I just set this to 272, because... well, why not? But
// Firetrack's loading screen has a flickery bit near the top with
// that value. I also found a few tests that displayed fine on a CRT
// TV looked a mess on the emulator with reasonable-sounding values
// for MAX_NUM_SCANNED_LINES.
//
// 500 seems OK. It doesn't have to be perfect, just something that
// means emulated TV output keeps going when there's no CRTC vsync
// output...
static const int MAX_NUM_SCANNED_LINES=500*HEIGHT_SCALE;

#define NOTHING_PALETTE_INDEX (0)

#if BUILD_TYPE_Debug
#ifdef _MSC_VER
#pragma optimize("tsg",on)
#endif
#endif

void TVOutput::Update(const VideoDataUnit *units,size_t num_units) {
    const VideoDataUnit *unit=units;

    for(size_t i=0;i<num_units;++i,++unit) {
        switch(m_state) {
            default:
                ASSERT(0);
                break;

            case TVOutputState_VerticalRetrace:
                ++m_num_fields;
                m_state=TVOutputState_VerticalRetraceWait;
                ++m_texture_data_version;
                m_x=0;
                m_y=0;
                m_pixels_line=m_texture_pixels.data();
#if VIDEO_TRACK_METADATA
                m_units_line=m_texture_units.data();
#endif

                // "Fun" (translation: brain-eating) fake interlace

                // if(m_num_fields&1) {
                //     m_line+=m_texture_pitch;
                //     ++m_y;
                // }

                m_state_timer=1;
                break;

            case TVOutputState_VerticalRetraceWait:
            {
                // Ignore everything.
                if(m_state_timer++>=VERTICAL_RETRACE_SCANLINES*SCANLINE_CYCLES) {
                    m_state_timer=0;
                    m_state=TVOutputState_Scanout;
                }
            }
                break;

            case TVOutputState_Scanout:
            {
                if(unit->pixels.pixels[1].bits.x&VideoDataUnitFlag_VSync) {
                    m_state=TVOutputState_VerticalRetrace;
                    break;
                }

                if(unit->pixels.pixels[1].bits.x&VideoDataUnitFlag_HSync) {
                    m_state=TVOutputState_HorizontalRetrace;
                    break;
                }

                uint32_t *pixels0;
                uint32_t *pixels1;

                switch(unit->pixels.pixels[0].bits.x) {
                    default:
                    {
                        ASSERT(false);
                    }
                        break;

                    case VideoDataType_Bitmap16MHz:
                    {
                        if(m_x<TV_TEXTURE_WIDTH&&m_y<TV_TEXTURE_HEIGHT) {
                            pixels0=m_pixels_line+m_x;
                            pixels1=pixels0+TV_TEXTURE_WIDTH;

                            const VideoDataPixel p0=unit->pixels.pixels[0];
                            pixels1[0]=pixels0[0]=m_rs[p0.bits.r]|m_gs[p0.bits.g]|m_bs[p0.bits.b];

                            const VideoDataPixel p1=unit->pixels.pixels[1];
                            pixels1[1]=pixels0[1]=m_rs[p1.bits.r]|m_gs[p1.bits.g]|m_bs[p1.bits.b];

                            const VideoDataPixel p2=unit->pixels.pixels[2];
                            pixels1[2]=pixels0[2]=m_rs[p2.bits.r]|m_gs[p2.bits.g]|m_bs[p2.bits.b];

                            const VideoDataPixel p3=unit->pixels.pixels[3];
                            pixels1[3]=pixels0[3]=m_rs[p3.bits.r]|m_gs[p3.bits.g]|m_bs[p3.bits.b];

                            const VideoDataPixel p4=unit->pixels.pixels[4];
                            pixels1[4]=pixels0[4]=m_rs[p4.bits.r]|m_gs[p4.bits.g]|m_bs[p4.bits.b];

                            const VideoDataPixel p5=unit->pixels.pixels[5];
                            pixels1[5]=pixels0[5]=m_rs[p5.bits.r]|m_gs[p5.bits.g]|m_bs[p5.bits.b];

                            const VideoDataPixel p6=unit->pixels.pixels[6];
                            pixels1[6]=pixels0[6]=m_rs[p6.bits.r]|m_gs[p6.bits.g]|m_bs[p6.bits.b];

                            const VideoDataPixel p7=unit->pixels.pixels[7];
                            pixels1[7]=pixels0[7]=m_rs[p7.bits.r]|m_gs[p7.bits.g]|m_bs[p7.bits.b];

#if VIDEO_TRACK_METADATA
                            VideoDataUnit *units0=m_units_line+m_x;
                            units0[7]=units0[6]=units0[5]=units0[4]=units0[3]=units0[2]=units0[1]=units0[0]=*unit;

                            VideoDataUnit *units1=units0+TV_TEXTURE_WIDTH;
                            units1[7]=units1[6]=units1[5]=units1[4]=units1[3]=units1[2]=units1[1]=units1[0]=*unit;
#endif
                        }
                    }
                        break;

                    case VideoDataType_Teletext:
                    {
                        if(m_x<TV_TEXTURE_WIDTH&&m_y<TV_TEXTURE_HEIGHT) {
                            pixels0=m_pixels_line+m_x;
                            pixels1=pixels0+TV_TEXTURE_WIDTH;

                            uint16_t p_0=unit->pixels.pixels[2].all;
                            uint16_t p_1=unit->pixels.pixels[3].all;

                            const VideoDataPixel p00=unit->pixels.pixels[p_0&1];
                            const VideoDataPixel p01=unit->pixels.pixels[p_1&1];

                            const VideoDataPixel p10=unit->pixels.pixels[p_0>>1&1];
                            const VideoDataPixel p11=unit->pixels.pixels[p_1>>1&1];

                            const VideoDataPixel p20=unit->pixels.pixels[p_0>>2&1];
                            const VideoDataPixel p21=unit->pixels.pixels[p_1>>2&1];

                            const VideoDataPixel p30=unit->pixels.pixels[p_0>>3&1];
                            const VideoDataPixel p31=unit->pixels.pixels[p_1>>3&1];

                            const VideoDataPixel p40=unit->pixels.pixels[p_0>>4&1];
                            const VideoDataPixel p41=unit->pixels.pixels[p_1>>4&1];

                            const VideoDataPixel p50=unit->pixels.pixels[p_0>>5&1];
                            const VideoDataPixel p51=unit->pixels.pixels[p_1>>5&1];

                            // 000
                            uint32_t r00=m_rs[p00.bits.r];
                            uint32_t g00=m_gs[p00.bits.g];
                            uint32_t b00=m_bs[p00.bits.b];

                            uint32_t r01=m_rs[p01.bits.r];
                            uint32_t g01=m_gs[p01.bits.g];
                            uint32_t b01=m_bs[p01.bits.b];

                            // 011
                            uint32_t r10=(uint32_t)m_blend[p00.bits.r][p10.bits.r]<<m_r_shift;
                            uint32_t g10=(uint32_t)m_blend[p00.bits.g][p10.bits.g]<<m_g_shift;
                            uint32_t b10=(uint32_t)m_blend[p00.bits.b][p10.bits.b]<<m_b_shift;

                            uint32_t r11=(uint32_t)m_blend[p01.bits.r][p11.bits.r]<<m_r_shift;
                            uint32_t g11=(uint32_t)m_blend[p01.bits.g][p11.bits.g]<<m_g_shift;
                            uint32_t b11=(uint32_t)m_blend[p01.bits.b][p11.bits.b]<<m_b_shift;

                            // 112
                            uint32_t r20=(uint32_t)m_blend[p20.bits.r][p10.bits.r]<<m_r_shift;
                            uint32_t g20=(uint32_t)m_blend[p20.bits.g][p10.bits.g]<<m_g_shift;
                            uint32_t b20=(uint32_t)m_blend[p20.bits.b][p10.bits.b]<<m_b_shift;

                            uint32_t r21=(uint32_t)m_blend[p21.bits.r][p11.bits.r]<<m_r_shift;
                            uint32_t g21=(uint32_t)m_blend[p21.bits.g][p11.bits.g]<<m_g_shift;
                            uint32_t b21=(uint32_t)m_blend[p21.bits.b][p11.bits.b]<<m_b_shift;

                            // 222
                            uint32_t r30=m_rs[p20.bits.r];
                            uint32_t g30=m_gs[p20.bits.g];
                            uint32_t b30=m_bs[p20.bits.b];

                            uint32_t r31=m_rs[p21.bits.r];
                            uint32_t g31=m_gs[p21.bits.g];
                            uint32_t b31=m_bs[p21.bits.b];

                            // 333
                            uint32_t r40=m_rs[p30.bits.r];
                            uint32_t g40=m_gs[p30.bits.g];
                            uint32_t b40=m_bs[p30.bits.b];

                            uint32_t r41=m_rs[p31.bits.r];
                            uint32_t g41=m_gs[p31.bits.g];
                            uint32_t b41=m_bs[p31.bits.b];

                            // 344
                            uint32_t r50=(uint32_t)m_blend[p30.bits.r][p40.bits.r]<<m_r_shift;
                            uint32_t g50=(uint32_t)m_blend[p30.bits.g][p40.bits.g]<<m_g_shift;
                            uint32_t b50=(uint32_t)m_blend[p30.bits.b][p40.bits.b]<<m_b_shift;

                            uint32_t r51=(uint32_t)m_blend[p31.bits.r][p41.bits.r]<<m_r_shift;
                            uint32_t g51=(uint32_t)m_blend[p31.bits.g][p41.bits.g]<<m_g_shift;
                            uint32_t b51=(uint32_t)m_blend[p31.bits.b][p41.bits.b]<<m_b_shift;

                            // 445
                            uint32_t r60=(uint32_t)m_blend[p50.bits.r][p40.bits.r]<<m_r_shift;
                            uint32_t g60=(uint32_t)m_blend[p50.bits.g][p40.bits.g]<<m_g_shift;
                            uint32_t b60=(uint32_t)m_blend[p50.bits.b][p40.bits.b]<<m_b_shift;

                            uint32_t r61=(uint32_t)m_blend[p51.bits.r][p41.bits.r]<<m_r_shift;
                            uint32_t g61=(uint32_t)m_blend[p51.bits.g][p41.bits.g]<<m_g_shift;
                            uint32_t b61=(uint32_t)m_blend[p51.bits.b][p41.bits.b]<<m_b_shift;

                            // 555
                            uint32_t r70=m_rs[p50.bits.r];
                            uint32_t g70=m_gs[p50.bits.g];
                            uint32_t b70=m_bs[p50.bits.b];

                            uint32_t r71=m_rs[p51.bits.r];
                            uint32_t g71=m_gs[p51.bits.g];
                            uint32_t b71=m_bs[p51.bits.b];

                            pixels0[0]=r00|g00|b00;
                            pixels0[1]=r10|g10|b10;
                            pixels0[2]=r20|g20|b20;
                            pixels0[3]=r30|g30|b30;
                            pixels0[4]=r40|g40|b40;
                            pixels0[5]=r50|g50|b50;
                            pixels0[6]=r60|g60|b60;
                            pixels0[7]=r70|g70|b70;

                            pixels1[0]=r01|g01|b01;
                            pixels1[1]=r11|g11|b11;
                            pixels1[2]=r21|g21|b21;
                            pixels1[3]=r31|g31|b31;
                            pixels1[4]=r41|g41|b41;
                            pixels1[5]=r51|g51|b51;
                            pixels1[6]=r61|g61|b61;
                            pixels1[7]=r71|g71|b71;

#if VIDEO_TRACK_METADATA
                            VideoDataUnit *units0=m_units_line+m_x;
                            units0[7]=units0[6]=units0[5]=units0[4]=units0[3]=units0[2]=units0[1]=units0[0]=*unit;

                            VideoDataUnit *units1=units0+TV_TEXTURE_WIDTH;
                            units1[7]=units1[6]=units1[5]=units1[4]=units1[3]=units1[2]=units1[1]=units1[0]=*unit;
#endif
                        }
                    }
                        break;

                    case VideoDataType_Bitmap12MHz:
                    {
                        if(m_x<TV_TEXTURE_WIDTH&&m_y<TV_TEXTURE_HEIGHT) {
                            pixels0=m_pixels_line+m_x;
                            pixels1=pixels0+TV_TEXTURE_WIDTH;

                            const VideoDataPixel p0=unit->pixels.pixels[0];
                            const VideoDataPixel p1=unit->pixels.pixels[1];
                            const VideoDataPixel p2=unit->pixels.pixels[2];
                            const VideoDataPixel p3=unit->pixels.pixels[3];
                            const VideoDataPixel p4=unit->pixels.pixels[4];
                            const VideoDataPixel p5=unit->pixels.pixels[5];

                            // 000
                            uint32_t r0=m_rs[p0.bits.r];
                            uint32_t g0=m_gs[p0.bits.g];
                            uint32_t b0=m_bs[p0.bits.b];

                            // 011
                            uint32_t r1=(uint32_t)m_blend[p0.bits.r][p1.bits.r]<<m_r_shift;
                            uint32_t g1=(uint32_t)m_blend[p0.bits.g][p1.bits.g]<<m_g_shift;
                            uint32_t b1=(uint32_t)m_blend[p0.bits.b][p1.bits.b]<<m_b_shift;

                            // 112
                            uint32_t r2=(uint32_t)m_blend[p2.bits.r][p1.bits.r]<<m_r_shift;
                            uint32_t g2=(uint32_t)m_blend[p2.bits.g][p1.bits.g]<<m_g_shift;
                            uint32_t b2=(uint32_t)m_blend[p2.bits.b][p1.bits.b]<<m_b_shift;

                            // 222
                            uint32_t r3=m_rs[p2.bits.r];
                            uint32_t g3=m_gs[p2.bits.g];
                            uint32_t b3=m_bs[p2.bits.b];

                            // 333
                            uint32_t r4=m_rs[p3.bits.r];
                            uint32_t g4=m_gs[p3.bits.g];
                            uint32_t b4=m_bs[p3.bits.b];

                            // 344
                            uint32_t r5=(uint32_t)m_blend[p3.bits.r][p4.bits.r]<<m_r_shift;
                            uint32_t g5=(uint32_t)m_blend[p3.bits.g][p4.bits.g]<<m_g_shift;
                            uint32_t b5=(uint32_t)m_blend[p3.bits.b][p4.bits.b]<<m_b_shift;

                            // 445
                            uint32_t r6=(uint32_t)m_blend[p5.bits.r][p4.bits.r]<<m_r_shift;
                            uint32_t g6=(uint32_t)m_blend[p5.bits.g][p4.bits.g]<<m_g_shift;
                            uint32_t b6=(uint32_t)m_blend[p5.bits.b][p4.bits.b]<<m_b_shift;

                            // 555
                            uint32_t r7=m_rs[p5.bits.r];
                            uint32_t g7=m_gs[p5.bits.g];
                            uint32_t b7=m_bs[p5.bits.b];

                            pixels1[0]=pixels0[0]=r0|g0|b0;
                            pixels1[1]=pixels0[1]=r1|g1|b1;
                            pixels1[2]=pixels0[2]=r2|g2|b2;
                            pixels1[3]=pixels0[3]=r3|g3|b3;
                            pixels1[4]=pixels0[4]=r4|g4|b4;
                            pixels1[5]=pixels0[5]=r5|g5|b5;
                            pixels1[6]=pixels0[6]=r6|g6|b6;
                            pixels1[7]=pixels0[7]=r7|g7|b7;

#if VIDEO_TRACK_METADATA
                            VideoDataUnit *units0=m_units_line+m_x;
                            units0[7]=units0[6]=units0[5]=units0[4]=units0[3]=units0[2]=units0[1]=units0[0]=*unit;

                            VideoDataUnit *units1=units0+TV_TEXTURE_WIDTH;
                            units1[7]=units1[6]=units1[5]=units1[4]=units1[3]=units1[2]=units1[1]=units1[0]=*unit;
#endif
                        }
                    }
                        break;
                }

                m_x+=8;

                if(m_state_timer++>=SCAN_OUT_CYCLES) {
                    m_state=TVOutputState_HorizontalRetrace;
                }
            }
                break;

            case TVOutputState_HorizontalRetrace:
                m_state=TVOutputState_HorizontalRetraceWait;
                m_x=0;
                m_y+=HEIGHT_SCALE;
                if(m_y>=MAX_NUM_SCANNED_LINES) {
                    // VBlank time anyway.
                    m_state=TVOutputState_VerticalRetrace;
                    break;
                }
                m_pixels_line+=TV_TEXTURE_WIDTH*HEIGHT_SCALE;
#if VIDEO_TRACK_METADATA
                m_units_line+=TV_TEXTURE_WIDTH*HEIGHT_SCALE;
#endif
                m_state_timer=2;//+1 for Scanout; +1 for this state
                m_state=TVOutputState_HorizontalRetraceWait;
                break;

            case TVOutputState_HorizontalRetraceWait:
            {
                // Ignore input in this state.
                ++m_state_timer;
                if(m_state_timer>=HORIZONTAL_RETRACE_CYCLES) {
                    m_state_timer=0;
                    m_state=TVOutputState_BackPorch;
                }
            }
                break;

            case TVOutputState_BackPorch:
            {
                // Ignore input in this state.
                ++m_state_timer;
                if(m_state_timer>=BACK_PORCH_CYCLES) {
                    m_state_timer=0;
                    m_state=TVOutputState_Scanout;
                }
            }
                break;
        }
    }
}

#if BUILD_TYPE_Debug
#ifdef _MSC_VER
#pragma optimize("",on)
#endif
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER

void TVOutput::FillWithTestPattern() {
    m_texture_pixels.clear();
    m_texture_pixels.reserve(TV_TEXTURE_WIDTH*TV_TEXTURE_HEIGHT);

    uint32_t palette[8];
    for(size_t i=0;i<8;++i) {
        palette[i]=(i&1?m_rs[15]:0)|(i&2?m_gs[15]:0)|(i&4?m_bs[15]:0);
    }

    for(int y=0;y<TV_TEXTURE_HEIGHT;++y) {
        for(int x=0;x<TV_TEXTURE_WIDTH;++x) {
            if((x^y)&1) {
                m_texture_pixels.push_back(palette[1]);
            } else {
                m_texture_pixels.push_back(palette[3]);
            }
        }
    }

    uint32_t colours[]={palette[1],palette[6]};

    for(size_t i=0;i<sizeof colours/sizeof colours[0];++i) {
        uint32_t colour=colours[i];

        for(size_t x=i;x<TV_TEXTURE_WIDTH-i;++x) {
            m_texture_pixels[i*TV_TEXTURE_WIDTH+x]=colour;
            m_texture_pixels[(TV_TEXTURE_HEIGHT-1-i)*TV_TEXTURE_WIDTH+x]=colour;
        }

        for(size_t y=i;y<TV_TEXTURE_HEIGHT-i;++y) {
            m_texture_pixels[y*TV_TEXTURE_WIDTH+i]=colour;
            m_texture_pixels[y*TV_TEXTURE_WIDTH+TV_TEXTURE_WIDTH-1-i]=colour;
        }
    }

    {
        for(size_t cy=0;cy<TV_TEXTURE_HEIGHT;cy+=50) {
            size_t tmp=cy;
            size_t cx=100;
            do {
                const uint8_t *digit=DIGITS[tmp%10];

                for(size_t gy=0;gy<13;++gy) {
                    if(cy+gy>=TV_TEXTURE_HEIGHT) {
                        break;
                    }

                    uint32_t *line=&m_texture_pixels[cx+(cy+gy)*TV_TEXTURE_WIDTH];
                    uint8_t row=*digit++;

                    for(size_t gx=0;gx<6;++gx) {
                        if(row&1) {
                            *line=palette[0];
                        }

                        row>>=1;
                        ++line;
                    }
                }

                cx-=6;
                tmp/=10;
            } while(tmp!=0);
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const uint32_t *TVOutput::GetTexturePixels(uint64_t *texture_data_version) const {
    if(texture_data_version) {
        *texture_data_version=m_texture_data_version;
    }

    return m_texture_pixels.data();
}


//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void TVOutput::CopyTexturePixels(void *dest_pixels,size_t dest_pitch_bytes) const {
    ASSERT(dest_pitch_bytes>0);
    size_t src_pitch_bytes=TV_TEXTURE_WIDTH*4;

    if(src_pitch_bytes==dest_pitch_bytes) {
        memcpy(dest_pixels,m_texture_pixels.data(),TV_TEXTURE_HEIGHT*TV_TEXTURE_WIDTH*4);
    } else {
        auto dest=(char *)dest_pixels;
        auto src=(const char *)m_texture_pixels.data();

        for(size_t y=0;y<TV_TEXTURE_HEIGHT;++y) {
            memcpy(dest,src,src_pitch_bytes);
            dest+=dest_pitch_bytes;
            src+=src_pitch_bytes;
        }
    }

    if(this->show_usec_markers||this->show_half_usec_markers) {
        for(size_t x=0;x<TV_TEXTURE_WIDTH;x+=8) {
            char *dest=(char*)((uint32_t *)dest_pixels+x);
            const char *src=(const char *)(m_texture_pixels.data()+x);

            if(this->show_usec_markers&&(x&15)==0) {
                for(size_t y=0;y<TV_TEXTURE_HEIGHT;++y) {
                    *(uint32_t *)dest=*(const uint32_t *)src^m_usec_marker_xor;

                    dest+=dest_pitch_bytes;
                    src+=src_pitch_bytes;
                }
            } else if(this->show_half_usec_markers&&(x&7)==0) {
                for(size_t y=0;y<TV_TEXTURE_HEIGHT;++y) {
                    *(uint32_t *)dest=*(const uint32_t *)src^m_half_usec_marker_xor;

                    dest+=dest_pitch_bytes;
                    src+=src_pitch_bytes;
                }
            }
        }
    }

#if VIDEO_TRACK_METADATA
    // It's best to do this stuff as a postprocess, so it can be toggled
    // while the emulation is paused.
    this->AddMetadataMarkers(dest_pixels,
                             dest_pitch_bytes,
                             this->show_6845_row_markers,
                             VideoDataUnitMetadataFlag_6845Raster0,
                             m_6845_raster0_marker_xor);

    this->AddMetadataMarkers(dest_pixels,
                             dest_pitch_bytes,
                             this->show_6845_dispen_markers,
                             VideoDataUnitMetadataFlag_6845DISPEN,
                             m_6845_dispen_marker_xor);
#endif


    if(this->show_usec_markers||
       this->show_half_usec_markers||
       this->show_6845_dispen_markers||
       this->show_6845_row_markers)
    {
        uint32_t bg=0;
        uint32_t fg=0xffu<<m_r_shift|0xffu<<m_g_shift|0xffu<<m_b_shift;

        for(size_t x=0;x<TV_TEXTURE_WIDTH;x+=16) {
            size_t column=x/16;
            size_t tens=column/10%10;
            size_t units=column%10;
            char *dest_bytes=(char *)((uint32_t *)dest_pixels+x+3);

            for(size_t y=0;y<13;++y) {
                auto dest=(uint32_t *)dest_bytes;

                dest[0]=DIGITS[tens][y]&0x01?fg:bg;
                dest[1]=DIGITS[tens][y]&0x02?fg:bg;
                dest[2]=DIGITS[tens][y]&0x04?fg:bg;
                dest[3]=DIGITS[tens][y]&0x08?fg:bg;
                dest[4]=DIGITS[tens][y]&0x10?fg:bg;
                dest[5]=DIGITS[tens][y]&0x20?fg:bg;
                dest[6]=DIGITS[units][y]&0x01?fg:bg;
                dest[7]=DIGITS[units][y]&0x02?fg:bg;
                dest[8]=DIGITS[units][y]&0x04?fg:bg;
                dest[9]=DIGITS[units][y]&0x08?fg:bg;
                dest[10]=DIGITS[units][y]&0x10?fg:bg;
                dest[11]=DIGITS[units][y]&0x20?fg:bg;

                dest_bytes+=dest_pitch_bytes;
            }
        }
    }

#if BBCMICRO_DEBUGGER
    if(this->show_beam_position) {
        if(m_x<TV_TEXTURE_WIDTH&&m_y<TV_TEXTURE_HEIGHT) {
            // Sneak it in on top. Don't read from the
            // locked data.

            //if(m_pixel_format->format==SDL_PIXELFORMAT_ARGB8888) {
            //} else {
            auto dest=(uint32_t *)((char *)dest_pixels+m_y*(size_t)dest_pitch_bytes);
            auto src=(const uint32_t *)((const char *)m_texture_pixels.data()+m_y*src_pitch_bytes);

            for(size_t i=m_x;i<TV_TEXTURE_WIDTH;++i) {
                dest[i]=src[i]^m_beam_marker_xor;
            }
        }
    }
#endif
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if VIDEO_TRACK_METADATA
const VideoDataUnit *TVOutput::GetTextureUnits() const {
    return m_texture_units.data();
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool TVOutput::GetBeamPosition(size_t *x,size_t *y) const {
    if(m_x>=TV_TEXTURE_WIDTH||m_y>=TV_TEXTURE_HEIGHT) {
        return false;
    } else {
        *x=m_x;
        *y=m_y;

        return true;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool TVOutput::IsInVerticalBlank() const {
    return m_state==TVOutputState_VerticalRetrace||m_state==TVOutputState_VerticalRetraceWait;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

double TVOutput::GetGamma() const {
    return m_gamma;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void TVOutput::SetGamma(double gamma) {
    m_gamma=gamma;

    this->InitPalette();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static uint8_t GetByte(double x) {
    if(x<0.) {
        return 0;
    } else if(x>=1.) {
        return 255;
    } else {
        return (uint8_t)(x*255.);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint32_t TVOutput::GetTexelValue(uint8_t r,uint8_t g,uint8_t b) const {
    uint32_t value=0;

    value|=(uint32_t)r<<m_r_shift;
    value|=(uint32_t)g<<m_g_shift;
    value|=(uint32_t)b<<m_b_shift;

    return value;
}

void TVOutput::InitPalette() {
    for(uint8_t i=0;i<16;++i) {
        uint8_t value=i<<4|i;

        m_rs[i]=this->GetTexelValue(value,0,0);
        m_gs[i]=this->GetTexelValue(0,value,0);
        m_bs[i]=this->GetTexelValue(0,0,value);
    }

    for(size_t i=0;i<16;++i) {
        for(size_t j=0;j<16;++j) {
            double a=pow(i/15.,m_gamma);
            double b=pow(j/15.,m_gamma);

            double value=pow((a+b+b)/3,1./m_gamma);

            m_blend[i][j]=GetByte(value);
        }
    }

    m_usec_marker_xor=this->GetTexelValue(0,128,128);
    m_half_usec_marker_xor=this->GetTexelValue(128,0,0);
    m_6845_raster0_marker_xor=this->GetTexelValue(128,128,0);
    m_6845_dispen_marker_xor=this->GetTexelValue(128,0,128);

    m_beam_marker_xor=this->GetTexelValue(255,255,255);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if VIDEO_TRACK_METADATA
void TVOutput::AddMetadataMarkers(void *dest_pixels,
                                  size_t dest_pitch_bytes,
                                  bool add,
                                  uint8_t metadata_flag,
                                  uint32_t xor_value) const
{
    if(!add) {
        return;
    }

    for(size_t y=0;y<TV_TEXTURE_HEIGHT;y+=2) {
        auto dest=(uint32_t *)((char *)dest_pixels+y*dest_pitch_bytes);
        const uint32_t *src=m_texture_pixels.data()+y*TV_TEXTURE_WIDTH;
        const VideoDataUnit *unit=m_texture_units.data()+y*TV_TEXTURE_WIDTH;

        for(size_t x=0;x<TV_TEXTURE_WIDTH;++x) {
            if(unit++->metadata.flags&metadata_flag) {
                dest[x]=src[x]^xor_value;
            }
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
