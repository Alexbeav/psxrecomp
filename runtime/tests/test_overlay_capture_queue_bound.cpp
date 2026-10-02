/* The queue of outgoing overlay snapshots is capped, and the cap changes nothing
 * in what the capture store holds.
 *
 * Before a CD-ROM DMA overwrites a page where interpreted code ran, the runtime
 * keeps the outgoing code (overlay_capture_before_dma). It queued a copy of the
 * whole 2 MiB of guest RAM, about 2.2 MB with its bitmaps, for one writer
 * thread, and the queue had no limit. In a cold start Colony Wars: Vengeance
 * queued about 2,700 copies in five seconds: 996 MB grew to 7,050 MB, and
 * another title reached 25 GB (PS1G-75, PS1G-39).
 *
 * With the queue at its cap the snapshot is committed on the emulation thread
 * instead, from live RAM, at the same moment. This test makes the same series
 * of DMAs four times and compares the stores file by file:
 *
 *   A  no cap in reach, writer held: every snapshot is queued (the old way);
 *   B  cap 4, writer held: 4 are queued, the rest are committed at the cap;
 *   C  cap 2, writer running: the queue never holds more than 2;
 *   D  cap 2, writer held, and the history folder cannot be written: a commit
 *      that fails at the cap is queued after all, and is written once the
 *      folder can be. Nothing is lost.
 *
 * It also shows that queued snapshots are written when the store is shut down
 * (overlay_capture_wait_pending), which is what a normal quit does.
 */
#include "code_provider.h"
#include "dirty_ram_interp.h"
#include "overlay_capture.h"

#include "psx_sdl.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

extern "C" void overlay_capture_test_set_preserve_cap(unsigned cap);
extern "C" unsigned overlay_capture_test_preserve_cap(void);
extern "C" void overlay_capture_test_hold_preserve_writer(int hold);
extern "C" void overlay_capture_test_preserve_counts(unsigned *held, unsigned *peak,
                                                     unsigned *enqueued,
                                                     unsigned *sync_commits);

extern "C" {
uint32_t g_dirty_ram_exec_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_dispatch_pc_bitmap[DIRTY_RAM_EXEC_BITMAP_WORDS]{};
uint32_t g_dirty_ram_exec_page_bitmap[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
uint64_t g_dirty_ram_insns_run = 0;
uint64_t g_dirty_window_dispatches = 0;
uint64_t s_frame_count = 0;
uint32_t g_overlay_region_floor = 0x00010000u;
}

namespace {

namespace fs = std::filesystem;

const unsigned kDmas = 24;              /* each queued one is a 2 MiB copy */
uint8_t g_ram[2u * 1024u * 1024u]{};
uint32_t g_dirty_pages[DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS]{};
int g_failures = 0;

void check(bool ok, const std::string &what) {
    if (ok) return;
    std::fprintf(stderr, "FAIL: %s\n", what.c_str());
    ++g_failures;
}

void set_exec(uint32_t phys) {
    const uint32_t word = phys >> 2;
    g_dirty_ram_exec_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    g_dirty_ram_dispatch_pc_bitmap[word >> 5] |= 1u << (word & 31u);
    const uint32_t page = phys >> 12;
    g_dirty_ram_exec_page_bitmap[page >> 5] |= 1u << (page & 31u);
    g_dirty_pages[page >> 5] |= 1u << (page & 31u);
}

std::string read_all(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

/* The store: the latest file and every file of the additive history. */
std::map<std::string, std::string> store_of(const fs::path &capture) {
    std::map<std::string, std::string> files;
    files["(latest)"] = read_all(capture);
    const fs::path history = capture.string() + ".d";
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(history, ec))
        if (entry.is_regular_file())
            files[entry.path().filename().string()] = read_all(entry.path());
    return files;
}

struct Counts { unsigned held = 0, peak = 0, enqueued = 0, sync = 0; };

Counts counts() {
    Counts c;
    overlay_capture_test_preserve_counts(&c.held, &c.peak, &c.enqueued, &c.sync);
    return c;
}

/* The same guest history every time: interpreted code runs in a page, a disc
 * read is about to overwrite it, the read happens. Five pages take turns, and
 * every third read finds code in the page after it as well. */
void make_dmas() {
    for (unsigned i = 0; i < kDmas; ++i) {
        const uint32_t page = 0x20000u + (i % 5u) * 0x1000u;
        for (uint32_t j = 0; j < 96u; ++j)
            g_ram[page + j] = static_cast<uint8_t>(i * 7u + j);
        set_exec(page + 4u * (i % 8u));
        if (i % 3u == 0u) {
            g_ram[page + 0x1000u + 8u] = static_cast<uint8_t>(0xA0u + i);
            set_exec(page + 0x1000u + 8u);
        }
        overlay_capture_before_dma(0x80000000u | page, 2048u);
        std::memset(&g_ram[page], static_cast<int>(0xE0u ^ i), 2048u);  /* the read lands */
    }
}

void fresh_guest() {
    std::memset(g_ram, 0, sizeof g_ram);
    std::memset(g_dirty_ram_exec_pc_bitmap, 0, sizeof g_dirty_ram_exec_pc_bitmap);
    std::memset(g_dirty_ram_dispatch_pc_bitmap, 0, sizeof g_dirty_ram_dispatch_pc_bitmap);
    std::memset(g_dirty_ram_exec_page_bitmap, 0, sizeof g_dirty_ram_exec_page_bitmap);
    std::memset(g_dirty_pages, 0, sizeof g_dirty_pages);
}

fs::path begin_run(const fs::path &root, const char *name, unsigned cap, bool hold) {
    const fs::path folder = root / name;
    fs::create_directories(folder);
    const fs::path capture = folder / "overlay_captures.json";
    fresh_guest();
    overlay_capture_set_path(capture.string().c_str());
    overlay_capture_set_enabled(1);
    /* the store wakes at the first DMA after the game has started */
    overlay_capture_on_dma(0x10000u, 4u, &g_ram[0x10000]);
    overlay_capture_test_set_preserve_cap(cap);
    overlay_capture_test_hold_preserve_writer(hold ? 1 : 0);
    (void)counts();                         /* start the counters again */
    return capture;
}

std::map<std::string, std::string> end_run(const fs::path &capture) {
    overlay_capture_test_hold_preserve_writer(0);
    overlay_capture_wait_pending();         /* a normal quit: drain, then stop */
    return store_of(capture);
}

void same_store(const std::map<std::string, std::string> &expected,
                const std::map<std::string, std::string> &got, const char *run) {
    check(got.size() == expected.size(),
          std::string(run) + ": " + std::to_string(got.size()) + " files in the store, expected " +
              std::to_string(expected.size()));
    for (const auto &entry : expected) {
        const auto found = got.find(entry.first);
        if (found == got.end())
            check(false, std::string(run) + ": the store lacks " + entry.first);
        else if (found->second != entry.second)
            check(false, std::string(run) + ": " + entry.first + " has other content");
    }
    for (const auto &entry : got)
        if (!expected.count(entry.first))
            check(false, std::string(run) + ": the store has an extra file " + entry.first);
}

}  // namespace

extern "C" uint8_t *memory_get_ram_ptr(void) { return g_ram; }
extern "C" uint32_t dirty_ram_get_bitmap_word_count(void) {
    return DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS;
}
extern "C" uint32_t dirty_ram_get_bitmap_word(uint32_t index) {
    return index < DIRTY_RAM_EXEC_PAGE_BITMAP_WORDS ? g_dirty_pages[index] : 0u;
}
extern "C" int cdrom_load_in_progress(void) { return 0; }
extern "C" int fntrace_is_game_started(void) { return 1; }
extern "C" void overlay_loader_check_cache(uint32_t, uint32_t, const uint8_t *) {}
extern "C" int overlay_loader_registered_count(void) { return 0; }
extern "C" const CodeProvider *code_provider_active(void) { return nullptr; }
extern "C" uint32_t crc32_compute(const uint8_t *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 16777619u;
    return hash ? hash : 1u;
}

int main() {
    if (psx_sdl_init(0) != 0) {
        std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    const fs::path root = fs::temp_directory_path() /
        ("psxrecomp-capture-queue-bound-" + std::to_string(
            static_cast<unsigned long long>(SDL_GetPerformanceCounter())));
    std::error_code ignored;
    fs::remove_all(root, ignored);

    const unsigned built_in = overlay_capture_test_preserve_cap();
    check(built_in >= 16u && built_in <= 256u,
          "the built-in cap is " + std::to_string(built_in) +
              "; at about 2.2 MB an entry it should stay between 16 and 256");

    /* A: the old way. Every snapshot is queued, and a shutdown writes them all. */
    fs::path capture = begin_run(root, "a-uncapped", 100000u, true);
    make_dmas();
    Counts c = counts();
    check(c.enqueued == kDmas && c.sync == 0u && c.held == kDmas,
          "A: expected every snapshot queued, got queued " + std::to_string(c.enqueued) +
              ", at the cap " + std::to_string(c.sync) + ", held " + std::to_string(c.held));
    const auto uncapped = end_run(capture);
    check(uncapped.size() > 10u && !uncapped.at("(latest)").empty(),
          "A: the store holds " + std::to_string(uncapped.size()) +
              " files; the series should leave more than ten");
    check(counts().held == 0u, "A: entries are still held after the shutdown drain");

    /* B: the queue fills to its cap; everything after it is committed at once. */
    capture = begin_run(root, "b-cap-4-held", 4u, true);
    make_dmas();
    c = counts();
    check(c.enqueued == 4u && c.held == 4u && c.peak == 4u && c.sync == kDmas - 4u,
          "B: expected 4 queued and " + std::to_string(kDmas - 4u) + " at the cap, got queued " +
              std::to_string(c.enqueued) + ", held " + std::to_string(c.held) + ", most at once " +
              std::to_string(c.peak) + ", at the cap " + std::to_string(c.sync));
    same_store(uncapped, end_run(capture), "B");

    /* C: with the writer running the queue still never passes its cap. */
    capture = begin_run(root, "c-cap-2-running", 2u, false);
    make_dmas();
    c = counts();
    check(c.peak <= 2u && c.enqueued + c.sync == kDmas,
          "C: most at once " + std::to_string(c.peak) + " (cap 2), queued " +
              std::to_string(c.enqueued) + " + at the cap " + std::to_string(c.sync) +
              " should be " + std::to_string(kDmas));
    same_store(uncapped, end_run(capture), "C");

    /* D: the history folder cannot be written while the reads happen. A file
     * stands where the folder belongs. A commit at the cap then fails, and the
     * snapshot must be queued for the writer's retry, not dropped. */
    capture = begin_run(root, "d-cap-2-history-blocked", 2u, true);
    const fs::path history = capture.string() + ".d";
    { std::ofstream blocker(history, std::ios::binary); blocker << "not a folder"; }
    make_dmas();
    c = counts();
    check(c.held == kDmas && c.sync == 0u,
          "D: a failed commit at the cap must be queued: held " + std::to_string(c.held) +
              " of " + std::to_string(kDmas) + ", committed at the cap " + std::to_string(c.sync));
    fs::remove(history, ignored);
    fs::remove(capture, ignored);           /* the failed commits still replaced the latest file */
    same_store(uncapped, end_run(capture), "D");

    overlay_capture_test_set_preserve_cap(0u);
    check(overlay_capture_test_preserve_cap() == built_in, "the built-in cap was not restored");
    fs::remove_all(root, ignored);
    SDL_Quit();
    if (g_failures) {
        std::fprintf(stderr, "overlay capture queue bound: %d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("PASS: the snapshot queue stops at its cap and the store holds the same files as without it");
    return 0;
}
