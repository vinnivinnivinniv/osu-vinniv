#include "osz_import.h"

#include <SDL3/SDL.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "miniz.h"

extern "C" void SDL_Log(const char* fmt, ...);

static bool is_dir(const std::string& p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

static void mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur.push_back(path[i]);
        if (path[i] == '/' && i > 0) {
            mkdir(cur.c_str(), 0777);
        }
    }
    mkdir(path.c_str(), 0777);
}

static bool ends_with(const std::string& s, const std::string& suf) {
    if (s.size() < suf.size()) return false;
    return s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static std::string join(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (a.back() == '/') return a + b;
    return a + "/" + b;
}

static std::string basename_no_ext(const std::string& path) {
    auto slash = path.find_last_of('/');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    auto dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    return base;
}

static bool extract_one(const std::string& osz_path, const std::string& dest_dir) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));

    if (!mz_zip_reader_init_file(&zip, osz_path.c_str(), 0)) {
        SDL_Log("OSZ: cannot open %s", osz_path.c_str());
        return false;
    }

    mkdir_p(dest_dir);

    int count = (int)mz_zip_reader_get_num_files(&zip);
    int extracted = 0;

    for (int i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        // Strip any leading directory components — .osz files often nest
        // entries inside a folder that matches the set name.
        std::string entry = stat.m_filename;
        auto slash = entry.find_last_of('/');
        std::string rel = (slash == std::string::npos) ? entry : entry.substr(slash + 1);
        if (rel.empty()) continue;

        std::string out_path = join(dest_dir, rel);

        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            mkdir_p(out_path);
            continue;
        }

        if (!mz_zip_reader_extract_to_file(&zip, i, out_path.c_str(), 0)) {
            SDL_Log("OSZ: extract failed %s", rel.c_str());
            continue;
        }
        extracted++;
    }

    mz_zip_reader_end(&zip);
    SDL_Log("OSZ: %s -> %d files", osz_path.c_str(), extracted);
    return extracted > 0;
}

int import_all_osz(const std::string& inbox_dir, const std::string& songs_dir) {
    DIR* d = opendir(inbox_dir.c_str());
    if (!d) {
        SDL_Log("OSZ: inbox not found: %s", inbox_dir.c_str());
        return 0;
    }

    int imported = 0;
    std::vector<std::string> done;

    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        if (!ends_with(name, ".osz")) continue;

        std::string full = join(inbox_dir, name);
        std::string setName = basename_no_ext(name);
        std::string dest = join(songs_dir, setName);

        if (extract_one(full, dest)) {
            imported++;
            done.push_back(full);
        }
    }
    closedir(d);

    // Remove successfully imported archives from inbox
    for (const auto& p : done) {
        if (remove(p.c_str()) == 0)
            SDL_Log("OSZ: removed %s", p.c_str());
    }

    return imported;
}
