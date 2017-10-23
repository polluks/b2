#ifndef HEADER_8E9F48F66B734BF09EFD8580786ECA18// -*- mode:c++ -*-
#define HEADER_8E9F48F66B734BF09EFD8580786ECA18

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#include <memory>

class BeebWindow;
class SettingsUI;

std::unique_ptr<SettingsUI> CreateDataRateUI(BeebWindow *beeb_window);

//////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////

#endif
