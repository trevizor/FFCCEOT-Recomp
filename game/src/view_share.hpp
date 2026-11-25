// One window for every local player (Settings -> Local co-op -> "One window for all instances"). The players are still
// separate programs, each with its own console, but players 2 to 4 have no window: they draw their picture into shared
// memory, and player 1's window shows all four in a grid. This is the shared memory, and where each picture goes.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace view_share {

constexpr int kMaxWidth = 1280, kMaxHeight = 720;  // the largest picture a player shares (bigger cells are scaled up)
constexpr int kBuffers = 3;                        // per player: one being written, one shown, one that is the latest
constexpr int kChildren = 3;                       // players 2 to 4

struct Cell {
    int x = 0, y = 0, w = 0, h = 0;
};
// Where player `index` (0 = player 1) is shown in a window of w x h pixels: side by side for two players, otherwise
// a grid of two by two (player 1 top left, 2 top right, 3 bottom left, 4 bottom right).
Cell cell(int players, int index, int w, int h);

// The size of the picture a player shares for a cell of w x h pixels: the same shape, at most kMaxWidth x kMaxHeight.
void shared_size(int w, int h, int& out_w, int& out_h);

// What the first player's window and the others use, in memory both can see. Zeroed memory is its empty state.
struct Slot {
    std::atomic<std::uint32_t> latest{0};   // the buffer written last, plus one (0: none yet)
    std::atomic<std::uint32_t> reading{0};  // the buffer the first player is reading, plus one (0: none)
    std::atomic<std::uint32_t> width{0}, height{0};  // of the latest picture
    std::atomic<std::uint64_t> serial{0};            // counts the pictures
};
struct Block {
    std::atomic<std::uint32_t> cell_w{0}, cell_h{0};  // the picture size the first player wants (0: not known yet)
    std::atomic<std::uint32_t> parent_pid{0};         // the first player's process, whose end ends the others
    Slot slots[kChildren];
    // The pictures follow: kChildren * kBuffers of kMaxWidth * kMaxHeight RGBA pixels, bottom row first.
};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free && std::atomic<std::uint64_t>::is_always_lock_free,
              "the shared block needs lock-free atomics");

constexpr std::size_t kPictureBytes = std::size_t(kMaxWidth) * kMaxHeight * 4;
constexpr std::size_t kBlockBytes = sizeof(Block) + kChildren * kBuffers * kPictureBytes;

// A player's side of the block. Child `slot` is player slot + 2.
class Writer {
public:
    explicit Writer(Block* block = nullptr, int slot = 0) : block_(block), slot_(slot) {}
    // The pixels to fill with a picture of w x h (RGBA, w * h * 4 bytes), or null. publish() makes it the latest.
    std::uint8_t* begin(int w, int h);
    void publish();

private:
    Block* block_;
    int slot_;
    int writing_ = -1;
    int w_ = 0, h_ = 0;
};

class Reader {
public:
    explicit Reader(Block* block = nullptr, int slot = 0) : block_(block), slot_(slot) {}
    // The latest picture, if it is newer than the last one acquired (and one has been published): its pixels stay valid
    // until release().
    const std::uint8_t* acquire(int& w, int& h);
    void release();
    std::uint64_t serial() const { return seen_; }  // of the picture acquire() returned last

private:
    Block* block_;
    int slot_;
    std::uint64_t seen_ = 0;
};

// The shared memory itself, by name (Windows: a file mapping; elsewhere nothing, and create and open fail).
class Mapping {
public:
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    Mapping(Mapping&& o) noexcept { *this = std::move(o); }
    Mapping& operator=(Mapping&& o) noexcept;
    ~Mapping();

    static bool supported();
    bool create(const std::string& name);  // the first player makes it
    bool open(const std::string& name);    // the others join it
    Block* block() const { return block_; }

private:
    void close();
    void* handle_ = nullptr;
    Block* block_ = nullptr;
};

// Windows: whether the first player's program is still running (the others end with it).
bool process_alive(std::uint32_t pid);
std::uint32_t current_pid();

}  // namespace view_share
