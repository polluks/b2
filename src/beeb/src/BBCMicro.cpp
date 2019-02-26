#include <shared/system.h>
#include <shared/log.h>
#include <shared/debug.h>
#include <beeb/BBCMicro.h>
#include <beeb/VideoULA.h>
#include <beeb/teletext.h>
#include <beeb/OutputData.h>
#include <beeb/sound.h>
#include <beeb/video.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <beeb/conf.h>
#include <errno.h>
#include <beeb/keys.h>
#include <beeb/conf.h>
#include <beeb/DiscInterface.h>
#include <beeb/ExtMem.h>
#include <beeb/Trace.h>
#include <memory>
#include <vector>
#include <beeb/DiscImage.h>
#include <map>
#include <limits.h>
#include <algorithm>
#include <inttypes.h>
#include <beeb/BeebLink.h>

#include <shared/enum_decl.h>
#include "BBCMicro_private.inl"
#include <shared/enum_end.h>

#include <shared/enum_def.h>
#include <beeb/BBCMicro.inl>
#include "BBCMicro_private.inl"
#include <shared/enum_end.h>

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static const std::shared_ptr<DiscImage> NULL_DISCIMAGE_PTR;
static std::map<DiscDriveType,std::array<std::vector<float>,DiscDriveSound_EndValue>> g_disc_drive_sounds;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// The key to press to start the paste going.
static const BeebKey PASTE_START_KEY=BeebKey_Space;

// The corresponding char, so it can be removed when copying the BASIC
// listing.
const char BBCMicro::PASTE_START_CHAR=' ';

#if BBCMICRO_TRACE
const TraceEventType BBCMicro::INSTRUCTION_EVENT("BBCMicroInstruction",sizeof(InstructionTraceEvent));
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
// The async call thunk lives in an undefined area of FRED.
static const M6502Word ASYNC_CALL_THUNK_ADDR={0xfc50};
static const int ASYNC_CALL_TIMEOUT=1000000;
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
//
// Total max addressable memory on the BBC is 336K:
//
// - 64K RAM (main+shadow+ANDY+HAZEL)
// - 256K ROM (16 * 16K)
// - 16K MOS
//
// The paging operates at a 4K resolution, so this can be divided into 84
// 4K pages, or (to pick a term) big pages. (1 big page = 16 pages.) The big pages
// are assigned like this:
//
// 0-7     main
// 8       ANDY (M128)/ANDY (B+)
// 9,10    HAZEL (M128)/ANDY (B+)
// 11-15   shadow RAM (M128/B+)
// 16-19   ROM 0
// 20-23   ROM 1
// ...
// 76-79   ROM 15
// 80-83   MOS
//
// (Three additional pages, for FRED/JIM/SHEILA, are planned.)
//
// (On the B, big pages 8-15 are the same as big pages 0-7.)
//
// Each big page can be set up once, when the BBCMicro is first created,
// simplifying some of the logic. When switching to ROM 1, for example,
// the buffers can be found by looking at big pages 20-23, rather than having
// to check m_state.sideways_rom_buffers[1] (etc.).
//
// The per-big page struct can also contain some cold info (debug flags, static
// data, etc.), as it's only fetched when the paging registers are changed,
// rather than for every instruction.
//
// Big pages are only really a concept in this flattened view of the memory,
// but they are used in a couple of places to refer to 6502 addresses when the
// 4K resolution makes sense, treating the 6502 memory map as 16 big pages.
//
// TODO: The M6502Word 8/8 MSB/LSB split makes sense for the 6502 emulation.
// But for the BBC-specific bits, perhaps an additional union member, with a
// 4/12 big page/offset split, would be useful...

//static constexpr size_t BIG_PAGE_SIZE_BYTES=4096;
//static constexpr size_t BIG_PAGE_OFFSET_MASK=4095;

//static constexpr size_t BIG_PAGE_SIZE_PAGES=BIG_PAGE_SIZE_BYTES/256u;

static constexpr uint8_t MAIN_BIG_PAGE_INDEX=0;
static constexpr uint8_t NUM_MAIN_BIG_PAGES=32/4;

static constexpr uint8_t ANDY_BIG_PAGE_INDEX=MAIN_BIG_PAGE_INDEX+NUM_MAIN_BIG_PAGES;
static constexpr uint8_t NUM_ANDY_BIG_PAGES=4/4;

static constexpr uint8_t HAZEL_BIG_PAGE_INDEX=ANDY_BIG_PAGE_INDEX+NUM_ANDY_BIG_PAGES;
static constexpr uint8_t NUM_HAZEL_BIG_PAGES=8/4;

static constexpr uint8_t BPLUS_RAM_BIG_PAGE_INDEX=ANDY_BIG_PAGE_INDEX;
static constexpr uint8_t NUM_BPLUS_RAM_BIG_PAGES=12/4;

static constexpr uint8_t SHADOW_BIG_PAGE_INDEX=HAZEL_BIG_PAGE_INDEX+NUM_HAZEL_BIG_PAGES;
static constexpr uint8_t NUM_SHADOW_BIG_PAGES=20/4;

static constexpr uint8_t ROM0_BIG_PAGE_INDEX=SHADOW_BIG_PAGE_INDEX+NUM_SHADOW_BIG_PAGES;
static constexpr uint8_t NUM_ROM_BIG_PAGES=16/4;

static constexpr uint8_t MOS_BIG_PAGE_INDEX=ROM0_BIG_PAGE_INDEX+16*NUM_ROM_BIG_PAGES;
static constexpr uint8_t NUM_MOS_BIG_PAGES=16/4;

static constexpr uint8_t NUM_BIG_PAGES=MOS_BIG_PAGE_INDEX+NUM_MOS_BIG_PAGES;

//#define ANDY_OFFSET (0x8000u+0u)
//#define NUM_ANDY_PAGES (0x10u)
//#define HAZEL_OFFSET (0x8000+0x1000u)
//#define NUM_HAZEL_PAGES (0x20u)
//#define SHADOW_OFFSET (0x8000+0x3000u)
//#define NUM_SHADOW_PAGES (0x30u)

#if BBCMICRO_DEBUGGER

static const BBCMicro::DebugState::ByteDebugFlags DUMMY_BYTE_DEBUG_FLAGS={};

#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static const BBCMicro::WriteMMIOFn g_R6522_write_fns[16]={
    &R6522::Write0,&R6522::Write1,&R6522::Write2,&R6522::Write3,&R6522::Write4,&R6522::Write5,&R6522::Write6,&R6522::Write7,
    &R6522::Write8,&R6522::Write9,&R6522::WriteA,&R6522::WriteB,&R6522::WriteC,&R6522::WriteD,&R6522::WriteE,&R6522::WriteF,
};

static const BBCMicro::ReadMMIOFn g_R6522_read_fns[16]={
    &R6522::Read0,&R6522::Read1,&R6522::Read2,&R6522::Read3,&R6522::Read4,&R6522::Read5,&R6522::Read6,&R6522::Read7,
    &R6522::Read8,&R6522::Read9,&R6522::ReadA,&R6522::ReadB,&R6522::ReadC,&R6522::ReadD,&R6522::ReadE,&R6522::ReadF,
};

static const BBCMicro::WriteMMIOFn g_WD1770_write_fns[]={&WD1770::Write0,&WD1770::Write1,&WD1770::Write2,&WD1770::Write3,};
static const BBCMicro::ReadMMIOFn g_WD1770_read_fns[]={&WD1770::Read0,&WD1770::Read1,&WD1770::Read2,&WD1770::Read3,};

static const uint8_t g_unmapped_reads[BBCMicro::BIG_PAGE_SIZE_BYTES]={0,};
static uint8_t g_unmapped_writes[BBCMicro::BIG_PAGE_SIZE_BYTES];

const uint16_t BBCMicro::SCREEN_WRAP_ADJUSTMENTS[]={
    0x4000>>3,
    0x2000>>3,
    0x5000>>3,
    0x2800>>3
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

BBCMicro::State::State(const BBCMicroType type,
                       const std::vector<uint8_t> &nvram_contents,
                       bool power_on_tone,
                       const tm *rtc_time,
                       uint64_t initial_num_2MHz_cycles):
num_2MHz_cycles(initial_num_2MHz_cycles)
{
    switch(type) {
    default:
        ASSERT(false);
        // fall through
    case BBCMicroType_B:
        this->ram_buffer.resize(32768);
        M6502_Init(&this->cpu,&M6502_nmos6502_config);
        break;

    case BBCMicroType_BPlus:
        this->ram_buffer.resize(65536);
        M6502_Init(&this->cpu,&M6502_nmos6502_config);
        break;

    case BBCMicroType_Master:
        this->ram_buffer.resize(65536);
        M6502_Init(&this->cpu,&M6502_cmos6502_config);
        this->rtc.SetRAMContents(nvram_contents);

        if(rtc_time) {
            this->rtc.SetTime(rtc_time);
        }

        break;
    }

    //for(int i=0;i<NUM_DRIVES;++i) {
    //    DiscDrive_Init(&this->drives[i],i);
    //}

    this->sn76489.Reset(power_on_tone);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

BBCMicro::BBCMicro(BBCMicroType type,
                   const DiscInterfaceDef *def,
                   const std::vector<uint8_t> &nvram_contents,
                   const tm *rtc_time,
                   bool video_nula,
                   bool ext_mem,
                   bool power_on_tone,
                   BeebLinkHandler *beeblink_handler,
                   uint64_t initial_num_2MHz_cycles):
m_state(type,
        nvram_contents,
        power_on_tone,
        rtc_time,
        initial_num_2MHz_cycles),
m_type(type),
m_disc_interface(def?def->create_fun():nullptr),
m_video_nula(video_nula),
m_ext_mem(ext_mem),
m_beeblink_handler(beeblink_handler)
{
    this->InitStuff();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

BBCMicro::BBCMicro(const BBCMicro &src):
    m_state(src.m_state),
    m_type(src.m_type),
    m_disc_interface(src.m_disc_interface?src.m_disc_interface->Clone():nullptr),
    m_video_nula(src.m_video_nula),
    m_ext_mem(src.m_ext_mem)
{
    ASSERT(src.GetCloneImpediments()==0);

    for(int i=0;i<NUM_DRIVES;++i) {
        std::shared_ptr<DiscImage> disc_image=DiscImage::Clone(src.GetDiscImage(i));
        this->SetDiscImage(i,std::move(disc_image));
    }

    this->InitStuff();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

BBCMicro::~BBCMicro() {
#if BBCMICRO_TRACE
    this->StopTrace();
#endif

    delete m_disc_interface;

    //for(int i=0;i<16;++i) {
    //    SetROMContents(this,&m_state.roms[i],nullptr,0);
    //}

    //SetROMContents(this,&m_state.os,nullptr,0);

    delete m_shadow_pages;
    m_shadow_pages=nullptr;

    delete[] m_pc_pages;
    m_pc_pages=nullptr;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint32_t BBCMicro::GetCloneImpediments() const {
    uint32_t result=0;

    for(int i=0;i<NUM_DRIVES;++i) {
        if(!!m_disc_images[i]) {
            if(!m_disc_images[i]->CanClone()) {
                result|=(uint32_t)BBCMicroCloneImpediment_Drive0<<i;
            }
        }
    }

    if(m_beeblink_handler) {
        result|=BBCMicroCloneImpediment_BeebLink;
    }

    return result;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::unique_ptr<BBCMicro> BBCMicro::Clone() const {
    if(this->GetCloneImpediments()!=0) {
        return nullptr;
    }

    return std::unique_ptr<BBCMicro>(new BBCMicro(*this));
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_TRACE
void BBCMicro::SetTrace(std::shared_ptr<Trace> trace,uint32_t trace_flags) {
    m_trace_ptr=std::move(trace);
    m_trace=m_trace_ptr.get();
    m_trace_current_instruction=nullptr;
    m_trace_flags=trace_flags;

    if(m_trace) {
        m_trace->SetTime(&m_state.num_2MHz_cycles);

        m_trace->AllocM6502ConfigEvent(m_state.cpu.config);
    }

    m_state.fdc.SetTrace(trace_flags&BBCMicroTraceFlag_1770?m_trace:nullptr);
    m_state.rtc.SetTrace(trace_flags&BBCMicroTraceFlag_RTC?m_trace:nullptr);
    m_state.crtc.SetTrace(
        (trace_flags&(BBCMicroTraceFlag_6845VSync|BBCMicroTraceFlag_6845Scanlines))?m_trace:nullptr,
        !!(trace_flags&BBCMicroTraceFlag_6845Scanlines));
    m_state.system_via.SetTrace(trace_flags&BBCMicroTraceFlag_SystemVIA?m_trace:nullptr);
    m_state.user_via.SetTrace(trace_flags&BBCMicroTraceFlag_UserVIA?m_trace:nullptr);
    m_state.video_ula.SetTrace(trace_flags&BBCMicroTraceFlag_VideoULA?m_trace:nullptr);
    m_state.sn76489.SetTrace(trace_flags&BBCMicroTraceFlag_SN76489?m_trace:nullptr);

    if(!!m_beeblink) {
        m_beeblink->SetTrace(trace_flags&BBCMicroTraceFlag_BeebLink?m_trace:nullptr);
    }

    this->UpdateCPUDataBusFn();
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SetPages(MemoryPages *pages0,
                        MemoryPages *pages1,
                        uint8_t big_page_index,
                        uint8_t num_big_pages,
                        uint8_t dest_big_page_index)
{

    ASSERT(dest_big_page_index<16);
    uint8_t page=dest_big_page_index<<4;

    for(uint8_t i=0;i<num_big_pages;++i) {
        const BigPage *bp=&m_big_pages[big_page_index+i];

        const uint8_t *r=bp->r;
        if(!r) {
            r=g_unmapped_reads;
        }

        uint8_t *w=bp->w;
        if(!w) {
            w=g_unmapped_writes;
        }

#if BBCMICRO_DEBUGGER
        ASSERT(bp->index<NUM_BIG_PAGES);
        DebugState::ByteDebugFlags *debug=m_debug->big_pages_debug_flags[bp->index];
#endif

        if(pages0&&pages1) {
            for(size_t j=0;j<BIG_PAGE_SIZE_PAGES;++j) {
                pages0->r[page]=pages1->r[page]=r;
                r+=256;

                pages0->w[page]=pages1->w[page]=w;
                w+=256;

#if BBCMICRO_DEBUGGER
                pages0->debug[page]=pages1->debug[page]=debug;
                debug+=256;

                pages0->bp[page]=pages1->bp[page]=bp;
#endif

                ++page;
            }
        } else {
            for(size_t j=0;j<BIG_PAGE_SIZE_PAGES;++j) {
                pages0->r[page]=r;
                r+=256;

                pages0->w[page]=w;
                w+=256;

#if BBCMICRO_DEBUGGER
                pages0->debug[page]=debug;
                debug+=256;

                pages0->bp[page]=bp;
#endif

                ++page;
            }
        }
    }
}

//void BBCMicro::SetPages(uint8_t page_,size_t num_pages,
//                        const uint8_t *read_data,size_t read_page_stride,
//                        uint8_t *write_data,size_t write_page_stride
//#if BBCMICRO_DEBUGGER////////////////////////////////////////////////////////<--note
//                        ,uint16_t debug_page//////////////////////<--note
//#endif///////////////////////////////////////////////////////////////////////<--note
//)////////////////////////////////////////////////////////////////////////////<--note
//{
//    ASSERT(read_page_stride==256||read_page_stride==0);
//    ASSERT(write_page_stride==256||write_page_stride==0);
//    uint8_t page=page_;
//
//    if(m_shadow_pages) {
//        for(size_t i=0;i<num_pages;++i) {
//            m_shadow_pages->r[page]=m_pages.r[page]=read_data;
//            read_data+=read_page_stride;
//
//            m_shadow_pages->w[page]=m_pages.w[page]=write_data;
//            write_data+=write_page_stride;
//
//#if BBCMICRO_DEBUGGER
//
//            m_shadow_pages->debug_page_index[page]=m_pages.debug_page_index[page]=debug_page;
//
//            if(m_debug) {
//                m_shadow_pages->debug[page]=m_pages.debug[page]=m_debug->pages[debug_page];
//            }
//
//            ++debug_page;
//#endif
//
//            ++page;
//        }
//    } else {
//        for(size_t i=0;i<num_pages;++i) {
//            m_pages.r[page]=read_data;
//            read_data+=read_page_stride;
//
//            m_pages.w[page]=write_data;
//            write_data+=write_page_stride;
//
//#if BBCMICRO_DEBUGGER
//            m_pages.debug_page_index[page]=debug_page;
//
//            if(m_debug) {
//                m_pages.debug[page]=m_debug->pages[debug_page];
//            }
//
//            ++debug_page;
//#endif
//
//            ++page;
//        }
//    }
//}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//void BBCMicro::SetOSPages(uint8_t dest_page,uint8_t src_page,uint8_t num_pages) {
//    if(!!m_state.os_buffer) {
//        const uint8_t *data=m_state.os_buffer->data();
//        this->SetPages(dest_page,num_pages,
//                       data+src_page*256,256,
//                       g_rom_writes,0
//#if BBCMICRO_DEBUGGER
//                       ,DEBUG_OS_PAGE
//#endif
//        );
//    } else {
//        this->SetPages(dest_page,num_pages,
//                       g_unmapped_rom_reads,0,
//                       g_rom_writes,0
//#if BBCMICRO_DEBUGGER
//                       ,DEBUG_OS_PAGE
//#endif
//        );
//    }
//}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//#if BBCMICRO_DEBUGGER
//#define SET_READ_ONLY_PAGES(PAGE,NUM_PAGES,DATA,DEBUG) SetPages((PAGE),(NUM_PAGES),(DATA),256,g_rom_writes,0,(DEBUG))
//#define SET_READWRITE_PAGES(PAGE,NUM_PAGES,DATA,DEBUG) SetPages((PAGE),(NUM_PAGES),(DATA),256,(DATA),256,(DEBUG))
//#else
//#define SET_READ_ONLY_PAGES(PAGE,NUM_PAGES,DATA,DEBUG) SetPages((PAGE),(NUM_PAGES),(DATA),256,g_rom_writes,0)
//#define SET_READWRITE_PAGES(PAGE,NUM_PAGES,DATA,DEBUG) SetPages((PAGE),(NUM_PAGES),(DATA),256,(DATA),256)
//#endif

void BBCMicro::SetROMPages(uint8_t rom,uint8_t num_skipped_big_pages) {
    ASSERT(rom<16);
    ASSERT(num_skipped_big_pages<NUM_ROM_BIG_PAGES);

    this->SetPages(&m_pages,
                   m_shadow_pages,
                   ROM0_BIG_PAGE_INDEX+rom*NUM_ROM_BIG_PAGES+num_skipped_big_pages,
                   NUM_ROM_BIG_PAGES-num_skipped_big_pages,
                   0x08+num_skipped_big_pages);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::UpdateBROMSELPages(BBCMicro *m) {
    m->SetROMPages(m->m_state.romsel.b_bits.pr,0);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::UpdateBACCCONPages(BBCMicro *m,const ACCCON *old) {
    (void)m,(void)old;

    // This function only exists to save on a couple of NULL checks.
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::UpdateBPlusROMSELPages(BBCMicro *m) {
    if(m->m_state.romsel.bplus_bits.ram) {
        m->SetPages(&m->m_pages,
                    m->m_shadow_pages,
                    BPLUS_RAM_BIG_PAGE_INDEX,
                    NUM_BPLUS_RAM_BIG_PAGES,
                    0x08);

        m->SetROMPages(m->m_state.romsel.bplus_bits.pr,NUM_BPLUS_RAM_BIG_PAGES);
    } else {
        m->SetROMPages(m->m_state.romsel.bplus_bits.pr,0);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::UpdateBPlusACCCONPages(BBCMicro *m,const ACCCON *old) {
    (void)old;

    const MemoryPages *pages;
    if(m->m_state.acccon.bplus_bits.shadow) {
        pages=m->m_shadow_pages;
        m->m_state.shadow_select_mask=0x8000;
    } else {
        pages=&m->m_pages;
        m->m_state.shadow_select_mask=0x0000;
    }

    // This is what BeebEm does! Also mentioned in passing in
    // http://beebwiki.mdfs.net/Paged_ROM - the NAUG on the other hand
    // says nothing.
    for(uint8_t i=0xa0;i<0xb0;++i) {
        m->m_pc_pages[i]=pages;
    }

    for(uint8_t i=0xc0;i<0xe0;++i) {
        m->m_pc_pages[i]=pages;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::UpdateMaster128ROMSELPages(BBCMicro *m) {
    if(m->m_state.romsel.m128_bits.ram) {
        m->SetPages(&m->m_pages,
                    m->m_shadow_pages,
                    ANDY_BIG_PAGE_INDEX,
                    NUM_ANDY_BIG_PAGES,
                    0x08);

        m->SetROMPages(m->m_state.romsel.m128_bits.pm,NUM_ANDY_BIG_PAGES);
    } else {
        m->SetROMPages(m->m_state.romsel.m128_bits.pm,0);
    }
}

// YXE  Usr  MOS
// ---  ---  ---
// 000   M    M
// 001   M    S
// 010   S    M
// 011   S    S
// 100   M    M
// 101   M    M
// 110   S    S
// 111   S    S
//
// Usr Shadow = E
//
// MOS Shadow = (Y AND X) OR (NOT Y AND E)

bool BBCMicro::DoesMOSUseShadow(ACCCON acccon) {
    if(acccon.m128_bits.y) {
        return acccon.m128_bits.x;
    } else {
        return acccon.m128_bits.e;
    }
}

void BBCMicro::UpdateMaster128ACCCONPages(BBCMicro *m,const ACCCON *old_) {
    ACCCON old;
    if(old_) {
        old=*old_;
    } else {
        old.value=~m->m_state.acccon.value;
    }

    ACCCON diff;
    diff.value=m->m_state.acccon.value^old.value;

    if(diff.m128_bits.y) {
        ASSERT(m->m_state.acccon.m128_bits.y!=old.m128_bits.y);
        if(m->m_state.acccon.m128_bits.y) {
            // 8K FS RAM at 0xC000
            m->SetPages(&m->m_pages,
                        m->m_shadow_pages,
                        HAZEL_BIG_PAGE_INDEX,
                        NUM_HAZEL_BIG_PAGES,
                        0x0c);
        } else {
            // MOS at 0xC0000
            m->SetPages(&m->m_pages,
                        m->m_shadow_pages,
                        MOS_BIG_PAGE_INDEX,
                        NUM_HAZEL_BIG_PAGES,
                        0x0c);
        }
    }

    int usr_shadow=!!m->m_state.acccon.m128_bits.x;
    int mos_shadow=DoesMOSUseShadow(m->m_state.acccon);

    int old_usr_shadow=!!old.m128_bits.x;
    int old_mos_shadow=DoesMOSUseShadow(old);

    if(usr_shadow!=old_usr_shadow||mos_shadow!=old_mos_shadow) {
        const MemoryPages *usr_pages;
        if(usr_shadow) {
            usr_pages=m->m_shadow_pages;
        } else {
            usr_pages=&m->m_pages;
        }

        const MemoryPages *mos_pages;
        if(mos_shadow) {
            mos_pages=m->m_shadow_pages;
        } else {
            mos_pages=&m->m_pages;
        }

        size_t page=0;

        while(page<0xc0) {
            m->m_pc_pages[page++]=usr_pages;
        }

        while(page<0xe0) {
            m->m_pc_pages[page++]=mos_pages;
        }

        while(page<0x100) {
            m->m_pc_pages[page++]=usr_pages;
        }
    }

    if(m->m_state.acccon.m128_bits.d) {
        m->m_state.shadow_select_mask=0x8000;
    } else {
        m->m_state.shadow_select_mask=0;
    }

    if(diff.m128_bits.tst) {
        if(m->m_state.acccon.m128_bits.tst) {
            for(int i=0;i<3;++i) {
                m->m_rmmio_fns[i]=m->m_rom_rmmio_fns.data();
                m->m_mmio_fn_contexts[i]=m->m_rom_mmio_fn_contexts.data();
                m->m_mmio_stretch[i]=m->m_rom_mmio_stretch.data();
            }
        } else {
            for(int i=0;i<3;++i) {
                m->m_rmmio_fns[i]=m->m_hw_rmmio_fns[i].data();
                m->m_mmio_fn_contexts[i]=m->m_hw_mmio_fn_contexts[i].data();
                m->m_mmio_stretch[i]=m->m_hw_mmio_stretch[i].data();
            }
        }
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::InitSomeBigPages(uint8_t big_page_index,
                                uint8_t num_big_pages,
                                const uint8_t *r,
                                uint8_t *w,
                                char code)
{
    for(size_t i=0;i<num_big_pages;++i) {
        BigPage *bp=&m_big_pages[big_page_index+i];

        ASSERT(bp->index==0);
        bp->index=big_page_index+i;

        ASSERT(!bp->r);
        bp->r=r;
        if(r) {
            r+=BIG_PAGE_SIZE_BYTES;
        }

        ASSERT(!bp->w);
        bp->w=w;
        if(w) {
            w+=BIG_PAGE_SIZE_BYTES;
        }

        ASSERT(bp->code==0);
        bp->code=code;
    }
}

void BBCMicro::InitShadowBigPages(uint8_t big_page_index,
                                  uint8_t num_big_pages,
                                  char code)
{
    ASSERT(big_page_index>=ANDY_BIG_PAGE_INDEX&&
           big_page_index+num_big_pages<=ANDY_BIG_PAGE_INDEX+8);

    uint8_t actual_big_page_index=big_page_index;

    if(m_state.ram_buffer.size()<65536) {
        // point the page into main RAM.
        actual_big_page_index-=ANDY_BIG_PAGE_INDEX;

        // override the code.
        code='m';
    }

    this->InitSomeBigPages(big_page_index,
                           num_big_pages,
                           m_state.ram_buffer.data()+actual_big_page_index*BIG_PAGE_SIZE_BYTES,
                           m_state.ram_buffer.data()+actual_big_page_index*BIG_PAGE_SIZE_BYTES,
                           code);
}

void BBCMicro::InitSidewaysROMBigPages(uint8_t rom) {
    ASSERT(rom<16);
    uint8_t big_page_index=ROM0_BIG_PAGE_INDEX+rom*NUM_ROM_BIG_PAGES;

    char code="0123456789abcdef"[rom];

    if(!!m_state.sideways_rom_buffers[rom]) {
        this->InitSomeBigPages(big_page_index,
                               NUM_ROM_BIG_PAGES,
                               m_state.sideways_rom_buffers[rom]->data(),
                               nullptr,
                               code);
    } else if(!m_state.sideways_ram_buffers[rom].empty()) {
        this->InitSomeBigPages(big_page_index,
                               NUM_ROM_BIG_PAGES,
                               m_state.sideways_ram_buffers[rom].data(),
                               m_state.sideways_ram_buffers[rom].data(),
                               code);
    } else {
        this->InitSomeBigPages(big_page_index,
                               NUM_ROM_BIG_PAGES,
                               nullptr,
                               nullptr,
                               code);
    }
}

void BBCMicro::InitBigPages() {
    for(uint8_t i=0;i<NUM_BIG_PAGES;++i) {
        m_big_pages[i]={};
    }

    m_pages=MemoryPages();

    if(m_shadow_pages) {
        *m_shadow_pages=MemoryPages();
    }

    this->InitSomeBigPages(MAIN_BIG_PAGE_INDEX,
                           NUM_MAIN_BIG_PAGES,
                           m_state.ram_buffer.data()+MAIN_BIG_PAGE_INDEX*BIG_PAGE_SIZE_BYTES,
                           m_state.ram_buffer.data()+MAIN_BIG_PAGE_INDEX*BIG_PAGE_SIZE_BYTES,
                           'm');

    this->InitShadowBigPages(ANDY_BIG_PAGE_INDEX,NUM_ANDY_BIG_PAGES,'n');

    // HAZEL doesn't exist on the B+ - that region is part of ANDY.
    this->InitShadowBigPages(HAZEL_BIG_PAGE_INDEX,
                             NUM_HAZEL_BIG_PAGES,
                             m_type==BBCMicroType_Master?'h':'n');

    this->InitShadowBigPages(SHADOW_BIG_PAGE_INDEX,NUM_SHADOW_BIG_PAGES,'s');

    for(size_t i=0;i<16;++i) {
        this->InitSidewaysROMBigPages(i);
    }

    this->InitSomeBigPages(MOS_BIG_PAGE_INDEX,
                           NUM_ROM_BIG_PAGES,
                           !!m_state.os_buffer?m_state.os_buffer->data():nullptr,
                           nullptr,
                           'o');

    for(uint8_t i=0;i<NUM_BIG_PAGES;++i) {
        ASSERT(m_big_pages[i].index==i);
        ASSERT(m_big_pages[i].code!=0);
    }

    // Reconfigure the paging.

    // Pages 0x00-0x30.
    this->SetPages(&m_pages,
                   m_shadow_pages,
                   MAIN_BIG_PAGE_INDEX,
                   3,
                   0x0);

    // Pages 0x30-0x7f.
    this->SetPages(&m_pages,
                   nullptr,
                   MAIN_BIG_PAGE_INDEX+3,
                   5,
                   0x3);

    if(m_shadow_pages) {
        this->SetPages(m_shadow_pages,
                       nullptr,
                       SHADOW_BIG_PAGE_INDEX,
                       NUM_SHADOW_BIG_PAGES,
                       0x3);
    }

    // Pages 0x80-0xbf.
    (*m_update_romsel_pages_fn)(this);

    // Pages 0xc0-0xff.
    this->SetPages(&m_pages,
                   m_shadow_pages,
                   MOS_BIG_PAGE_INDEX,
                   NUM_MOS_BIG_PAGES,
                   0xc);

    // Update ACCCON last - updating the OS pages may have
    // made a mess on the M128.
    (*m_update_acccon_pages_fn)(this,nullptr);

    CheckMemoryPages(&m_pages,true);
    CheckMemoryPages(m_shadow_pages,true);

    this->UpdateDebugState();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::Write1770ControlRegister(void *m_,M6502Word a,uint8_t value) {
    auto m=(BBCMicro *)m_;
    (void)a;

    ASSERT(m->m_disc_interface);
    m->m_state.disc_control=m->m_disc_interface->GetControlFromByte(value);

#if BBCMICRO_TRACE
    if(m->m_trace) {
        m->m_trace->AllocStringf("1770 - Control Register: Reset=%d; DDEN=%d; drive=%d, side=%d\n",m->m_state.disc_control.reset,m->m_state.disc_control.dden,m->m_state.disc_control.drive,m->m_state.disc_control.side);
    }
#endif

    LOGF(1770,"Write control register: 0x%02X: Reset=%d; DDEN=%d; drive=%d, side=%d\n",value,m->m_state.disc_control.reset,m->m_state.disc_control.dden,m->m_state.disc_control.drive,m->m_state.disc_control.side);

    if(m->m_state.disc_control.reset) {
        m->m_state.fdc.Reset();
    }

    m->m_state.fdc.SetDDEN(!!m->m_state.disc_control.dden);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint8_t BBCMicro::Read1770ControlRegister(void *m_,M6502Word a) {
    auto m=(BBCMicro *)m_;
    (void)a;

    ASSERT(m->m_disc_interface);

    uint8_t value=m->m_disc_interface->GetByteFromControl(m->m_state.disc_control);
    return value;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_TRACE
void BBCMicro::TracePortB(SystemVIAPB pb) {
    Log log("",m_trace->GetLogPrinter(1000));

    log.f("PORTB - PB = $%02X (%%%s): ",pb.value,BINARY_BYTE_STRINGS[pb.value]);

    if(m_type==BBCMicroType_Master) {
        log.f("RTC AS=%u; RTC CS=%u; ",pb.m128_bits.rtc_address_strobe,pb.m128_bits.rtc_chip_select);
    }

    const char *name=nullptr;
    bool value=pb.bits.latch_value;

    switch(pb.bits.latch_index) {
        case 0:
            name="Sound Write";
            value=!value;
        print_bool:;
            log.f("%s=%s\n",name,BOOL_STR(value));
            break;

        case 1:
            name=m_type==BBCMicroType_Master?"RTC Read":"Speech Read";
            goto print_bool;

        case 2:
            name=m_type==BBCMicroType_Master?"RTC DS":"Speech Write";
            goto print_bool;

        case 3:
            name="KB Read";
            goto print_bool;

        case 4:
        case 5:
            log.f("Screen Wrap Adjustment=$%04x\n",SCREEN_WRAP_ADJUSTMENTS[m_state.addressable_latch.bits.screen_base]);
            break;

        case 6:
            name="Caps Lock LED";
            goto print_bool;

        case 7:
            name="Shift Lock LED";
            goto print_bool;
    }

    m_trace->FinishLog(&log);

    //Trace_AllocStringf(m_trace,
    //    "PORTB - PB = $%02X (
    //    "PORTB - PB = $%02X (Latch = $%02X - Snd=%d; Kb=%d; Caps=%d; Shf=%d; RTCdat=%d; RTCrd=%d) (RTCsel=%d; RTCaddr=%d)",
    //    pb.value,
    //    m_state.addressable_latch.value,
    //    !m_state.addressable_latch.bits.not_sound_write,
    //    !m_state.addressable_latch.bits.not_kb_write,
    //    m_state.addressable_latch.bits.caps_lock_led,
    //    m_state.addressable_latch.bits.shift_lock_led,
    //    m_state.addressable_latch.m128_bits.rtc_data_strobe,
    //    m_state.addressable_latch.m128_bits.rtc_read,
    //    pb.m128_bits.rtc_chip_select,
    //    pb.m128_bits.rtc_address_strobe);
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::WriteUnmappedMMIO(void *m_,M6502Word a,uint8_t value) {
    (void)m_,(void)a,(void)value;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint8_t BBCMicro::ReadUnmappedMMIO(void *m_,M6502Word a) {
    (void)m_,(void)a;

    return 0;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint8_t BBCMicro::ReadROMMMIO(void *m_,M6502Word a) {
    auto m=(BBCMicro *)m_;

    return m->m_pages.r[a.b.h][a.b.l];
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint8_t BBCMicro::ReadROMSEL(void *m_,M6502Word a) {
    auto m=(BBCMicro *)m_;
    (void)a;

    return m->m_state.romsel.value;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::WriteROMSEL(void *m_,M6502Word a,uint8_t value) {
    auto m=(BBCMicro *)m_;
    (void)a;

    if((m->m_state.romsel.value^value)&m->m_romsel_mask) {
        m->m_state.romsel.value=value&m->m_romsel_mask;

        (*m->m_update_romsel_pages_fn)(m);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint8_t BBCMicro::ReadACCCON(void *m_,M6502Word a) {
    auto m=(BBCMicro *)m_;
    (void)a;

    return m->m_state.acccon.value;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::WriteACCCON(void *m_,M6502Word a,uint8_t value) {
    auto m=(BBCMicro *)m_;
    (void)a;

    if((m->m_state.acccon.value^value)&m->m_acccon_mask) {
        ACCCON old=m->m_state.acccon;

        m->m_state.acccon.value=value&m->m_acccon_mask;

        (*m->m_update_acccon_pages_fn)(m,&old);
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

BBCMicroType BBCMicro::GetType() const {
    return m_type;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const uint64_t *BBCMicro::GetNum2MHzCycles() const {
    return &m_state.num_2MHz_cycles;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint8_t BBCMicro::GetKeyState(BeebKey key) {
    ASSERT(key>=0&&(int)key<128);

    uint8_t *column=&m_state.key_columns[key&0x0f];
    uint8_t mask=1<<(key>>4);

    return !!(*column&mask);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//uint8_t BBCMicro::ReadMemory(uint16_t address) {
//    M6502Word addr={address};
//    if(addr.b.h>=0xfc&&addr.b.h<0xff) {
//        return 0;
//    } else if(m_pc_pages) {
//        return m_pc_pages[0]->r[addr.b.h][addr.b.l];
//    } else {
//        return m_pages.r[addr.b.h][addr.b.l];
//    }
//}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const uint8_t *BBCMicro::GetRAM() const {
    return m_ram;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::SetKeyState(BeebKey key,bool new_state) {
    ASSERT(key>=0&&(int)key<128);

    uint8_t *column=&m_state.key_columns[key&0x0f];
    uint8_t mask=1<<(key>>4);
    bool old_state=(*column&mask)!=0;

    if(key==BeebKey_Break) {
        if(new_state!=m_state.resetting) {
            m_state.resetting=new_state;

            if(new_state) {
                M6502_Halt(&m_state.cpu);
            } else {
                M6502_Reset(&m_state.cpu);
                this->StopPaste();
            }

            return true;
        }
    } else {
        if(!old_state&&new_state) {
            ASSERT(m_state.num_keys_down<256);
            ++m_state.num_keys_down;
            *column|=mask;

            return true;
        } else if(old_state&&!new_state) {
            ASSERT(m_state.num_keys_down>0);
            --m_state.num_keys_down;
            *column&=~mask;

            return true;
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::HasNumericKeypad() const {
    return m_type==BBCMicroType_Master;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
bool BBCMicro::GetTeletextDebug() const {
    return m_state.saa5050.IsDebug();
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::SetTeletextDebug(bool teletext_debug) {
    m_state.saa5050.SetDebug(teletext_debug);
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
uint8_t BBCMicro::ReadAsyncCallThunk(void *m_,M6502Word a) {
    auto m=(BBCMicro *)m_;

    ASSERT(a.w>=ASYNC_CALL_THUNK_ADDR.w);
    size_t offset=(size_t)(a.w-ASYNC_CALL_THUNK_ADDR.w);
    ASSERT(offset<sizeof m->m_state.async_call_thunk_buf);

    //LOGF(OUTPUT,"%s: type=%u a=$%04x v=$%02x cycles=%" PRIu64 "\n",__func__,m->m_state.cpu.read,a.w,m->m_state.async_call_thunk_buf[offset],m->m_state.num_2MHz_cycles);

    return m->m_state.async_call_thunk_buf[offset];
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::HandleReadByteDebugFlags(uint8_t read,DebugState::ByteDebugFlags *flags) {
    if(flags->bits.break_execute) {
        if(read==M6502ReadType_Opcode) {
            this->DebugHalt("execute: $%04x",m_state.cpu.abus.w);
        }
    } else if(flags->bits.temp_execute) {
        if(read==M6502ReadType_Opcode) {
            this->DebugHalt("single step");
        }
    }

    if(flags->bits.break_read) {
        if(read<=M6502ReadType_LastInterestingDataRead) {
            this->DebugHalt("data read: $%04x",m_state.cpu.abus.w);
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::HandleInterruptBreakpoints() {
    if(M6502_IsProbablyIRQ(&m_state.cpu)) {
        if((m_state.system_via.ifr.value&m_state.system_via.ier.value&m_debug->hw.system_via_irq_breakpoints.value)||
            (m_state.user_via.ifr.value&m_state.user_via.ier.value&m_debug->hw.user_via_irq_breakpoints.value))
        {
            this->SetDebugStepType(BBCMicroStepType_StepIntoIRQHandler);
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//
//if(mmio_page<3) {
//    if(m->m_state.cpu.read) {
//        ReadMMIOFn fn=m->m_rmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
//        void *context=m->m_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
//        m->m_state.cpu.dbus=(*fn)(context,m->m_state.cpu.abus);
//    } else {
//        WriteMMIOFn fn=m->m_hw_wmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
//        void *context=m->m_hw_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
//        (*fn)(context,m->m_state.cpu.abus,m->m_state.cpu.dbus);
//    }
//} else {

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::HandleCPUDataBusMainRAMOnly(BBCMicro *m) {
    uint8_t mmio_page=m->m_state.cpu.abus.b.h-0xfc;
    if(const uint8_t read=m->m_state.cpu.read) {
        if(mmio_page<3) {
            ReadMMIOFn fn=m->m_rmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            m->m_state.cpu.dbus=(*fn)(context,m->m_state.cpu.abus);
        } else {
            m->m_state.cpu.dbus=m->m_pages.r[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l];
        }
    } else {
        if(mmio_page<3) {
            WriteMMIOFn fn=m->m_hw_wmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_hw_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            (*fn)(context,m->m_state.cpu.abus,m->m_state.cpu.dbus);
        } else {
            m->m_pages.w[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l]=m->m_state.cpu.dbus;
        }
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::HandleCPUDataBusMainRAMOnlyDebug(BBCMicro *m) {
    uint8_t mmio_page=m->m_state.cpu.abus.b.h-0xfc;
    if(const uint8_t read=m->m_state.cpu.read) {
        if(mmio_page<3) {
            ReadMMIOFn fn=m->m_rmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            m->m_state.cpu.dbus=(*fn)(context,m->m_state.cpu.abus);
        } else {
            m->m_state.cpu.dbus=m->m_pages.r[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l];
        }

        DebugState::ByteDebugFlags *flags=&m->m_pages.debug[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l];
        if(flags->value!=0) {
            m->HandleReadByteDebugFlags(read,flags);
        }

        if(read==M6502ReadType_Interrupt) {
            m->HandleInterruptBreakpoints();
        }
    } else {
        if(mmio_page<3) {
            WriteMMIOFn fn=m->m_hw_wmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_hw_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            (*fn)(context,m->m_state.cpu.abus,m->m_state.cpu.dbus);
        } else {
            m->m_pages.w[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l]=m->m_state.cpu.dbus;
        }

        if(m->m_pages.debug[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l].bits.break_write) {
            m->DebugHalt("data write: $%04x",m->m_state.cpu.abus.w);
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::HandleCPUDataBusWithShadowRAM(BBCMicro *m) {
    uint8_t mmio_page=m->m_state.cpu.abus.b.h-0xfc;
    if(const uint8_t read=m->m_state.cpu.read) {
        if(mmio_page<3) {
            ReadMMIOFn fn=m->m_rmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            m->m_state.cpu.dbus=(*fn)(context,m->m_state.cpu.abus);
        } else {
            m->m_state.cpu.dbus=m->m_pc_pages[m->m_state.cpu.opcode_pc.b.h]->r[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l];
        }
    } else {
        if(mmio_page<3) {
            WriteMMIOFn fn=m->m_hw_wmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_hw_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            (*fn)(context,m->m_state.cpu.abus,m->m_state.cpu.dbus);
        } else {
            m->m_pc_pages[m->m_state.cpu.opcode_pc.b.h]->w[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l]=m->m_state.cpu.dbus;
        }
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::HandleCPUDataBusWithShadowRAMDebug(BBCMicro *m) {
    uint8_t mmio_page=m->m_state.cpu.abus.b.h-0xfc;
    if(const uint8_t read=m->m_state.cpu.read) {
        if(mmio_page<3) {
            ReadMMIOFn fn=m->m_rmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            m->m_state.cpu.dbus=(*fn)(context,m->m_state.cpu.abus);
        } else {
            m->m_state.cpu.dbus=m->m_pc_pages[m->m_state.cpu.opcode_pc.b.h]->r[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l];
        }

        DebugState::ByteDebugFlags *flags=&m->m_pc_pages[m->m_state.cpu.opcode_pc.b.h]->debug[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l];
        if(flags->value!=0) {
            m->HandleReadByteDebugFlags(read,flags);
        }

        if(read==M6502ReadType_Interrupt) {
            m->HandleInterruptBreakpoints();
        }
    } else {
        if(mmio_page<3) {
            WriteMMIOFn fn=m->m_hw_wmmio_fns[mmio_page][m->m_state.cpu.abus.b.l];
            void *context=m->m_hw_mmio_fn_contexts[mmio_page][m->m_state.cpu.abus.b.l];
            (*fn)(context,m->m_state.cpu.abus,m->m_state.cpu.dbus);
        } else {
            m->m_pc_pages[m->m_state.cpu.opcode_pc.b.h]->w[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l]=m->m_state.cpu.dbus;
        }

        if(m->m_pc_pages[m->m_state.cpu.opcode_pc.b.h]->debug[m->m_state.cpu.abus.b.h][m->m_state.cpu.abus.b.l].bits.break_write) {
            m->DebugHalt("data write: $%04x",m->m_state.cpu.abus.w);
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::HandleCPUDataBusWithHacks(BBCMicro *m) {
#if BBCMICRO_DEBUGGER
    if(m->m_state.async_call_address.w!=INVALID_ASYNC_CALL_ADDRESS) {
        if(m->m_state.cpu.read==M6502ReadType_Interrupt&&M6502_IsProbablyIRQ(&m->m_state.cpu)) {
            TRACEF(m->m_trace,"Enqueuing async call: address=$%04x, A=%03u ($%02x) X=%03u ($%02x) Y=%03u ($%02X) C=%s\n",
                   m->m_state.async_call_address.w,
                   m->m_state.async_call_a,m->m_state.async_call_a,
                   m->m_state.async_call_x,m->m_state.async_call_x,
                   m->m_state.async_call_y,m->m_state.async_call_y,
                   BOOL_STR(m->m_state.async_call_c));

            // Already on the stack is the actual return that the
            // thunk will RTI to.

            // Manually push current PC and status register.
            {
                M6502P p=M6502_GetP(&m->m_state.cpu);

                const BigPage *bp=m->DebugGetBigPage(m->m_state.cpu.s.b.h,0);

                // Add the thunk call address that the IRQ routine will RTI to.
                bp->w[m->m_state.cpu.s.w&BIG_PAGE_OFFSET_MASK]=m->m_state.cpu.pc.b.h;
                --m->m_state.cpu.s.b.l;

                bp->w[m->m_state.cpu.s.w&BIG_PAGE_OFFSET_MASK]=m->m_state.cpu.pc.b.l;
                --m->m_state.cpu.s.b.l;

                bp->w[m->m_state.cpu.s.w&BIG_PAGE_OFFSET_MASK]=p.value;
                --m->m_state.cpu.s.b.l;

                // Set up CPU as if it's about to execute the thunk, so
                // the IRQ routine will return to the desired place.
                p.bits.c=m->m_state.async_call_c;
                M6502_SetP(&m->m_state.cpu,p.value);
                m->m_state.cpu.pc=ASYNC_CALL_THUNK_ADDR;
            }

            // Set up thunk.
            {
                uint8_t *p=m->m_state.async_call_thunk_buf;

                *p++=0x48;//pha
                *p++=0x8a;//txa
                *p++=0x48;//pha
                *p++=0x98;//tya
                *p++=0x48;//pha
                *p++=0xa9;
                *p++=m->m_state.async_call_a;
                *p++=0xa2;
                *p++=m->m_state.async_call_x;
                *p++=0xa0;
                *p++=m->m_state.async_call_y;
                *p++=m->m_state.async_call_c?0x38:0x18;//sec:clc
                *p++=0x20;//jsr abs
                *p++=m->m_state.async_call_address.b.l;
                *p++=m->m_state.async_call_address.b.h;
                *p++=0x68;//pla
                *p++=0xa8;//tay
                *p++=0x68;//pla
                *p++=0xaa;//tax
                *p++=0x68;//pla
                *p++=0x40;//rti

                ASSERT((size_t)(p-m->m_state.async_call_thunk_buf)<=sizeof m->m_state.async_call_thunk_buf);
            }

            m->FinishAsyncCall(true);
        } else {
            --m->m_state.async_call_timeout;
            if(m->m_state.async_call_timeout<0) {
                m->FinishAsyncCall(false);
            }
        }
    }
#endif

    (*m->m_default_handle_cpu_data_bus_fn)(m);

    if(M6502_IsAboutToExecute(&m->m_state.cpu)) {
        if(!m->m_instruction_fns.empty()) {

            // This is a bit bizarre, but I just can't stomach the
            // idea of literally like 1,000,000 std::vector calls per
            // second. But this way, it's hopefully more like only
            // 300,000.

            auto *fn=m->m_instruction_fns.data();
            auto *fns_end=fn+m->m_instruction_fns.size();
            bool removed=false;

            while(fn!=fns_end) {
                if((*fn->first)(m,&m->m_state.cpu,fn->second)) {
                    ++fn;
                } else {
                    removed=true;
                    *fn=*--fns_end;
                }
            }

            if(removed) {
                m->m_instruction_fns.resize((size_t)(fns_end-m->m_instruction_fns.data()));

                m->UpdateCPUDataBusFn();
            }
        }

        if(m->m_state.hack_flags&BBCMicroHackFlag_Paste) {
            ASSERT(m->m_state.paste_state!=BBCMicroPasteState_None);

            if(m->m_state.cpu.pc.w==0xffe1) {
                // OSRDCH

                // Put next byte in A.
                switch(m->m_state.paste_state) {
                case BBCMicroPasteState_None:
                    ASSERT(false);
                    break;

                case BBCMicroPasteState_Wait:
                    m->SetKeyState(PASTE_START_KEY,false);
                    m->m_state.paste_state=BBCMicroPasteState_Delete;
                    // fall through
                case BBCMicroPasteState_Delete:
                    m->m_state.cpu.a=127;
                    m->m_state.paste_state=BBCMicroPasteState_Paste;
                    break;

                case BBCMicroPasteState_Paste:
                    ASSERT(m->m_state.paste_index<m->m_state.paste_text->size());
                    m->m_state.cpu.a=(uint8_t)m->m_state.paste_text->at(m->m_state.paste_index);

                    ++m->m_state.paste_index;
                    if(m->m_state.paste_index==m->m_state.paste_text->size()) {
                        m->StopPaste();
                    }
                    break;
                }

                // No Escape.
                m->m_state.cpu.p.bits.c=0;

                // Pretend the instruction was RTS.
                m->m_state.cpu.dbus=0x60;
            }
        }

#if BBCMICRO_TRACE
        if(m->m_trace) {
            InstructionTraceEvent *e;

            // Fill out results of last instruction.
            if((e=m->m_trace_current_instruction)!=NULL) {
                e->a=m->m_state.cpu.a;
                e->x=m->m_state.cpu.x;
                e->y=m->m_state.cpu.y;
                e->p=m->m_state.cpu.p.value;
                e->data=m->m_state.cpu.data;
                e->opcode=m->m_state.cpu.opcode;
                e->s=m->m_state.cpu.s.b.l;
                //e->pc=m_state.cpu.pc.w;//...for next instruction
                e->ad=m->m_state.cpu.ad.w;
                e->ia=m->m_state.cpu.ia.w;
            }

            // Allocate event for next instruction.
            e=m->m_trace_current_instruction=(InstructionTraceEvent *)m->m_trace->AllocEvent(INSTRUCTION_EVENT);

            if(e) {
                e->pc=m->m_state.cpu.abus.w;

                /* doesn't matter if the last instruction ends up
                * bogus... there are no invalid values.
                */
            }
        }
#endif
    }

#if BBCMICRO_DEBUGGER
    if(m->m_debug) {
        if(m->m_state.cpu.read>=M6502ReadType_Opcode) {
            switch(m->m_debug->step_type) {
            default:
                ASSERT(false);
                // fall through
            case BBCMicroStepType_None:
                break;

            case BBCMicroStepType_StepIn:
                {
                    if(m->m_state.cpu.read==M6502ReadType_Opcode) {
                        // Done.
                        m->DebugHalt("single step");
                    } else {
                        ASSERT(m->m_state.cpu.read==M6502ReadType_Interrupt);
                        // The instruction was interrupted, so set a temp
                        // breakpoint in the right place.
                        m->DebugAddTempBreakpoint(m->m_state.cpu.pc);
                    }

                    m->SetDebugStepType(BBCMicroStepType_None);
                }
                break;

            case BBCMicroStepType_StepIntoIRQHandler:
                {
                    ASSERT(m->m_state.cpu.read==M6502ReadType_Opcode||m->m_state.cpu.read==M6502ReadType_Interrupt);
                    if(m->m_state.cpu.read==M6502ReadType_Opcode) {
                        m->SetDebugStepType(BBCMicroStepType_None);
                        m->DebugHalt("IRQ/NMI");
                    }
                }
                break;
            }
        }
    }
#endif
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::CheckMemoryPages(const MemoryPages *pages,bool non_null) {
    if(pages) {
        for(size_t i=0;i<256;++i) {
            ASSERT(!!pages->r[i]==non_null);
            ASSERT(!!pages->w[i]==non_null);
#if BBCMICRO_DEBUGGER
            ASSERT(!!pages->debug[i]==non_null);
            ASSERT(!!pages->bp[i]==non_null);
#endif
        }
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// The cursor pattern is spread over the next 4 displayed columns.
//
// <pre>
// b7 b6 b5  Shape
// -- -- --  -----
//  0  0  0  <none>
//  0  0  1    __
//  0  1  0   _  
//  0  1  1   ___
//  1  0  0  _
//  1  0  1  _ __ 
//  1  1  0  __
//  1  1  1  ____
// </pre>
//
// Bit 7 control the first column, bit 6 controls the second column,
// and bit 7 controls the 3rd and 4th.
static const uint8_t CURSOR_PATTERNS[8]={
    0+0+0+0,
    0+0+4+8,
    0+2+0+0,
    0+2+4+8,
    1+0+0+0,
    1+0+4+8,
    1+2+0+0,
    1+2+4+8,
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
uint16_t BBCMicro::DebugGetBeebAddressFromCRTCAddress(uint8_t h,uint8_t l) const {
    M6502Word addr;
    addr.b.h=h;
    addr.b.l=l;

    if(addr.w&0x2000) {
        return (addr.w&0x3ff)|m_teletext_bases[addr.w>>11&1];
    } else {
        if(addr.w&0x1000) {
            addr.w-=SCREEN_WRAP_ADJUSTMENTS[m_state.addressable_latch.bits.screen_base];
            addr.w&=~0x1000;
        }

        return addr.w<<3;
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::Update(VideoDataUnit *video_unit,SoundDataUnit *sound_unit) {
    uint8_t odd_cycle=m_state.num_2MHz_cycles&1;
    bool sound=false;

    ++m_state.num_2MHz_cycles;

#if VIDEO_TRACK_METADATA
    video_unit->metadata.flags=0;

    if(odd_cycle) {
        video_unit->metadata.flags|=VideoDataUnitMetadataFlag_OddCycle;
    }
#endif

    // Update video hardware.
    if(m_state.video_ula.control.bits.fast_6845|odd_cycle) {
        CRTC::Output output=m_state.crtc.Update(m_state.video_ula.control.bits.fast_6845);

        m_state.system_via.a.c1=output.vsync;
        m_state.cursor_pattern>>=1;

        uint16_t addr=(uint16_t)output.address;

        if(addr&0x2000) {
            addr=(addr&0x3ff)|m_teletext_bases[addr>>11&1];
        } else {
            if(addr&0x1000) {
                addr-=SCREEN_WRAP_ADJUSTMENTS[m_state.addressable_latch.bits.screen_base];
                addr&=~0x1000;
            }

            addr<<=3;

            // When output.raster>=8, this address is bogus. There's a
            // check later.
            addr|=output.raster&7;
        }

        ASSERTF(addr<32768,"output: hsync=%u vsync=%u display=%u address=0x%x raster=%u; addr=0x%x; latch screen_base=%u\n",
                output.hsync,output.vsync,output.display,output.address,output.raster,
                addr,
                m_state.addressable_latch.bits.screen_base);
        addr|=m_state.shadow_select_mask;

        // Teletext update.
        if(odd_cycle) {
            if(output.vsync) {
                if(!m_state.crtc_last_output.vsync) {
                    m_state.last_frame_2MHz_cycles=m_state.num_2MHz_cycles-m_state.last_vsync_2MHz_cycles;
                    m_state.last_vsync_2MHz_cycles=m_state.num_2MHz_cycles;

                    m_state.saa5050.VSync();
                }
            }

            if(m_state.video_ula.control.bits.teletext) {
                // Teletext line boundary stuff.
                //
                // The hsync output is linked up to the SAA505's GLR
                // ("General line reset") pin, which sounds like it should
                // do line stuff. The data sheet is a bit vague, though:
                // "required for internal synchronization of remote control
                // data signals"...??
                //
                // https://github.com/mist-devel/mist-board/blob/f6cc6ff597c22bdd8b002c04c331619a9767eae0/cores/bbc/rtl/saa5050/saa5050.v
                // seems to ignore it completely, and does everything based
                // on the LOSE pin, connected to 6845 DISPEN/DISPTMSG. So
                // that's what this does...
                //
                // (Evidence in favour of this: normally, R5 doesn't affect
                // the teletext chars, even though it must vary the number
                // of hsyncs between vsync and the first visible scanline.
                // But after setting R6=255, changing R5 does have an
                // affect, suggesting that DISPTMSG transitions are being
                // counted and hsyncs aren't.)
                if(output.display) {
                    if(!m_state.crtc_last_output.display) {
                        m_state.saa5050.StartOfLine();
                    }
                } else {
                    m_state.ic15_byte|=0x40;

                    if(m_state.crtc_last_output.display) {
                        m_state.saa5050.EndOfLine();
                    }
                }
            }

            m_state.saa5050.Byte(m_state.ic15_byte,output.display);

            if(output.address&0x2000) {
                m_state.ic15_byte=m_ram[addr];
            } else {
                m_state.ic15_byte=0;
            }

#if VIDEO_TRACK_METADATA
            video_unit->metadata.flags|=VideoDataUnitMetadataFlag_HasValue;
            video_unit->metadata.value=m_state.ic15_byte;
#endif
        }

        if(!m_state.video_ula.control.bits.teletext) {
            if(!m_state.crtc_last_output.display) {
                m_state.video_ula.DisplayEnabled();
            }

            uint8_t value=m_ram[addr];
            m_state.video_ula.Byte(value);

#if VIDEO_TRACK_METADATA
            video_unit->metadata.flags|=VideoDataUnitMetadataFlag_HasValue;
            video_unit->metadata.value=value;
#endif
        }

        if(output.cudisp) {
            m_state.cursor_pattern=CURSOR_PATTERNS[m_state.video_ula.control.bits.cursor];
        }

        m_last_video_access_address=addr;

        video_unit->metadata.flags|=VideoDataUnitMetadataFlag_HasAddress;
        video_unit->metadata.address=m_last_video_access_address;

        m_state.crtc_last_output=output;
    }

    // Update display output.
    //if(m_state.crtc_last_output.display) {
#if VIDEO_TRACK_METADATA
    if(m_state.crtc_last_output.raster==0) {
        video_unit->metadata.flags|=VideoDataUnitMetadataFlag_6845Raster0;
    }

    if(m_state.crtc_last_output.display) {
        video_unit->metadata.flags|=VideoDataUnitMetadataFlag_6845DISPEN;
    }

    if(m_state.crtc_last_output.cudisp) {
        video_unit->metadata.flags|=VideoDataUnitMetadataFlag_6845CUDISP;
    }
#endif

    if(m_state.video_ula.control.bits.teletext) {
        m_state.saa5050.EmitPixels(&video_unit->pixels);

        if(m_state.cursor_pattern&1) {
            video_unit->pixels.pixels[0].all^=0x0fff;
            video_unit->pixels.pixels[1].all^=0x0fff;
        }
    } else {
        if(m_state.crtc_last_output.display&&
           m_state.crtc_last_output.raster<8)
        {
            m_state.video_ula.EmitPixels(&video_unit->pixels);

            if(m_state.cursor_pattern&1) {
                video_unit->pixels.values[0]^=0x0fff0fff0fff0fffull;
                video_unit->pixels.values[1]^=0x0fff0fff0fff0fffull;
            }
        } else {
            if(m_state.cursor_pattern&1) {
                video_unit->pixels.values[1]=video_unit->pixels.values[0]=0x0fff0fff0fff0fffull;
            } else {
                video_unit->pixels.values[1]=video_unit->pixels.values[0]=0;
            }
        }
    }

    video_unit->pixels.pixels[1].bits.x=0;

    if(m_state.crtc_last_output.hsync) {
        video_unit->pixels.pixels[1].bits.x|=VideoDataUnitFlag_HSync;
    }

    if(m_state.crtc_last_output.vsync) {
        video_unit->pixels.pixels[1].bits.x|=VideoDataUnitFlag_VSync;
    }

    if(odd_cycle) {
        // Update keyboard.
        if(m_state.addressable_latch.bits.not_kb_write) {
            if(m_state.key_columns[m_state.key_scan_column]&0xfe) {
                m_state.system_via.a.c2=1;
            } else {
                m_state.system_via.a.c2=0;
            }

            ++m_state.key_scan_column;
            m_state.key_scan_column&=0x0f;
        } else {
            // manual scan
            BeebKey key=(BeebKey)(m_state.system_via.a.p&0x7f);
            uint8_t kcol=key&0x0f;
            uint8_t krow=(uint8_t)(key>>4);

            uint8_t *column=&m_state.key_columns[kcol];

            // row 0 doesn't cause an interrupt
            if(*column&0xfe) {
                m_state.system_via.a.c2=1;
            } else {
                m_state.system_via.a.c2=0;
            }

            m_state.system_via.a.p&=0x7f;
            if(*column&1<<krow) {
                m_state.system_via.a.p|=0x80;
            }

            //if(key==m_state.auto_reset_key) {
            //    //*column&=~(1<<krow);
            //    m_state.auto_reset_key=BeebKey_None;
            //}
        }

        // Update joysticks.
        m_state.system_via.b.p|=1<<4|1<<5;

        if(m_beeblink_handler) {
            // Update BeebLink.
            m_beeblink->Update(&m_state.user_via);
        } else {
            // Nothing connected to the user port.
            m_state.user_via.b.p=255;
            m_state.user_via.b.c1=1;
        }

        // Update IRQs.
        if(m_state.system_via.Update()) {
            M6502_SetDeviceIRQ(&m_state.cpu,BBCMicroIRQDevice_SystemVIA,1);
        } else {
            M6502_SetDeviceIRQ(&m_state.cpu,BBCMicroIRQDevice_SystemVIA,0);
        }

        if(m_state.user_via.Update()) {
            M6502_SetDeviceIRQ(&m_state.cpu,BBCMicroIRQDevice_UserVIA,1);
        } else {
            M6502_SetDeviceIRQ(&m_state.cpu,BBCMicroIRQDevice_UserVIA,0);
        }

        // Update addressable latch and RTC.
        if(m_state.old_system_via_pb!=m_state.system_via.b.p) {
            SystemVIAPB pb;
            pb.value=m_state.system_via.b.p;

            SystemVIAPB old_pb;
            old_pb.value=m_state.old_system_via_pb;

            uint8_t mask=1<<pb.bits.latch_index;

            m_state.addressable_latch.value&=~mask;
            if(pb.bits.latch_value) {
                m_state.addressable_latch.value|=mask;
            }

#if BBCMICRO_TRACE
            if(m_trace) {
                if(m_trace_flags&BBCMicroTraceFlag_SystemVIA) {
                    TracePortB(pb);
                }
            }
#endif

            if(pb.m128_bits.rtc_chip_select) {
                uint8_t x=m_state.system_via.a.p;

                if(old_pb.m128_bits.rtc_address_strobe==1&&pb.m128_bits.rtc_address_strobe==0) {
                    m_state.rtc.SetAddress(x);
                }

                AddressableLatch test;
                test.value=m_state.old_addressable_latch.value^m_state.addressable_latch.value;
                if(test.m128_bits.rtc_data_strobe) {
                    if(m_state.addressable_latch.m128_bits.rtc_data_strobe) {
                        // 0->1
                        if(m_state.addressable_latch.m128_bits.rtc_read) {
                            m_state.system_via.a.p=m_state.rtc.Read();
                        }
                    } else {
                        // 1->0
                        if(!m_state.addressable_latch.m128_bits.rtc_read) {
                            m_state.rtc.SetData(x);
                        }
                    }
                }
            }

            m_state.old_system_via_pb=m_state.system_via.b.p;
        }

        // Update RTC.
        if(m_has_rtc) {
            m_state.rtc.Update();
        }

        // Update NMI.
        M6502_SetDeviceNMI(&m_state.cpu,BBCMicroNMIDevice_1770,m_state.fdc.Update().value);

        // Update sound.
        if((m_state.num_2MHz_cycles&((1<<SOUND_CLOCK_SHIFT)-1))==0) {
            sound_unit->sn_output=m_state.sn76489.Update(!m_state.addressable_latch.bits.not_sound_write,
                                                         m_state.system_via.a.p);

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
            // The disc drive sounds are pretty quiet. 
            sound_unit->disc_drive_sound=this->UpdateDiscDriveSound(&m_state.drives[0]);
            sound_unit->disc_drive_sound+=this->UpdateDiscDriveSound(&m_state.drives[1]);
#endif
            sound=true;
        }

        m_state.old_addressable_latch=m_state.addressable_latch;
    }

    // Update CPU.
    if(m_state.stretched_cycles_left>0) {
        --m_state.stretched_cycles_left;
        if(m_state.stretched_cycles_left==0) {
            ASSERT(odd_cycle);
        }
    } else {
        (*m_state.cpu.tfn)(&m_state.cpu);

        uint8_t mmio_page=m_state.cpu.abus.b.h-0xfc;
        if(mmio_page<3) {
            uint8_t num_stretch_cycles=1+odd_cycle;

            uint8_t stretch;
            if(m_state.cpu.read) {
                stretch=m_mmio_stretch[mmio_page][m_state.cpu.abus.b.l];
            } else {
                stretch=m_hw_mmio_stretch[mmio_page][m_state.cpu.abus.b.l];
            }

            m_state.stretched_cycles_left=num_stretch_cycles&stretch;
        }
    }

    if(m_state.stretched_cycles_left==0) {
        (*m_handle_cpu_data_bus_fn)(this);
    }

    return sound;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
void BBCMicro::SetDiscDriveSound(DiscDriveType type,DiscDriveSound sound,std::vector<float> samples) {
    ASSERT(sound>=0&&sound<DiscDriveSound_EndValue);
    ASSERT(g_disc_drive_sounds[type][sound].empty());
    ASSERT(samples.size()<=INT_MAX);
    g_disc_drive_sounds[type][sound]=std::move(samples);
}

//void BBCMicro::SetDiscDriveSound(int drive,DiscDriveSound sound,const float *samples,size_t num_samples) {
//    ASSERT(drive>=0&&drive<NUM_DRIVES);
//    DiscDrive_SetSoundData(&m_state.drives[drive],sound,samples,num_samples);
//}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

uint32_t BBCMicro::GetLEDs() {
    uint32_t leds=0;

    if(!(m_state.addressable_latch.bits.caps_lock_led)) {
        leds|=BBCMicroLEDFlag_CapsLock;
    }

    if(!(m_state.addressable_latch.bits.shift_lock_led)) {
        leds|=BBCMicroLEDFlag_ShiftLock;
    }

    for(int i=0;i<NUM_DRIVES;++i) {
        if(m_state.drives[i].motor) {
            leds|=(uint32_t)BBCMicroLEDFlag_Drive0<<i;
        }
    }

    return leds;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::vector<uint8_t> BBCMicro::GetNVRAM() const {
    if(m_has_rtc) {
        return m_state.rtc.GetRAMContents();
    } else {
        return std::vector<uint8_t>();
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SetOSROM(std::shared_ptr<const ROMData> data) {
    m_state.os_buffer=std::move(data);
    this->InitBigPages();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SetSidewaysROM(uint8_t bank,std::shared_ptr<const ROMData> data) {
    ASSERT(bank<16);

    m_state.sideways_ram_buffers[bank].clear();

    m_state.sideways_rom_buffers[bank]=std::move(data);

    this->InitBigPages();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SetSidewaysRAM(uint8_t bank,std::shared_ptr<const ROMData> data) {
    ASSERT(bank<16);

    if(data) {
        m_state.sideways_ram_buffers[bank]=std::vector<uint8_t>(data->begin(),data->end());
    } else {
        m_state.sideways_ram_buffers[bank]=std::vector<uint8_t>(16384);
    }

    m_state.sideways_rom_buffers[bank]=nullptr;

    this->InitBigPages();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_TRACE
void BBCMicro::StartTrace(uint32_t trace_flags,size_t max_num_bytes) {
    this->StopTrace();

    this->SetTrace(std::make_shared<Trace>(max_num_bytes),trace_flags);
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_TRACE
std::shared_ptr<Trace> BBCMicro::StopTrace() {
    std::shared_ptr<Trace> old_trace=m_trace_ptr;

    if(m_trace) {
        if(m_trace_current_instruction) {
            m_trace->CancelEvent(INSTRUCTION_EVENT,m_trace_current_instruction);
            m_trace_current_instruction=NULL;
        }

        this->SetTrace(nullptr,0);
    }

    return old_trace;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_TRACE
int BBCMicro::GetTraceStats(struct TraceStats *stats) {
    if(!m_trace) {
        return 0;
    }

    m_trace->GetStats(stats);
    return 1;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::AddInstructionFn(InstructionFn fn,void *context) {
    ASSERT(std::find(m_instruction_fns.begin(),m_instruction_fns.end(),std::make_pair(fn,context))==m_instruction_fns.end());

    m_instruction_fns.emplace_back(fn,context);

    this->UpdateCPUDataBusFn();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SetMMIOFns(uint16_t addr,ReadMMIOFn read_fn,WriteMMIOFn write_fn,void *context) {
    ASSERT(addr>=0xfc00&&addr<=0xfeff);

    M6502Word tmp;
    tmp.w=addr;
    tmp.b.h-=0xfc;

    m_hw_rmmio_fns[tmp.b.h][tmp.b.l]=read_fn?read_fn:&ReadUnmappedMMIO;
    m_hw_wmmio_fns[tmp.b.h][tmp.b.l]=write_fn?write_fn:&WriteUnmappedMMIO;
    m_hw_mmio_fn_contexts[tmp.b.h][tmp.b.l]=context;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::shared_ptr<DiscImage> BBCMicro::TakeDiscImage(int drive) {
    if(drive>=0&&drive<NUM_DRIVES) {
        std::shared_ptr<DiscImage> tmp=std::move(m_disc_images[drive]);
        return tmp;
    } else {
        return nullptr;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

std::shared_ptr<const DiscImage> BBCMicro::GetDiscImage(int drive) const {
    if(drive>=0&&drive<NUM_DRIVES) {
        return m_disc_images[drive];
    } else {
        return nullptr;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SetDiscImage(int drive,std::shared_ptr<DiscImage> disc_image) {
    if(drive<0||drive>=NUM_DRIVES) {
        return;
    }

    m_disc_images[drive]=std::move(disc_image);
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::GetAndResetDiscAccessFlag() {
    bool result=m_disc_access;

    m_disc_access=false;

    return result;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::IsPasting() const {
    return (m_state.hack_flags&BBCMicroHackFlag_Paste)!=0;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::StartPaste(std::shared_ptr<const std::string> text) {
    this->StopPaste();

    m_state.hack_flags|=BBCMicroHackFlag_Paste;
    m_state.paste_state=BBCMicroPasteState_Wait;
    m_state.paste_text=std::move(text);
    m_state.paste_index=0;
    m_state.paste_wait_end=m_state.num_2MHz_cycles+2000000;

    this->SetKeyState(PASTE_START_KEY,true);

    this->UpdateCPUDataBusFn();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::StopPaste() {
    m_state.paste_state=BBCMicroPasteState_None;
    m_state.paste_index=0;
    m_state.paste_text.reset();

    m_state.hack_flags&=(uint32_t)~BBCMicroHackFlag_Paste;
    this->UpdateCPUDataBusFn();
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const M6502 *BBCMicro::GetM6502() const {
    return &m_state.cpu;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
const CRTC *BBCMicro::DebugGetCRTC() const {
    return &m_state.crtc;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER

const ExtMem *BBCMicro::DebugGetExtMem() const {
    if(m_ext_mem) {
        return &m_state.ext_mem;
    } else {
        return nullptr;
    }
}

#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
const VideoULA *BBCMicro::DebugGetVideoULA() const {
    return &m_state.video_ula;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
const BBCMicro::AddressableLatch BBCMicro::DebugGetAddressableLatch() const {
    return m_state.addressable_latch;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
const R6522 *BBCMicro::DebugGetSystemVIA() const {
    return &m_state.system_via;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
const R6522 *BBCMicro::DebugGetUserVIA() const {
    return &m_state.user_via;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//#if BBCMICRO_DEBUGGER
//uint16_t BBCMicro::DebugGetFlatPage(uint8_t page) const {
//    if(m_pc_pages) {
//        return m_pc_pages[m_state.cpu.opcode_pc.b.h]->debug_page_index[page];
//    } else {
//        return m_pages.debug_page_index[page];
//    }
//}
//#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//#if BBCMICRO_DEBUGGER
//void BBCMicro::DebugCopyMemory(void *bytes_dest_,
//                               DebugState::ByteDebugFlags *debug_dest,
//                               M6502Word addr_,
//                               uint32_t dpo,
//                               size_t num_bytes) const
//{
//    const BigPage *bp=nullptr;
//    M6502Word addr=addr_;
//    uint8_t bp_page=~addr.b.h;
//    auto dest=(uint8_t *)bytes_dest_;
//
//    // this loop could be cleverer.
//    for(size_t i=0;i<num_bytes;++i) {
//        if(addr.b.h!=bp_page) {
//            bp=this->DebugGetBigPage(addr.b.h,dpo);
//            bp_page=addr.b.h;
//        }
//
//        if(bp->r) {
//            *dest++=bp->r[addr.w&BIG_PAGE_OFFSET_MASK];
//        } else {
//            *dest++=0;
//        }
//
//        ++addr.w;
//    }
//}
//#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::FinishAsyncCall(bool called) {
    if(m_async_call_fn) {
        (*m_async_call_fn)(called,m_async_call_context);
    }

    m_state.async_call_address.w=INVALID_ASYNC_CALL_ADDRESS;
    m_state.async_call_timeout=0;
    m_async_call_fn=nullptr;
    m_async_call_context=nullptr;

    this->UpdateCPUDataBusFn();
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

const BBCMicro::BigPage *BBCMicro::DebugGetBigPage(uint8_t page,uint32_t dpo) const {
    dpo&=m_dpo_mask;

    // Don't look too closely at the logic of this...

    const BigPage *bp;

    // Handle the IO region special case first.
    if(page>=0xfc&&page<0xff) {
        if((dpo&BBCMicroDebugPagingOverride_OverrideOS)&&
           (dpo&BBCMicroDebugPagingOverride_OS))
        {
            goto os;
        } else {
            // Access IO. Not yet supported.
            goto os;
            //return nullptr;
        }
    } else {
        switch(page>>4) {
            case 0x0: // 0x0000-0x0fff
            case 0x1: // 0x1000-0x1fff
            case 0x2: // 0x2000-0x2fff
            no_overrides:
            {
                if(m_pc_pages) {
                    // TODO - should be the option of taking instruction address
                    // into account!
                    bp=m_pc_pages[0]->bp[page];
                } else {
                    bp=m_pages.bp[page];
                }
                break;
            }

            case 0x3: // 0x3000-0x3fff
            case 0x4: // 0x4000-0x4fff
            case 0x5: // 0x5000-0x5fff
            case 0x6: // 0x6000-0x6fff
            case 0x7: // 0x7000-0x7fff
                if(dpo&BBCMicroDebugPagingOverride_OverrideShadow) {
                    if(dpo&BBCMicroDebugPagingOverride_Shadow) {
                        bp=&m_big_pages[SHADOW_BIG_PAGE_INDEX+((page>>4)-3)];
                        break;
                    } else {
                        bp=&m_big_pages[page>>4];
                        break;
                    }
                } else {
                    goto no_overrides;
                }

            case 0x8: // 0x8000-0x8fff // ANDY in M128/B+
            maybe_andy:
                if(dpo&BBCMicroDebugPagingOverride_OverrideANDY) {
                    if(dpo&BBCMicroDebugPagingOverride_ANDY) {
                        // don't mask, just subtract - it's 4K on the Master, but 12K on the
                        // B+.
                        bp=&m_big_pages[ANDY_BIG_PAGE_INDEX+((page>>4)-0x08)];
                        break;
                    } else {
                        goto sideways_rom;
                    }
                } else {
                    goto sideways_rom;
                }

            case 0x9: // 0x9000-0x9fff // ANDY in B+ only
            case 0xa: // 0xa000-0xafff // ANDY in B+ only
                if(m_type==BBCMicroType_BPlus) {
                    goto maybe_andy;
                } else {
                    goto sideways_rom;
                }

            case 0xb: // 0xb000-0xbfff
            sideways_rom:
            {
                if(dpo&BBCMicroDebugPagingOverride_OverrideROM) {
                    bp=&m_big_pages[ROM0_BIG_PAGE_INDEX+(dpo&BBCMicroDebugPagingOverride_ROM)*NUM_ROM_BIG_PAGES+((page>>4)-0x08)];
                    break;
                } else {
                    goto no_overrides;
                }
            }

            case 0xc: // 0xc000-0xcfff
            case 0xd: // 0xd000-0xdfff
                if((dpo&BBCMicroDebugPagingOverride_OverrideHAZEL)) {
                    if(dpo&BBCMicroDebugPagingOverride_HAZEL) {
                        bp=&m_big_pages[HAZEL_BIG_PAGE_INDEX+((page>>4)-0x0c)];
                        break;
                    } else {
                    os:
                        bp=&m_big_pages[MOS_BIG_PAGE_INDEX+((page>>4)-0x0c)];
                        break;
                    }
                } else {
                    goto no_overrides;
                }

            case 0xe: // 0xe000-0xefff
            case 0xf: // 0xf000-0xfbff, 0xff00-0xfeff
                goto os;
        }
    }

    ASSERT(bp>=m_big_pages&&bp<m_big_pages+NUM_BIG_PAGES);
    return bp;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::DebugGetBytes(uint8_t *bytes,size_t num_bytes,M6502Word addr,uint32_t dpo) {
    // Not currently very clever.
    for(size_t i=0;i<num_bytes;++i) {
        const BigPage *bp=this->DebugGetBigPage(addr.b.h,dpo);

        if(bp->r) {
            bytes[i]=bp->r[addr.w&BIG_PAGE_OFFSET_MASK];
        } else {
            bytes[i]=0;
        }

        ++addr.w;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::DebugSetBytes(M6502Word addr,uint32_t dpo,const uint8_t *bytes,size_t num_bytes) {
    // Not currently very clever.
    for(size_t i=0;i<num_bytes;++i) {
        const BigPage *bp=this->DebugGetBigPage(addr.b.h,dpo);

        if(bp->w) {
            bp->w[addr.w&BIG_PAGE_OFFSET_MASK]=bytes[i];
        }

        ++addr.w;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//#if BBCMICRO_DEBUGGER
//int BBCMicro::DebugGetByte(M6502Word addr,uint32_t dpo) const {
//    const BigPage *bp=this->DebugGetBigPage(addr.b.h,dpo);
//
//    if(bp->r) {
//        return bp->r[addr.w&BIG_PAGE_OFFSET_MASK];
//    } else {
//        return -1;
//    }
//}
//#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//void BBCMicro::DebugSetByte(M6502Word addr,uint32_t dpo,uint8_t value) {
//    const BigPage *bp=this->DebugGetBigPage(addr.b.h,dpo);
//
//    if(bp->w) {
//        bp->w[addr.w&BIG_PAGE_OFFSET_MASK]=value;
//    }
//}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//#if BBCMICRO_DEBUGGER
//void BBCMicro::SetMemory(M6502Word addr,uint8_t value) {
//    if(m_pc_pages) {
//        m_pc_pages[m_state.cpu.opcode_pc.b.h]->w[addr.b.h][addr.b.l]=value;
//    } else {
//        m_pages.w[addr.b.h][addr.b.l]=value;
//    }
//}
//#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER

void BBCMicro::SetExtMemory(uint32_t addr, uint8_t value) {
    ExtMem::WriteMemory(&m_state.ext_mem, addr, value);
}

#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::DebugHalt(const char *fmt,...) {
    if(m_debug) {
        m_debug->is_halted=true;

        if(fmt) {
            va_list v;

            va_start(v,fmt);
            vsnprintf(m_debug->halt_reason,sizeof m_debug->halt_reason,fmt,v);
            va_end(v);
        } else {
            m_debug->halt_reason[0]=0;
        }

        for(DebugState::ByteDebugFlags *flags:m_debug->temp_execute_breakpoints) {
            ASSERT(flags>=(void *)m_debug->big_pages_debug_flags);
            ASSERT(flags<(void *)((char *)m_debug->big_pages_debug_flags+sizeof m_debug->big_pages_debug_flags));
            ASSERT(flags->bits.temp_execute);
            flags->bits.temp_execute=0;
        }

        m_debug->temp_execute_breakpoints.clear();

        this->SetDebugStepType(BBCMicroStepType_None);
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
bool BBCMicro::DebugIsHalted() const {
    if(!m_debug) {
        return false;
    }

    return m_debug->is_halted;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
const char *BBCMicro::DebugGetHaltReason() const {
    if(!m_debug) {
        return nullptr;
    }

    if(m_debug->halt_reason[0]==0) {
        return nullptr;
    }

    return m_debug->halt_reason;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::DebugRun() {
    if(!m_debug) {
        return;
    }

    m_debug->is_halted=false;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
BBCMicro::DebugState::ByteDebugFlags BBCMicro::DebugGetByteFlags(M6502Word addr) const {
    if(!m_debug) {
        return DUMMY_BYTE_DEBUG_FLAGS;
    }

    if(m_pc_pages) {
        return m_pc_pages[m_state.cpu.opcode_pc.b.h]->debug[addr.b.h][addr.b.l];
    } else {
        return m_pages.debug[addr.b.h][addr.b.l];
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::DebugSetByteFlags(M6502Word addr,DebugState::ByteDebugFlags flags) {
    if(!m_debug) {
        return;
    }

    if(m_pc_pages) {
        m_pc_pages[m_state.cpu.opcode_pc.b.h]->debug[addr.b.h][addr.b.l]=flags;
    } else {
        m_pages.debug[addr.b.h][addr.b.l]=flags;
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::DebugAddTempBreakpoint(M6502Word addr) {
    if(!m_debug) {
        return;
    }

    DebugState::ByteDebugFlags *flags;
    if(m_pc_pages) {
        flags=&m_pc_pages[m_state.cpu.opcode_pc.b.h]->debug[addr.b.h][addr.b.l];
    } else {
        flags=&m_pages.debug[addr.b.h][addr.b.l];
    }

    if(flags->bits.temp_execute) {
        ASSERT(std::find(m_debug->temp_execute_breakpoints.begin(),
                         m_debug->temp_execute_breakpoints.end(),
                         flags)!=m_debug->temp_execute_breakpoints.end());
    } else {
        flags->bits.temp_execute=1;
        m_debug->temp_execute_breakpoints.push_back(flags);
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::DebugStepIn() {
    if(!m_debug) {
        return;
    }

    this->SetDebugStepType(BBCMicroStepType_StepIn);
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

//#if BBCMICRO_DEBUGGER
//char BBCMicro::DebugGetFlatPageCode(uint16_t flat_page) {
//    static_assert(sizeof DEBUG_PAGE_CODES==DebugState::NUM_DEBUG_FLAGS_PAGES+1,"");
//    if(flat_page==DebugState::INVALID_PAGE_INDEX) {
//        return '!';
//    } else {
//        ASSERT(flat_page<DebugState::NUM_DEBUG_FLAGS_PAGES);
//        return DEBUG_PAGE_CODES[flat_page];
//    }
//}
//#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
bool BBCMicro::HasDebugState() const {
    return !!m_debug;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
std::unique_ptr<BBCMicro::DebugState> BBCMicro::TakeDebugState() {
    std::unique_ptr<BBCMicro::DebugState> debug=std::move(m_debug_ptr);

    m_debug=nullptr;

    this->UpdateDebugState();

    return debug;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::SetDebugState(std::unique_ptr<DebugState> debug) {
    m_debug_ptr=std::move(debug);
    m_debug=m_debug_ptr.get();

    this->UpdateDebugState();
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
BBCMicro::HardwareDebugState BBCMicro::GetHardwareDebugState() const {
    if(!m_debug) {
        return HardwareDebugState();
    }

    return m_debug->hw;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::SetHardwareDebugState(const HardwareDebugState &hw) {
    if(!m_debug) {
        return;
    }

    m_debug->hw=hw;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::DebugSetAsyncCall(uint16_t address,uint8_t a,uint8_t x,uint8_t y,bool c,DebugAsyncCallFn fn,void *context) {
    this->FinishAsyncCall(false);

    m_state.async_call_address.w=address;
    m_state.async_call_timeout=ASYNC_CALL_TIMEOUT;
    m_state.async_call_a=a;
    m_state.async_call_x=x;
    m_state.async_call_y=y;
    m_state.async_call_c=c;
    m_async_call_fn=fn;
    m_async_call_context=context;

    this->UpdateCPUDataBusFn();
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
uint32_t BBCMicro::DebugGetPageOverrideMask() const {
    return m_dpo_mask;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
uint32_t BBCMicro::DebugGetCurrentPageOverride() const {
    switch(m_type) {
        case BBCMicroType_B:
        {
            uint32_t dpo=0;

            dpo|=m_state.romsel.b_bits.pr;

            return dpo;
        }

        case BBCMicroType_BPlus:
        {
            uint32_t dpo=0;

            dpo|=m_state.romsel.bplus_bits.pr;

            if(m_state.romsel.bplus_bits.ram) {
                dpo|=BBCMicroDebugPagingOverride_ANDY;
            }

            if(m_state.acccon.bplus_bits.shadow) {
                dpo|=BBCMicroDebugPagingOverride_Shadow;
            }

            return dpo;
        }

        case BBCMicroType_Master:
        {
            uint32_t dpo=0;

            dpo|=m_state.romsel.m128_bits.pm;

            if(m_state.romsel.m128_bits.ram) {
                dpo|=BBCMicroDebugPagingOverride_ANDY;
            }

            if(m_state.acccon.m128_bits.x) {
                dpo|=BBCMicroDebugPagingOverride_Shadow;
            }

            if(m_state.acccon.m128_bits.y) {
                dpo|=BBCMicroDebugPagingOverride_HAZEL;
            }

            if(m_state.acccon.m128_bits.tst) {
                dpo|=BBCMicroDebugPagingOverride_OS;
            }

            return dpo;
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SendBeebLinkResponse(std::vector<uint8_t> data) {
    ASSERT(!!m_beeblink);
    m_beeblink->SendResponse(std::move(data));
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::UpdateDebugPages(MemoryPages *pages) {
    for(size_t i=0;i<256;++i) {
        if(m_debug) {
            ASSERT(pages->bp[i]);
            uint16_t offset=(i<<8)&BIG_PAGE_OFFSET_MASK;
            pages->debug[i]=m_debug->big_pages_debug_flags[pages->bp[i]->index]+offset;
        } else {
            pages->debug[i]=nullptr;
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::UpdateDebugState() {
    this->UpdateCPUDataBusFn();

    // Update the debug page pointers.
    if(m_shadow_pages) {
        this->UpdateDebugPages(m_shadow_pages);
    }

    this->UpdateDebugPages(&m_pages);
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_DEBUGGER
void BBCMicro::SetDebugStepType(BBCMicroStepType step_type) {
    if(m_debug) {
        if(m_debug->step_type!=step_type) {
            m_debug->step_type=step_type;
            this->UpdateCPUDataBusFn();
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::InitStuff() {
    CHECK_SIZEOF(AddressableLatch,1);
    CHECK_SIZEOF(ROMSEL,1);
    CHECK_SIZEOF(ACCCON,1);
    CHECK_SIZEOF(SystemVIAPB,1);
    static_assert(::NUM_BIG_PAGES==BBCMicro::NUM_BIG_PAGES,"oops");

    m_ram=m_state.ram_buffer.data();

    m_state.cpu.context=this;

#if BBCMICRO_DEBUGGER
    //for(uint16_t i=0;i<NUM_DEBUG_FLAGS_PAGES;++i) {
    //    m_debug_flags_pages[i].flat_page=i;
    //}
#endif

    for(int i=0;i<3;++i) {
        m_hw_rmmio_fns[i]=std::vector<ReadMMIOFn>(256);
        m_hw_wmmio_fns[i]=std::vector<WriteMMIOFn>(256);
        m_hw_mmio_fn_contexts[i]=std::vector<void *>(256);
        m_hw_mmio_stretch[i]=std::vector<uint8_t>(256);

        // Assume hardware is mapped. It will get fixed up later if
        // not.
        m_rmmio_fns[i]=m_hw_rmmio_fns[i].data();
        m_mmio_fn_contexts[i]=m_hw_mmio_fn_contexts[i].data();
        m_mmio_stretch[i]=m_hw_mmio_stretch[i].data();
    }

//    for(int i=0;i<128;++i) {
//        m_pages.r[0x00+i]=m_pages.w[0x00+i]=&m_ram[i*256];
//#if BBCMICRO_DEBUGGER
//        m_pages.debug_page_index[0x00+i]=(uint16_t)i;
//#endif
//    }

    //for(int i=0;i<128;++i) {
    //    m_pages.w[0x80+i]=g_rom_writes;
    //    m_pages.r[0x80+i]=g_unmapped_rom_reads;
    //}

    // initially no I/O
    for(uint16_t i=0xfc00;i<0xff00;++i) {
        this->SetMMIOFns(i,nullptr,nullptr,nullptr);
    }

#if BBCMICRO_DEBUGGER
    for(size_t i=0;i<sizeof m_state.async_call_thunk_buf;++i) {
        this->SetMMIOFns((uint16_t)(ASYNC_CALL_THUNK_ADDR.w+i),&ReadAsyncCallThunk,nullptr,this);
    }
#endif

    if(m_ext_mem) {
        m_state.ext_mem.AllocateBuffer();

        this->SetMMIOFns(0xfc00,nullptr,&ExtMem::WriteAddressL,&m_state.ext_mem);
        this->SetMMIOFns(0xfc01,nullptr,&ExtMem::WriteAddressH,&m_state.ext_mem);
        this->SetMMIOFns(0xfc02,&ExtMem::ReadAddressL,nullptr,&m_state.ext_mem);
        this->SetMMIOFns(0xfc03,&ExtMem::ReadAddressH,nullptr,&m_state.ext_mem);

        for (uint16_t i=0xfd00;i<=0xfdff;++i) {
            this->SetMMIOFns(i,&ExtMem::ReadData,&ExtMem::WriteData,&m_state.ext_mem);
        }
    }

    // I/O: VIAs
    for(uint16_t i=0;i<32;++i) {
        this->SetMMIOFns(0xfe40+i,g_R6522_read_fns[i&15],g_R6522_write_fns[i&15],&m_state.system_via);
        this->SetMMIOFns(0xfe60+i,g_R6522_read_fns[i&15],g_R6522_write_fns[i&15],&m_state.user_via);
    }

    // I/O: 6845
    for(int i=0;i<8;i+=2) {
        this->SetMMIOFns((uint16_t)(0xfe00+i+0),&CRTC::ReadAddress,&CRTC::WriteAddress,&m_state.crtc);
        this->SetMMIOFns((uint16_t)(0xfe00+i+1),&CRTC::ReadData,&CRTC::WriteData,&m_state.crtc);

    }

    // I/O: Video ULA
    for(int i=0;i<2;++i) {
        this->SetMMIOFns((uint16_t)(0xfe20+i*2),nullptr,&VideoULA::WriteControlRegister,&m_state.video_ula);
        this->SetMMIOFns((uint16_t)(0xfe21+i*2),nullptr,&VideoULA::WritePalette,&m_state.video_ula);
    }

    if(m_video_nula) {
        this->SetMMIOFns(0xfe22,nullptr,&VideoULA::WriteNuLAControlRegister,&m_state.video_ula);
        this->SetMMIOFns(0xfe23,nullptr,&VideoULA::WriteNuLAPalette,&m_state.video_ula);
    }

    // I/O: disc interface
    if(m_disc_interface) {
        m_state.fdc.SetHandler(this);
        m_state.fdc.SetNoINTRQ(!!(m_disc_interface->flags&DiscInterfaceFlag_NoINTRQ));
        m_state.fdc.Set1772(!!(m_disc_interface->flags&DiscInterfaceFlag_1772));

        M6502Word c={m_disc_interface->control_addr};
        c.b.h-=0xfc;
        ASSERT(c.b.h<3);

        M6502Word f={m_disc_interface->fdc_addr};
        f.b.h-=0xfc;
        ASSERT(f.b.h<3);

        for(int i=0;i<4;++i) {
            this->SetMMIOFns((uint16_t)(m_disc_interface->fdc_addr+i),g_WD1770_read_fns[i],g_WD1770_write_fns[i],&m_state.fdc);
        }

        this->SetMMIOFns(m_disc_interface->control_addr,&Read1770ControlRegister,&Write1770ControlRegister,this);

        m_disc_interface->InstallExtraHardware(this);
    } else {
        m_state.fdc.SetHandler(nullptr);
    }

    m_state.system_via.SetID(BBCMicroVIAID_SystemVIA,"SystemVIA");
    m_state.user_via.SetID(BBCMicroVIAID_UserVIA,"UserVIA");

    m_state.old_system_via_pb=m_state.system_via.b.p;

    // Fill in shadow RAM stuff.
    if(m_state.ram_buffer.size()>=65536) {
        m_shadow_pages=new MemoryPages(m_pages);

//        for(uint8_t page=0x30;page<0x80;++page) {
//            m_shadow_pages->r[page]+=0x8000;
//            m_shadow_pages->w[page]+=0x8000;
//#if BBCMICRO_DEBUGGER
//            m_shadow_pages->debug_page_index[page]=DEBUG_SHADOW_RAM_PAGE+page-0x30;
//#endif
//        }

        m_pc_pages=new const MemoryPages *[256];

        for(size_t i=0;i<256;++i) {
            m_pc_pages[i]=&m_pages;
        }
    }

    if(m_beeblink_handler) {
        m_beeblink=std::make_unique<BeebLink>(m_beeblink_handler);
    }

    this->UpdateCPUDataBusFn();

    switch(m_type) {
        default:
            ASSERT(false);
            // fall through
        case BBCMicroType_B:
            m_update_romsel_pages_fn=&UpdateBROMSELPages;
            m_romsel_mask=0x0f;
            m_update_acccon_pages_fn=&UpdateBACCCONPages;
            m_acccon_mask=0;
            m_teletext_bases[0]=0x3c00;
            m_teletext_bases[1]=0x7c00;
            for(uint16_t i=0;i<16;++i) {
                this->SetMMIOFns((uint16_t)(0xfe30+i),&ReadROMSEL,&WriteROMSEL,this);
            }
            m_dpo_mask=(BBCMicroDebugPagingOverride_OverrideROM|
                        BBCMicroDebugPagingOverride_ROM);
            break;

        case BBCMicroType_BPlus:
            m_update_romsel_pages_fn=&UpdateBPlusROMSELPages;
            m_romsel_mask=0x8f;
            m_update_acccon_pages_fn=&UpdateBPlusACCCONPages;
            m_acccon_mask=0x80;
            m_teletext_bases[0]=0x3c00;
            m_teletext_bases[1]=0x7c00;
            m_dpo_mask=(BBCMicroDebugPagingOverride_ROM|
                        BBCMicroDebugPagingOverride_OverrideROM|
                        BBCMicroDebugPagingOverride_ANDY|
                        BBCMicroDebugPagingOverride_OverrideANDY|
                        BBCMicroDebugPagingOverride_Shadow|
                        BBCMicroDebugPagingOverride_OverrideShadow);
        romsel_and_acccon:
            for(uint16_t i=0;i<4;++i) {
                this->SetMMIOFns((uint16_t)(0xfe30+i),&ReadROMSEL,&WriteROMSEL,this);
                this->SetMMIOFns((uint16_t)(0xfe34+i),&ReadACCCON,&WriteACCCON,this);
            }
            break;

        case BBCMicroType_Master:
            for(int i=0;i<3;++i) {
                m_rom_rmmio_fns=std::vector<ReadMMIOFn>(256,&ReadROMMMIO);
                m_rom_mmio_fn_contexts=std::vector<void *>(256,this);
                m_rom_mmio_stretch=std::vector<uint8_t>(256,0x00);
            }

            m_has_rtc=true;
            m_update_romsel_pages_fn=&UpdateMaster128ROMSELPages;
            m_romsel_mask=0x8f;
            m_update_acccon_pages_fn=&UpdateMaster128ACCCONPages;
            m_acccon_mask=0xff;
            m_teletext_bases[0]=0x7c00;
            m_teletext_bases[1]=0x7c00;
            m_dpo_mask=(BBCMicroDebugPagingOverride_ROM|
                        BBCMicroDebugPagingOverride_OverrideROM|
                        BBCMicroDebugPagingOverride_ANDY|
                        BBCMicroDebugPagingOverride_OverrideANDY|
                        BBCMicroDebugPagingOverride_HAZEL|
                        BBCMicroDebugPagingOverride_OverrideHAZEL|
                        BBCMicroDebugPagingOverride_Shadow|
                        BBCMicroDebugPagingOverride_OverrideShadow|
                        BBCMicroDebugPagingOverride_OS|
                        BBCMicroDebugPagingOverride_OverrideOS);
            goto romsel_and_acccon;
    }

    for(M6502Word a={0};a.b.h<3;++a.w) {
        bool stretch;

        switch(a.b.h) {
            case 0:
            case 1:
                // FRED/JIM
                stretch=true;
                break;

            case 2:
                // SHEILA
                if(a.b.l>=0x00&&a.b.l<=0x1f) {
                    stretch=true;
                } else if(a.b.l>=0x28&&a.b.l<=0x2b) {
                    stretch=m_type==BBCMicroType_Master;
                } else if(a.b.l>=0x40&&a.b.l<=0x7f) {
                    stretch=true;
                } else if(a.b.l>=0xc0&&a.b.l<=0xdf) {
                    stretch=m_type!=BBCMicroType_Master;
                } else {
                    stretch=false;
                }
                break;
        }

        m_hw_mmio_stretch[a.b.h][a.b.l]=stretch?0xff:0x00;
    }

    // Page in current ROM bank and sort out ACCCON.
    this->InitBigPages();

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
    switch(m_type) {
    default:
        ASSERT(false);
        // fall through
    case BBCMicroType_B:
    case BBCMicroType_BPlus:
    case BBCMicroType_Master:
        this->InitDiscDriveSounds(DiscDriveType_133mm);
        break;
    }
#endif

#if BBCMICRO_TRACE
    this->SetTrace(nullptr,0);
#endif

    for(int i=0;i<3;++i) {
        ASSERT(m_rmmio_fns[i]==m_hw_rmmio_fns[i].data()||m_rmmio_fns[i]==m_rom_rmmio_fns.data());
        ASSERT(m_mmio_fn_contexts[i]==m_hw_mmio_fn_contexts[i].data()||m_mmio_fn_contexts[i]==m_rom_mmio_fn_contexts.data());
        ASSERT(m_mmio_stretch[i]==m_hw_mmio_stretch[i].data()||m_mmio_stretch[i]==m_rom_mmio_stretch.data());
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::IsTrack0() {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        return dd->track==0;
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::StepOut() {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        if(dd->track>0) {
            --dd->track;

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
            this->StepSound(dd);
#endif
        }
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::StepIn() {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        if(dd->track<255) {
            ++dd->track;

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
            this->StepSound(dd);
#endif
        }
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SpinUp() {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        dd->motor=true;

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
        dd->spin_sound_index=0;
        dd->spin_sound=DiscDriveSound_SpinStartLoaded;
#endif
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::SpinDown() {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        dd->motor=false;

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
        dd->spin_sound_index=0;
        dd->spin_sound=DiscDriveSound_SpinEnd;
#endif
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::IsWriteProtected() {
    if(this->GetDiscDrive()) {
        if(m_disc_images[m_state.disc_control.drive]) {
            if(m_disc_images[m_state.disc_control.drive]->IsWriteProtected()) {
                return true;
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::GetByte(uint8_t *value,uint8_t sector,size_t offset) {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        m_disc_access=true;

        if(m_disc_images[m_state.disc_control.drive]) {
            if(m_disc_images[m_state.disc_control.drive]->Read(value,
                                                               m_state.disc_control.side,
                                                               dd->track,
                                                               sector,
                                                               offset))
            {
                return true;
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::SetByte(uint8_t sector,size_t offset,uint8_t value) {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        m_disc_access=true;

        if(m_disc_images[m_state.disc_control.drive]) {
            if(m_disc_images[m_state.disc_control.drive]->Write(m_state.disc_control.side,
                                                                dd->track,
                                                                sector,
                                                                offset,
                                                                value))
            {
                return true;
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

bool BBCMicro::GetSectorDetails(uint8_t *track,uint8_t *side,size_t *size,uint8_t sector,bool double_density) {
    if(DiscDrive *dd=this->GetDiscDrive()) {
        m_disc_access=true;

        if(m_disc_images[m_state.disc_control.drive]) {
            if(m_disc_images[m_state.disc_control.drive]->GetDiscSectorSize(size,m_state.disc_control.side,dd->track,sector,double_density)) {
                *track=dd->track;
                *side=m_state.disc_control.side;
                return true;
            }
        }
    }

    return false;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

BBCMicro::DiscDrive *BBCMicro::GetDiscDrive() {
    if(m_state.disc_control.drive>=0&&m_state.disc_control.drive<NUM_DRIVES) {
        return &m_state.drives[m_state.disc_control.drive];
    } else {
        return nullptr;
    }
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
void BBCMicro::InitDiscDriveSounds(DiscDriveType type) {
    auto &&it=g_disc_drive_sounds.find(type);
    if(it==g_disc_drive_sounds.end()) {
        return;
    }

    for(size_t i=0;i<DiscDriveSound_EndValue;++i) {
        m_disc_drive_sounds[i]=&it->second[i];
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND

// As per http://www.ninerpedia.org/index.php?title=MAME_Floppy_sound_emulation

struct SeekSound {
    size_t clock_ticks;
    DiscDriveSound sound;
};

#define SEEK_SOUND(N) {SOUND_CLOCKS_FROM_MS(N),DiscDriveSound_Seek##N##ms,}

static const SeekSound g_seek_sounds[]={
    {SOUND_CLOCKS_FROM_MS(17),DiscDriveSound_Seek20ms,},
    {SOUND_CLOCKS_FROM_MS(10),DiscDriveSound_Seek12ms,},
    {SOUND_CLOCKS_FROM_MS(4),DiscDriveSound_Seek6ms,},
    {1,DiscDriveSound_Seek2ms,},
    {0},
};

void BBCMicro::StepSound(DiscDrive *dd) {
    if(dd->step_sound_index<0) {
        // step
        dd->step_sound_index=0;
    } else if(dd->seek_sound==DiscDriveSound_EndValue) {
        // skip a bit of the step sound
        dd->step_sound_index+=SOUND_CLOCK_HZ/100;

        // seek. Start with 20ms... it's as good a guess as any.
        dd->seek_sound=DiscDriveSound_Seek20ms;
        dd->seek_sound_index=0;
    } else {
        for(const SeekSound *seek_sound=g_seek_sounds;seek_sound->clock_ticks!=0;++seek_sound) {
            if(dd->seek_sound_index>=seek_sound->clock_ticks) {
                if(dd->seek_sound!=seek_sound->sound) {
                    dd->seek_sound=seek_sound->sound;
                    dd->seek_sound_index=0;
                    break;
                }
            }
        }
    }
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if BBCMICRO_ENABLE_DISC_DRIVE_SOUND
float BBCMicro::UpdateDiscDriveSound(DiscDrive *dd) {
    float acc=0.f;

    if(dd->spin_sound!=DiscDriveSound_EndValue) {
        ASSERT(dd->spin_sound>=0&&dd->spin_sound<DiscDriveSound_EndValue);
        const std::vector<float> *spin_sound=m_disc_drive_sounds[dd->spin_sound];

        acc+=(*spin_sound)[dd->spin_sound_index];

        ++dd->spin_sound_index;
        if(dd->spin_sound_index>=spin_sound->size()) {
            switch(dd->spin_sound) {
            case DiscDriveSound_SpinStartEmpty:
            case DiscDriveSound_SpinEmpty:
                dd->spin_sound=DiscDriveSound_SpinEmpty;
                break;

            case DiscDriveSound_SpinStartLoaded:
            case DiscDriveSound_SpinLoaded:
                dd->spin_sound=DiscDriveSound_SpinLoaded;
                break;

            default:
                dd->spin_sound=DiscDriveSound_EndValue;
                break;
            }

            dd->spin_sound_index=0;
        }
    }

    if(dd->seek_sound!=DiscDriveSound_EndValue) {
        const std::vector<float> *seek_sound=m_disc_drive_sounds[dd->seek_sound];

        acc+=(*seek_sound)[dd->seek_sound_index];

        ++dd->seek_sound_index;
        if((size_t)dd->seek_sound_index>=seek_sound->size()) {
            dd->seek_sound=DiscDriveSound_EndValue;
        }
    } else if(dd->step_sound_index>=0) {
        const std::vector<float> *step_sound=m_disc_drive_sounds[DiscDriveSound_Step];

        // check for end first as the playback position is adjusted in
        // StepSound.
        if((size_t)dd->step_sound_index>=step_sound->size()) {
            dd->step_sound_index=-1;
        } else {
            acc+=(*step_sound)[(size_t)dd->step_sound_index++];
        }
    }

    return acc;
}
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

void BBCMicro::UpdateCPUDataBusFn() {
    if(m_state.ram_buffer.size()>=65536) {
        m_default_handle_cpu_data_bus_fn=
#if BBCMICRO_DEBUGGER
            m_debug?&HandleCPUDataBusWithShadowRAMDebug:
#endif
            &HandleCPUDataBusWithShadowRAM;
    } else {
        m_default_handle_cpu_data_bus_fn=
#if BBCMICRO_DEBUGGER
            m_debug?&HandleCPUDataBusMainRAMOnlyDebug:
#endif
            &HandleCPUDataBusMainRAMOnly;
    }

    if(m_state.hack_flags!=0) {
        goto hack;
    }

#if BBCMICRO_TRACE
    if(m_trace) {
        goto hack;
    }
#endif

#if BBCMICRO_DEBUGGER
    if(m_debug) {
        if(m_debug->step_type!=BBCMicroStepType_None) {
            goto hack;
        }
    }
#endif

    if(!m_instruction_fns.empty()) {
        goto hack;
    }

#if BBCMICRO_DEBUGGER
    if(m_state.async_call_address.w!=INVALID_ASYNC_CALL_ADDRESS) {
        goto hack;
    }
#endif
    
    // No hacks.
    m_handle_cpu_data_bus_fn=m_default_handle_cpu_data_bus_fn;
    return;

hack:;
    m_handle_cpu_data_bus_fn=&HandleCPUDataBusWithHacks;
}

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////
