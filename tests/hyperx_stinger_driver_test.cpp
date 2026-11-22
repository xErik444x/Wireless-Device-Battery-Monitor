// Exercise the Stinger Core driver with scripted feature reports, without accessing hardware.
#include "../src/drivers/drivers.h"

#include <cassert>
#include <cstdio>

namespace {
using Reports = std::vector<std::vector<BYTE>>;
Reports linkReplies, batteryReplies, pending, writes;
int opens = 0, reads = 0;

HidColl Collection(USHORT vid = 0x03F0, USHORT pid = 0x0A8E) {
    HidColl c;
    c.path = L"stinger";
    c.vid = vid;
    c.pid = pid;
    c.iface = 3;
    c.caps.UsagePage = 0xFF00;
    c.caps.Usage = 1;
    c.caps.InputReportByteLength = c.caps.OutputReportByteLength = c.caps.FeatureReportByteLength = 64;
    return c;
}
std::vector<BYTE> Reply(BYTE type, BYTE command, BYTE code, int mv) {
    std::vector<BYTE> r(64, 0);
    r[0] = 0xFF;
    r[1] = type;
    r[2] = command;
    r[9] = code;
    r[11] = (BYTE)mv;
    r[12] = (BYTE)(mv >> 8);
    return r;
}
auto Link(bool connected) { return Reply(1, 0, 0, connected ? 1 : 0); }
auto Battery(BYTE code = 3, int mv = 3954) { return Reply(0x12, 5, code, mv); }

PollContext Poll(const HidColl& c, Reports links, Reports batteries) {
    linkReplies = std::move(links);
    batteryReplies = std::move(batteries);
    pending.clear(); writes.clear(); opens = reads = 0;
    PollContext ctx;
    ctx.colls = { c };
    PollHyperX(ctx);
    return ctx;
}
}

bool HidDev::Open(const HidColl& c) { c_ = c; ++opens; return true; }
void HidDev::Close() {}
bool HidDev::Write(const std::vector<BYTE>&, int) { assert(false); return false; }
int HidDev::Read(std::vector<BYTE>&, int) { assert(false); return -1; }
bool HidDev::SetFeature(const std::vector<BYTE>& data) {
    static const std::vector<BYTE> connection = {
        0xFF, 7, 0, 0xFD, 4, 0, 0, 2, 0x82, 3, 5, 0x29, 0x0F, 0, 0, 3, 0x68, 0x10, 0, 0 };
    static const std::vector<BYTE> battery = { 0xFF, 7, 0, 0xFD, 4, 0x0C, 0xF1, 2, 1, 4, 0xF0, 0x0C };
    assert(data == connection || data == battery);  // only existing read-state requests
    writes.push_back(data);
    pending = data == connection ? linkReplies : batteryReplies;
    return true;
}
bool HidDev::GetFeature(BYTE id, std::vector<BYTE>& out) {
    assert(id == 0xFF);
    ++reads;
    if (pending.empty()) return false;
    out = pending.front();
    pending.erase(pending.begin());
    return true;
}
bool HidDev::GetInputReport(BYTE, std::vector<BYTE>&) { return false; }
std::wstring Hex4(USHORT v) {
    wchar_t text[8];
    swprintf(text, 8, L"%04X", v);
    return text;
}
int Clamp100(int v) { return v < 0 ? 0 : v > 100 ? 100 : v; }
int Interpolate(int mv, const int* volts, const int* percent, int count) {
    // These tests use exact curve knots to check that the right curve is selected.
    for (int i = 0; i < count; ++i) if (mv == volts[i]) return percent[i];
    assert(false);
    return -1;
}

int main() {
    const auto dts = Collection();
    auto ctx = Poll(dts, Reports(10, Link(true)), { Link(true), Battery() });
    assert(ctx.out.size() == 1 && ctx.IsClaimed(0x03F0, 0x0A8E));
    const auto& s = ctx.out[0];
    assert(s.key == L"hyperx:0A8E" && s.name == L"HyperX Cloud Stinger Core Wireless DTS");
    assert(s.kind == DeviceKind::Headset && s.connected && !s.charging && s.percent == 80 && s.millivolts == 3954);
    assert(writes.size() == 2 && reads == 12);  // skips stale battery reply, never resends a command

    ctx = Poll(dts, { Link(true) }, { Battery(5, 4050) });
    assert(ctx.out.size() == 1 && ctx.out[0].charging && ctx.out[0].percent == 80);
    ctx = Poll(dts, { Link(true) }, { Battery(6, 4130) });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 100 && !ctx.out[0].charging);

    ctx = Poll(dts, Reports(10, Link(false)), { Battery() });
    assert(ctx.out.size() == 1 && !ctx.out[0].connected && ctx.out[0].percent == -1 && writes.size() == 1);

    // Wrong header, stale replies, invalid charge and short packets must never become battery readings.
    for (const auto& invalid : { Link(true), Reply(0x12, 6, 3, 3954), Reply(0x11, 5, 3, 3954),
                                Battery(0xFF), std::vector<BYTE>{0xFF, 0x12, 5} }) {
        ctx = Poll(dts, { Link(true) }, Reports(12, invalid));
        assert(ctx.out.size() == 1 && ctx.out[0].percent == -1 && ctx.out[0].millivolts == 0);
        assert(reads <= 12);  // bounded retry budget, even if unrelated reports keep arriving
    }
    ctx = Poll(dts, {}, {});
    assert(ctx.out.empty() && !ctx.IsClaimed(0x03F0, 0x0A8E));

    // Consumer controls and the two-byte microphone collection must not receive battery commands.
    for (USHORT page : { (USHORT)0x000C, (USHORT)0xFFC0 }) {
        auto wrong = dts; wrong.caps.UsagePage = page;
        ctx = Poll(wrong, { Link(true) }, { Battery() });
        assert(opens == 0 && writes.empty() && ctx.out.empty());
    }
    auto wrong = dts; wrong.caps.Usage = 2;
    ctx = Poll(wrong, {}, {}); assert(opens == 0);
    wrong = dts; wrong.caps.FeatureReportByteLength = 2;
    ctx = Poll(wrong, {}, {}); assert(opens == 0);
    ctx = Poll(Collection(0x03F0, 0x0591), {}, {}); assert(opens == 0); // unrelated HP keyboard
    ctx = Poll(Collection(0x0951, 0x0A8E), {}, {}); assert(opens == 0); // exact VID/PID matching

    PollContext claimed; claimed.colls = { dts }; claimed.Claim(dts.vid, dts.pid);
    opens = 0; PollHyperX(claimed); assert(opens == 0 && claimed.out.empty());

    // Original Kingston receiver (0951:170B) also returns stale FF 01 00 replies before FF 12 05 arrives.
    const auto kingston = Collection(0x0951, 0x170B);
    auto staleConnDischarge = Reply(1, 0, 3, 0x0E01);  // stale connection buffer with residual 0x0E high byte (3585 mV / 5%)
    auto staleConnCharging = Reply(1, 0, 1, 0x0E01);   // stale connection buffer with non-battery code 0x01
    auto connOn = Reply(0x0C, 5, 3, 0x0E01);
    auto connOff = Reply(0x0C, 5, 3, 0x0E03);
    ctx = Poll(kingston, { Battery(3, 3954), connOn }, { staleConnDischarge, Battery(3, 3838) });
    assert(ctx.out.size() == 1 && ctx.out[0].connected && !ctx.out[0].charging);
    assert(ctx.out[0].percent == 60 && ctx.out[0].millivolts == 3838 && reads == 4);
    ctx = Poll(kingston, { connOn }, { staleConnCharging, Battery(5, 4000) });
    assert(ctx.out.size() == 1 && ctx.out[0].connected && ctx.out[0].charging);
    assert(ctx.out[0].percent == 70 && ctx.out[0].millivolts == 4000 && reads == 3);
    ctx = Poll(kingston, { connOff }, { Battery() });
    assert(ctx.out.size() == 1 && !ctx.out[0].connected && ctx.out[0].percent == -1 && writes.size() == 1);
    ctx = Poll(kingston, { connOn }, Reports(10, staleConnDischarge));
    assert(ctx.out.size() == 1 && ctx.out[0].connected && ctx.out[0].percent == -1 && ctx.out[0].millivolts == 0);
    ctx = Poll(kingston, { connOn }, Reports(10, Reply(0, 0, 3, 3954)));
    assert(ctx.out.size() == 1 && ctx.out[0].connected && ctx.out[0].percent == -1 && ctx.out[0].millivolts == 0);

    for (const auto& ps : { Collection(0x03F0, 0x0A93), Collection(0x0951, 0x1713) }) {
        ctx = Poll(ps, { Link(true) }, { Link(true), Battery() });
        assert(ctx.out.size() == 1 && ctx.out[0].percent == 80);
        assert(ctx.out[0].name == L"HyperX Cloud Stinger Core Wireless");
    }
    std::puts("HyperX Stinger Core tests passed (DTS, Kingston 170B, charging, stale/invalid replies, filtering).");
}
