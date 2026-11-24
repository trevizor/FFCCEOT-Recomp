// The pictures players share when they play in one window: the cells they are shown in and the hand-over of a picture.
#include <cstdio>
#include <cstring>
#include <new>
#include <vector>

#include "view_share.hpp"

static int failures = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                                   \
        }                                                                                 \
    } while (0)

int main() {
    using namespace view_share;

    const Cell two = cell(2, 1, 1281, 720);
    CHECK(two.x == 640 && two.y == 0 && two.w == 641 && two.h == 720);
    const Cell four = cell(4, 3, 1280, 720);
    CHECK(four.x == 640 && four.y == 360 && four.w == 640 && four.h == 360);
    CHECK(cell(3, 2, 1280, 720).x == 0 && cell(3, 2, 1280, 720).y == 360);

    int w = 0, h = 0;
    shared_size(640, 360, w, h);
    CHECK(w == 640 && h == 360);
    shared_size(2560, 1440, w, h);
    CHECK(w == 1280 && h == 720);

    std::vector<unsigned char> memory(kBlockBytes + 64);
    Block* block = new (memory.data()) Block();
    Writer writer(block, 1);
    Reader reader(block, 1);

    CHECK(reader.acquire(w, h) == nullptr);  // nothing published yet
    CHECK(writer.begin(kMaxWidth + 1, 10) == nullptr);

    std::uint8_t* a = writer.begin(4, 2);
    CHECK(a != nullptr);
    std::memset(a, 0xAB, 4 * 2 * 4);
    writer.publish();
    const std::uint8_t* got = reader.acquire(w, h);
    CHECK(got && w == 4 && h == 2 && got[0] == 0xAB);
    // While a picture is being read, the writer never writes into it, however many it publishes.
    for (int i = 0; i < 5; i++) {
        std::uint8_t* next = writer.begin(4, 2);
        CHECK(next != got);
        std::memset(next, i, 4 * 2 * 4);
        writer.publish();
    }
    CHECK(got[0] == 0xAB);
    reader.release();
    const std::uint8_t* newer = reader.acquire(w, h);
    CHECK(newer && newer[0] == 4);
    reader.release();
    CHECK(reader.acquire(w, h) == nullptr);  // the same picture is not given twice

    if (failures) std::fprintf(stderr, "%d failed\n", failures);
    return failures ? 1 : 0;
}
