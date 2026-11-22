// Exercise the HyperX Pulsefire Dart driver with a simulated HID transport; no hardware access.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {
std::vector<std::vector<BYTE>> replies;  // input reports queued for the next Read() calls
std::vector<std::vector<BYTE>> writes;
int opens = 0;

HidColl Collection(USHORT page = 0xFF00, USHORT inLen = 64, USHORT outLen = 64, USHORT pid = 0x16E1) {
    HidColl c;
    c.path = L"hyperx-dart";
    c.vid = 0x0951;
    c.pid = pid;
    c.caps.UsagePage = page;
    c.caps.Usage = 0x0001;
    c.caps.InputReportByteLength = inLen;
    c.caps.OutputReportByteLength = outLen;
    return c;
}

// Heartbeat reply as Read() hands it to the driver: 51 00 00 03, battery at [4], charging at [5].
std::vector<BYTE> Heartbeat(BYTE percent, BYTE charging = 0) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x51;
    r[3] = 0x03;
    r[4] = percent;
    r[5] = charging;
    r[6] = 0x0F;
    return r;
}

// The other heartbeat reply a real 16E1 dongle sends for minutes at a time (no level): 51 00 02 EC 05 00 ...
std::vector<BYTE> NoLevel() {
    std::vector<BYTE> r(64, 0);
    r[0] = 0x51;
    r[2] = 0x02;
    r[3] = 0xEC;
    r[4] = 0x05;
    return r;
}

PollContext Poll(const HidColl& c, std::vector<std::vector<BYTE>> queued) {
    replies = std::move(queued);
    writes.clear();
    PollContext ctx;
    ctx.colls = { c };
    PollHyperX(ctx);
    return ctx;
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
    writes.push_back(data);
    return true;
}
int HidDev::Read(std::vector<BYTE>& out, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    if (replies.empty()) return -1;
    out = replies.front();
    replies.erase(replies.begin());
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
    const std::vector<BYTE> kHeartbeat = { 0x00, 0x51 };  // report ID 0, command 0x51

    // Level and name; the query is the read-only heartbeat and nothing else.
    auto ctx = Poll(Collection(), { Heartbeat(73) });
    assert(writes.size() == 1 && writes[0] == kHeartbeat);
    assert(ctx.out.size() == 1);
    assert(ctx.out[0].key == L"hyperx:16E1" && ctx.out[0].name == L"HyperX Pulsefire Dart");
    assert(ctx.out[0].kind == DeviceKind::Mouse && ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].percent == 73 && !ctx.out[0].charging && ctx.out[0].connected);
    assert(ctx.IsClaimed(0x0951, 0x16E1));

    // The collection exactly as a real 16E1 dongle lists it on Windows: MI_02, FF00:0001, 65-byte reports.
    auto real = Collection(0xFF00, 65, 65);
    real.iface = 2;
    real.caps.FeatureReportByteLength = 65;
    ctx = Poll(real, { Heartbeat(74) });
    assert(writes.size() == 1 && writes[0] == kHeartbeat);
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 74 && ctx.out[0].name == L"HyperX Pulsefire Dart");

    // Charging flag at [5].
    ctx = Poll(Collection(), { Heartbeat(100, 1) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && ctx.out[0].charging);

    // Unrelated input reports before the reply are skipped.
    std::vector<BYTE> noise(64, 0);
    noise[0] = 0xFF;
    ctx = Poll(Collection(), { noise, noise, Heartbeat(12) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 12);

    // The 51 00 02 EC 05 reply has no level: its 05 must not show as 5%. The mouse stays listed as asleep with an
    // unknown level, so the tray keeps the last reading.
    ctx = Poll(Collection(), { NoLevel() });
    assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent < 0 && !ctx.out[0].charging);
    assert(ctx.out[0].connected && ctx.IsClaimed(0x0951, 0x16E1));

    // Only the exact 00 00 03 header carries a level: any other value at [1], [2] or [3] does not.
    for (int i : { 1, 2, 3 }) {
        auto r = Heartbeat(60);
        r[i] ^= 0x01;
        ctx = Poll(Collection(), { r });
        assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent < 0);
    }

    // A level above 100 is not a level either: listed as asleep with an unknown level.
    ctx = Poll(Collection(), { Heartbeat(101) });
    assert(ctx.out.size() == 1 && ctx.out[0].asleep && ctx.out[0].percent < 0);
    ctx = Poll(Collection(), { Heartbeat(100) });
    assert(ctx.out.size() == 1 && !ctx.out[0].asleep && ctx.out[0].percent == 100);

    // No reply: nothing listed, nothing claimed.
    ctx = Poll(Collection(), {});
    assert(ctx.out.empty() && ctx.claimed.empty());

    // A reply too short to hold the charging byte is not a heartbeat (r[5] must never be read past the end).
    ctx = Poll(Collection(), { { 0x51, 0x00, 0x00, 0x00, 50 }, { 0x51 } });
    assert(ctx.out.empty() && ctx.claimed.empty());

    // Wrong collections are never opened: not a vendor page, short reports, or another product (the 16E2 cable).
    opens = 0;
    for (const auto& c : { Collection(0x0001), Collection(0xFF13), Collection(0xFF00, 4, 64), Collection(0xFF00, 64, 1),
                           Collection(0xFF00, 64, 64, 0x16E2) }) {
        ctx = Poll(c, { Heartbeat(70) });
        assert(ctx.out.empty() && writes.empty());
    }
    assert(opens == 0);

    // A claimed product is skipped.
    PollContext claimed;
    claimed.colls = { Collection() };
    claimed.Claim(0x0951, 0x16E1);
    replies = { Heartbeat(70) };
    PollHyperX(claimed);
    assert(claimed.out.empty() && opens == 0);

    std::puts("HyperX Pulsefire Dart driver tests passed "
              "(level, charging, noise, no-level reply, header check, invalid level, no reply, short reply, "
              "filtering).");
}
