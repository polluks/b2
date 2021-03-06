#ifndef HEADER_DE3F3F401AB041929180F37C8DEFC129// -*- mode:c++ -*-
#define HEADER_DE3F3F401AB041929180F37C8DEFC129

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include <string>
#include <memory>
#include <array>
#include <vector>

class BBCMicro;
class Messages;
struct DiscInterfaceDef;
struct BBCMicroType;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

static const size_t ROM_SIZE=16384;

typedef std::array<uint8_t,ROM_SIZE> BeebRomData;

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// The BeebConfig holds all the config info that gets saved to the
// JSON config file.

class BeebConfig {
public:
    struct ROM {
        std::string file_name;
        bool writeable=false;
    };

    std::string name;

    const BBCMicroType *type=nullptr;
    std::string os_file_name;
    ROM roms[16];
    uint8_t keyboard_links=0;
    const DiscInterfaceDef *disc_interface=nullptr;
    bool video_nula=true;
    bool ext_mem=false;
    bool beeblink=false;
protected:
private:
};

void InitDefaultBeebConfigs();

size_t GetNumDefaultBeebConfigs();
const BeebConfig *GetDefaultBeebConfigByIndex(size_t index);

std::vector<uint8_t> GetDefaultNVRAMContents(const BBCMicroType *type);
void ResetDefaultNVRAMContents(const BBCMicroType *type);
void SetDefaultNVRAMContents(const BBCMicroType *type,std::vector<uint8_t> nvram_contents);

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

// The BeebLoadedConfig holds the actual data for a BeebConfig.
//
// For each BeebLoadedConfig::ROM: if the corresponding
// BeebConfig::ROM's writeable flag is false, contents must be
// non-null, and it points to the data to be used for that ROM by all
// BBCMicro objects referring to it.
//
// Otherwise (writeable is true), if contents is non-null, it's the
// initial data to be used for that sideways RAM bank, else the
// sideways RAM bank is initially all zero.

class BeebLoadedConfig {
public:
    BeebConfig config;

    std::shared_ptr<const BeebRomData> os,roms[16];

    static bool Load(BeebLoadedConfig *dest,const BeebConfig &src,Messages *msg);

    void ReuseROMs(const BeebLoadedConfig &oth);
protected:
private:
};

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif
