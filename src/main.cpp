#include <wx/wx.h>
#include <wx/image.h>
#include "MainFrame.h"

class App : public wxApp
{
public:
    bool OnInit() override
    {
        if (!wxApp::OnInit())
            return false;

        // Without this, wxImage::LoadFile() (used for icon-view file
        // thumbnails — see FileExplorerTab::ReloadDirectory) only
        // understands BMP; PNG/JPEG/GIF/TIFF/etc. would silently fail to
        // decode and every one of those files would just fall back to the
        // generic file icon.
        wxInitAllImageHandlers();

        MainFrame* frame = new MainFrame();
        frame->Show(true);
        return true;
    }
};

wxIMPLEMENT_APP(App);
