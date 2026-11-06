// Zeroes sun shadow pages before they are drawn again (0: nothing there,
// lit), and the shadow page requests each frame.
#include "giant/bindless.slang"

struct ClearPush {
    uint target;      // write descriptor
    uint list;        // read descriptor of the physical pages to clear (u32 each)
    uint list_offset;
    uint count;       // pages (cs_pages), or words (cs_words)
};
ConstantBuffer<ClearPush> push : register(b0);

static const uint k_page_words = 128 * 128;

// One group row per page: 64 groups of 64 threads, 4 words each.
[shader("compute")]
[numthreads(64, 1, 1)]
void cs_pages(uint3 id : SV_DispatchThreadID) {
    if (id.y >= push.count || id.x * 4 >= k_page_words) {
        return;
    }
    const uint page = read_buffer(push.list).Load(push.list_offset + id.y * 4);
    write_buffer(push.target).Store4((page * k_page_words + id.x * 4) * 4, uint4(0, 0, 0, 0));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cs_words(uint3 id : SV_DispatchThreadID) {
    if (id.x < push.count) {
        write_buffer(push.target).Store(id.x * 4, 0u);
    }
}
