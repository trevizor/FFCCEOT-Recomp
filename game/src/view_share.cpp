#include "view_share.hpp"

#include <algorithm>
#include <new>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace view_share {

namespace {

std::uint8_t* picture(Block* block, int slot, int buffer) {
    auto* base = reinterpret_cast<std::uint8_t*>(block) + sizeof(Block);
    return base + (std::size_t(slot) * kBuffers + std::size_t(buffer)) * kPictureBytes;
}

}  // namespace

Cell cell(int players, int index, int w, int h) {
    if (players <= 1) return {0, 0, w, h};
    if (players == 2) {
        const int half = w / 2;
        return index == 0 ? Cell{0, 0, half, h} : Cell{half, 0, w - half, h};
    }
    const int cw = w / 2, ch = h / 2, col = index % 2, row = index / 2;
    return {col * cw, row * ch, col ? w - cw : cw, row ? h - ch : ch};
}

void shared_size(int w, int h, int& out_w, int& out_h) {
    w = std::max(w, 16);
    h = std::max(h, 16);
    const double scale = std::min({1.0, double(kMaxWidth) / w, double(kMaxHeight) / h});
    out_w = std::max(16, int(w * scale));
    out_h = std::max(16, int(h * scale));
}

std::uint8_t* Writer::begin(int w, int h) {
    if (!block_ || w <= 0 || h <= 0 || w > kMaxWidth || h > kMaxHeight) return nullptr;
    Slot& s = block_->slots[slot_];
    // Not the latest picture, and not the one being read: there is always one of the three left.
    const std::uint32_t latest = s.latest.load(), reading = s.reading.load();
    int pick = 0;
    while (pick == int(latest) - 1 || pick == int(reading) - 1) pick++;
    writing_ = pick;
    w_ = w;
    h_ = h;
    return picture(block_, slot_, pick);
}

void Writer::publish() {
    if (!block_ || writing_ < 0) return;
    Slot& s = block_->slots[slot_];
    s.width.store(std::uint32_t(w_));
    s.height.store(std::uint32_t(h_));
    s.serial.fetch_add(1);
    s.latest.store(std::uint32_t(writing_) + 1);
    writing_ = -1;
}

const std::uint8_t* Reader::acquire(int& w, int& h) {
    if (!block_) return nullptr;
    Slot& s = block_->slots[slot_];
    std::uint32_t latest;
    for (;;) {  // (claim the picture, then check that it is still the latest: the writer leaves a claimed one alone)
        latest = s.latest.load();
        if (latest == 0) return nullptr;
        s.reading.store(latest);
        if (s.latest.load() == latest) break;
    }
    const std::uint64_t serial = s.serial.load();
    if (serial == seen_) {
        s.reading.store(0);
        return nullptr;
    }
    seen_ = serial;
    w = int(s.width.load());
    h = int(s.height.load());
    if (w <= 0 || h <= 0 || w > kMaxWidth || h > kMaxHeight) {
        s.reading.store(0);
        return nullptr;
    }
    return picture(block_, slot_, int(latest) - 1);
}

void Reader::release() {
    if (block_) block_->slots[slot_].reading.store(0);
}

#ifdef _WIN32

bool Mapping::supported() { return true; }

Mapping& Mapping::operator=(Mapping&& o) noexcept {
    if (this != &o) {
        close();
        handle_ = o.handle_;
        block_ = o.block_;
        o.handle_ = nullptr;
        o.block_ = nullptr;
    }
    return *this;
}

Mapping::~Mapping() { close(); }

void Mapping::close() {
    if (block_) UnmapViewOfFile(block_);
    if (handle_) CloseHandle(static_cast<HANDLE>(handle_));
    block_ = nullptr;
    handle_ = nullptr;
}

namespace {
std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
}  // namespace

bool Mapping::create(const std::string& name) {
    close();
    const std::uint64_t size = kBlockBytes;
    handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, DWORD(size >> 32), DWORD(size & 0xFFFFFFFFu),
                                 wide(name).c_str());
    if (!handle_) return false;
    void* view = MapViewOfFile(static_cast<HANDLE>(handle_), FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) {
        close();
        return false;
    }
    block_ = new (view) Block();  // (a new mapping is zeroed already; this only starts the objects' lifetimes)
    block_->parent_pid.store(current_pid());
    return true;
}

bool Mapping::open(const std::string& name) {
    close();
    handle_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, wide(name).c_str());
    if (!handle_) return false;
    void* view = MapViewOfFile(static_cast<HANDLE>(handle_), FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!view) {
        close();
        return false;
    }
    block_ = static_cast<Block*>(view);
    return true;
}

bool process_alive(std::uint32_t pid) {
    if (pid == 0) return true;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

std::uint32_t current_pid() { return GetCurrentProcessId(); }

#else  // nothing to share memory with: the local players need Windows (and its local wireless)

bool Mapping::supported() { return false; }
Mapping& Mapping::operator=(Mapping&&) noexcept { return *this; }
Mapping::~Mapping() = default;
void Mapping::close() {}
bool Mapping::create(const std::string&) { return false; }
bool Mapping::open(const std::string&) { return false; }
bool process_alive(std::uint32_t) { return true; }
std::uint32_t current_pid() { return 0; }

#endif

}  // namespace view_share
