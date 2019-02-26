//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define ENAME VideoDataType
EBEGIN()
// Bitmap modes, blank/cursor-only areas
EPNV(Bitmap16MHz,0)

// Video NuLA attribute modes
EPN(Bitmap12MHz)

// Teletext - same output scaling as 12MHz modes, but a different encoding, to
// get the higher-resolution-looking characters. (This should really be done
// at the TVOutput end... maybe one day...)
EPN(Teletext)

EEND()
#undef ENAME

// Ensure a memset(x,0,sizeof *x) (or similar) produces blank pixels.
static_assert(VideoDataType_Bitmap16MHz==0,"");

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#define ENAME VideoDataUnitFlag
EBEGIN()
// VSync is on.
EPNV(VSync,1<<0)

// HSync is on.
EPNV(HSync,1<<1)
EEND()
#undef ENAME

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#if VIDEO_TRACK_METADATA
#define ENAME VideoDataUnitMetadataFlag
EBEGIN()
EPNV(HasAddress,1<<0)
EPNV(OddCycle,1<<1)
EPNV(HasValue,1<<2)
EPNV(6845Raster0,1<<3)
EPNV(6845DISPEN,1<<4)
EPNV(6845CUDISP,1<<5)
EEND()
#undef ENAME
#endif

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

