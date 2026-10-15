#include "InfoBar.h"

InfoBar::InfoBar(wxWindow* parent) : wxPanel(parent)
{
    m_label = new wxStaticText(this, wxID_ANY, "");

    wxBoxSizer* sizer = new wxBoxSizer(wxHORIZONTAL);
    sizer->Add(m_label, 0, wxALIGN_CENTER_VERTICAL | wxALL, 6);
    SetSizer(sizer);
}

void InfoBar::SetText(const wxString& text)
{
    m_label->SetLabel(text);
    Layout();
}
