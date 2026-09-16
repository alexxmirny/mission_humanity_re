#include <cstdint>
#include <algorithm>
#include <array>
#include <vector>

namespace mh::misc {

_declspec(align(1)) struct sight_line {
    int8_t x   = static_cast<int8_t>(0x80);
    int8_t y   = static_cast<int8_t>(0x80);
    int8_t len = static_cast<int8_t>(0x80);
};

int GetCircleHordeLen(double y, double r) {
    r = r - 0.5;
    return static_cast<int>(ceil(sqrt(r * r - y * y)) - 1);
}


static std::array<std::vector<sight_line>, 32> areas = {};

void GenerateSightArea(const int r) {
    //const size_t size = sizeof(sight_line) * 2 * r;
    std::vector<sight_line> &lines = areas[r - 1];
    lines.resize(2 * r); // 2*r -1 lines + one terminator
    int prev_x = 0;
    {
        int len      = GetCircleHordeLen(-r + 1, r);
        lines[0].x   = -len;
        lines[0].y   = -r + 1;
        lines[0].len = 2 * len + 1;
        prev_x       = len + 1; // points to tile after end
    }
    //std::vector<int> line_lens = std::vector<int>(2*r -1);
    for (int y = -r + 2, i = 1; y < r; y++, i++) {
        int len      = GetCircleHordeLen(y, r);
        lines[i].x   = -prev_x - len; // move from prev_x to -len
        lines[i].y   = 1;
        lines[i].len = 2 * len + 1;
        prev_x       = len + 1;
    }
}

sight_line *GetSightAreaFromRadius(uint8_t r) {
    r = std::min(r, (uint8_t)areas.size());
    r = std::max(r, (uint8_t)1);

    if (!areas[r - 1].empty()) {
        return areas[r - 1].data();
    }
    GenerateSightArea(r);
    return areas[r - 1].data();
}
} // namespace mh::misc


extern "C" void *MH_MISC_GetSightAreaFromRadius(unsigned char radius) {
    return mh::misc::GetSightAreaFromRadius(radius);
}