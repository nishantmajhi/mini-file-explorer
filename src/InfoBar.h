#pragma once
#include <wx/wx.h>

// A single-line status strip docked at the bottom of the window, in the
// same slot the terminal occupies — shown whenever the terminal isn't (see
// MainFrame, which toggles the two panes' visibility oppositely). Purely
// a display widget: MainFrame decides what text belongs here and pushes it
// in via SetText() whenever the focused pane's selection/directory changes.
class InfoBar : public wxPanel
{
public:
    explicit InfoBar(wxWindow* parent);

    void SetText(const wxString& text);

private:
    wxStaticText* m_label;
};
