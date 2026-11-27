// PS1B-703: MinGW-w64 libstdc++ lists the process's current directory, with
// no error, for a directory whose path is 260 characters or more, so
// directory_iterator, recursive_directory_iterator and remove_all go wrong on
// a deep mod, texture-pack or install tree (remove_all never returns).
//
// This test builds a tree that crosses 260 characters under TEMP, with a known
// set of files and folders, then lists one deep folder, walks the whole tree
// and removes it through psx_host_dir.h, the one path the runtime now uses.
// The listing and the walk must name exactly the entries the test made, and
// the removal must finish within its time limit and leave nothing behind.
#include "psx_host_dir.h"
#include "psx_host_long_path.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <set>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using PSXRecompV4::HostDirEntry;
using PSXRecompV4::host_list_dir;
using PSXRecompV4::host_long_path;
using PSXRecompV4::host_remove_tree;
using PSXRecompV4::host_visit_tree;
using PSXRecompV4::host_walk_tree;

static int failures;
static void check(bool ok, const char* what) {
    if (!ok) { std::cerr << "FAIL: " << what << '\n'; ++failures; }
}

static std::string rel(const fs::path& root, const fs::path& p) {
    return p.lexically_relative(root).generic_string();
}

static void make_file(const fs::path& p, const std::string& bytes) {
    std::ofstream(host_long_path(p), std::ios::binary) << bytes;
}

int main() {
    std::error_code ec;
    const fs::path root = fs::temp_directory_path(ec) /
                          ("psx-host-dir-test-" + std::to_string(std::chrono::steady_clock::now()
                                                                     .time_since_epoch().count()));
    check(!ec, "temp_directory_path");

    // Folders of 40 characters until the deepest one is well past 260.
    const std::string seg(40, 'd');
    fs::path deep = root;
    std::set<std::string> want;
    while (deep.native().size() < 320) {
        deep /= seg;
        want.insert(rel(root, deep));
    }
    fs::create_directories(host_long_path(deep), ec);
    check(!ec, "create the deep tree through the long spelling");
    const fs::path sub = deep / "sub";
    fs::create_directories(host_long_path(sub), ec);
    want.insert(rel(root, sub));
    make_file(root / "top.txt", "top");
    make_file(deep / "a.bin", "aaaa");
    make_file(deep / "b.bin", "bbbbbbbb");
    make_file(sub / "c.bin", "c");
    for (const char* f : {"top.txt"}) want.insert(f);
    for (const fs::path& f : {deep / "a.bin", deep / "b.bin", sub / "c.bin"}) want.insert(rel(root, f));
    std::cout << "deepest folder " << deep.native().size() << " characters, deepest file "
              << (sub / "c.bin").native().size() << '\n';

    // One deep folder: exactly a.bin, b.bin and sub, with their kinds and sizes.
    std::vector<HostDirEntry> list = host_list_dir(deep, ec);
    check(!ec, "host_list_dir on the deep folder reports no error");
    std::set<std::string> names;
    for (const HostDirEntry& e : list) {
        names.insert(e.path.filename().string());
        if (e.path.filename() == "b.bin") check(e.is_regular_file && e.size == 8, "b.bin is a file of 8 bytes");
        if (e.path.filename() == "sub") check(e.is_directory && !e.is_regular_file, "sub is a directory");
        check(e.path.parent_path() == deep, "entries keep the caller's spelling of the folder");
    }
    check(names == std::set<std::string>{"a.bin", "b.bin", "sub"},
          "host_list_dir names exactly the deep folder's entries");

    // The whole tree: every entry the test made, nothing else, parents first.
    std::vector<HostDirEntry> all = host_walk_tree(root, ec);
    check(!ec, "host_walk_tree reports no error");
    std::set<std::string> got;
    for (const HostDirEntry& e : all) got.insert(rel(root, e.path));
    check(got == want, "host_walk_tree names exactly the entries the test made");
    if (got != want) {
        for (const std::string& s : got) if (!want.count(s)) std::cerr << "  extra: " << s << '\n';
        for (const std::string& s : want) if (!got.count(s)) std::cerr << "  missing: " << s << '\n';
    }
    for (size_t i = 0; i < all.size(); ++i) {
        const fs::path parent = all[i].path.parent_path();
        if (parent == root) continue;
        const bool parent_first = std::any_of(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(i),
                                              [&](const HostDirEntry& e) { return e.path == parent; });
        check(parent_first, "each folder is listed before its contents");
    }

    // Depths, and a visitor that does not walk into a folder.
    int deepest = -1;
    size_t skipped_seen = 0;
    host_visit_tree(root, ec, [&](const HostDirEntry& e) {
        deepest = std::max(deepest, e.depth);
        if (e.path == deep) return PSXRecompV4::HostVisit::Skip;
        if (e.path.parent_path() == deep) ++skipped_seen;
        return PSXRecompV4::HostVisit::Continue;
    });
    check(!ec && skipped_seen == 0, "a skipped folder's contents are not visited");
    int levels = 0;
    for (fs::path p = deep; p != root; p = p.parent_path()) ++levels;
    check(deepest == levels - 1, "the deepest visited entry has the depth of the skipped folder");

    // A copy of the whole tree, compared entry for entry and byte for byte.
    const fs::path copy = root.parent_path() / (root.filename().string() + "-copy");
    PSXRecompV4::host_copy_tree(root, copy, ec);
    check(!ec, "host_copy_tree reports no error");
    std::set<std::string> copied;
    for (const HostDirEntry& e : host_walk_tree(copy, ec)) copied.insert(rel(copy, e.path));
    check(copied == want, "host_copy_tree copies exactly the tree's entries");
    {
        std::ifstream in(host_long_path(copy / rel(root, deep) / "b.bin"), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(bytes == "bbbbbbbb", "a deep copied file has the source's bytes");
    }
    std::error_code copy_rm_ec;
    check(host_remove_tree(copy, copy_rm_ec) == want.size() + 1 && !copy_rm_ec, "the copy is removed");

    // Removal within the limit. remove_all on this tree never returned.
    std::promise<std::uintmax_t> done;
    std::future<std::uintmax_t> removed = done.get_future();
    std::error_code rm_ec;
    std::thread remover([&] { done.set_value(host_remove_tree(root, rm_ec)); });
    if (removed.wait_for(std::chrono::seconds(20)) != std::future_status::ready) {
        std::cerr << "FAIL: host_remove_tree did not return within 20 seconds\n";
        std::fflush(nullptr);
        std::_Exit(1);
    }
    remover.join();
    const std::uintmax_t n = removed.get();
    check(!rm_ec, "host_remove_tree reports no error");
    check(n == want.size() + 1, "host_remove_tree counts every entry and the root");
    check(!fs::exists(host_long_path(root), ec), "nothing is left of the tree");
    check(host_remove_tree(root, rm_ec) == 0 && !rm_ec, "removing a missing tree is not an error");

    if (failures) return 1;
    std::cout << "host_dir_test: " << want.size() << " entries listed, walked and removed\n";
    return 0;
}
