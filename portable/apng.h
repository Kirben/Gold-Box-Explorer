#pragma once

#include "pictures.h"

class wxOutputStream;

namespace goldbox {
bool write_apng(wxOutputStream& output, const std::vector<Frame>& frames,
                const PictureArchive::Clip& clip);
}
