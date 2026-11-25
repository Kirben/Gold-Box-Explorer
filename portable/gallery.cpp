#include "pictures.h"
#include "apng.h"
#include "tlb.h"
#include "vace.h"
#include "dax.h"
#include "geo.h"
#include "walls.h"
#include "records.h"
#include "save_map.h"

#include <wx/dcbuffer.h>
#include <wx/dirctrl.h>
#include <wx/file.h>
#include <wx/filename.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/scrolwin.h>
#include <wx/splitter.h>
#include <wx/timer.h>
#include <wx/treectrl.h>
#include <wx/wx.h>
#include <wx/wfstream.h>

#include <algorithm>
#include <fstream>
#include <functional>
#include <limits>
#include <set>

namespace {
namespace fs = std::filesystem;

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
            dc.SetBackground(wxBrush(wxColour(28, 29, 33)));
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
        details_ = new wxListCtrl(tables_, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_SINGLE_SEL);
        details_->AppendColumn("Field", wxLIST_FORMAT_LEFT, 205);
        details_->AppendColumn("Value", wxLIST_FORMAT_LEFT, 220);
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
        file_menu->Append(export_png_id, "Export PNG...");
        file_menu->Append(export_frames_id, "Export All Frames...");
        file_menu->Append(export_animation_id, "Export Animation...");
        file_menu->AppendSeparator();
        file_menu->Append(wxID_EXIT, "E&xit");
        auto* menu_bar = new wxMenuBar;
        menu_bar->Append(file_menu, "&File");
        SetMenuBar(menu_bar);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { Close(); }, wxID_EXIT);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(preview_->can_export());
        }, export_png_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_png(*preview_); }, export_png_id);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(can_export_frames());
        }, export_frames_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_frames(); }, export_frames_id);
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& event) {
            event.Enable(selected_clip() != nullptr);
        }, export_animation_id);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { export_animation(); }, export_animation_id);
        for (auto* pane : {preview_, perspective_}) {
            pane->Bind(wxEVT_CONTEXT_MENU, [this, pane](wxContextMenuEvent&) {
                if (!pane->can_export())
                    return;
                wxMenu menu;
                menu.Append(wxID_SAVE, "Export PNG...");
                menu.Bind(wxEVT_MENU, [this, pane](wxCommandEvent&) { export_png(*pane); }, wxID_SAVE);
                pane->PopupMenu(&menu);
            });
        }

        folders->Bind(wxEVT_DIRCTRL_SELECTIONCHANGED, [this, folders](wxTreeEvent& event) {
            const auto path = folders->GetPath(event.GetItem());
            if (!path.empty())
                browse(fs::u8path(path.ToUTF8().data()), {});
        });
        files_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) { open(); });
        pictures_->Bind(wxEVT_LISTBOX, [this](wxCommandEvent&) {
            reset_playback();
            preview_->Scroll(0, 0);
            render();
        });
        play_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { toggle_playback(); });
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            const auto* clip = selected_clip();
            if (active_ && clip) {
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
        CallAfter([folders] {
            auto* tree = folders->GetTreeCtrl();
            const auto selected = tree->GetSelection();
            if (selected.IsOk())
                tree->EnsureVisible(selected);
        });
    }

private:
    static wxSplitterWindow* make_splitter(wxWindow* parent)
    {
        auto* splitter = new wxSplitterWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                               wxSP_3DSASH | wxSP_LIVE_UPDATE);
        splitter->SetMinimumPaneSize(splitter->FromDIP(60));
        return splitter;
    }

    static constexpr int export_png_id = wxID_HIGHEST + 1;
    static constexpr int export_frames_id = wxID_HIGHEST + 2;
    static constexpr int export_animation_id = wxID_HIGHEST + 3;

    wxString export_name(const wxString& suffix) const
    {
        return wxString::FromUTF8(paths_.at(files_->GetSelection()).stem().u8string()) + suffix + ".png";
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
                goldbox::is_vace_archive(paths_.at(selected)));
    }

    void export_frames()
    {
        if (!can_export_frames())
            return;
        const auto& archive = paths_.at(files_->GetSelection());
        wxDirDialog dialog(this, "Choose a folder for the PNG frames",
                           wxString::FromUTF8(archive.parent_path().u8string()));
        if (dialog.ShowModal() != wxID_OK)
            return;

        std::set<wxString> names;
        std::vector<wxString> targets;
        const auto count = static_cast<std::size_t>(pictures_->GetCount());
        targets.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            const auto& frame = frames_[i];
            const auto suffix = frame_suffix(frame);
            auto name = export_name(suffix);
            for (unsigned copy = 2; !names.insert(name).second; ++copy)
                name = export_name(suffix + wxString::Format("-%u", copy));
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
                auto image = frame_image(frames_[i]);
                {
                    wxFileOutputStream stream(file);
                    saved = image.SaveFile(stream, wxBITMAP_TYPE_PNG) && stream.IsOk();
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
        SetStatusText(wxString::Format("Exported %zu PNG frames", count));
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

    void export_png(const Preview& pane)
    {
        if (!pane.can_export())
            return;
        // Snapshot the current frame before the dialog runs its modal event loop.
        auto image = pane.export_image();
        wxFileDialog dialog(this, "Export PNG", {}, pane.filename(), "PNG images (*.png)|*.png",
                            wxFD_SAVE | wxFD_OVERWRITE_PROMPT);
        if (dialog.ShowModal() == wxID_OK && !image.SaveFile(dialog.GetPath(), wxBITMAP_TYPE_PNG))
            wxMessageBox("Could not save the PNG image.", "Export PNG", wxOK | wxICON_ERROR, this);
    }

    void browse(const fs::path& folder, const fs::path& selected)
    {
        paths_.clear();
        files_->Clear();
        std::error_code error;
        for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
            std::error_code file_error;
            if (it->is_regular_file(file_error))
                paths_.push_back(it->path());
        }
        std::sort(paths_.begin(), paths_.end());
        int index = 0;
        for (const auto& path : paths_) {
            const auto row = files_->Append(wxString::FromUTF8(path.filename().u8string()));
            if (path.filename() == selected)
                index = row;
        }
        if (!paths_.empty())
            files_->SetSelection(index);
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
        record_list_->load({});
        details_->DeleteAllItems();
        banks_.clear();
        preview_->Scroll(0, 0);
        geo_ = false;
        saved_map_.reset();
        original_map_->SetValue(false);
        const auto selected = files_->GetSelection();
        const bool wall = selected != wxNOT_FOUND && goldbox::is_wall_archive(paths_.at(selected));
        const bool tlb = selected != wxNOT_FOUND && goldbox::is_tlb_archive(paths_.at(selected));
        const bool glb = selected != wxNOT_FOUND && goldbox::is_geo_glb(paths_.at(selected));
        const bool vace = selected != wxNOT_FOUND && goldbox::is_vace_archive(paths_.at(selected));
        const bool picture = selected != wxNOT_FOUND &&
            (wall || tlb || glb || vace || goldbox::is_picture_archive(paths_.at(selected)));
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
                            record_list_->load(std::move(rows));
                            show_view(false, false, false, false, true);
                            show_fields(0);
                            return;
                        }
                    }
                    if (archive_file) {
                        records_ = goldbox::read_dax(path);
                        if (!records_.empty()) {
                            geo_ = goldbox::is_geo_archive(path);
                            if (geo_ && goldbox::has_geo_trailer(records_)) {
                                records_.erase(std::remove_if(records_.begin(), records_.end(),
                                    [](const auto& record) { return record.id >= 253; }), records_.end());
                            }
                            if (!records_.empty()) {
                                show_view(geo_, !geo_, true);
                                for (const auto& record : records_)
                                    pictures_->Append(geo_ ? wxString::Format("Map %u", record.id)
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
                auto archive = wall ? goldbox::PictureArchive{goldbox::read_wall_pieces(path), {}}
                                    : tlb ? goldbox::read_tlb(path)
                                    : glb ? goldbox::read_geo_glb(path)
                                    : vace ? goldbox::read_vace(path) : goldbox::read_pictures(path);
                if (archive.frames.empty())
                    throw std::runtime_error("Archive contains no wall pieces");
                const auto count = archive.gallery_count();
                frames_ = std::move(archive.frames);
                clips_ = std::move(archive.clips);
                for (std::size_t i = 0; i < count; ++i) {
                    const auto& frame = frames_[i];
                    const auto label = glb ? wxString::Format("Map %u", frame.id)
                        : vace ? wxString::Format("Frame %u", frame.index + 1)
                        : wxString::Format(wall ? "$%02X   Piece %u" : "$%02X   Frame %u",
                                           frame.id, frame.index + 1);
                    pictures_->Append(label);
                }
                pictures_->SetSelection(0);
                auto status = wxString::Format(glb ? "%zu maps" : "%zu picture frames", count);
                if (!clips_.empty())
                    status += wxString::Format("   |   %zu animations", clips_.size());
                if (!archive.skipped_ids.empty())
                    status += wxString::Format(", %zu unsupported records skipped", archive.skipped_ids.size());
                SetStatusText(status);
            } catch (const std::exception& error) {
                auto message = wxString::FromUTF8(error.what());
                if (archive_file || tlb || glb || vace || kind != goldbox::RecordKind::none || world) {
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
            active_ = true;
            show_playback_step(*clip);
        }
    }

    void show_playback_step(const goldbox::PictureArchive::Clip& clip)
    {
        const auto& step = clip.steps[step_];
        show_frame(frames_.at(step.frame), clip.whole_archive);
        play_->SetLabel(step.duration_ms ? "Pause" : "Gallery");
        if (step.duration_ms)
            playback_timer_.StartOnce(step.duration_ms);
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
        details_->Freeze();
        details_->DeleteAllItems();
        for (const auto& field : record.fields) {
            const auto line = details_->InsertItem(details_->GetItemCount(), wxString::FromUTF8(field.first));
            details_->SetItem(line, 1, wxString::FromUTF8(field.second));
        }
        details_->Thaw();
        SetStatusText(wxString::Format("Record %zu of %zu   |   ", index + 1, rows.size())
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
                if (entry.second) {
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
                    SetStatusText(wxString::Format("Map %u   |   16 x 16   |   No compatible wall set",
                                                   record.id));
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
    wxListCtrl* details_;
    std::vector<fs::path> paths_;
    std::vector<goldbox::Frame> frames_;
    std::vector<goldbox::PictureArchive::Clip> clips_;
    std::vector<goldbox::DaxRecord> records_;
    bool geo_ = false;
    int cursor_x_ = 8;
    int cursor_y_ = 8;
    int picture_width_ = 0;
    int perspective_width_ = 0;
    std::map<unsigned, goldbox::WallBanks> banks_;
    std::optional<goldbox::SavedMap> saved_map_;
    wxTimer playback_timer_;
    std::size_t step_ = 0;
    bool active_ = false;
};

class ExplorerApp : public wxApp {
    bool OnInit() override
    {
        wxImage::AddHandler(new wxPNGHandler);
        fs::path initial;
        if (argc > 1)
            initial = fs::u8path(wxString(argv[1]).ToUTF8().data());
        (new Gallery(initial))->Show();
        return true;
    }
};
}
wxIMPLEMENT_APP(ExplorerApp);
