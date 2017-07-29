#ifdef HEADER_33477416E2224FD8A7F0B58AC38F831D// -*- mode:c++ -*-
// sorry... this header is too much of a monster.
#error
#else
#define HEADER_33477416E2224FD8A7F0B58AC38F831D

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include "conf.h"

struct BeebWindowInitArguments;
class VBlankMonitor;
class BeebThread;
class TimelineUI;
struct SDL_Texture;
struct SDL_Renderer;
struct Keycap;
class ConfigsUI;
class KeymapsUI;
class MessagesUI;
#if BBCMICRO_TRACE
class TraceUI;
#endif
class NVRAMUI;
class DataRateUI;
class BeebState;

#include "keys.h"
#include "dear_imgui.h"
#include <string>
#include <SDL.h>
#include "TVOutput.h"
#include "native_ui.h"
#include <beeb/conf.h>
#include <shared/log.h>
#include <functional>
#include "Messages.h"
#include <map>
#include <set>
#include <limits.h>
#include "BeebConfig.h"
#include "BeebKeymap.h"
#include "commands.h"

#include <shared/enum_decl.h>
#include "BeebWindow.inl"
#include <shared/enum_end.h>

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

struct BeebWindowTextureDataVersion {
    uint64_t version=0;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

struct BeebWindowSettings {
    uint32_t ui_flags=0;

    float bbc_volume=0.f;
    float disc_volume=0.f;

    bool display_auto_scale=true;
    float display_overall_scale=1.f;
    float display_scale_x=1.2f/1.25f;
    float display_scale_y=1.f;
    BeebWindowDisplayAlignment display_alignment_x=BeebWindowDisplayAlignment_Centre;
    BeebWindowDisplayAlignment display_alignment_y=BeebWindowDisplayAlignment_Centre;
    bool display_filter=false;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

struct BeebWindowInitArguments {
public:
    // Index of SDL render driver, as supplied to SDL_CreateRenderer.
    int render_driver_index=-1;

    // SDL pixel format to use.
    uint32_t pixel_format=0;

    // SDL_AudioDeviceID of device to play to. The window doesn't
    // create the audio device, but it needs to know which ID to use
    // so it can respond to the appropriate callback.
    uint32_t sound_device=0;

    // Sound playback details.
    SDL_AudioSpec sound_spec;

    // Name for the window.
    std::string name;

    // Placement data to use for this window. Empty if the default
    // position is OK.
    std::vector<uint8_t> placement_data;

#if SYSTEM_OSX

    // Frame name to load window frame from, or empty if the default
    // is OK.
    std::string frame_name;

#endif

    // If set, the emulator is initially paused.
    //
    // (This isn't actually terribly useful, because there's no way to
    // then unpause it from the public API - but the flag has to go
    // somewhere, and it has to funnel through CreateBeebWindow, so...
    // here it is.)
    bool initially_paused=false;

    // When INITIAL_STATE is non-null, it is used as the initial state
    // for the window, and PARENT_TIMELINE_EVENT_ID is the parent
    // event id. (This is used to split the timeline at a non-start
    // event, without having to introduce a new start event.)
    //
    // When INITIAL_STATE is null, but PARENT_TIMELINE_EVENT_ID is
    // non-zero, PARENT_TIMELINE_EVENT_ID is the event to load the
    // state from. (It must be a start event.)
    //
    // Otherwise (INITIAL_STATE==null, PARENT_TIMELINE_EVENT_ID==0),
    // DEFAULT_CONFIG holds the config to start with, and a new root
    // node will be created to start the timeline.
    //
    // DEFAULT_CONFIG must always be valid, even when INITIAL_STATE
    // and PARENT_TIMELINE_EVENT_ID will be used, as it is the config
    // used for new windows created by Window|New.
    std::shared_ptr<BeebState> initial_state;
    uint64_t parent_timeline_event_id=0;
    BeebLoadedConfig default_config;

    // Message list to be used to populate the window's message list.
    // This will almost always be null; it's used to collect messages
    // from the initial startup that are printed before there's any
    // windows to display them on.
    //
    // If the window init fails, and preinit_messages!=nullptr, any
    // additional init messages will be added here too.
    std::shared_ptr<MessageList> preinit_message_list;

    // If preinit_messages is null, this should be non-zero. It's the
    // ID of the window to send init messages to if the init fails.
    //
    // (If the initiating window has gone away, messages will be
    // printed to Messages::stdio.)
    uint32_t initiating_window_id=0;

    // If non-empty, name of the initial keymap to select.
    //
    // (Keymap pointer is no good, as this struct gets sent to the
    // BeebThread; the keymap could be deleted by the time it's used.)
    std::string keymap_name;

    // When USE_SETTINGS is true, copy new window's settings from
    // SETTINGS. Otherwise, use BeebWindow::defaults.
    bool use_settings=false;
    BeebWindowSettings settings;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

class BeebWindow {
public:
    struct VBlankRecord {
        uint64_t num_ticks=0;
        size_t num_video_units=0;
    };

    static const char SDL_WINDOW_DATA_NAME[];

    BeebWindow(BeebWindowInitArguments init_arguments);
    ~BeebWindow();

    bool Init();

    // Saves all settings, including whatever SavePosition does.
    void SaveSettings();

    // Saves position only.
    void SavePosition();

    const std::string &GetName() const;
    void SetName(std::string name);

    bool GetBeebKeyState(BeebKey key) const;

    uint32_t GetSDLWindowID() const;

    void HandleSDLKeyEvent(const SDL_KeyboardEvent &event);

    void SetSDLMouseWheelState(int x,int y);
    void HandleSDLTextInput(const char *text);

    void ThreadFillAudioBuffer(SDL_AudioDeviceID audio_device_id,float *mix_buffer,size_t num_samples);

    bool HandleVBlank(VBlankMonitor *vblank_monitor,void *display_data,uint64_t ticks);

    bool HandleVBlank(uint64_t ticks);

    void UpdateTitle();

    void BeebKeymapWillBeDeleted(BeebKeymap *keymap);

    // If this BeebWindow has a SDL_Texture holding the current
    // display contents that's suitable for rendering with the given
    // SDL_Renderer, return a pointer to it. Otherwise, nullptr.
    SDL_Texture *GetTextureForRenderer(SDL_Renderer *renderer) const;

    // If this BeebWindow's texture data has changed since the last
    // time this function was called with the given version object, or
    // if it's the first time of calling with the given version
    // object, fill the out parameters appropriately and return true.
    // Otherwise, out parameters are filled with unspecified values,
    // and returns false.
    bool GetTextureData(BeebWindowTextureDataVersion *version,
        const SDL_PixelFormat **format_ptr,const void **pixels_ptr) const;

    // Get pointer to this window's BeebThread. (The lifespan of the
    // BeebThread is the same as that of the window.)
    std::shared_ptr<BeebThread> GetBeebThread() const;

    //
    std::shared_ptr<MessageList> GetMessageList() const;

#if SYSTEM_WINDOWS
    void *GetHWND() const;
#endif
#if SYSTEM_OSX
    void *GetNSWindow() const;
#endif

    std::vector<VBlankRecord> GetVBlankRecords() const;
protected:
private:
    struct DriveState {
        FolderDialog open_65link_folder_dialog;
        OpenFileDialog open_disc_image_file_dialog;

        DriveState();
    };

    BeebWindowInitArguments m_init_arguments;
    std::string m_name;

    // SDLstuff.
    SDL_Window *m_window=nullptr;
    SDL_Renderer *m_renderer=nullptr;
    SDL_PixelFormat *m_pixel_format=nullptr;

#if SYSTEM_WINDOWS
    void *m_hwnd=nullptr;
#elif SYSTEM_OSX
    void *m_nswindow=nullptr;
#elif SYSTEM_LINUX
#include <shared/pshpack1.h>
    struct WindowPlacementData {
        uint8_t maximized=0;
        int x=INT_MIN,y=INT_MIN;
        int width=0,height=0;
    };
#include <shared/poppack.h>
#endif
    std::vector<VBlankRecord> m_vblank_records;
    size_t m_vblank_index=0;
    uint64_t m_last_vblank_ticks=0;

    // TV output.
    TVOutput m_tv;
    SDL_Texture *m_tv_texture=nullptr;

    float m_blend_amt=0.f;

    // Audio output
    SDL_AudioDeviceID m_sound_device=0;

    const BeebKeymap *m_keymap=nullptr;

    // number of emulated us that had passed at the time of the last
    // title update.
    uint64_t m_last_title_update_2MHz_cycles=0;
    uint64_t m_last_title_update_ticks=0;

    // copy of BeebThread state
    DriveState m_drives[NUM_DRIVES];

    // This is a shared_ptr since it can get passed to another thread
    // during the cloning process. It's set in the constructor and
    // then never modified.
    std::shared_ptr<BeebThread> m_beeb_thread;

    // Track Beeb key syms that were made pressed by a given
    // (unmodified) SDL keycode. SDL modifier key releases affect
    // future keycodes, so you can get (say) Shift+6 down, then Shift
    // up, then 6 up - which is no good if the states are only being
    // tracked by BBC key.
    //
    // This is a pretty crazy data type, but there's only so many keys
    // you can press per unit time.
    std::map<uint32_t,std::set<BeebKeySym>> m_beeb_keysyms_by_keycode;

    BeebWindowSettings m_settings;

    //
    //bool m_ui=false;
    ImGuiStuff *m_imgui_stuff=nullptr;
    bool m_imgui_has_kb_focus=false;

#if ENABLE_IMGUI_DEMO
    bool m_imgui_demo=false;
#endif

    std::vector<CommandContext *> m_cc_stack;

    std::vector<std::string> m_display_size_options;

    std::unique_ptr<KeymapsUI> m_keymaps_ui;
    std::unique_ptr<MessagesUI> m_messages_ui;
#if TIMELINE_UI_ENABLED
    std::unique_ptr<TimelineUI> m_timeline_ui;
#endif
    std::unique_ptr<ConfigsUI> m_configs_ui;
#if BBCMICRO_TRACE
    std::unique_ptr<TraceUI> m_trace_ui;
#endif
    std::unique_ptr<NVRAMUI> m_nvram_ui;
    std::unique_ptr<DataRateUI> m_data_rate_ui;

    bool m_messages_popup_ui_active=false;
    uint64_t m_messages_popup_ticks=0;
    uint32_t m_leds=0;

    bool m_leds_popup_ui_active=false;
    uint64_t m_leds_popup_ticks=0;

    //
    std::shared_ptr<MessageList> m_message_list;
    uint64_t m_msg_last_num_messages_printed=0;
    uint64_t m_msg_last_num_errors_and_warnings_printed=0;
    // Keep this towards the end. It's quite a large object.
    Messages m_msg;

    ObjectCommandContext<BeebWindow> m_cc;

    bool InitInternal();
    bool LoadDiscImageFile(int drive,const std::string &path);
    bool Load65LinkFolder(int drive,const std::string &path);
    void DoOptionsGui();
    bool DoImGui(int output_width,int output_height);
    BeebWindowInitArguments GetNewWindowInitArguments() const;
    void MaybeSaveConfig(bool save_config);
    void DoOptionsCheckbox(const char *label,bool (BeebThread::*get_mfn)() const,void (BeebThread::*send_mfn)(bool));
    void HardReset();
    void LoadLastState();
    void SaveState();
    bool HandleBeebKey(const SDL_Keysym &keysym,bool state);
    bool RecreateTexture();

    static ObjectCommandTable<BeebWindow> ms_command_table;
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif
