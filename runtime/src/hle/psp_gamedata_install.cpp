// See psp_gamedata_install.h. Ported from PPSSPP (GPL-2.0-or-later),
// Core/Dialog/PSPGamedataInstallDialog.cpp + Core/HLE/sceIo.cpp.

#include "psp_gamedata_install.h"

#include "hle/psp_savedata.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace psp_gamedata_install {

DeviceSize ms_device_size(uint64_t free_bytes) {
    DeviceSize d;
    // PPSSPP sceIo.cpp 0x02425818 (both the ms0: and fatms0: branches):
    // maxClusters = freeSize * 95 / 100 / (sectorSize * sectorCount),
    // and free/max clusters/sectors all report that same value.
    uint64_t usable = free_bytes * 95 / 100;
    uint64_t per_cluster =
        uint64_t{kSectorSize} * uint64_t{kSectorCount};
    uint32_t clusters = static_cast<uint32_t>(usable / per_cluster);
    d.max_clusters = clusters;
    d.free_clusters = clusters;
    d.max_sectors = clusters;
    return d;
}

std::string install_dir_name(const std::string& game_name,
                             const std::string& data_name) {
    return game_name + data_name;
}

std::string install_dir(const std::string& savedata_root,
                        const std::string& game_name,
                        const std::string& data_name) {
    fs::path p = fs::path(savedata_root) / install_dir_name(game_name,
                                                            data_name);
    return p.string();
}

std::string install_file_path(const std::string& savedata_root,
                              const std::string& game_name,
                              const std::string& data_name,
                              const std::string& filename) {
    fs::path p = fs::path(install_dir(savedata_root, game_name, data_name)) /
                 filename;
    return p.string();
}

Installer::Installer(const std::string& src_dir, const std::string& dst_dir,
                     const SfoParams& sfo)
    : src_dir_(src_dir), dst_dir_(dst_dir), sfo_(sfo) {
    dir_name_ = fs::path(dst_dir_).filename().string();
    std::error_code ec;
    if (!fs::is_directory(src_dir_, ec)) {
        return;
    }
    for (fs::directory_iterator it(src_dir_, ec), end; !ec && it != end;
         it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2)) {
            continue;
        }
        uint64_t size = it->file_size(e2);
        if (e2) {
            continue;
        }
        files_.push_back(it->path().filename().string());
        sizes_.push_back(size);
        total_bytes_ += size;
    }
    if (ec) {
        files_.clear();
        sizes_.clear();
        total_bytes_ = 0;
        return;
    }
    // Deterministic order (PPSSPP uses the host listing order).
    std::vector<size_t> order(files_.size());
    for (size_t i = 0; i < order.size(); i++) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return files_[a] < files_[b]; });
    std::vector<std::string> names;
    std::vector<uint64_t> lens;
    names.reserve(files_.size());
    lens.reserve(sizes_.size());
    for (size_t i : order) {
        names.push_back(files_[i]);
        lens.push_back(sizes_[i]);
    }
    files_ = std::move(names);
    sizes_ = std::move(lens);
    ok_ = !files_.empty() && total_bytes_ > 0;
}

int Installer::progress() const {
    if (total_bytes_ == 0) {
        return 100;
    }
    return static_cast<int>((copied_bytes_ * 100) / total_bytes_);
}

void Installer::step() {
    if (finished_ || !ok_) {
        return;
    }
    if (index_ >= files_.size()) {
        write_sfo();
        finished_ = true;
        return;
    }
    if (!file_open_) {
        open_next();
        return;
    }
    copy_chunks();
    if (!file_open_) {
        // The file just completed (or failed): advance like PPSSPP's
        // CloseCurrentFile (++readFiles), and finish the install with the
        // SFO write once the last file is done.
        read_files_++;
        index_++;
        if (index_ >= files_.size()) {
            write_sfo();
            finished_ = true;
        }
    }
}

void Installer::open_next() {
    std::error_code ec;
    fs::create_directories(dst_dir_, ec);
    if (ec) {
        // Cannot stage the destination: skip the file like PPSSPP's
        // unwritable-output path does.
        std::fprintf(stderr,
                     "[GamedataInstall] cannot create %s, skipping %s\n",
                     dst_dir_.c_str(), files_[index_].c_str());
        read_files_++;
        index_++;
        return;
    }
    fs::path src = fs::path(src_dir_) / files_[index_];
    fs::path dst = fs::path(dst_dir_) / files_[index_];
    in_.open(src, std::ios::binary);
    if (!in_) {
        std::fprintf(stderr, "[GamedataInstall] cannot read %s, skipping\n",
                     src.string().c_str());
        read_files_++;
        index_++;
        return;
    }
    out_.open(dst, std::ios::binary | std::ios::trunc);
    if (!out_) {
        std::fprintf(stderr, "[GamedataInstall] cannot write %s, skipping\n",
                     dst.string().c_str());
        in_.close();
        read_files_++;
        index_++;
        return;
    }
    file_left_ = sizes_[index_];
    file_open_ = true;
}

void Installer::copy_chunks() {
    std::vector<char> buf(kBytesPerRead);
    for (uint32_t i = 0; i < kReadsPerUpdate; i++) {
        if (file_left_ == 0) {
            break;
        }
        uint64_t want = std::min<uint64_t>(file_left_, buf.size());
        in_.read(buf.data(), static_cast<std::streamsize>(want));
        std::streamsize got = in_.gcount();
        if (got <= 0) {
            // Shorter than listed, or a read error: move on rather than
            // retry forever (PPSSPP CopyCurrentFileData).
            std::fprintf(stderr,
                         "[GamedataInstall] %s ended %llu bytes early\n",
                         files_[index_].c_str(),
                         static_cast<unsigned long long>(file_left_));
            file_left_ = 0;
            break;
        }
        out_.write(buf.data(), got);
        if (!out_) {
            file_left_ = 0;
            break;
        }
        file_left_ -= static_cast<uint64_t>(got);
        copied_bytes_ += static_cast<uint64_t>(got);
    }
    if (file_left_ == 0) {
        close_current();
    }
}

void Installer::close_current() {
    if (out_.is_open()) {
        out_.close();
    }
    if (in_.is_open()) {
        in_.close();
    }
    file_open_ = false;
}

void Installer::write_sfo() {
    fs::path sfo_path = fs::path(dst_dir_) / "PARAM.SFO";
    psp_savedata::Psf sfo;
    {
        std::ifstream f(sfo_path, std::ios::binary);
        if (f) {
            std::vector<uint8_t> raw(
                (std::istreambuf_iterator<char>(f)),
                std::istreambuf_iterator<char>());
            if (!raw.empty()) {
                psp_savedata::Psf parsed;
                if (psp_savedata::Psf::parse(raw.data(), raw.size(),
                                             &parsed)) {
                    sfo = std::move(parsed);
                }
            }
        }
    }
    // PPSSPP WriteSfoFile field set.
    sfo.set_string("TITLE", sfo_.title, 128);
    sfo.set_string("SAVEDATA_TITLE", sfo_.savedata_title, 128);
    sfo.set_string("SAVEDATA_DETAIL", sfo_.detail, 1024);
    sfo.set_int("PARENTAL_LEVEL", sfo_.parental_level);
    sfo.set_string("CATEGORY", "MS", 4);
    sfo.set_string("SAVEDATA_DIRECTORY", dir_name_, 64);
    std::vector<uint8_t> bytes = sfo.serialize();
    std::error_code ec;
    fs::create_directories(dst_dir_, ec);
    fs::path tmp = sfo_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            return;
        }
        if (!bytes.empty()) {
            f.write(reinterpret_cast<const char*>(bytes.data()),
                    static_cast<std::streamsize>(bytes.size()));
        }
        f.flush();
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, sfo_path, ec);
    if (ec) {
        fs::remove(sfo_path, ec);
        ec.clear();
        fs::rename(tmp, sfo_path, ec);
    }
}

}  // namespace psp_gamedata_install
