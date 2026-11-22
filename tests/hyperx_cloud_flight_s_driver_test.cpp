// Regression tests for HyperX Cloud Flight S (0951:16EA/16EB), issue #348.
// The production HyperX driver is linked unchanged against a scripted HID transport.
// No physical headset is required.
#include "../src/drivers/drivers.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cwchar>
#include <map>
#include <utility>

namespace {
using Reports = std::vector<std::vector<BYTE>>;
std::map<BYTE, Reports> script;
Reports pending;
std::vector<std::vector<BYTE>> writes;
int opens = 0;
// Cloud II Wireless (Kingston 0951:1718) behaviour observed on hardware: only the FF13:0001 collection
// answers, a write only goes through after a report-6 input report read on the same handle, and each
// handle accepts a single write.
bool cloud2Kingston = false;
bool vendorCollection = false;
bool primed = false;
int writesOnHandle = 0;
int primes = 0;

HidColl Collection(USHORT pid, USHORT page = 0xFF13, USHORT usage = 0x0001,
                   USHORT inLen = 62, USHORT outLen = 62, USHORT vid = 0x0951) {
    HidColl c;
    c.path = L"cloud-flight-s";
    c.vid = vid;
    c.pid = pid;
    c.iface = 5;
    c.caps.UsagePage = page;
    c.caps.Usage = usage;
    c.caps.InputReportByteLength = inLen;
    c.caps.OutputReportByteLength = outLen;
    return c;
}

std::vector<BYTE> Reply(BYTE cmd, BYTE value, bool validHeader = true) {
    std::vector<BYTE> r(62, 0);
    r[0] = validHeader ? 0x0B : 0x09;
    r[2] = 0xBB;
    r[3] = cmd;
    r[cmd == 0x02 ? 7 : 4] = value;
    return r;
}

PollContext Poll(const std::vector<HidColl>& colls, std::map<BYTE, Reports> replies) {
    script = std::move(replies);
    pending.clear();
    writes.clear();
    opens = 0;
    primed = false;
    writesOnHandle = 0;
    primes = 0;
    PollContext ctx;
    ctx.colls = colls;
    PollHyperX(ctx);
    return ctx;
}
PollContext Poll(const HidColl& c, std::map<BYTE, Reports> replies) {
    return Poll(std::vector<HidColl>{ c }, std::move(replies));
}
std::map<BYTE, Reports> Device(BYTE pct, BYTE charging = 0) {
    return { { 0x02, { Reply(0x02, pct) } }, { 0x03, { Reply(0x03, charging) } } };
}

void CheckRequests() {
    assert(writes.size() == 2);
    for (size_t i = 0; i < writes.size(); i++) {
        const auto& r = writes[i];
        const std::vector<BYTE> prefix = { 0x06, 0x00, 0x02, 0x00, 0x9A, 0x00, 0x00, 0x68,
                                           0x4A, 0x8E, 0x0A, 0x00, 0x00, 0x00, 0xBB };
        assert(r.size() == 17);
        assert(std::equal(prefix.begin(), prefix.end(), r.begin()));
        assert(r[15] == (i == 0 ? 0x02 : 0x03));
        assert(r[16] == 0);
    }
}
}  // namespace

// Scripted HID layer. No Win32 handle is opened and no device settings are changed.
bool HidDev::Open(const HidColl& c) {
    ++opens;
    cloud2Kingston = c.vid == 0x0951 && c.pid == 0x1718;
    vendorCollection = c.page() == 0xFF13 && c.usage() == 0x0001;
    primed = false;
    writesOnHandle = 0;
    return true;
}
void HidDev::Close() {}
bool HidDev::GetInputReport(BYTE reportId, std::vector<BYTE>& out) {
    if (reportId != 0x06 || (cloud2Kingston && !vendorCollection)) return false;
    ++primes;
    primed = true;
    out.assign(62, 0);
    return true;
}
bool HidDev::Write(const std::vector<BYTE>& r, int) {
    assert(r.size() == 17 && r[14] == 0xBB);
    if (cloud2Kingston && (!vendorCollection || !primed || writesOnHandle > 0)) return false;
    ++writesOnHandle;
    writes.push_back(r);
    auto it = script.find(r[15]);
    if (it != script.end()) pending.insert(pending.end(), it->second.begin(), it->second.end());
    return true;
}
int HidDev::Read(std::vector<BYTE>& r, int timeoutMs) {
    assert(timeoutMs > 0 && timeoutMs <= 1000);
    if (pending.empty()) return -1;
    r = pending.front();
    pending.erase(pending.begin());
    return (int)r.size();
}
bool HidDev::SetFeature(const std::vector<BYTE>&) { return false; }
bool HidDev::GetFeature(BYTE, std::vector<BYTE>&) { return false; }
std::wstring Hex4(USHORT v) {
    wchar_t b[8];
    swprintf(b, 8, L"%04X", v);
    return b;
}
int Clamp100(int x) { return x < 0 ? 0 : x > 100 ? 100 : x; }
int Interpolate(int, const int*, const int*, int) { return 0; }

int main() {
    const auto flightS = Collection(0x16EA);

    // Exact receiver from issue #348: interface 5, FF13:0001, 62/62 reports.
    auto ctx = Poll(flightS, Device(74, 1));
    CheckRequests();
    assert(opens == 1 && ctx.out.size() == 1);
    assert(ctx.out[0].key == L"hyperx:16EA");
    assert(ctx.out[0].name == L"HyperX Cloud Flight S");
    assert(ctx.out[0].kind == DeviceKind::Headset && ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].percent == 74 && ctx.out[0].charging);
    assert(ctx.IsClaimed(0x0951, 0x16EA));

    // Alternate VID:PID from the HyperHeadset device table.
    ctx = Poll(Collection(0x16EB), Device(41));
    CheckRequests();
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"hyperx:16EB");
    assert(ctx.out[0].name == L"HyperX Cloud Flight S");
    assert(ctx.out[0].percent == 41 && !ctx.out[0].charging);

    // Windows enumerates two Consumer Control collections as well as FF13.
    // Only the vendor control collection should be opened; do not double-count a headset.
    ctx = Poll({ Collection(0x16EA, 0x000C), Collection(0x16EA, 0x000C, 1, 2, 0),
                 flightS, flightS }, Device(88));
    assert(opens == 1 && ctx.out.size() == 1 && ctx.out[0].percent == 88);
    CheckRequests();

    // Unrelated/stale input reports must not be interpreted as a battery reply.
    ctx = Poll(flightS, { { 0x02, { Reply(0x03, 1), Reply(0x02, 60, false), Reply(0x02, 65) } },
                           { 0x03, { Reply(0x03, 0) } } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 65);
    CheckRequests();

    // A missing charging response does not invalidate a valid battery percentage.
    ctx = Poll(flightS, { { 0x02, { Reply(0x02, 52) } } });
    assert(ctx.out.size() == 1 && ctx.out[0].percent == 52 && !ctx.out[0].charging);
    CheckRequests();

    // No battery reply, out-of-range percentage or malformed response: don't claim a headset.
    ctx = Poll(flightS, {});
    assert(ctx.out.empty() && ctx.claimed.empty() && writes.size() == 1);
    ctx = Poll(flightS, { { 0x02, { Reply(0x02, 101) } } });
    assert(ctx.out.empty() && ctx.claimed.empty());
    ctx = Poll(flightS, { { 0x02, { Reply(0x02, 80, false) } } });
    assert(ctx.out.empty() && ctx.claimed.empty());

    // Strict matching prevents writes to the unrelated HID interfaces or unknown hardware.
    for (const auto& c : { Collection(0x16EA, 0x000C), Collection(0x16EA, 0xFF13, 2),
                           Collection(0x16EA, 0xFF13, 1, 61, 62), Collection(0x16EA, 0xFF13, 1, 62, 61),
                           Collection(0x16EA, 0xFF13, 1, 62, 62, 0x03F0),
                           Collection(0x16EC) }) {
        ctx = Poll(c, Device(83));
        assert(ctx.out.empty() && ctx.claimed.empty());
        assert(opens == 0 && writes.empty());
    }

    // Already claimed by a higher-priority driver: do not open it again.
    script = Device(70);
    pending.clear();
    writes.clear();
    opens = 0;
    PollContext claimed;
    claimed.colls = { flightS };
    claimed.Claim(0x0951, 0x16EA);
    PollHyperX(claimed);
    assert(claimed.out.empty() && opens == 0 && writes.empty());

    // Regression: Kingston Cloud II Wireless (0951:1718) uses the same frame, but only the FF13:0001
    // collection answers, the dongle needs a report-6 input report read on the same handle before the
    // write, and it accepts a single write per handle, so the level is the only thing read.
    ctx = Poll(Collection(0x1718), Device(76, 1));
    assert(opens == 1 && primes == 1 && writes.size() == 1);
    assert(ctx.out.size() == 1 && ctx.out[0].key == L"hyperx:1718");
    assert(ctx.out[0].name == L"HyperX Cloud II Wireless");
    assert(ctx.out[0].kind == DeviceKind::Headset && ctx.out[0].via == L"2.4 GHz receiver");
    assert(ctx.out[0].percent == 76 && !ctx.out[0].charging);
    assert(ctx.IsClaimed(0x0951, 0x1718));

    // Windows also lists a Consumer Control collection with 62/62 reports for this dongle: it does not
    // answer, so it must not be claimed and the headset must not be counted twice.
    ctx = Poll({ Collection(0x1718, 0x000C), Collection(0x1718) }, Device(64));
    assert(opens == 2 && ctx.out.size() == 1 && ctx.out[0].percent == 64);

    // No answer, out-of-range level or a malformed reply: don't claim a headset.
    ctx = Poll(Collection(0x1718), {});
    assert(ctx.out.empty() && ctx.claimed.empty());
    ctx = Poll(Collection(0x1718), { { 0x02, { Reply(0x02, 101) } } });
    assert(ctx.out.empty() && ctx.claimed.empty());
    ctx = Poll(Collection(0x1718), { { 0x02, { Reply(0x02, 80, false) } } });
    assert(ctx.out.empty() && ctx.claimed.empty());

    std::puts("HyperX Cloud Flight S tests passed (16EA/16EB, 62-byte queries, charging, filters, failures, "
              "Cloud II Kingston priming and single write).");
}
