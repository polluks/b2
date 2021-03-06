#ifndef HEADER_356A8AF5B2414C64849DD8808824A13D
#define HEADER_356A8AF5B2414C64849DD8808824A13D

#include "conf.h"
#include <memory>

#if BBCMICRO_TRACE

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// Simple thing for recording trace events in a vaguely efficient
// fashion - main purpose is for recording instruction/register logs,
// so 500,000 events/sec or whatever, ideally without slowing things
// down when running in speed-limited mode. So it records fixed-sized
// blocks of data and has an efficientish encoding scheme so that
// there's only a 2-byte overhead for each event blah blah. The
// original intention was that most stuff would be logged this way,
// and would get turned into strings later, because calling snprintf
// thousands of times a second just isn't a very good idea.
//
// But it turns out that PCs are fast enough - or at least mine is? -
// that for non-instruction events it doesn't matter. So in the end
// pretty much everything else just saves strings using
// Trace_GetLogPrinter.
//
// (Trace objects are designed to be created with make_shared and
// mainly used via shared_ptr. But in the interests of non-terrible
// debug build performance, some of the emulator objects hold plain
// pointers to the current Trace object, relying on the BBCMicro to
// extend the Trace object's lifespan appropriately (by holding a
// shared_ptr to it) and resetting their pointers to null
// appropriately.)

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include <shared/log.h>
#include <string>
#include "type.h"

struct M6502Config;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

class TraceEventType {
public:
    const uint8_t type_id;
    const size_t size;

    explicit TraceEventType(const char *name,size_t size=0);
    ~TraceEventType();

    TraceEventType(const TraceEventType &)=delete;
    TraceEventType &operator=(const TraceEventType &)=delete;

    TraceEventType(TraceEventType &&)=delete;
    TraceEventType &operator=(TraceEventType &&)=delete;

    const std::string &GetName() const;
protected:
private:
    std::string m_name;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

struct TraceStats {
    size_t num_events=0;
    size_t num_used_bytes=0;
    size_t num_allocated_bytes=0;
    uint64_t max_time=0;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

struct TraceEvent {
    // type of this event.
    const TraceEventType *type;

    // absolute time of this event.
    uint64_t time;

    // size of this event's data.
    size_t size;

    // pointer to this event's data.
    const void *event;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

class Trace:
    public std::enable_shared_from_this<Trace>
{
public:
#include <shared/pshpack1.h>
    struct WriteROMSELEvent {
        ROMSEL romsel;
    };
#include <shared/poppack.h>

#include <shared/pshpack1.h>
    struct WriteACCCONEvent {
        ACCCON acccon;
    };
#include <shared/poppack.h>

    static const TraceEventType BLANK_LINE_EVENT;
    static const TraceEventType STRING_EVENT;
    static const TraceEventType DISCONTINUITY_EVENT;
    static const TraceEventType WRITE_ROMSEL_EVENT;
    static const TraceEventType WRITE_ACCCON_EVENT;

    // max_num_bytes is approximate - actual consumption may be greater.
    // Supply SIZE_MAX to just have the data grow indefinitely.
    //
    // bbc_micro_type is actually a BBCMicroType value - I made a mess of the
    // header structure here :(
    explicit Trace(size_t max_num_bytes,
                   const BBCMicroType *type,
                   ROMSEL initial_romsel_value,
                   ACCCON initial_acccon_value);
    ~Trace();

    Trace(const Trace &)=delete;
    Trace &operator=(const Trace &)=delete;

    Trace(Trace &&)=delete;
    Trace &operator=(Trace &&)=delete;

    // When the trace's time pointer is non-NULL, it is used to fill
    // out each event's time field.
    void SetTime(const uint64_t *time_ptr);

    // Allocate a new event with fixed-size data, returning a pointer
    // to its data or nullptr if the data size is 0. (The type is used to find
    // the data size.)
    void *AllocEvent(const TraceEventType &type);

    // TYPE must be the type of event the data was allocated for.
    void CancelEvent(const TraceEventType &type,void *data);

    void AllocBlankLineEvent();

    // Inefficient convenience functions for low-frequency trace events.
    //
    // When a string doesn't finish with a '\n', one will be added when
    // saving the output.

    // Space for format expansion is limited to 1K.
    void PRINTF_LIKE(2,3) AllocStringf(const char *fmt,...);
    void AllocStringv(const char *fmt,va_list v);
    void AllocString(const char *str);

    // Allocate events for ROMSEL/ACCCON writes. These need special handling so
    // the initial values can be correctly maintained.
    void AllocWriteROMSELEvent(ROMSEL romsel);
    void AllocWriteACCCONEvent(ACCCON acccon);

    // max_len bytes is allocated. Call FinishLog to try to truncate the
    // allocation if possible.
    LogPrinter *GetLogPrinter(size_t max_len);
    void FinishLog(Log *log);

    void GetStats(TraceStats *stats) const;

    const BBCMicroType *GetBBCMicroType() const;
    ROMSEL GetInitialROMSEL() const;
    ACCCON GetInitialACCCON() const;

    typedef bool (*ForEachEventFn)(Trace *t,const TraceEvent *e,void *context);

    // return true to continue iteration, false to stop it. returns
    // false if iteration was canceled.
    int ForEachEvent(ForEachEventFn fn,void *context);
protected:
private:
    struct Chunk;

    class LogPrinterTrace:
        public LogPrinter
    {
    public:
        LogPrinterTrace(Trace *t);

        void Print(const char *str,size_t str_len) override;
    protected:
    private:
        Trace *m_t=nullptr;
    };

    Chunk *m_head=nullptr,*m_tail=nullptr;
    TraceStats m_stats;
    uint8_t *m_last_alloc=nullptr;
    uint64_t m_last_time=0;
    const uint64_t *m_time_ptr=nullptr;

    char *m_log_data=nullptr;
    size_t m_log_len=0;
    size_t m_log_max_len=0;
    LogPrinterTrace m_log_printer{this};
    size_t m_max_num_bytes;

    const BBCMicroType *m_bbc_micro_type=nullptr;
    ROMSEL m_romsel={};
    ACCCON m_acccon={};

    // Allocate a new event with variable-sized data, and return a
    // pointer to its data. (The event must have been registered with
    // a size of 0.)
    void *AllocEventWithSize(const TraceEventType &type,size_t size);
    char *AllocString2(const char *str,size_t len);

    void *Alloc(uint64_t time,size_t n);
    void Check();
    static void PrintToTraceLog(const char *str,size_t str_len,void *data);
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define TRACEF(T,...)\
BEGIN_MACRO {\
    if(T) {\
        (T)->AllocStringf(__VA_ARGS__);\
    }\
} END_MACRO

#define TRACEF_IF(COND,T,...)\
BEGIN_MACRO {\
    if(T) {\
        if(COND) {\
            (T)->AllocStringf(__VA_ARGS__);\
        }\
    }\
} END_MACRO

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#else

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define TRACEF(...) ((void)0)

#define TRACEF_IF(...) ((void)0)

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif
#endif
