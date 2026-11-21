#include "plus_services.h"
#include "counter_state.h"
#include "pocket_wifi_connection_model.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
using namespace plus_services;
namespace fs = std::filesystem;

int main(int argc, char** argv) {
    assert(argc == 2);
    const fs::path root(argv[1]);
    fs::create_directories(root / "text");
    fs::create_directories(root / "state");
    std::string error, text = "unchanged";
    TextStore store((root / "text").string(), "note", 12);
    assert(!store.Save("before load", &error));
    assert(store.Load(&text, &error) && text.empty());
    assert(store.Save(u8"中文\n", &error));
    assert(store.Saved() == u8"中文\n");
    assert(!store.Save(std::string(13, 'x'), &error));
    assert(!ValidText(std::string("\xc0\x80", 2), 10));
    assert(!ValidText(std::string("\xed\xa0\x80", 3), 10));
    assert(!ValidText(std::string("a\0b", 3), 10));
    assert(ValidText(u8"𠀀\t\n", 6));
    assert(store.Saved() == u8"中文\n");
    TextStore reloaded((root / "text").string(), "note", 12);
    assert(reloaded.Load(&text, &error) && text == u8"中文\n");
    assert(reloaded.Save(std::string("a\r\nb\x1a", 5), &error) == false);
    fs::rename(root / "text", root / "unmounted");
    assert(!reloaded.Save("must fail", &error));
    fs::rename(root / "unmounted", root / "text");
    TextStore recover((root / "text").string(), "note", 12);
    assert(recover.Load(&text, &error) && text == u8"中文\n");
    assert(recover.Save("external", &error));
    assert(!store.Save("stale", &error)); // does not overwrite another writer
    std::ofstream(root / "text" / "note.a.pkr", std::ios::binary | std::ios::trunc) << "bad-a";
    std::ofstream(root / "text" / "note.b.pkr", std::ios::binary | std::ios::trunc) << "bad-b";
    TextStore corrupt((root / "text").string(), "note", 12);
    assert(!corrupt.Load(&text, &error) && !corrupt.Save("", &error));
    assert(fs::file_size(root / "text" / "note.a.pkr") == 5);

    CounterState standalone((root / "state").string());
    assert(standalone.Load(&error) && standalone.Value() == 0);
    assert(standalone.Save(UINT64_MAX, &error));
    CounterState integrated((root / "state").string());
    assert(integrated.Load(&error) && integrated.Value() == UINT64_MAX);

    assert(pocket::RecordStore((root / "state").string()).Save("counter", 1, "").ok());
    CounterState empty_not_missing((root / "state").string());
    assert(!empty_not_missing.Load(&error));

    TaskFence tasks;
    std::atomic<bool> inside{false}, release{false}, freed{false};
    std::atomic<unsigned> published{0};
    struct Resource { std::atomic<bool>& flag; ~Resource() { flag.store(true); } };
    std::thread worker([&] {
        assert(RunTracked(tasks, [&](const TaskFence::Lease& lease) {
            Resource resource{freed};
            inside.store(true);
            while (!release.load()) std::this_thread::yield();
            assert(lease.Cancelled());
            assert(!tasks.CommitIfCurrent(lease, [&] { ++published; }));
        }));
    });
    while (!inside.load()) std::this_thread::yield();
    tasks.RequestStop();
    assert(!tasks.TryStart());
    assert(!tasks.WaitStopped(std::chrono::milliseconds(1)));
    assert(!tasks.Resume() && !freed.load());
    release.store(true);
    assert(tasks.WaitStopped(std::chrono::milliseconds(3000)) && freed.load());
    worker.join();
    assert(published == 0 && tasks.Resume());
    assert(RunTracked(tasks, [&](const TaskFence::Lease& lease) {
        assert(tasks.CommitIfCurrent(lease, [&] { ++published; }));
    }));
    bool saved = false;
    assert(!StopThenSave(tasks, std::chrono::milliseconds(1), [] { return false; }, [&] { saved = true; return true; }));
    assert(!saved);
    assert(StopThenSave(tasks, std::chrono::milliseconds(1), [] { return true; }, [&] { saved = true; return true; }));
    assert(saved && published == 1);

    std::string body = "keep";
    std::string input = u8"深圳+25°C"; std::size_t pos = 0;
    auto reader = [&](char* out, std::size_t) {
        if (pos == input.size()) return 0;
        *out = input[pos++]; return 1;
    };
    assert(ReadHttpBody(reader, [] { return true; }, [] { return false; }, 64, &body, &error));
    assert(body == input); // complete was true BEFORE the first read
    pos = 0; body = "keep";
    assert(!ReadHttpBody(reader, [] { return true; }, [] { return false; }, 2, &body, &error) && body == "keep");
    assert(!ReadHttpBody([](char*, std::size_t) { return 0; }, [] { return false; }, [] { return false; }, 8, &body, &error));
    assert(!ReadHttpBody([](char*, std::size_t) { return -1; }, [] { return true; }, [] { return false; }, 8, &body, &error));
    assert(!ReadHttpBody(reader, [] { return true; }, [] { return true; }, 8, &body, &error));
    assert(!ReadHttpBody([](char*, std::size_t) { return 0; }, [] { return true; }, [] { return false; }, 8, &body, &error));
    std::size_t dma = 0;
    assert(DmaBytes(6, 256, 2, 4, &dma) && dma == 12288);
    assert(!DmaBytes(SIZE_MAX, 2, 1, 1, &dma));
    assert(HasDmaBudget({47107, 38912, 0, 0}, 12288, 2048, 12000));
    assert(!HasDmaBudget({47107, 1024, 0, 0}, 12288, 2048, 0));
    assert(MicSlotToPcm16(INT32_MIN) == -32767);
    assert(MicSlotToPcm16(INT32_MAX) == 32767);
    assert(MicSlotToPcm16(-1) == -1);
    assert(Pcm16ToPlaybackSlot(100, 25) == 409600);
    assert(Pcm16ToPlaybackSlot(-32768, 100) == INT32_MIN);
    std::vector<unsigned char> received;
    const unsigned char samples[] = {1, 2, 3, 4, 5};
    auto writer = [&](const unsigned char* data, std::size_t bytes, std::size_t* n) {
        *n = bytes > 2 ? 2 : bytes; received.insert(received.end(), data, data + *n); return true;
    };
    assert(WriteAllPcm(writer, [] { return false; }, samples, sizeof(samples)) && received.size() == 5);
    assert(!WriteAllPcm([](const unsigned char*, std::size_t, std::size_t* n) { *n = 0; return true; },
                       [] { return false; }, samples, sizeof(samples)));
    assert(CompositeLineArt(0, 1, true) == 0);
    assert(CompositeLineArt(0, 0, false) == 255);
    const std::uint8_t old[] = {100, 0, 255}, next[] = {100, 255, 0}, gray[] = {100, 120, 255};
    assert(ChangedPixelsAreBinary(old, next, 3) && !ChangedPixelsAreBinary(old, gray, 3));
    assert(CompositeGray4(0, 0, false) == 15 && CompositeGray4(15, 128, true) == 0);
    const std::uint8_t old4[] = {6, 0, 15}, next4[] = {6, 15, 0};
    assert(ChangedPixelsAreBinary(old4, next4, 3, 15));
    FrameReceipt frame;
    assert(frame.Submitted(4) && !frame.Submitted(5));
    assert(!frame.Complete(3, true) && !frame.Complete(4, false));
    assert(frame.Complete(4, true) && !frame.Submitted(4) && frame.Submitted(6));
    using namespace pocket::wifi_detail;
    assert(UninitializedRadioIsStopped(true, true, false, false));
    assert(!UninitializedRadioIsStopped(false, true, false, false)); // unknown driver error
    assert(!UninitializedRadioIsStopped(true, false, false, false)); // unknown AP error
    assert(!UninitializedRadioIsStopped(true, true, true, false));
    assert(!UninitializedRadioIsStopped(true, true, false, true));
    assert(AcceptLinkEvent(true, 2, 2, false));
    assert(!AcceptLinkEvent(false, 2, 2, false));
    assert(!AcceptLinkEvent(true, 1, 2, false));
    assert(!AcceptLinkEvent(true, 2, 2, true));
    assert(!LinkReady(true, false, true, true, "a", "a"));
    assert(!LinkReady(true, true, true, true, "a", "b"));
    assert(LinkReady(true, true, true, true, "a", "a"));
    assert(DisplayableSsid(u8"中文网络") && !DisplayableSsid(std::string("bad\0ssid", 8)));
    const auto points = VisibleAccessPoints({{"a", -80, true}, {"a", -40, true}, {"b", -50, false}});
    assert(points.size() == 2 && points[0].ssid == "a" && points[0].rssi == -40);
    std::cout << "PASS durable UTF8 state, no destructive recovery, dual-mode shared root, TaskFence ownership/cancel/timeout, HTTP cache drain, resource budget, PCM/partial write, physical frame receipts, WiFi link model\n";
}

