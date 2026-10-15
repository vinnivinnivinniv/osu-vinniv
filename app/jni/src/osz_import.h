#pragma once

#include <string>

// Extract all .osz files in <inbox_dir> into <songs_dir>.
// Each .osz becomes one subdirectory of <songs_dir>, named after the
// .osz file's basename (minus extension).
//
// Returns the number of successfully extracted archives.
int import_all_osz(const std::string& inbox_dir, const std::string& songs_dir);
