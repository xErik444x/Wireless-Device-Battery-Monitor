// HyperX wireless headsets and Pulsefire Haste 2 / Dart mice (HP VID 03F0, older Kingston VID 0951).
// Protocols: Pulsefire Haste 2 Core Wireless (03F0:0AB5), Haste 2 Wireless (03F0:0F98) and Haste 2 Mini (03F0:0DA0)
// verified on hardware, see HasteWireless;
// Cloud III Wireless verified on hardware (03F0:0C9D) and github.com/Medprj/HyperXWM;
// Cloud III S from github.com/LennardKittner/HyperHeadset; Cloud Stinger Core from
// github.com/davidsantanaalves/HyperBatteryMonitor (also Cloud Core Wireless DTS 0951:173F, verified on hardware);
// Cloud II Wireless (Kingston) framing and the report-6 pre-read from
// github.com/auto94/HyperX-Cloud-2-Battery-Monitor;
// Pulsefire Dart (0951:16E1, read on hardware) follows the reverse-engineering notes in
// github.com/santeri3700/hyperx_pulsefire_dart_reverse_engineering (CC0), also implemented in
// github.com/radoslavchobanov/PlasmaNGenuity (MIT), see PulsefireDart;
// the others follow github.com/Sapd/HeadsetControl.
#include "drivers.h"

#include <cmath>

namespace {

// Writes `req` and returns the first input report accepted by `match` within `timeoutMs`.
template <class Match>
bool Query(HidDev& d, const std::vector<BYTE>& req, std::vector<BYTE>& resp, int timeoutMs, Match match) {
    if (!d.Write(req)) return false;
    ULONGLONG end = GetTickCount64() + timeoutMs;
    for (;;) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (left <= 0 || d.Read(resp, left) < 0) return false;
        if (match(resp)) return true;
    }
}

DeviceStatus Base(USHORT pid, const wchar_t* name) {
    DeviceStatus s;
    s.key = L"hyperx:" + Hex4(pid);
    s.name = name;
    s.kind = DeviceKind::Headset;
    s.via = L"2.4 GHz receiver";
    return s;
}

// Cloud III Wireless: report 66 <cmd>; 82 -> [2] connected, 8A -> [2] charging, 89 -> [2..3] mV, [4] %.
bool Cloud3(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    auto q = [&](BYTE cmd, std::vector<BYTE>& r) {
        return Query(d, { 0x66, cmd }, r, 800, [cmd](const std::vector<BYTE>& r) { return r.size() >= 5 && r[0] == 0x66 && r[1] == cmd; });
    };
    std::vector<BYTE> r;
    if (!q(0x82, r)) return false;
    s = Base(c.pid, (c.pid == 0x069F || c.pid == 0x0995) ? L"HyperX Cloud II Core Wireless" : L"HyperX Cloud III Wireless");
    s.connected = r[2] != 0;
    if (s.connected) {
        if (q(0x8A, r)) s.charging = r[2] == 1;
        if (q(0x89, r) && r[4] <= 100) { s.percent = r[4]; s.millivolts = (r[2] << 8) | r[3]; }
    }
    return true;
}

// Cloud II Wireless (HP): report 06 FF BB <cmd>; 02 -> [7] %, 03 -> [4] charging.
bool Cloud2(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    auto q = [&](BYTE cmd, std::vector<BYTE>& r) {
        return Query(d, { 0x06, 0xFF, 0xBB, cmd }, r, 1000,
                     [cmd](const std::vector<BYTE>& r) { return r.size() >= 8 && r[0] == 0x06 && r[1] == 0xFF && r[2] == 0xBB && r[3] == cmd; });
    };
    std::vector<BYTE> r;
    if (!q(0x02, r) || r[7] > 100) return false;
    s = Base(c.pid, L"HyperX Cloud II Wireless");
    s.percent = r[7];
    s.millivolts = (r[5] << 8) | r[6];
    if (q(0x03, r)) s.charging = r[4] == 1;
    return true;
}

// Cloud II Wireless (Kingston 0951:1718) and Cloud Flight S (0951:16EA/16EB):
// fixed 62-byte frame, reply 0B 00 BB <cmd>.
// The Cloud II Kingston dongle only answers on its FF13:0001 collection, drops the write unless a
// report-6 input report has been read from the control pipe on the same handle first, and accepts a
// single write per handle (all three observed on hardware 2026-11-21, 0951:1718, before and after the
// fix). That is the same pre-read as github.com/auto94/HyperX-Cloud-2-Battery-Monitor (MainForm.cpp),
// so only the level is queried there: charging and voltage are not decoded for this dongle.
bool Cloud2Kingston(const HidColl& c, DeviceStatus& s) {
    const bool cloud2 = c.pid == 0x1718;
    HidDev d;
    if (!d.Open(c)) return false;
    auto q = [&](BYTE cmd, std::vector<BYTE>& r) {
        if (cloud2) {  // prime the dongle: without it the write is dropped
            std::vector<BYTE> echo;
            d.GetInputReport(0x06, echo);
        }
        std::vector<BYTE> req = { 0x06, 0x00, 0x02, 0x00, 0x9A, 0x00, 0x00, 0x68, 0x4A, 0x8E, 0x0A, 0x00, 0x00, 0x00, 0xBB, cmd, 0x00 };
        return Query(d, req, r, 1000, [cmd](const std::vector<BYTE>& r) { return r.size() >= 8 && r[0] == 0x0B && r[2] == 0xBB && r[3] == cmd; });
    };
    std::vector<BYTE> r;
    if (!q(0x02, r) || r[7] > 100) return false;
    s = Base(c.pid, (c.pid == 0x16EA || c.pid == 0x16EB) ? L"HyperX Cloud Flight S" : L"HyperX Cloud II Wireless");
    s.percent = r[7];
    if (!cloud2 && q(0x03, r)) s.charging = r[4] == 1;
    return true;
}

// Cloud Alpha Wireless: 21 BB <cmd>; 03 -> [3]==1 offline, 0C -> [3]==1 charging, 0B -> [3] %.
bool CloudAlpha(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    auto q = [&](BYTE cmd, std::vector<BYTE>& r) {
        return Query(d, { 0x21, 0xBB, cmd }, r, 1000, [cmd](const std::vector<BYTE>& r) { return r.size() >= 4 && r[0] == 0x21 && r[2] == cmd; });
    };
    std::vector<BYTE> r;
    if (!q(0x03, r)) return false;
    s = Base(c.pid, L"HyperX Cloud Alpha Wireless");
    s.connected = r[3] != 0x01;
    if (!s.connected) return true;
    if (q(0x0C, r)) s.charging = r[3] == 0x01;
    if (q(0x0B, r) && r[3] <= 100) s.percent = r[3];
    return true;
}

// Cloud Alpha 2 Wireless: [cmd][sub]; reply [cmd+1][sub]...; 52 01 -> [2] connected, 50 02 -> [2] %, [3] charging.
bool CloudAlpha2(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    auto q = [&](BYTE cmd, BYTE sub, std::vector<BYTE>& r) {
        return Query(d, { cmd, sub }, r, 1000, [cmd, sub](const std::vector<BYTE>& r) { return r.size() >= 8 && r[0] == cmd + 1 && r[1] == sub; });
    };
    std::vector<BYTE> r;
    if (!q(0x52, 0x01, r)) return false;
    s = Base(c.pid, L"HyperX Cloud Alpha 2 Wireless");
    s.connected = r[2] != 0;
    if (s.connected && q(0x50, 0x02, r) && r[2] <= 100) {
        s.percent = r[2];
        s.charging = r[3] == 0x01;
        s.millivolts = r[6] | (r[7] << 8);
    }
    return true;
}

// Original Pulsefire Haste Wireless (not Haste 2): NGENUITY2 64-byte vendor output/input reports.
// Source: github.com/OpenMouse-Project/mouse-protocol (src/hyperx/index.ts and src/drivers/hyperx/hid.ts),
// building on github.com/evan-razzaque/open-pulsefire-haste. Only the GET_BATTERY (0x51) opcode is sent.
// HID buffers include the unnumbered report ID 0; the payload starts with 0x51, reply echoes it.
// 03F0:028E = 2.4 GHz receiver, 03F0:048E = cable / wired mode.
bool HasteOriginal(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    std::vector<BYTE> r;
    if (!Query(d, { 0x00, 0x51 }, r, 800, [](const std::vector<BYTE>& r) {
            return r.size() >= 6 && r[0] == 0x51;
        })) return false;

    s = Base(c.pid, L"HyperX Pulsefire Haste Wireless");
    s.kind = DeviceKind::Mouse;
    s.via = c.pid == 0x028E ? L"2.4 GHz receiver" : L"USB cable";
    // 0 and FF mean unavailable on this protocol, notably in wired mode.
    if (r[4] >= 1 && r[4] <= 100) {
        s.percent = r[4];
        s.charging = r[5] == 0x01 || r[5] == 0x02;
        s.full = r[5] == 0x02;
    }
    return true;
}

// Pulsefire Haste 2 (Core) Wireless: NGENUITY's read-only battery query, the same 52 01 / 50 02 pair as
// Cloud Alpha 2, on the dongle's vendor collection (interface 2). 64-byte reports.
// From github.com/PMarinov1994/ngenuity-linux-cli (Haste 2 Wireless). Verified on a Core Wireless (03F0:0AB5), on
// a Haste 2 Wireless (03F0:0F98, MI_02 FF00:0001, 64-byte reports; level matched NGENUITY, PR #312 by uzumakiprime)
// and on a Haste 2 Mini (03F0:0DA0, level matches NGENUITY); the 0D97 dongle is untested. Protocol notes:
// docs/hyperx_haste_2.md.
bool HasteWireless(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    auto q = [&](BYTE cmd, BYTE sub, std::vector<BYTE>& r) {
        return Query(d, { cmd, sub }, r, 1000, [cmd, sub](const std::vector<BYTE>& r) { return r.size() >= 8 && r[0] == cmd + 1 && r[1] == sub; });
    };
    std::vector<BYTE> r;
    // After a reconnect the dongle floods the channel with unrelated reports (44 xx, 45 xx, FB xx) and the
    // link reply can miss the time limit, so a missing link reply is not an error: the battery query decides.
    bool linkKnown = q(0x52, 0x01, r);
    bool linked = !linkKnown || r[2] != 0;
    bool gotBattery = linked && q(0x50, 0x02, r) && r[2] <= 100;
    if (!linkKnown && !gotBattery) return false;

    // For about 5 s after it powers on the mouse reports 40% instead of its real level (measured 2026-11-17 on
    // a Core Wireless: 40% for 2-5 s, then 70%). A reading inside that settle window is not trusted. The first
    // poll after the app starts assumes the mouse has been on for a while.
    static ULONGLONG linkUpAt = 0;
    static bool polled = false;
    const ULONGLONG kSettleMs = 6000;
    ULONGLONG now = GetTickCount64();
    if (!linked) linkUpAt = 0;
    else if (linkUpAt == 0) linkUpAt = polled ? now : now - kSettleMs;
    polled = true;
    bool settled = now - linkUpAt >= kSettleMs;

    s = Base(c.pid, c.pid == 0x0AB5 ? L"HyperX Pulsefire Haste 2 Core Wireless"
                 : c.pid == 0x0DA0 ? L"HyperX Pulsefire Haste 2 Mini"
                 : L"HyperX Pulsefire Haste 2 Wireless");
    s.kind = DeviceKind::Mouse;
    s.connected = linked;
    if (gotBattery && settled) {  // an unsettled reading stays unknown, so the tray keeps the last value dimmed
        s.percent = r[2];
        s.charging = r[3] == 0x01 || r[3] == 0x02;
        s.millivolts = r[6] | (r[7] << 8);
    }
    return true;
}

// Pulsefire Dart (Kingston dongle 0951:16E1, vendor collection FF00 on interface 2): the "heartbeat"
// query, a 64-byte zero-padded output report whose first byte is the command 0x51 (sent here with report ID 0).
// The documented reply is the input report 51 00 00 03 <battery 0..100 %> <1 = charging> ...; bytes [6..10] are
// undocumented, so no voltage is reported. The dongle can also answer 51 00 02 EC 05 00 ...: on a real 16E1 dongle
// (2026-11-21) it did so in stretches of half a minute to minutes while NGENUITY showed 69%, trigger unknown. That
// 05 is not a level (reading it as one showed 5%), so only the 00 00 03 layout gives a level; any other heartbeat
// reply lists the mouse as asleep with an unknown level, and the tray keeps the last reading.
// Source: community reverse engineering from NGENUITY captures (see the credits at the top of the file). Read on a
// real 16E1 dongle on Windows: the collection is MI_02 FF00:0001 with 65-byte input/output/feature reports, and
// the documented reply gave 69%, the level NGENUITY showed at the same time; charging not yet observed. The cable
// variant (0951:16E2, FF13) is not handled. Protocol notes: docs/hyperx_pulsefire_dart.md.
bool PulsefireDart(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    std::vector<BYTE> r;
    auto heartbeat = [](const std::vector<BYTE>& r) { return r.size() >= 6 && r[0] == 0x51; };
    if (!Query(d, { 0x00, 0x51 }, r, 1000, heartbeat)) return false;
    s = Base(c.pid, L"HyperX Pulsefire Dart");
    s.kind = DeviceKind::Mouse;
    if (r[1] != 0x00 || r[2] != 0x00 || r[3] != 0x03 || r[4] > 100) {
        s.asleep = true;  // answered without a level (e.g. 51 00 02 EC 05): keep the last reading
        return true;
    }
    s.percent = r[4];
    s.charging = r[5] == 0x01;
    return true;
}

// Cloud Flight: 21 FF 05 -> [3..4] mV (above 0x100B means charging).
bool CloudFlight(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    std::vector<BYTE> r;
    if (!Query(d, { 0x21, 0xFF, 0x05 }, r, 1000, [](const std::vector<BYTE>& r) { return r.size() >= 5; })) return false;
    int mv = (r[3] << 8) | r[4];
    s = Base(c.pid, L"HyperX Cloud Flight");
    s.millivolts = mv;
    if (mv > 0x100B) {
        s.charging = true;
    } else if (mv <= 3648) {
        s.percent = Clamp100((int)(0.00125 * mv));
    } else if (mv > 3975) {
        s.percent = 100;
    } else {
        double v = mv;
        s.percent = Clamp100((int)std::lround(0.00000002547505 * std::pow(v, 4) - 0.0003900299 * std::pow(v, 3) +
                                              2.238321 * v * v - 5706.256 * v + 5452299.0));
    }
    return true;
}

// Cloud III S Wireless: report 0C [02 03 01 00 <cmd>]; reply [5] = cmd, [6] = value (FF = invalid).
//   02 -> [6] == 2 connected, 06 -> [6] %, 48 -> [6] 0 discharging / 1 charging / 2 full.
// From github.com/LennardKittner/HyperHeadset (cloud_iii_s_wireless.rs).
bool Cloud3S(const HidColl& c, DeviceStatus& s) {
    HidDev d;
    if (!d.Open(c)) return false;
    auto q = [&](BYTE cmd, std::vector<BYTE>& r) {
        return Query(d, { 0x0C, 0x02, 0x03, 0x01, 0x00, cmd }, r, 1000,
                     [cmd](const std::vector<BYTE>& r) { return r.size() >= 7 && r[0] == 0x0C && r[5] == cmd && r[6] != 0xFF; });
    };
    std::vector<BYTE> r;
    if (!q(0x02, r)) return false;
    s = Base(c.pid, L"HyperX Cloud III S Wireless");
    s.connected = r[6] == 2;
    if (!s.connected) return true;
    if (q(0x06, r) && r[6] <= 100) s.percent = r[6];
    if (q(0x48, r)) s.charging = r[6] == 1 || r[6] == 2;
    return true;
}

// Cloud Stinger Core Wireless (+ 7.1): feature report FF on the FF00:0001 collection (MI_03 Col03).
// From github.com/davidsantanaalves/HyperBatteryMonitor (Devices/CloudStingerCoreWirelessDevice.cs, MIT);
// the PlayStation receiver 03F0:0A93 was verified on hardware there (PR #6: 52 queries, codes 03/05/06).
// SetFeature(command), wait 20 ms, GetFeature(FF):
//   connection: reply [11] == 1 means the headset is on (3 when off; header FF 0C 05 on 0951:170B).
//   battery:    reply FF 12 05, [9] = 03 discharging / 05 charging / 06 full, [11..12] = battery mV (LE).
// All five receivers (03F0:0A93, 03F0:0A8E, 0951:170B, 0951:1713, 0951:173F) can return a stale previous
// report with header FF 01 00 at 20 ms before the fresh reply arrives (observed at 40 ms on 0951:170B,
// where reading the stale connection reply as battery produced 0x0E01 = 3585 mV / 5% or dropped the reading).
// Replies are therefore polled up to 10 x 20 ms until a fresh connection or FF 12 05 battery frame arrives.
// Hardware verified: 0951:170B (discharging, charging, off), 0951:1713, 0951:173F (Cloud Core Wireless DTS,
// using the Stinger Core mV curve), and 03F0:0A8E (HyperBatteryMonitor PR #16).
bool StingerCore(const HidColl& c, DeviceStatus& s) {
    static const std::vector<BYTE> kConnection = { 0xFF, 0x07, 0x00, 0xFD, 0x04, 0x00, 0x00, 0x02, 0x82, 0x03,
                                                   0x05, 0x29, 0x0F, 0x00, 0x00, 0x03, 0x68, 0x10, 0x00, 0x00 };
    static const std::vector<BYTE> kBattery = { 0xFF, 0x07, 0x00, 0xFD, 0x04, 0x0C, 0xF1, 0x02, 0x01, 0x04, 0xF0, 0x0C };
    // mV -> % in 10% steps, from the same source (separate curves while charging and on battery).
    static const int kPct[] = { 100, 90, 80, 70, 60, 50, 40, 30, 20, 10, 0 };
    static const int kChargingMv[] = { 4130, 4090, 4050, 4000, 3967, 3946, 3930, 3916, 3871, 3831, 3400 };
    static const int kDischargingMv[] = { 4121, 4042, 3954, 3905, 3838, 3785, 3763, 3738, 3700, 3651, 3517 };

    HidDev d;
    if (!d.Open(c)) return false;
    auto exchange = [&](const std::vector<BYTE>& cmd, std::vector<BYTE>& r, bool (*good)(const std::vector<BYTE>&)) {
        if (!d.SetFeature(cmd)) return false;
        for (int i = 0; i < 10; i++) {
            Sleep(20);
            if (!d.GetFeature(0xFF, r) || r.size() < 13) return false;
            if (good(r)) return true;
        }
        return false;
    };
    auto fresh = [](const std::vector<BYTE>& r) {
        return !(r[1] == 0x01 && r[2] == 0x00) && !(r[1] == 0x12 && r[2] == 0x05);
    };
    auto battery = [](const std::vector<BYTE>& r) { return r[0] == 0xFF && r[1] == 0x12 && r[2] == 0x05; };

    std::vector<BYTE> r;
    // Some receivers/fixtures only expose FF 01 00 on connection reads, so keep the last one after 10 retries.
    if (!exchange(kConnection, r, fresh) && !(r.size() >= 13 && r[1] == 0x01 && r[2] == 0x00)) return false;
    s = Base(c.pid, c.pid == 0x173F ? L"HyperX Cloud Core Wireless"
                  : c.pid == 0x0A8E ? L"HyperX Cloud Stinger Core Wireless DTS"
                  : L"HyperX Cloud Stinger Core Wireless");
    s.connected = r[11] == 0x01;
    if (!s.connected || !exchange(kBattery, r, battery)) return true;
    BYTE code = r[9];
    if (code != 0x03 && code != 0x05 && code != 0x06) return true;
    s.millivolts = r[11] | (r[12] << 8);
    s.charging = code == 0x05;
    s.percent = code == 0x06 ? 100 : Interpolate(s.millivolts, code == 0x05 ? kChargingMv : kDischargingMv, kPct, 11);
    return true;
}

struct Model {
    USHORT vid;
    std::vector<USHORT> pids;
    bool (*accept)(const HidColl&);  // which collection carries the protocol
    bool (*poll)(const HidColl&, DeviceStatus&);
};

const Model kModels[] = {
    { 0x03F0, { 0x0FCC, 0x05B7, 0x0C9D }, [](const HidColl& c) { return c.outLen() == 62 && c.inLen() == 62; }, Cloud3 },
    // Cloud II Core Wireless: same 66 <cmd> protocol as Cloud III Wireless, 64-byte reports (HyperHeadset).
    { 0x03F0, { 0x069F, 0x0995 }, [](const HidColl& c) { return c.page() >= 0xFF00 && c.outLen() >= 62 && c.inLen() >= 62; }, Cloud3 },
    // Cloud III S: report 0C lives on the 64/64-byte collection, which Windows lists under usage page 01C0
    // (not a vendor page). The FF13 (62-byte) and FFC0 (32-byte) collections don't carry it.
    { 0x03F0, { 0x06BE, 0x02CC }, [](const HidColl& c) { return c.outLen() == 64 && c.inLen() == 64; }, Cloud3S },
    { 0x03F0, { 0x0696, 0x018B, 0x0D93, 0x0C8C }, [](const HidColl& c) { return c.page() == 0xFF90 && c.usage() == 0x0303; }, Cloud2 },  // 0C8C: Cloud Flight for PS, device request #2561, untested
    { 0x0951, { 0x1718 }, [](const HidColl& c) { return c.outLen() == 62; }, Cloud2Kingston },
    // Cloud Flight S (0951:16EA / 16EB) shares the Cloud II Wireless read-only protocol.
    // 0951:16EA: FF13:0001 on interface 5, 62-byte input/output; issue #348.
    // Protocol: LennardKittner/HyperHeadset (MIT); hardware confirmation pending.
    { 0x0951, { 0x16EA, 0x16EB }, [](const HidColl& c) {
        return c.page() == 0xFF13 && c.usage() == 0x0001 && c.inLen() == 62 && c.outLen() == 62;
    }, Cloud2Kingston },
    { 0x03F0, { 0x098D }, [](const HidColl& c) { return c.page() >= 0xFF00 && c.outLen() >= 3; }, CloudAlpha },
    { 0x03F0, { 0x08BE }, [](const HidColl& c) { return c.page() == 0xFF13 && c.usage() == 0xFF00; }, CloudAlpha2 },
    // Original Pulsefire Haste Wireless, both 2.4 GHz dongle and USB cable: 64-byte unnumbered vendor reports.
    { 0x03F0, { 0x028E, 0x048E }, [](const HidColl& c) {
        return c.page() == 0xFF00 && c.usage() == 0x0001 && c.inLen() >= 65 && c.outLen() >= 65;
    }, HasteOriginal },
    // Pulsefire Haste 2 Wireless (0F98 tested, 0D97 untested), Haste 2 Core Wireless (0AB5 tested) and Haste 2 Mini
    // (0DA0 tested) dongles.
    { 0x03F0, { 0x0AB5, 0x0D97, 0x0F98, 0x0DA0 }, [](const HidColl& c) { return c.page() >= 0xFF00 && c.outLen() >= 2 && c.inLen() >= 8; }, HasteWireless },
    // Pulsefire Dart dongle (Kingston): vendor collection FF00:0001 on MI_02, 65-byte reports on Windows.
    { 0x0951, { 0x16E1 }, [](const HidColl& c) {
        return c.page() == 0xFF00 && c.outLen() >= 2 && c.inLen() >= 6;
    }, PulsefireDart },
    { 0x0951, { 0x16C4, 0x1723, 0x1749 }, [](const HidColl& c) { return c.page() >= 0xFF00 && c.outLen() >= 3; }, CloudFlight },
    { 0x03F0, { 0x0E90 }, [](const HidColl& c) { return c.page() >= 0xFF00 && c.outLen() >= 3; }, CloudFlight },  // Cloud Flight (HP)
    // Cloud Stinger Core Wireless: PlayStation receivers (HP 0A93, Kingston 1713), the HP DTS receiver (0A8E), the
    // original Kingston receiver, and the Cloud Core Wireless DTS receiver (Kingston 173F)
    { 0x03F0, { 0x0A93, 0x0A8E }, [](const HidColl& c) { return c.page() == 0xFF00 && c.usage() == 0x0001 && c.featLen() >= 32; }, StingerCore },
    { 0x0951, { 0x170B, 0x1713, 0x173F }, [](const HidColl& c) { return c.page() == 0xFF00 && c.usage() == 0x0001 && c.featLen() >= 32; }, StingerCore },
};

}  // namespace

void PollHyperX(PollContext& ctx) {
    for (const auto& m : kModels) {
        for (USHORT pid : m.pids) {
            if (ctx.IsClaimed(m.vid, pid)) continue;
            for (const HidColl* c : ctx.Find(m.vid, pid, m.accept)) {
                DeviceStatus s;
                if (m.poll(*c, s)) { ctx.Add(s, m.vid, pid); break; }
            }
        }
    }
}
