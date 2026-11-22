// Exercise the HyperX Pulsefire Haste 2 driver with a simulated HID transport; no hardware access.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>
#include <map>

namespace {
using Reports = std::vector<std::vector<BYTE>>;

// Scripted dongle: the reports queued when a request (command byte, sub byte) is written.
std::map<std::pair<BYTE, BYTE>, Reports> script;
Reports pending;
std::vector<std::vector<BYTE>> writes;
int opens = 0;

HidColl Collection(USHORT pid, USHORT page, USHORT inLen = 64, USHORT outLen = 64) {
    HidColl c;
    c.path = L"hyperx";
    c.vid = 0x03F0;
    c.pid = pid;
    c.caps.UsagePage = page;
    c.caps.Usage = 0x0001;
    c.caps.InputReportByteLength = inLen;
    c.caps.OutputReportByteLength = outLen;
    return c;
}

// 64-byte input report as Read() hands it to the driver.
std::vector<BYTE> Report(BYTE id, BYTE sub, std::initializer_list<std::pair<int, BYTE>> bytes = {}) {
    std::vector<BYTE> r(64, 0);
    r[0] = id;
    r[1] = sub;
    for (auto [i, v] : bytes) r[i] = v;
    return r;
}
std::vector<BYTE> Link(BYTE linked) { return Report(0x53, 0x01, { { 2, linked } }); }
std::vector<BYTE> Battery(BYTE percent, BYTE charge = 0) { return Report(0x51, 0x02, { { 2, percent }, { 3, charge } }); }
// Reports the dongle sends on its own after a reconnect; the driver must skip them.
const std::vector<BYTE> kNoise1 = Report(0x44, 0x01), kNoise2 = Report(0x45, 0x02), kNoise3 = Report(0xFB, 0x01, { { 2, 1 } });

const std::vector<BYTE> kAskLink = { 0x52, 0x01 };
const std::vector<BYTE> kAskBattery = { 0x50, 0x02 };

PollContext Poll(const HidColl& c, std::map<std::pair<BYTE, BYTE>, Reports> replies) {
    script = std::move(replies);
    pending.clear();
    writes.clear();
    PollContext ctx;
    ctx.colls = { c };
    PollHyperX(ctx);
    return ctx;
}
std::map<std::pair<BYTE, BYTE>, Reports> Dongle(const std::vector<BYTE>& link, const std::vector<BYTE>& battery) {
    return { { { 0x52, 0x01 }, { kNoise1, kNoise2, link } }, { { 0x50, 0x02 }, { kNoise3, battery } } };
}
}  // namespace

// Stand-ins for the Win32 HID transport. The production driver is linked unchanged.
bool HidDev::Open(const HidColl& c) {
    c_ = c;
    h_ = reinterpret_cast<HANDLE>(1);
    overlapped_ = true;
    ++opens;
    return true;
}
void HidDev::Close() { h_ = INVALID_HANDLE_VALUE; }
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    assert(data.size() == 2);
    writes.push_back(data);
    auto it = script.find({ data[0], data[1] });
    if (it != script.end()) pending.insert(pending.end(), it->second.begin(), it->second.end());
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    if (pending.empty()) return -1;
    out = pending.front();
    pending.erase(pending.begin());
    return (int)out.size();
}
// The Stinger Core path of the same file; never reached by these tests.
bool HidDev::SetFeature(const std::vector<BYTE>&) { return false; }
bool HidDev::GetFeature(BYTE, std::vector<BYTE>&) { return false; }
bool HidDev::GetInputReport(BYTE, std::vector<BYTE>&) { return false; }
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
int Interpolate(int, const int*, const int*, int) { return 0; }

int main() {
    const auto core = Collection(0x0AB5, 0xFF00);

    // Link up and a battery reply: level, name, kind and claim. Unrelated reports before the replies are skipped.
    auto ctx = Poll(core, Dongle(Link(1), Battery(70)));
    assert((writes == std::vector<std::vector<BYTE>>{ kAskLink, kAskBattery }));
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].key == L"hyperx:0AB5" && ctx.out[0].name == L"HyperX Pulsefire Haste 2 Core Wireless");
    assert(ctx.out[0].kind == DeviceKind::Mouse && ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].connected && ctx.out[0].percent == 70 && !ctx.out[0].charging);
    assert(ctx.IsClaimed(0x03F0, 0x0AB5));

    // Charge status 1 (charging) and 2 (full, still on the charger) both count as charging; 0 and 3 do not.
    ctx = Poll(core, Dongle(Link(1), Battery(55, 1)));
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 55 && ctx.out[0].charging);
    ctx = Poll(core, Dongle(Link(1), Battery(100, 2)));
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].charging);
    ctx = Poll(core, Dongle(Link(1), Battery(40, 3)));
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 40 && !ctx.out[0].charging);

    // Voltage bytes 6/7 are read when the mouse sends them (little endian); the Core sends zero.
    ctx = Poll(core, Dongle(Link(1), Report(0x51, 0x02, { { 2, 80 }, { 6, 0xA4 }, { 7, 0x0F } })));
    assert(ctx.out.size() == 1 && ctx.out[0].millivolts == 0x0FA4);

    // After a reconnect the link reply can be missing: the battery reply decides.
    ctx = Poll(core, { { { 0x50, 0x02 }, { kNoise1, kNoise3, Battery(70) } } });
    assert((writes == std::vector<std::vector<BYTE>>{ kAskLink, kAskBattery }));
    assert(ctx.out.size() == 1 && ctx.out[0].connected && ctx.out[0].percent == 70);

    // A battery level above 100 is not a level: the mouse is listed with an unknown level.
    ctx = Poll(core, Dongle(Link(1), Battery(101)));
    assert(ctx.out.size() == 1 && ctx.out[0].connected && ctx.out[0].percent < 0);

    // The other Haste 2 Wireless dongles use the same protocol and their own name.
    ctx = Poll(Collection(0x0D97, 0xFF00), Dongle(Link(1), Battery(33)));
    assert(ctx.out.size() == 1 && ctx.out[0].name == L"HyperX Pulsefire Haste 2 Wireless" && ctx.out[0].percent == 33);
    ctx = Poll(Collection(0x0F98, 0xFF00), Dongle(Link(1), Battery(34)));
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"hyperx:0F98" && ctx.out[0].percent == 34);
    // 0F98 exactly as reported from hardware in PR #312: MI_02, FF00:0001, 64-byte input/output/feature reports.
    auto haste2 = Collection(0x0F98, 0xFF00);
    haste2.iface = 2;
    haste2.caps.FeatureReportByteLength = 64;
    ctx = Poll(haste2, Dongle(Link(1), Battery(86, 1)));
    assert((writes == std::vector<std::vector<BYTE>>{ kAskLink, kAskBattery }));
    assert(ctx.out.size() == 1 && ctx.out[0].name == L"HyperX Pulsefire Haste 2 Wireless");
    assert(ctx.out[0].kind == DeviceKind::Mouse && ctx.out[0].percent == 86 && ctx.out[0].charging);
    assert(ctx.IsClaimed(0x03F0, 0x0F98));
    ctx = Poll(Collection(0x0DA0, 0xFF90), Dongle(Link(1), Battery(21)));
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"hyperx:0DA0" && ctx.out[0].name == L"HyperX Pulsefire Haste 2 Mini" &&
           ctx.out[0].percent == 21);

    // Original Haste Wireless uses an unnumbered output report, not the Haste 2 GET (50 02).
    // Response bytes 4/5 contain percent and charging status; only 00 51 may be sent.
    auto original = Collection(0x028E, 0xFF00, 65, 65);
    auto originalReply = Report(0x51, 0, { { 4, 73 }, { 5, 0 } });
    ctx = Poll(original, { { { 0x00, 0x51 }, { kNoise1, originalReply } } });
    assert((writes == std::vector<std::vector<BYTE>>{ { 0x00, 0x51 } }));
    assert(ctx.out.size() == 1 && ctx.out[0].name == L"HyperX Pulsefire Haste Wireless");
    assert(ctx.out[0].key == L"hyperx:028E" && ctx.out[0].kind == DeviceKind::Mouse);
    assert(ctx.out[0].via == L"2.4 GHz receiver" && ctx.out[0].percent == 73 && !ctx.out[0].charging);
    assert(ctx.IsClaimed(0x03F0, 0x028E));
    originalReply = Report(0x51, 0, { { 4, 64 }, { 5, 1 } });
    ctx = Poll(Collection(0x048E, 0xFF00, 65, 65), { { { 0x00, 0x51 }, { originalReply } } });
    assert(ctx.out.size() == 1 && ctx.out[0].via == L"USB cable");
    assert(ctx.out[0].percent == 64 && ctx.out[0].charging && !ctx.out[0].full);
    originalReply = Report(0x51, 0, { { 4, 100 }, { 5, 2 } });
    ctx = Poll(original, { { { 0x00, 0x51 }, { originalReply } } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].full);
    for (BYTE invalid : { (BYTE)0, (BYTE)0xFF }) {
        originalReply = Report(0x51, 0, { { 4, invalid }, { 5, 1 } });
        ctx = Poll(original, { { { 0x00, 0x51 }, { originalReply } } });
        assert(ctx.out.size() == 1 && ctx.out[0].percent < 0 && !ctx.out[0].charging);
    }
    ctx = Poll(original, {});
    assert(ctx.out.empty() && !ctx.IsClaimed(0x03F0, 0x028E));
    ctx = Poll(Collection(0x028E, 0x0001, 65, 65), { { { 0x00, 0x51 }, { Report(0x51, 0, { { 4, 50 } }) } } });
    assert(ctx.out.empty() && writes.empty());

    // No reply at all (dongle without a mouse): nothing listed, nothing claimed.
    ctx = Poll(core, {});
    assert(ctx.out.empty() && ctx.claimed.empty());
    assert((writes == std::vector<std::vector<BYTE>>{ kAskLink, kAskBattery }));

    // Collections that are not vendor-page, or have short reports, are never opened. A claimed product is skipped.
    opens = 0;
    for (const auto& c : { Collection(0x0AB5, 0x0001), Collection(0x0AB5, 0xFF00, 4, 64), Collection(0x0AB5, 0xFF00, 64, 1) }) {
        ctx = Poll(c, Dongle(Link(1), Battery(70)));
        assert(ctx.out.empty() && writes.empty());
    }
    PollContext claimed;
    claimed.colls = { core };
    claimed.Claim(0x03F0, 0x0AB5);
    script = Dongle(Link(1), Battery(70));
    PollHyperX(claimed);
    assert(claimed.out.empty() && opens == 0);

    // Link down: the mouse is listed as not connected with an unknown level, and no battery query is sent.
    ctx = Poll(core, Dongle(Link(0), Battery(70)));
    assert((writes == std::vector<std::vector<BYTE>>{ kAskLink }));
    assert(ctx.out.size() == 1 && !ctx.out[0].connected && ctx.out[0].percent < 0);
    assert(ctx.IsClaimed(0x03F0, 0x0AB5));

    // Link up again right after: the mouse reports about 40% for a few seconds after power-on, so a reading
    // less than 6 s after the link came up stays unknown and the tray keeps the last value.
    ctx = Poll(core, Dongle(Link(1), Battery(40)));
    assert(ctx.out.size() == 1 && ctx.out[0].connected && ctx.out[0].percent < 0);

    std::puts("HyperX Haste original and Haste 2 driver tests passed (levels, charging, link states, malformed and missing replies, strict matching).");
}
