#include "pictures.h"
#include "apng.h"
#include "tlb.h"
#include "vace.h"
#include "cmp.h"
#include "dax.h"
#include "geo.h"
#include "c64.h"
#include "ecl.h"
#include "ecl_containers.h"
#include "walls.h"
#include "records.h"
#include "save_map.h"
#include "xmi.h"
#include "disk_image.h"

#include <wx/dcbuffer.h>
#include <wx/dirctrl.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/mstream.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/wx.h>
#include <wx/wfstream.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <limits>
#include <random>
#include <set>
#include <sstream>

namespace {
namespace fs = std::filesystem;

struct ImageExportFormat {
    const char* name;
    const char* extension;
    const char* filter;
    wxBitmapType type;
};

constexpr ImageExportFormat image_export_formats[]{
    {"PNG", "png", "PNG images (*.png)|*.png", wxBITMAP_TYPE_PNG},
    {"JPEG", "jpg", "JPEG images (*.jpg;*.jpeg)|*.jpg;*.jpeg", wxBITMAP_TYPE_JPEG},
    {"BMP", "bmp", "Bitmap images (*.bmp)|*.bmp", wxBITMAP_TYPE_BMP},
};

constexpr unsigned preview_background[]{28, 29, 33};

bool save_image(wxImage image, wxOutputStream& stream, const ImageExportFormat& format)
{
    if (!image.IsOk() || !stream.IsOk())
        return false;
    if (format.type != wxBITMAP_TYPE_PNG) {
        image = image.Copy();
        // JPG and 24-bit BMP use the preview pane's background behind transparent pixels.
        const auto* alpha = image.GetAlpha();
        const bool masked = image.HasMask();
        const unsigned char mask[]{image.GetMaskRed(), image.GetMaskGreen(), image.GetMaskBlue()};
        auto* rgb = image.GetData();
        const auto pixels = static_cast<std::size_t>(image.GetWidth()) * image.GetHeight();
        for (std::size_t i = 0; i < pixels; ++i, rgb += 3) {
            const unsigned opacity = masked && std::equal(mask, mask + 3, rgb) ? 0 : alpha ? alpha[i] : 255;
            for (unsigned channel = 0; channel < 3; ++channel)
                rgb[channel] = static_cast<unsigned char>((rgb[channel] * opacity +
                    preview_background[channel] * (255 - opacity) + 127) / 255);
        }
        if (alpha)
            image.ClearAlpha();
        image.SetMask(false);
        if (format.type == wxBITMAP_TYPE_JPEG)
            image.SetOption(wxIMAGE_OPTION_QUALITY, 95);
    }
    return image.SaveFile(stream, format.type) && stream.IsOk();
}

wxString amiga_map_label(unsigned id)
{
    static const char* const names[]{
        "Civilized Area, New Phlan", "Buccaneer Base", "Cadorna Textile House",
        "Valjevo Castle, North West", "Valjevo Castle, North East", "Valjevo Castle, South East",
        "Valjevo Castle, South West", "Valjevo Castle, Inner Tower", nullptr, "Stojanow Gate",
        "Valhingen Graveyard", nullptr, nullptr, "Kobold Caves", "Kovel Mansion", "Mendor's Library",
        "Lizard Men Keep", "Nomad Camp", "Podal Plaza", nullptr, "Slums", "Sokal Keep",
        "Sorcerer's Island, Level 1", "Sorcerer's Island, Level 2 and 3", "Temple of Bane",
        "Western Wilderness Caves", "Central Wilderness Groves and Ruined Huts", "Eastern Wilderness Caves",
        "Outpost of Zhentil Keep", "Kuto's Well", "Lizard Men Catacombs", "Wealthy Area",
        "Kuto's Well Catacombs"
    };
    if (id < std::size(names) && names[id])
        return wxString::FromUTF8(names[id]);
    return wxString::Format("Map %u", id);
}

class RecordList : public wxListCtrl {
public:
    explicit RecordList(wxWindow* parent)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxSize(220, -1),
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL)
    {
        AppendColumn("ID", wxLIST_FORMAT_LEFT, 62);
        AppendColumn("Name", wxLIST_FORMAT_LEFT, 155);
    }

    void load(std::vector<goldbox::RecordRow> rows)
    {
        SetItemCount(0);
        rows_ = std::move(rows);
        SetItemCount(static_cast<long>(rows_.size()));
        if (!rows_.empty())
            SetItemState(0, wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED,
                         wxLIST_STATE_SELECTED | wxLIST_STATE_FOCUSED);
    }

    const std::vector<goldbox::RecordRow>& rows() const { return rows_; }

private:
    wxString OnGetItemText(long index, long column) const override
    {
        if (index < 0 || static_cast<std::size_t>(index) >= rows_.size())
            return {};
        const auto& row = rows_[index];
        return column ? wxString::FromUTF8(row.name)
                      : wxString::Format("$%02X.%u", row.id, row.slot + 1);
    }
    std::vector<goldbox::RecordRow> rows_;
};

class RecordDetails : public wxListCtrl {
public:
    explicit RecordDetails(wxWindow* parent)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL)
    {
        AppendColumn("Field", wxLIST_FORMAT_LEFT, FromDIP(205));
        AppendColumn("Value", wxLIST_FORMAT_LEFT, FromDIP(220));
    }

    void load(const std::vector<goldbox::Field>* fields, goldbox::RecordKind kind = goldbox::RecordKind::none)
    {
        const bool scripts = kind == goldbox::RecordKind::ecl;
        const bool dialogue = kind == goldbox::RecordKind::luck;
        SetItemCount(0);
        fields_ = fields;
        wxListItem column;
        column.SetText(scripts ? "Address" : "Field");
        SetColumn(0, column);
        column.SetText(scripts ? "Instruction or data" : dialogue ? "Dialogue" : "Value");
        SetColumn(1, column);
        SetColumnWidth(0, FromDIP(scripts || dialogue ? 90 : 205));
        SetColumnWidth(1, FromDIP(scripts ? 800 : dialogue ? 500 : 220));
        if (fields_)
            SetItemCount(static_cast<long>(fields_->size()));
    }

private:
    wxString OnGetItemText(long index, long column) const override
    {
        if (!fields_ || index < 0 || static_cast<std::size_t>(index) >= fields_->size())
            return {};
        const auto& field = (*fields_)[index];
        return wxString::FromUTF8(column ? field.second : field.first);
    }
    const std::vector<goldbox::Field>* fields_ = nullptr;
};

class HexView : public wxListCtrl {
public:
    explicit HexView(wxWindow* parent)
        : wxListCtrl(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                     wxLC_REPORT | wxLC_VIRTUAL | wxLC_SINGLE_SEL)
    {
        SetFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
        AppendColumn("Offset", wxLIST_FORMAT_LEFT, 108);
        AppendColumn("Hex", wxLIST_FORMAT_LEFT, 420);
        AppendColumn("Text", wxLIST_FORMAT_LEFT, 155);
    }

    void open(const fs::path& path)
    {
        clear();
        stream_.open(path, std::ios::binary | std::ios::ate);
        const auto size = stream_.tellg();
        if (size < 0)
            throw std::runtime_error("Unable to open file");
        set_size(static_cast<std::uintmax_t>(size));
    }

    void open(const std::vector<std::uint8_t>& bytes)
    {
        clear();
        bytes_ = &bytes;
        set_size(bytes.size());
    }

    void set_size(std::uintmax_t size)
    {
        size_ = size;
        const auto rows = size_ / 16 + (size_ % 16 != 0);
        if (rows > static_cast<std::uintmax_t>(std::numeric_limits<long>::max()))
            throw std::runtime_error("File is too large for the hex viewer");
        SetItemCount(static_cast<long>(rows));
        if (rows)
            EnsureVisible(0);
    }

    void clear()
    {
        SetItemCount(0);
        stream_.close();
        stream_.clear();
        bytes_ = nullptr;
        size_ = 0;
    }

    std::uintmax_t size() const { return size_; }

private:
    wxString OnGetItemText(long row, long column) const override
    {
        const auto offset = static_cast<std::uintmax_t>(row) * 16;
        if (column == 0)
            return wxString::Format("%08llX", static_cast<unsigned long long>(offset));

        if (offset >= size_)
            return {};
        char bytes[16];
        auto count = static_cast<std::size_t>(std::min<std::uintmax_t>(16, size_ - offset));
        if (bytes_) {
            std::copy_n(bytes_->data() + offset, count, bytes);
        } else {
            stream_.clear();
            stream_.seekg(static_cast<std::streamoff>(offset));
            stream_.read(bytes, static_cast<std::streamsize>(count));
            count = static_cast<std::size_t>(stream_.gcount());
        }
        wxString text;
        for (std::size_t i = 0; i < count; ++i) {
            const auto byte = static_cast<unsigned char>(bytes[i]);
            if (column == 1) {
                if (i)
                    text += ' ';
                text += wxString::Format("%02X", static_cast<unsigned>(byte));
            } else {
                text += static_cast<wxChar>(byte >= 32 && byte <= 126 ? byte : '.');
            }
        }
        return text;
    }

    mutable std::ifstream stream_;
    const std::vector<std::uint8_t>* bytes_ = nullptr;
    std::uintmax_t size_ = 0;
};

wxImage frame_image(const goldbox::Frame& frame)
{
    wxImage image(frame.width, frame.height);
    image.InitAlpha();
    auto* rgb = image.GetData();
    auto* alpha = image.GetAlpha();
    for (std::size_t i = 0; i < frame.rgba.size() / 4; ++i) {
        std::copy_n(frame.rgba.data() + i * 4, 3, rgb + i * 3);
        alpha[i] = frame.rgba[i * 4 + 3];
    }
    return image;
}

class Preview : public wxScrolledWindow {
public:
    explicit Preview(wxWindow* parent) : wxScrolledWindow(parent)
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetScrollRate(12, 12);
        Bind(wxEVT_PAINT, [this](wxPaintEvent&) {
            wxAutoBufferedPaintDC dc(this);
            PrepareDC(dc);
            dc.SetBackground(wxBrush(wxColour(preview_background[0], preview_background[1], preview_background[2])));
            dc.Clear();
            if (bitmap_.IsOk()) {
                const auto size = GetClientSize();
                const auto x = fit_ ? (size.x - bitmap_.GetWidth()) / 2 : 12;
                const auto y = fit_ ? (size.y - bitmap_.GetHeight()) / 2 : 12;
                dc.DrawBitmap(bitmap_, x, y, true);
            }
        });
        Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
            if (fit_) {
                scale_to_fit();
                Refresh();
            }
            event.Skip();
        });
    }

    void show(const goldbox::Frame* frame, int zoom, const wxString& filename = {})
    {
        native_image_ = wxNullImage;
        map_image_ = {};
        filename_ = filename;
        bitmap_ = wxNullBitmap;
        fit_ = frame && zoom == 0;
        if (frame) {
            native_image_ = frame_image(*frame);
            if (fit_) {
                Scroll(0, 0);
                scale_to_fit();
            } else {
                bitmap_ = wxBitmap(zoom == 1 ? native_image_ : native_image_.Scale(
                    frame->width * zoom, frame->height * zoom, wxIMAGE_QUALITY_NEAREST));
            }
        }
        SetVirtualSize(!fit_ && bitmap_.IsOk() ? wxSize(bitmap_.GetWidth() + 24, bitmap_.GetHeight() + 24)
                                               : wxSize(0, 0));
        Refresh();
    }

    void show_map(const goldbox::GeoMap& map, int zoom, int cursor_x, int cursor_y, int facing,
                  const wxString& filename,
                  const goldbox::MapMask* visited = nullptr,
                  const goldbox::MapMask* changed = nullptr)
    {
        filename_ = filename;
        fit_ = false;
        native_image_ = wxNullImage;
        if (zoom == 1) {
            map_image_ = {};
        } else {
            using Mask = std::optional<goldbox::MapMask>;
            map_image_ = [map, cursor_x, cursor_y, facing,
                          seen = visited ? Mask(*visited) : Mask{},
                          diff = changed ? Mask(*changed) : Mask{}] {
                return draw_map(map, 1, cursor_x, cursor_y, facing,
                                seen ? &*seen : nullptr, diff ? &*diff : nullptr).ConvertToImage();
            };
        }
        bitmap_ = draw_map(map, zoom, cursor_x, cursor_y, facing, visited, changed);
        SetVirtualSize(bitmap_.GetWidth() + 24, bitmap_.GetHeight() + 24);
        Refresh();
    }

    bool can_export() const { return IsShownOnScreen() && bitmap_.IsOk(); }
    const wxString& filename() const { return filename_; }
    wxImage export_image() const
    {
        if (native_image_.IsOk())
            return native_image_;
        return map_image_ ? map_image_() : bitmap_.ConvertToImage();
    }

private:
    void scale_to_fit()
    {
        const auto size = GetClientSize();
        const int width = native_image_.GetWidth();
        const int height = native_image_.GetHeight();
        const double scale = std::min(std::max(1, size.x - 24) / double(width),
                                      std::max(1, size.y - 24) / double(height));
        bitmap_ = wxBitmap(native_image_.Scale(std::max(1, int(width * scale)),
                                               std::max(1, int(height * scale)), wxIMAGE_QUALITY_NEAREST));
    }

    static wxBitmap draw_map(const goldbox::GeoMap& map, int zoom, int cursor_x, int cursor_y,
                             int facing, const goldbox::MapMask* visited,
                             const goldbox::MapMask* changed)
    {
        constexpr int side = goldbox::map_side;
        const int cell = 12 * zoom;
        const int edge = (side + 1) * cell;
        const int wall = std::max(1, cell / 12);
        wxBitmap bitmap(edge + 1, edge + 1);
        wxMemoryDC dc(bitmap);
        dc.SetBackground(wxBrush(wxColour(239, 239, 239)));
        dc.Clear();
        dc.SetFont(wxFontInfo(std::max(6, cell / 3)).Family(wxFONTFAMILY_TELETYPE));
        dc.SetTextForeground(wxColour(85, 85, 85));
        for (int axis = 0; axis < side; ++axis) {
            const auto label = wxString::Format("%d", axis);
            const auto text = dc.GetTextExtent(label);
            const int dx = (cell - text.x) / 2;
            const int dy = (cell - text.y) / 2;
            dc.DrawText(label, (axis + 1) * cell + dx, dy);
            dc.DrawText(label, dx, (axis + 1) * cell + dy);
        }

        const wxBrush floor(wxColour(85, 85, 85));
        const wxBrush event_floor(wxColour(34, 139, 34));
        const wxBrush visited_floor(wxColour(96, 128, 154));
        const wxBrush visited_event(wxColour(84, 174, 107));
        const wxBrush walls(wxColour(170, 170, 170));
        const std::array<wxBrush, 4> doors{*wxTRANSPARENT_BRUSH, *wxWHITE_BRUSH,
                                          wxBrush(wxColour(255, 255, 0)), *wxRED_BRUSH};
        dc.SetPen(*wxTRANSPARENT_PEN);
        for (int row = 0; row < side; ++row) {
            for (int col = 0; col < side; ++col) {
                const auto index = row * side + col;
                const auto& square = map[index];
                const int x = (col + 1) * cell;
                const int y = (row + 1) * cell;
                const bool has_event = square.event || square.event_flag;
                const bool seen = visited && visited->test(index);
                dc.SetBrush(has_event ? (seen ? visited_event : event_floor)
                                      : (seen ? visited_floor : floor));
                dc.DrawRectangle(x, y, cell, cell);
                if (square.event) {
                    dc.SetTextForeground(*wxBLACK);
                    dc.DrawText(wxString::Format("%u", square.event), x + wall, y + wall);
                }

                auto paint_edges = [&](const auto& types, int thickness, int inset, bool door) {
                    const int length = cell - 2 * inset;
                    const wxRect edges[] = {{x + inset, y, length, thickness},
                        {x + cell - thickness, y + inset, thickness, length},
                        {x + inset, y + cell - thickness, length, thickness},
                        {x, y + inset, thickness, length}};
                    for (unsigned dir = 0; dir < 4; ++dir) {
                        if (types[dir]) {
                            dc.SetBrush(door ? doors[types[dir]] : walls);
                            dc.DrawRectangle(edges[dir]);
                        }
                    }
                };
                paint_edges(square.walls, wall, 0, false);
                paint_edges(square.doors, 2 * wall, 2 * wall, true);
                if (changed && changed->test(index)) {
                    dc.SetPen(wxPen(wxColour(255, 160, 40), 2));
                    dc.SetBrush(*wxTRANSPARENT_BRUSH);
                    dc.DrawRectangle(x + 2, y + 2, cell - 4, cell - 4);
                    dc.SetPen(*wxTRANSPARENT_PEN);
                }
            }
        }
        dc.SetPen(wxPen(wxColour(32, 32, 32), 1, wxPENSTYLE_DOT));
        for (int i = 1; i <= side + 1; ++i) {
            dc.DrawLine(cell, i * cell, edge, i * cell);
            dc.DrawLine(i * cell, cell, i * cell, edge);
        }
        if (facing >= 0) {
            const int x = (cursor_x + 1) * cell + cell / 2;
            const int y = (cursor_y + 1) * cell + cell / 2;
            constexpr int dx[] = {0, 1, 0, -1};
            constexpr int dy[] = {-1, 0, 1, 0};
            const int span = cell / 3;
            const int side_x = dx[(facing + 1) % 4] * span;
            const int side_y = dy[(facing + 1) % 4] * span;
            dc.SetPen(wxPen(*wxRED, 2));
            dc.DrawLine(x + side_x, y + side_y, x + dx[facing] * span, y + dy[facing] * span);
            dc.DrawLine(x - side_x, y - side_y, x + dx[facing] * span, y + dy[facing] * span);
        }
        dc.SelectObject(wxNullBitmap);
        return bitmap;
    }

    wxBitmap bitmap_;
    wxImage native_image_;
    std::function<wxImage()> map_image_;
    wxString filename_;
    bool fit_ = false;
};

class Gallery : public wxFrame {
public:
    explicit Gallery(const fs::path& initial)
        : wxFrame(nullptr, wxID_ANY, "Gold Box Explorer", wxDefaultPosition, wxSize(1240, 600)),
          playback_timer_(this)
    {
        auto* panel = new wxPanel(this);
        auto* layout = new wxBoxSizer(wxVERTICAL);
        const auto folder = initial.empty() ? fs::current_path()
                          : fs::is_directory(initial) ? initial : fs::absolute(initial).parent_path();

        auto* columns = make_splitter(panel);
        auto* file_split = make_splitter(columns);
        picture_split_ = make_splitter(file_split);
        auto* content = new wxPanel(picture_split_);
        auto* content_layout = new wxBoxSizer(wxVERTICAL);
        auto* folders = new wxGenericDirCtrl(columns, wxID_ANY,
            wxString::FromUTF8(folder.u8string()), wxDefaultPosition,
            wxSize(220, -1), wxDIRCTRL_DIR_ONLY);
        files_ = new wxListBox(file_split, wxID_ANY);
        pictures_ = new wxListBox(picture_split_, wxID_ANY);
        pictures_->Hide();
        preview_split_ = make_splitter(content);
        preview_split_->SetSashGravity(1.0);
        preview_ = new Preview(preview_split_);
        perspective_ = new Preview(preview_split_);
        perspective_->Hide();
        preview_split_->Initialize(preview_);
        hex_ = new HexView(content);
        hex_->Hide();
        tables_ = make_splitter(content);
        record_list_ = new RecordList(tables_);
        details_ = new RecordDetails(tables_);
        tables_->SplitVertically(record_list_, details_, FromDIP(245));
        tables_->Hide();
        content_layout->Add(preview_split_, 1, wxEXPAND);
        content_layout->Add(hex_, 1, wxEXPAND);
        content_layout->Add(tables_, 1, wxEXPAND);
        content->SetSizer(content_layout);
        picture_split_->Initialize(content);
        file_split->SplitVertically(files_, picture_split_, FromDIP(165));
        columns->SplitVertically(folders, file_split, FromDIP(220));
        layout->Add(columns, 1, wxEXPAND | wxTOP | wxLEFT | wxRIGHT, 8);

        zoom_ = new wxChoice(panel, wxID_ANY);
        for (int i = 1; i <= 4; ++i)
            zoom_->Append(wxString::Format("%dx", i));
        zoom_->SetSelection(1);
        auto* controls = new wxBoxSizer(wxHORIZONTAL);
        controls->Add(zoom_, 0, wxRIGHT, 8);
        play_ = new wxButton(panel, wxID_ANY, "Play");
        play_->Hide();
        controls->Add(play_, 0, wxRIGHT, 8);
        facing_ = new wxChoice(panel, wxID_ANY);
        for (const auto* direction : {"North", "East", "South", "West"})
            facing_->Append(direction);
        facing_->SetSelection(0);
        facing_->Hide();
        controls->Add(facing_, 0);
        original_map_ = new wxCheckBox(panel, wxID_ANY, "Show original map");
        original_map_->Hide();
        controls->Add(original_map_, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
        layout->Add(controls, 0, wxALL, 8);
        panel->SetSizer(layout);
        CreateStatusBar();

        auto* file_menu = new wxMenu;
        file_menu->Append(wxID_OPEN, "&Open Disk Image...\tCtrl+O");
        file_menu->AppendSeparator();
        auto* image_menu = new wxMenu;
        auto* frames_menu = new wxMenu;
        for (unsigned i = 0; i < std::size(image_export_formats); ++i) {
            const auto& format = image_export_formats[i];
            if (!wxImage::FindHandler(format.type))
                continue;
            const auto label = wxString::FromUTF8(format.name) + "...";
            image_menu->Append(export_image_id + i, label);
            frames_menu->Append(export_frames_id + i, label);
            Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
                event.Enable(preview_->can_export());
            }, export_image_id + i);
            Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
                event.Enable(can_export_frames());
            }, export_frames_id + i);
            Bind(wxEVT_MENU, [this, i](wxCommandEvent&) {
                export_image(*preview_, image_export_formats[i]);
            }, export_image_id + i);
            Bind(wxEVT_MENU, [this, i](wxCommandEvent&) {
                export_frames(image_export_formats[i]);
            }, export_frames_id + i);
        }
        file_menu->AppendSubMenu(image_menu, "Export Image");
        file_menu->AppendSubMenu(frames_menu, "Export All Frames");
        file_menu->Append(export_animation_id, "Export Animation...");
        file_menu->AppendSeparator();
        file_menu->Append(export_script_id, "Export Selected Script...");
        file_menu->Append(export_scripts_id, "Export All Scripts...");
        file_menu->Append(export_sample_id, "Export Selected Sample as WAV...");
        file_menu->Append(export_midi_id, "Export Selected Sequence as MIDI...");
        file_menu->AppendSeparator();
        file_menu->Append(wxID_EXIT, "E&xit");
        auto* menu_bar = new wxMenuBar;
        menu_bar->Append(file_menu, "&File");
        auto* help_menu = new wxMenu;
        help_menu->Append(wxID_ABOUT, "&About Gold Box Explorer...");
        menu_bar->Append(help_menu, "&Help");
        SetMenuBar(menu_bar);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { show_about(); }, wxID_ABOUT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) {
            wxFileDialog dialog(this, "Open disk image", {}, {},
                "Amiga and Commodore disk images (*.adf;*.d64)|*.adf;*.d64;*.ADF;*.D64",
                wxFD_OPEN | wxFD_FILE_MUST_EXIST);
            if (dialog.ShowModal() == wxID_OK)
                mount_image(fs::u8path(dialog.GetPath().ToUTF8().data()));
        }, wxID_OPEN);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(selected_clip() != nullptr);
        }, export_animation_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_animation(); }, export_animation_id);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(selected_record(goldbox::RecordKind::ecl) >= 0);
        }, export_script_id);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(record_kind_ == goldbox::RecordKind::ecl && !record_list_->rows().empty());
        }, export_scripts_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_script_listing(false); }, export_script_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_script_listing(true); }, export_scripts_id);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(selected_record(goldbox::RecordKind::sound_samples) >= 0);
        }, export_sample_id);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(selected_record(goldbox::RecordKind::xmi) >= 0);
        }, export_midi_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_audio(false); }, export_sample_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_audio(true); }, export_midi_id);
        for (auto* pane : {preview_, perspective_}) {
            pane->Bind(wxEVT_CONTEXT_MENU, [this, pane](wxContextMenuEvent&) {
                if (!pane->can_export())
                    return;
                wxMenu menu;
                for (unsigned i = 0; i < std::size(image_export_formats); ++i) {
                    const auto& format = image_export_formats[i];
                    if (!wxImage::FindHandler(format.type))
                        continue;
                    menu.Append(export_image_id + i, "Export " + wxString::FromUTF8(format.name) + "...");
                    menu.Bind(wxEVT_MENU, [this, pane, i](wxCommandEvent&) {
                        export_image(*pane, image_export_formats[i]);
                    }, export_image_id + i);
                }
                pane->PopupMenu(&menu);
            });
        }

        folders->Bind(wxEVT_DIRCTRL_SELECTIONCHANGED, [this, folders](wxTreeEvent& event) {
            const auto path = folders->GetPath(event.GetItem());
            if (!path.empty()) {
                active_image_.clear();
                browse(fs::u8path(path.ToUTF8().data()), {});
            }
        });
        files_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { open(); });
        files_->Bind(wxEVT_LISTBOX_DCLICK, [this](wxCommandEvent&) {
            const auto selected = files_->GetSelection();
            if (selected == wxNOT_FOUND)
                return;
            const auto path = paths_.at(selected);
            if (!active_image_.empty() && selected == 0) {
                if (current_folder_ == mounted_images_.at(active_image_)) {
                    const auto image = active_image_;
                    active_image_.clear();
                    browse(image.parent_path(), image.filename());
                } else {
                    browse(current_folder_.parent_path(), current_folder_.filename());
                }
            } else {
                std::error_code error;
                if (fs::is_directory(path, error))
                    browse(path, {});
                else if (goldbox::is_disk_image(path))
                    mount_image(path);
            }
        });
        pictures_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
            reset_playback();
            preview_->Scroll(0, 0);
            render();
        });
        play_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { toggle_playback(); });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            const auto* clip = selected_clip();
            if (active_ && clip) {
                if (clip->realtime) {
                    const auto now = PlaybackClock::now();
                    while (step_ + 1 < clip->steps.size() && now >= playback_deadline_) {
                        ++step_;
                        playback_deadline_ += std::chrono::milliseconds(clip->steps[step_].duration_ms);
                    }
                    if (now < playback_deadline_) {
                        show_playback_step(*clip);
                        return;
                    }
                }
                if (step_ + 1 == clip->steps.size()) {
                    reset_playback();
                    render();
                } else {
                    ++step_;
                    show_playback_step(*clip);
                }
            }
        }, playback_timer_.GetId());
        zoom_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { preview_->Scroll(0, 0); render(); });
        facing_->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { render(); });
        original_map_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) { render(); });
        record_list_->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& event) {
            if (event.GetIndex() >= 0)
                show_fields(static_cast<std::size_t>(event.GetIndex()));
        });
        preview_->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
            if (!geo_ || banks_.empty() || !perspective_->IsShown())
                return;
            const auto p = preview_->CalcUnscrolledPosition(event.GetPosition()) - wxPoint(12, 12);
            const int cell = 12 * (zoom_->GetSelection() + 1);
            if (p.x >= cell && p.y >= cell && p.x < 17 * cell && p.y < 17 * cell) {
                cursor_x_ = p.x / cell - 1;
                cursor_y_ = p.y / cell - 1;
                render();
            }
        });
        browse(folder, initial.filename());
        if (goldbox::is_disk_image(initial))
            mount_image(initial);
        CallAfter([folders] {
            auto* tree = folders->GetTreeCtrl();
            const auto selected = tree->GetSelection();
            if (selected.IsOk())
                tree->EnsureVisible(selected);
        });
    }

    ~Gallery() override
    {
        if (!disk_cache_.empty()) {
            std::error_code error;
            fs::remove_all(disk_cache_, error);
        }
    }

private:
    void show_about()
    {
        static constexpr unsigned char logo[]{
#include "about_logo.inc"
        };
        wxMemoryInputStream logo_stream(logo, sizeof(logo));
        wxImage artwork(logo_stream, wxBITMAP_TYPE_PNG);
        wxDialog dialog(this, wxID_ANY, "About Gold Box Explorer");
        auto* content = new wxBoxSizer(wxHORIZONTAL);
        content->Add(new wxStaticBitmap(&dialog, wxID_ANY, wxBitmap(artwork)),
                     0, wxALL, dialog.FromDIP(9));

        auto* details = new wxBoxSizer(wxVERTICAL);
        details->Add(new wxStaticText(&dialog, wxID_ANY, "Gold Box Explorer"), 0, wxBOTTOM, dialog.FromDIP(8));
        details->Add(new wxStaticText(&dialog, wxID_ANY, "Version " GOLDBOX_VERSION),
                     0, wxBOTTOM, dialog.FromDIP(8));
        details->Add(new wxStaticText(&dialog, wxID_ANY,
                     wxString::FromUTF8("Copyright\xc2\xa9 Bil Simser 2010-2013\nCopyright\xc2\xa9 Marainein 2013-2015")),
                     0, wxBOTTOM, dialog.FromDIP(8));
        details->Add(new wxTextCtrl(&dialog, wxID_ANY,
                     "A file explorer-like interface for viewing Gold Box game files from the 1990s. "
                     "Kudos to Simeon Pilgrim for all his hard work on decrypting many files "
                     "and reprogramming the DOS version of Curse of the Azure Bonds.",
                     wxDefaultPosition, dialog.FromDIP(wxSize(270, 120)), wxTE_MULTILINE | wxTE_READONLY),
                     1, wxEXPAND | wxBOTTOM, dialog.FromDIP(8));
        auto* ok = new wxButton(&dialog, wxID_OK, "&OK");
        details->Add(ok, 0, wxALIGN_RIGHT);
        ok->SetDefault();
        content->Add(details, 1, wxEXPAND | wxALL, dialog.FromDIP(9));
        dialog.SetSizerAndFit(content);
        dialog.CentreOnParent();
        dialog.ShowModal();
    }

    static wxSplitterWindow* make_splitter(wxWindow* parent)
    {
        auto* splitter = new wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                               wxSP_3DSASH | wxSP_LIVE_UPDATE);
        splitter->SetMinimumPaneSize(splitter->FromDIP(60));
        return splitter;
    }

    static constexpr int export_image_id = wxID_HIGHEST + 8;
    static constexpr int export_frames_id = export_image_id + std::size(image_export_formats);
    static constexpr int export_animation_id = wxID_HIGHEST + 3;
    static constexpr int export_script_id = wxID_HIGHEST + 4;
    static constexpr int export_scripts_id = wxID_HIGHEST + 5;
    static constexpr int export_sample_id = wxID_HIGHEST + 6;
    static constexpr int export_midi_id = wxID_HIGHEST + 7;

    wxString export_name(const wxString& suffix, const char* extension = "png") const
    {
        return wxString::FromUTF8(paths_.at(files_->GetSelection()).stem().u8string()) + suffix + "." + wxString::FromUTF8(extension);
    }

    static wxString frame_suffix(const goldbox::Frame& frame)
    {
        return wxString::Format("-%02X-frame-%02u", frame.id, frame.index + 1);
    }

    bool can_export_frames() const
    {
        const auto selected = files_->GetSelection();
        return !frames_.empty() && selected != wxNOT_FOUND &&
               (goldbox::is_picture_archive(paths_.at(selected)) ||
                goldbox::is_tlb_archive(paths_.at(selected)) ||
                goldbox::is_geo_glb(paths_.at(selected)) ||
                goldbox::is_cmp_archive(paths_.at(selected)) ||
                goldbox::is_anm_archive(paths_.at(selected)) ||
                goldbox::is_packwall_archive(paths_.at(selected)) ||
                goldbox::is_vace_archive(paths_.at(selected)));
    }

    void export_frames(const ImageExportFormat& format)
    {
        if (!can_export_frames())
            return;
        const auto& archive = paths_.at(files_->GetSelection());
        const auto folder = active_image_.empty() ? archive.parent_path() : active_image_.parent_path();
        wxDirDialog dialog(this, "Choose a folder for the " + wxString::FromUTF8(format.name) + " frames",
                           wxString::FromUTF8(folder.u8string()));
        if (dialog.ShowModal() != wxID_OK)
            return;

        std::set<wxString> names;
        std::vector<wxString> targets;
        const auto count = static_cast<std::size_t>(pictures_->GetCount());
        targets.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& frame = frames_[i];
            const auto suffix = frame_suffix(frame);
            auto name = export_name(suffix, format.extension);
            for (unsigned copy = 2; !names.insert(name).second; ++copy)
                name = export_name(suffix + wxString::Format("-%u", copy), format.extension);
            auto path = wxFileName(dialog.GetPath(), name).GetFullPath();
            if (wxFileName::Exists(path, wxFILE_EXISTS_ANY | wxFILE_EXISTS_NO_FOLLOW)) {
                wxMessageBox("The destination already contains " + name +
                             ". Choose a different folder.", "Export All Frames",
                             wxOK | wxICON_WARNING, this);
                return;
            }
            targets.push_back(std::move(path));
        }

        for (std::size_t i = 0; i < count; ++i) {
            wxFile file;
            bool saved = false;
            if (file.Create(targets[i], false)) {
                {
                    wxFileOutputStream stream(file);
                    saved = save_image(frame_image(frames_[i]), stream, format);
                }
                saved = file.Close() && saved;
                if (!saved)
                    wxRemoveFile(targets[i]);
            }
            if (!saved) {
                wxMessageBox(wxString::Format("Export stopped after saving %zu of %zu frames.\n", i,
                                              count) + targets[i],
                             "Export All Frames", wxOK | wxICON_ERROR, this);
                return;
            }
        }
        SetStatusText(wxString::Format("Exported %zu %s frames", count, wxString::FromUTF8(format.name)));
    }

    void export_animation()
    {
        const auto* clip = selected_clip();
        if (!clip)
            return;
        const auto name = export_name(clip->whole_archive ? wxString("-animation")
                                                         : wxString::Format("-%02X-animation", clip->id));
        wxFileDialog dialog(this, "Export Animation", {}, name,
                            "Animated PNG (*.png)|*.png", wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK)
            return;
        wxTempFileOutputStream output(dialog.GetPath());
        if (!output.IsOk() || !goldbox::write_apng(output, frames_, *clip) || !output.Commit()) {
            wxMessageBox("Could not save the animated PNG.", "Export Animation",
                         wxOK | wxICON_ERROR, this);
            return;
        }
        SetStatusText("Exported animated PNG");
    }

    void export_image(const Preview& pane, const ImageExportFormat& format)
    {
        if (!pane.can_export())
            return;
        // Snapshot the frame before the modal dialog can tick the playback timer.
        auto image = pane.export_image();
        wxFileName filename(pane.filename());
        filename.SetExt(wxString::FromUTF8(format.extension));
        const auto title = "Export " + wxString::FromUTF8(format.name);
        wxFileDialog dialog(this, title, {}, filename.GetFullName(), wxString::FromUTF8(format.filter),
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK)
            return;
        wxTempFileOutputStream output(dialog.GetPath());
        if (!save_image(std::move(image), output, format) || !output.Commit())
            wxMessageBox("Could not save the image.", title, wxOK | wxICON_ERROR, this);
    }

    long selected_record(goldbox::RecordKind kind) const
    {
        if (record_kind_ != kind)
            return -1;
        return record_list_->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    }

    template<typename MakeContent>
    bool save_export(const fs::path& source, const wxString& title, const wxString& name,
                     const wxString& filter, MakeContent make_content)
    {
        wxFileDialog dialog(this, title, {}, name, filter, wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() != wxID_OK)
            return false;
        try {
            std::error_code ec;
            const auto destination = fs::u8path(dialog.GetPath().ToUTF8().data());
            if (fs::equivalent(source, destination, ec))
                throw std::runtime_error("Choose a destination other than the original file.");
            const auto content = make_content();
            wxTempFileOutputStream output(dialog.GetPath());
            if (!output.IsOk() || output.Write(content.data(), content.size()).LastWrite() != content.size() ||
                !output.Commit())
                throw std::runtime_error("Could not save the export file.");
            return true;
        } catch (const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()), title, wxOK | wxICON_ERROR, this);
            return false;
        }
    }

    void export_script_listing(bool all)
    {
        const auto archive_index = files_->GetSelection();
        const auto selected = selected_record(goldbox::RecordKind::ecl);
        const auto& scripts = record_list_->rows();
        if (record_kind_ != goldbox::RecordKind::ecl || archive_index == wxNOT_FOUND ||
            scripts.empty() || (!all && (selected < 0 || static_cast<std::size_t>(selected) >= scripts.size())))
            return;

        const auto archive = paths_.at(archive_index);
        auto name = wxString::FromUTF8(archive.stem().u8string());
        const auto count = all ? scripts.size() : 1;
        std::ostringstream listing;
        listing << archive.filename().u8string() << '\n' << '\n';
        if (all) {
            name += "-scripts.txt";
            for (std::size_t i = 0; i < scripts.size(); ++i) {
                if (i)
                    listing << '\n';
                goldbox::write_ecl_listing(listing, scripts[i]);
            }
        } else {
            const auto& script = scripts[static_cast<std::size_t>(selected)];
            name += wxString::Format("-script-%02X.txt", script.id);
            goldbox::write_ecl_listing(listing, script);
        }
        const auto title = all ? "Export All Scripts" : "Export Selected Script";
        if (save_export(archive, title, name, "Text files (*.txt)|*.txt",
                        [&listing] { return listing.str(); }))
            SetStatusText(wxString::Format("Exported %zu %s", count, count == 1 ? "script" : "scripts"));
    }

    void export_audio(bool midi)
    {
        const auto archive_index = files_->GetSelection();
        const auto selected = selected_record(midi ? goldbox::RecordKind::xmi : goldbox::RecordKind::sound_samples);
        const auto& records = record_list_->rows();
        if (archive_index == wxNOT_FOUND || selected < 0 || static_cast<std::size_t>(selected) >= records.size())
            return;
        const auto archive = paths_.at(archive_index);
        const auto id = records[static_cast<std::size_t>(selected)].id;
        const auto suffix = midi ? wxString::Format("-sequence-%02u.mid", id + 1) :
                                   wxString::Format("-sample-%02u.wav", id);
        const auto name = wxString::FromUTF8(archive.stem().u8string()) + suffix;
        const auto title = midi ? "Export Selected Sequence" : "Export Selected Sample";
        const auto filter = midi ? "MIDI files (*.mid)|*.mid" : "WAV audio (*.wav)|*.wav";
        if (save_export(archive, title, name, filter, [&] {
                return midi ? goldbox::xmi_sequence_midi(archive, id) : goldbox::sample_wav(archive, id);
            }))
            SetStatusText(midi ? wxString::Format("Exported sequence %u as MIDI", id + 1) :
                                 wxString::Format("Exported sample %u as WAV", id));
    }

    void mount_image(const fs::path& path)
    {
        try {
            const auto image = fs::canonical(path);
            auto cached = mounted_images_.find(image);
            if (cached == mounted_images_.end()) {
                if (disk_cache_.empty()) {
                    const auto temp = fs::temp_directory_path();
                    std::random_device random;
                    for (unsigned attempt = 0; attempt < 16 && disk_cache_.empty(); ++attempt) {
                        const auto candidate = temp / ("goldbox-disks-" + std::to_string(random()));
                        if (fs::create_directory(candidate))
                            disk_cache_ = candidate;
                    }
                    if (disk_cache_.empty())
                        throw std::runtime_error("Could not create a temporary disk cache");
                }
                const auto directory = disk_cache_ / std::to_string(mounted_images_.size());
                fs::create_directory(directory);
                try {
                    goldbox::extract_disk_image(image, directory);
                    cached = mounted_images_.emplace(image, directory).first;
                } catch (...) {
                    std::error_code error;
                    fs::remove_all(directory, error);
                    throw;
                }
            }
            active_image_ = image;
            const auto directory = cached->second;
            browse(directory, {});
        } catch (const std::exception& error) {
            wxMessageBox(wxString::FromUTF8(error.what()), "Could not open disk image", wxOK | wxICON_ERROR, this);
        }
    }

    void browse(const fs::path& folder, const fs::path& selected)
    {
        current_folder_ = folder;
        paths_.clear();
        files_->Clear();
        if (active_image_.empty()) {
            SetTitle("Gold Box Explorer");
        } else {
            const auto& root = mounted_images_.at(active_image_);
            const auto location = folder.lexically_relative(root);
            auto title = wxString::FromUTF8(active_image_.filename().u8string());
            if (location != ".")
                title += " / " + wxString::FromUTF8(location.generic_u8string());
            SetTitle("Gold Box Explorer - " + title);
        }
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            std::error_code file_error;
            if (it->is_regular_file(file_error) || (!active_image_.empty() && it->is_directory(file_error)))
                paths_.push_back(it->path());
        }
        std::sort(paths_.begin(), paths_.end());
        int index = active_image_.empty() ? 0 : 1;
        if (!active_image_.empty())
            files_->Append("[..]");
        for (const auto& path : paths_) {
            auto label = wxString::FromUTF8(path.filename().u8string());
            std::error_code type_error;
            if (fs::is_directory(path, type_error) || goldbox::is_disk_image(path))
                label = "[" + label + "]";
            const auto row = files_->Append(label);
            if (path.filename() == selected)
                index = row;
        }
        if (!active_image_.empty())
            paths_.insert(paths_.begin(), current_folder_.parent_path());
        if (!paths_.empty())
            files_->SetSelection(std::min<int>(index, paths_.size() - 1));
        open();
        if (error)
            SetStatusText(wxString::FromUTF8(error.message()));
    }

    void open()
    {
        reset_playback();
        clips_.clear();
        pictures_->Clear();
        frames_.clear();
        hex_->clear();
        records_.clear();
        details_->load(nullptr);
        record_list_->load({});
        record_kind_ = goldbox::RecordKind::none;
        banks_.clear();
        preview_->Scroll(0, 0);
        geo_ = amiga_geo_ = false;
        saved_map_.reset();
        original_map_->SetValue(false);
        const auto selected = files_->GetSelection();
        if (selected != wxNOT_FOUND) {
            const auto& path = paths_.at(selected);
            std::error_code error;
            if ((!active_image_.empty() && selected == 0) || fs::is_directory(path, error) ||
                goldbox::is_disk_image(path)) {
                show_view(false, false);
                SetStatusText("Double-click to open.");
                render();
                return;
            }
        }
        const bool wall = selected != wxNOT_FOUND && goldbox::is_wall_archive(paths_.at(selected));
        const bool tlb = selected != wxNOT_FOUND && goldbox::is_tlb_archive(paths_.at(selected));
        const bool glb = selected != wxNOT_FOUND && goldbox::is_geo_glb(paths_.at(selected));
        const bool cmp = selected != wxNOT_FOUND && goldbox::is_cmp_archive(paths_.at(selected));
        const bool anm = selected != wxNOT_FOUND && goldbox::is_anm_archive(paths_.at(selected));
        const bool packwall = selected != wxNOT_FOUND && goldbox::is_packwall_archive(paths_.at(selected));
        const bool vace = selected != wxNOT_FOUND && goldbox::is_vace_archive(paths_.at(selected));
        const bool c64 = selected != wxNOT_FOUND && goldbox::is_c64_picture(paths_.at(selected));
        const bool picture = selected != wxNOT_FOUND &&
            (wall || tlb || glb || cmp || anm || packwall || vace || c64 || goldbox::is_picture_archive(paths_.at(selected)));
        show_view(picture, selected != wxNOT_FOUND && !picture);
        SetStatusText("Choose a file to inspect.");
        if (selected != wxNOT_FOUND) {
            const auto& path = paths_.at(selected);
            const bool archive_file = wxString::FromUTF8(path.extension().u8string()).CmpNoCase(".dax") == 0;
            const auto kind = goldbox::record_kind(path);
            const bool world = goldbox::is_saved_world(path);
            try {
                if (!picture) {
                    if (world) {
                        saved_map_ = goldbox::read_saved_map(path);
                        if (saved_map_->original || saved_map_->copied) {
                            render();
                        } else {
                            const auto& save = *saved_map_;
                            goldbox::RecordRow row{save.map_id, 0, path.filename().u8string(), {
                                {"Disk", std::to_string(save.disk)},
                                {"Script", std::to_string(save.script)},
                                {"Map ID", std::to_string(save.map_id)},
                                {"Location type", std::to_string(save.location)},
                                {"Position", std::to_string(save.x) + ", " + std::to_string(save.y)},
                                {"Facing", facing_->GetString(save.facing).ToStdString()},
                                {"Map preview", save.location > 1 ? "Wilderness" : "Matching GEO data unavailable"}
                            }};
                            saved_map_.reset();
                            record_list_->load({std::move(row)});
                            show_view(false, false, false, false, true);
                            show_fields(0);
                        }
                        return;
                    }
                    if (kind != goldbox::RecordKind::none) {
                        auto rows = goldbox::read_record_rows(path);
                        if (!rows.empty()) {
                            record_kind_ = kind;
                            record_list_->load(std::move(rows));
                            show_view(false, false, false, false, true);
                            show_fields(0);
                            return;
                        }
                    }
                    if (archive_file) {
                        geo_ = goldbox::is_geo_archive(path);
                        if (geo_) {
                            records_ = goldbox::try_amiga_dax(path);
                            amiga_geo_ = !records_.empty();
                        }
                        if (!amiga_geo_)
                            records_ = goldbox::read_dax(path);
                        if (!records_.empty()) {
                            if (geo_ && !amiga_geo_ && goldbox::has_geo_trailer(records_)) {
                                records_.erase(std::remove_if(records_.begin(), records_.end(),
                                    [](const auto& record) { return record.id >= 253; }), records_.end());
                            }
                            if (!records_.empty()) {
                                show_view(geo_, !geo_, true);
                                for (const auto& record : records_)
                                    pictures_->Append(amiga_geo_ && path.stem().u8string().size() == 3
                                                       ? amiga_map_label(record.id) : geo_ ? wxString::Format("Map %u", record.id)
                                                           : wxString::Format("Record $%02X", record.id));
                                pictures_->SetSelection(0);
                                render();
                                return;
                            }
                        }
                    }
                    hex_->open(path);
                    SetStatusText(wxString::Format("%llu bytes",
                        static_cast<unsigned long long>(hex_->size())));
                    return;
                }
                auto archive = c64 ? goldbox::read_c64_picture(path)
                                    : wall ? goldbox::read_wall_archive(path)
                                    : tlb ? goldbox::read_tlb(path)
                                    : glb ? goldbox::read_geo_glb(path)
                                    : cmp ? goldbox::read_cmp(path)
                                    : anm ? goldbox::read_anm(path)
                                    : packwall ? goldbox::read_packwall(path)
                                    : vace ? goldbox::read_vace(path) : goldbox::read_pictures(path);
                if (archive.frames.empty())
                    throw std::runtime_error("Archive contains no supported images");
                const auto count = archive.gallery_count();
                frames_ = std::move(archive.frames);
                clips_ = std::move(archive.clips);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& frame = frames_[i];
                    const auto label = i < archive.frame_labels.size() ? wxString::FromUTF8(archive.frame_labels[i])
                        : glb ? wxString::Format("Map %u", frame.id)
                        : packwall ? wxString::Format("Wall set %u", frame.id)
                        : cmp && count > 1 ? wxString::Format("Tile %u", frame.id)
                        : anm ? wxString::Format("Frame %u", frame.id)
                        : vace ? wxString::Format("Frame %u", frame.index + 1)
                        : wxString::Format(wall ? "$%02X   Piece %u" : "$%02X   Frame %u",
                                           frame.id, frame.index + 1);
                    pictures_->Append(label);
                }
                pictures_->SetSelection(0);
                auto status = wxString::Format(glb ? "%zu maps" : packwall ? "%zu wall sets" :
                                               cmp && count > 1 ? "%zu tiles" :
                                               "%zu picture frames", count);
                if (!clips_.empty())
                    status += wxString::Format("   |   %zu animations", clips_.size());
                if (!archive.skipped_ids.empty())
                    status += wxString::Format(", %zu unsupported records skipped", archive.skipped_ids.size());
                if (!archive.partial_ids.empty())
                    status += wxString::Format(", %zu wall records with unresolved glyphs", archive.partial_ids.size());
                SetStatusText(status);
            } catch (const std::exception& error) {
                auto message = wxString::FromUTF8(error.what());
                if (archive_file || tlb || glb || cmp || anm || packwall || vace ||
                    kind != goldbox::RecordKind::none || world) {
                    try {
                        hex_->open(paths_.at(selected));
                        show_view(false, true);
                        message += " (showing raw bytes)";
                    } catch (const std::exception& hex_error) {
                        message += "; raw view: " + wxString::FromUTF8(hex_error.what());
                    }
                }
                SetStatusText(message);
            }
        }
        render();
    }

    void show_picture_list(bool show)
    {
        if (show && !picture_split_->IsSplit()) {
            auto* content = picture_split_->GetWindow1();
            pictures_->Show();
            picture_split_->SplitVertically(pictures_, content,
                                            picture_width_ ? picture_width_ : FromDIP(135));
        } else if (!show && picture_split_->IsSplit()) {
            picture_width_ = picture_split_->GetSashPosition();
            picture_split_->Unsplit(pictures_);
        }
    }

    void show_view(bool picture, bool raw, bool records = false, bool perspective = false,
                   bool fields = false, bool map_comparison = false)
    {
        show_picture_list((picture && !saved_map_) || records);
        preview_split_->Show(picture);
        zoom_->Show(picture);
        play_->Show(picture && selected_clip());
        if (perspective && !preview_split_->IsSplit()) {
            perspective_->Show();
            preview_split_->SplitVertically(preview_, perspective_,
                                            -(perspective_width_ ? perspective_width_ : FromDIP(205)));
        } else if (!perspective && preview_split_->IsSplit()) {
            perspective_width_ = preview_split_->GetClientSize().x - preview_split_->GetSashPosition();
            preview_split_->Unsplit(perspective_);
        }
        facing_->Show(perspective);
        hex_->Show(raw);
        tables_->Show(fields);
        original_map_->Show(map_comparison);
        preview_split_->GetParent()->Layout();
        zoom_->GetParent()->Layout();
    }

    const goldbox::PictureArchive::Clip* selected_clip() const
    {
        const auto selected = pictures_->GetSelection();
        if (selected == wxNOT_FOUND || static_cast<std::size_t>(selected) >= frames_.size())
            return nullptr;
        const auto id = frames_[selected].id;
        const auto clip = std::find_if(clips_.begin(), clips_.end(),
            [id](const auto& candidate) { return candidate.whole_archive || candidate.id == id; });
        return clip == clips_.end() ? nullptr : &*clip;
    }

    void reset_playback()
    {
        playback_timer_.Stop();
        if (active_ && !clips_.empty() && clips_.front().whole_archive) {
            show_picture_list(true);
            zoom_->Show();
        }
        active_ = false;
        step_ = 0;
        play_->SetLabel("Play");
    }

    void toggle_playback()
    {
        const auto* clip = selected_clip();
        if (!clip)
            return;
        if (playback_timer_.IsRunning()) {
            playback_timer_.Stop();
            playback_remaining_ = playback_deadline_ - PlaybackClock::now();
            play_->SetLabel("Resume");
        } else if (active_ && clip->steps[step_].duration_ms == 0) {
            reset_playback();
            render();
        } else {
            if (!active_ && clip->whole_archive) {
                show_picture_list(false);
                zoom_->Hide();
                zoom_->GetParent()->Layout();
            }
            if (clip->realtime)
                playback_deadline_ = PlaybackClock::now() + (active_ ?
                    std::max(PlaybackClock::duration::zero(), playback_remaining_) :
                    std::chrono::milliseconds(clip->steps[step_].duration_ms));
            active_ = true;
            show_playback_step(*clip);
        }
    }

    void show_playback_step(const goldbox::PictureArchive::Clip& clip)
    {
        const auto& step = clip.steps[step_];
        show_frame(frames_.at(step.frame), clip.whole_archive);
        play_->SetLabel(step.duration_ms ? "Pause" : "Gallery");
        if (step.duration_ms) {
            const auto ms = clip.realtime ? std::max<int>(1, std::chrono::ceil<std::chrono::milliseconds>(
                playback_deadline_ - PlaybackClock::now()).count()) : step.duration_ms;
            playback_timer_.StartOnce(ms);
        }
    }

    void show_frame(const goldbox::Frame& frame, bool fit = false)
    {
        const auto suffix = fit ? "-scene" + frame_suffix(frame) : frame_suffix(frame);
        preview_->show(&frame, fit ? 0 : zoom_->GetSelection() + 1, export_name(suffix));
    }

    void show_fields(std::size_t index)
    {
        const auto& rows = record_list_->rows();
        if (index >= rows.size())
            return;
        const auto& record = rows[index];
        details_->load(&record.fields, record_kind_);
        SetStatusText(wxString::Format("%s %zu of %zu   |   ", record_kind_ == goldbox::RecordKind::ecl ? "Script" : "Record",
                                      index + 1, rows.size())
                      + wxString::FromUTF8(record.name));
    }

    void show_record()
    {
        const auto selected = pictures_->GetSelection();
        if (selected == wxNOT_FOUND)
            return;
        const auto& record = records_.at(selected);
        hex_->open(record.bytes);
        SetStatusText(wxString::Format("Record $%02X   %u stored bytes   %zu decoded bytes",
                                      record.id, record.stored_size, record.bytes.size()));
    }

    void render()
    {
        if (saved_map_) {
            const auto& save = *saved_map_;
            const bool compare = save.original && save.copied;
            const bool original = save.original && (!save.copied || original_map_->GetValue());
            const auto& map = original ? *save.original : *save.copied;
            preview_->show_map(map, zoom_->GetSelection() + 1, save.x, save.y, save.facing,
                               export_name(wxString::Format("-map-%02X", save.map_id)),
                               save.visited ? &*save.visited : nullptr, compare ? &save.changed : nullptr);
            show_view(true, false, false, false, false, compare);
            auto status = wxString::Format("Saved map %u   |   Party (%u, %u)   |   ",
                                           save.map_id, save.x, save.y);
            status += compare ? wxString::Format("%zu changed cells", save.changed.count())
                              : wxString("One GEO source available");
            if (save.visited)
                status += wxString::Format("   |   %zu visited cells", save.visited->count());
            SetStatusText(status);
            return;
        }
        const auto selected = pictures_->GetSelection();
        if (geo_ && selected != wxNOT_FOUND) {
            const auto& record = records_.at(selected);
            try {
                const auto map = goldbox::read_geo_map(record);
                const auto entry = banks_.try_emplace(record.id);
                if (entry.second && !amiga_geo_) {
                    try {
                        entry.first->second = goldbox::load_map_walls(
                            paths_.at(files_->GetSelection()), record.id);
                    } catch (const std::exception&) {
                        // Missing companion data still allows overhead inspection.
                    }
                }
                const auto& banks = entry.first->second;
                const bool has_walls = !banks[0].empty();
                const int direction = facing_->GetSelection();
                preview_->show_map(map, zoom_->GetSelection() + 1, cursor_x_, cursor_y_,
                                   has_walls ? direction : -1,
                                   export_name(wxString::Format("-map-%02X", record.id)));
                show_view(true, false, true, has_walls);
                if (has_walls) {
                    const auto view = goldbox::wall_perspective(map, banks, cursor_x_, cursor_y_, direction);
                    perspective_->show(&view, 2, export_name(wxString::Format("-map-%02X-view-%d-%d-%d",
                        record.id, cursor_x_, cursor_y_, direction)));
                    SetStatusText(wxString::Format("Map %u   |   Camera (%d, %d)   |   Click a square to move",
                                                   record.id, cursor_x_, cursor_y_));
                } else {
                    SetStatusText(amiga_geo_ ? pictures_->GetString(selected) + "   |   16 x 16" :
                        wxString::Format("Map %u   |   16 x 16   |   No compatible wall set", record.id));
                }
            } catch (const std::exception& error) {
                show_view(false, true, true);
                show_record();
                SetStatusText(wxString::FromUTF8(error.what()) + " (showing record bytes)");
            }
            return;
        }
        if (!records_.empty()) {
            show_record();
            return;
        }
        const auto* clip = selected_clip();
        play_->Show(clip != nullptr);
        play_->GetParent()->Layout();
        const auto index = active_ && clip ? clip->steps[step_].frame : selected;
        if (selected == wxNOT_FOUND)
            preview_->show(nullptr, 1);
        else
            show_frame(frames_.at(index), active_ && clip && clip->whole_archive);
    }

    wxListBox* files_;
    wxListBox* pictures_;
    wxChoice* zoom_;
    wxButton* play_;
    wxChoice* facing_;
    wxCheckBox* original_map_;
    Preview* preview_;
    Preview* perspective_;
    wxSplitterWindow* picture_split_;
    wxSplitterWindow* preview_split_;
    HexView* hex_;
    wxSplitterWindow* tables_;
    RecordList* record_list_;
    RecordDetails* details_;
    goldbox::RecordKind record_kind_ = goldbox::RecordKind::none;
    fs::path current_folder_;
    fs::path disk_cache_;
    fs::path active_image_;
    std::map<fs::path, fs::path> mounted_images_;
    std::vector<fs::path> paths_;
    std::vector<goldbox::Frame> frames_;
    std::vector<goldbox::PictureArchive::Clip> clips_;
    std::vector<goldbox::DaxRecord> records_;
    bool geo_ = false;
    bool amiga_geo_ = false;
    int cursor_x_ = 8;
    int cursor_y_ = 8;
    int picture_width_ = 0;
    int perspective_width_ = 0;
    std::map<unsigned, goldbox::WallBanks> banks_;
    std::optional<goldbox::SavedMap> saved_map_;
    wxTimer playback_timer_;
    using PlaybackClock = std::chrono::steady_clock;
    PlaybackClock::time_point playback_deadline_;
    PlaybackClock::duration playback_remaining_{};
    std::size_t step_ = 0;
    bool active_ = false;
};

class ExplorerApp : public wxApp {
    bool OnInit() override
    {
        wxImage::AddHandler(new wxPNGHandler);
#if wxUSE_LIBJPEG
        wxImage::AddHandler(new wxJPEGHandler);
#endif
        fs::path initial;
        if (argc > 1)
            initial = fs::u8path(wxString(argv[1]).ToUTF8().data());
        (new Gallery(initial))->Show();
        return true;
    }
};
}
wxIMPLEMENT_APP(ExplorerApp);
