#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP3XX.h>
#include <ScioSense_ENS16x.h>

#include <cc1101.h>
#include <hal/cc1101_hal_arduino.h>
#include <afec_node.h>

#include "afec_platform_arduino.h"
#include "afec_cmd.h"
#include "config.h"

CC1101HalArduino hal(SPI, PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN, 4000000, PIN_GDO0);
CC1101 radio;

Adafruit_BMP3XX bmp;
Adafruit_AHTX0 aht;
ENS160 ens160;

afec::Node node;
static CC1101::Config   rcfg;
static afec::NodeConfig ncfg;
static cmd::LineReader  lines;

static uint32_t lastTx = 0;          // legacy sensor mode
static uint16_t curBlock = 0;        // from the last `mark`
static uint32_t aseq = 0;            // synthetic-payload counter, per boot

// One `run` = one measurement block of synthetic packets.
struct Run {
    bool     active = false;
    uint16_t n = 0, idx = 0;
    uint8_t  len = 32;
    uint32_t interval = TX_INTERVAL_MS, lastTx = 0, t0 = 0;
    uint16_t ok = 0, noAck = 0, duty = 0, other = 0;
} run;

void debugPrint(const char *format, ...) {
    char buf[192];
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    if (n < 0) return;
    logsink::write(buf, n < (int)sizeof buf ? n : sizeof buf - 1);
}

static const char* resultName(afec::Result r) {
    switch (r) {
        case afec::Result::OK:         return "OK";
        case afec::Result::NO_ACK:     return "NO_ACK";
        case afec::Result::DUTY_LIMIT: return "DUTY_LIMIT";
        case afec::Result::TOO_LONG:   return "TOO_LONG";
        case afec::Result::RADIO_ERR:  return "RADIO_ERR";
        default:                       return "?";
    }
}

// Re-initialise radio and node with the current rcfg/ncfg (emits the library's `begin` event),
// then reset the link to the peer so the block starts from initialLevel.
// Note: node.begin() also refills the duty-cycle burst budget (run plan, decision 4).
static bool reinit() {
    if (USE_IRQ) radio.disableInterrupt();
    if (!radio.begin(hal, rcfg)) { cmd::ev('E', "radio_err", ",\"err\":\"CC1101 not found\""); return false; }
    if (USE_IRQ && !radio.enableInterrupt()) cmd::ev('W', "irq_fallback");
    node.begin(radio, ncfg, afec::arduinoPlatform());
    node.resetLink(DEST_NODE_ID);
    return true;
}

static void endRun(bool aborted) {
    cmd::ev('I', "block_end",
            ",\"block\":%u,\"n\":%u,\"sent\":%u,\"ok\":%u,\"no_ack\":%u,\"duty_limit\":%u,\"other\":%u,\"aborted\":%u,\"dur_ms\":%lu",
            curBlock, run.n, run.idx, run.ok, run.noAck, run.duty, run.other, (unsigned)aborted,
            (unsigned long)(millis() - run.t0));
    run.active = false;
}

static void sendOne() {
    uint8_t p[afec::PAYLOAD_MAX];
    run.lastTx = millis();
    cmd::synBuild(p, run.len, aseq, curBlock, run.idx);
    const uint32_t t0 = millis();
    const afec::Result r = node.send(DEST_NODE_ID, p, run.len);
    cmd::ev('I', "app_tx", ",\"aseq\":%lu,\"block\":%u,\"idx\":%u,\"len\":%u,\"res\":\"%s\",\"lvl_next\":%u,\"dur_ms\":%lu",
            (unsigned long)aseq, curBlock, run.idx, run.len, resultName(r), node.peerLevel(DEST_NODE_ID),
            (unsigned long)(millis() - t0));
    switch (r) {
        case afec::Result::OK:         run.ok++; break;
        case afec::Result::NO_ACK:     run.noAck++; break;
        case afec::Result::DUTY_LIMIT: run.duty++; break;
        default:                       run.other++; break;
    }
    aseq++;
    if (++run.idx >= run.n) endRun(false);
}

// ------------------------------------------------------------------ bench (RQ4, no radio)
struct Acc {
    uint32_t mn = UINT32_MAX, mx = 0; uint64_t sum = 0; uint32_t n = 0;
    void add(uint32_t c) { if (c < mn) mn = c; if (c > mx) mx = c; sum += c; n++; }
};

static void benchOut(const char* op, unsigned lvl, int s, const Acc& a, unsigned ok) {
    cmd::ev('I', "bench", ",\"op\":\"%s\",\"lvl\":%u,\"s\":%d,\"n\":%lu,\"ok\":%u,\"min\":%lu,\"mean\":%lu,\"max\":%lu",
            op, lvl, s, (unsigned long)a.n, ok, (unsigned long)a.mn,
            (unsigned long)(a.n ? a.sum / a.n : 0), (unsigned long)a.mx);
}

// Flip `s` distinct bits (Hamming/BCH) or corrupt `s` distinct bytes (RS) in the data part.
static void inject(uint8_t* d, uint8_t len, bool bytes, uint8_t s) {
    uint16_t pos[16]; uint8_t k = 0;
    const uint16_t span = bytes ? len : (uint16_t)len * 8;
    while (k < s) {
        const uint16_t x = (uint16_t)random(span);
        bool dup = false;
        for (uint8_t i = 0; i < k; i++) if (pos[i] == x) { dup = true; break; }
        if (dup) continue;
        pos[k++] = x;
        if (bytes) d[x] ^= (uint8_t)(1 + random(255));
        else       d[x >> 3] ^= (uint8_t)(1u << (x & 7));
    }
}

// bench reps=<n> len=<payload B>: cycle counts for ECC encode, decode with s = 0..t+1 errors,
// whole-frame encode/decode, CRC-16 and the LHS score. Blocks the loop; run it on an idle node.
static void bench(const cmd::Args& a) {
    static const char* const keys[] = { "reps", "len", nullptr };
    if (!a.only(keys)) return;
    uint16_t reps = 200; uint8_t plen = 32;
    if (!a.num("reps", 1, 5000, reps) || !a.num("len", 1, afec::maxPayload(afec::MAX_LEVEL), plen)) return;
    const uint8_t L = afec::INNER_OVERHEAD + plen;            // ECC-protected inner length
    cmd::ev('I', "bench_start", ",\"cpu_mhz\":%lu,\"reps\":%u,\"plen\":%u,\"inner_len\":%u",
            (unsigned long)getCpuFrequencyMhz(), reps, plen, L);

    uint8_t data[64], work[64], parity[16];
    for (uint8_t i = 0; i < L; i++) data[i] = (uint8_t)random(256);
    afec::eccInit();

    for (uint8_t lvl = 0; lvl <= afec::MAX_LEVEL; lvl++) {
        const afec::LevelInfo& li = afec::levelInfo(lvl);
        Acc enc;
        for (uint16_t r = 0; r < reps; r++) {
            const uint32_t c0 = ESP.getCycleCount();
            afec::eccEncode(lvl, data, L, parity);
            enc.add(ESP.getCycleCount() - c0);
        }
        benchOut("enc", lvl, 0, enc, reps);
        for (uint8_t s = 0; s <= li.t + 1; s++) {
            Acc dec; unsigned ok = 0;
            for (uint16_t r = 0; r < reps; r++) {
                memcpy(work, data, L);
                inject(work, L, li.scheme == afec::Scheme::RS, s);
                uint8_t corr = 0;
                const uint32_t c0 = ESP.getCycleCount();
                const bool d = afec::eccDecode(lvl, work, L, parity, &corr);
                dec.add(ESP.getCycleCount() - c0);
                if (d && corr == s && !memcmp(work, data, L)) ok++;
            }
            benchOut("dec", lvl, s, dec, ok);
        }
        yield();
    }

    // Whole frame (header Hamming(8,4) + ECC + CRC-16), error-free, incl. LEVEL_NONE.
    afec::Frame f; f.src = 2; f.dst = 1; f.plen = plen;
    memcpy(f.payload, data, plen);
    const uint8_t frameLevels[] = { 0, 1, 2, 3, 4, 5, afec::LEVEL_NONE };
    for (uint8_t lvl : frameLevels) {
        uint8_t buf[afec::AIR_MAX], n = 0;
        Acc fe, fd; unsigned ok = 0;
        for (uint16_t r = 0; r < reps; r++) {
            const uint32_t c0 = ESP.getCycleCount();
            afec::encodeFrame(f, lvl, buf, &n);
            fe.add(ESP.getCycleCount() - c0);
        }
        for (uint16_t r = 0; r < reps; r++) {
            afec::Frame g; afec::DecodeInfo di;
            const uint32_t c0 = ESP.getCycleCount();
            const afec::DecodeStatus st = afec::decodeFrame(buf, n, g, di);
            fd.add(ESP.getCycleCount() - c0);
            if (st == afec::DecodeStatus::OK) ok++;
        }
        benchOut("frame_enc", lvl, 0, fe, reps);
        benchOut("frame_dec", lvl, 0, fd, ok);
        yield();
    }

    Acc crc;
    for (uint16_t r = 0; r < reps; r++) {
        const uint32_t c0 = ESP.getCycleCount();
        volatile uint16_t x = afec::crc16(data, L - afec::CRC_LEN); (void)x;
        crc.add(ESP.getCycleCount() - c0);
    }
    benchOut("crc16", afec::LEVEL_NONE, 0, crc, reps);

    afec::Lhs lhs; lhs.begin();
    Acc sc;
    for (uint16_t r = 0; r < reps; r++) {
        const uint8_t t = 1 + random(8), s = random(t + 1);
        const int16_t rssi = -120 + (int16_t)random(80);
        const uint8_t lqi = random(128);
        const uint32_t c0 = ESP.getCycleCount();
        volatile uint8_t x = lhs.score(s, t, rssi, lqi, false); (void)x;
        sc.add(ESP.getCycleCount() - c0);
    }
    benchOut("lhs_score", afec::LEVEL_NONE, 0, sc, reps);
    cmd::ev('I', "bench_end");
}

// ------------------------------------------------------------------ commands
static void handle(char* line) {
    cmd::Args a;
    if (!a.parse(line)) return;
    const char* c = a.name;

    if (!strcmp(c, "hello")) {
        cmd::ev('I', "hello", ",\"role\":\"sender\",\"peer\":%u,\"build\":\"%s %s\",\"sink\":\"%s\"", DEST_NODE_ID, __DATE__, __TIME__, logsink::name());
        cmd::printCfg(ncfg, rcfg.txPowerDbm);
        return;
    }
    if (!strcmp(c, "stats"))  { cmd::printStats(node.stats()); return; }
    if (!strcmp(c, "mark"))   { cmd::mark(a, curBlock); return; }
    if (!strcmp(c, "stop"))   { if (run.active) endRun(true); else cmd::fail(c, "no run active"); return; }
    if (run.active)           { cmd::fail(c, "busy", "run active, send stop first"); return; }

    if (!strcmp(c, "cfg")) {
        afec::NodeConfig nc = ncfg; int8_t tx = rcfg.txPowerDbm;
        if (!cmd::parseCfg(a, nc, tx)) return;
        ncfg = nc; rcfg.txPowerDbm = tx;
        if (reinit()) cmd::printCfg(ncfg, rcfg.txPowerDbm);
        return;
    }
    if (!strcmp(c, "reset")) {
        static const char* const keys[] = { "peer", nullptr };
        uint8_t peer = DEST_NODE_ID;
        if (!a.only(keys) || !a.num("peer", 1, 254, peer)) return;
        node.resetLink(peer);
        return;
    }
    if (!strcmp(c, "run")) {
        static const char* const keys[] = { "n", "len", "interval", "block", nullptr };
        Run r; r.n = 100;
        if (!a.only(keys) || !a.num("n", 1, 65535, r.n) ||
            !a.num("len", cmd::SYN_MIN, afec::maxPayload(0), r.len) ||
            !a.num("interval", 0, 600000, r.interval) || !a.num("block", 0, 65535, curBlock)) return;
        r.active = true;
        r.t0 = millis();
        r.lastTx = r.t0 - r.interval;                         // first packet goes out at once
        run = r;
        cmd::ev('I', "run_start", ",\"block\":%u,\"n\":%u,\"len\":%u,\"interval_ms\":%lu,\"aseq0\":%lu",
                curBlock, run.n, run.len, (unsigned long)run.interval, (unsigned long)aseq);
        return;
    }
    if (!strcmp(c, "bench")) { bench(a); return; }
    cmd::fail(c, "unknown command", "hello|cfg|mark|reset|run|stop|stats|bench");
}

void setup() {
    // Large TX ring buffer: DBG logging (~1 KB/s) must not block the protocol. With the default
    // buffer every log line waits ~15 ms for the UART, which inflates RTT unevenly across configs.
    Serial.setTxBufferSize(8192);    // must precede begin()
    Serial.begin(115200);
    delay(500);
    cmd::nodeId() = NODE_ID;
    logsink::begin(NODE_ID, "tx");          // AFEC_LOG_SINK: off / serial / mqtt (see afec_log_sink.h)

    rcfg.freqMHz    = RF_FREQ_MHZ;
    rcfg.syncWord   = RF_SYNC_WORD;
    rcfg.txPowerDbm = RF_TX_DBM;
    rcfg.whitening  = true;
    rcfg.hwCrc      = false;

    ncfg.id = NODE_ID; ncfg.bitrateBps = 4800;     // everything else: library defaults (C5)

    if (logsink::enabled()) node.printDebug = debugPrint;   // null: the library skips log formatting

    if (!radio.begin(hal, rcfg)) {
        Serial.println("CC1101 not found - check wiring/power");
        while (true) delay(1000);
    }
    if (USE_IRQ) {
        if (!radio.enableInterrupt()) Serial.println("IRQ setup failed, falling back to polling");
    }
    Serial.printf("CC1101 ready, VERSION=0x%02X, f=%.3f MHz\n", radio.version(), RF_FREQ_MHZ);

    if (SENSOR_TX) {
        Wire.begin();
        delay(1000);
        aht.begin();
        bmp.begin_I2C();
        ens160.begin(&Wire, ENS160_ADDR);
        ens160.init();
        ens160.startStandardMeasure();
    }

    node.begin(radio, ncfg, afec::arduinoPlatform());
    cmd::ev('I', "hello", ",\"role\":\"sender\",\"peer\":%u,\"build\":\"%s %s\",\"sink\":\"%s\"", DEST_NODE_ID, __DATE__, __TIME__, logsink::name());
    cmd::printCfg(ncfg, rcfg.txPowerDbm);
}

// Legacy behaviour (SENSOR_TX): sensor readings every TX_INTERVAL_MS while no run is active.
static void sensorTx() {
    ens160.wait();
    sensors_event_t tempEvent, humidityEvent;
    aht.getEvent(&humidityEvent, &tempEvent);
    ens160.writeCompensation(tempEvent.temperature, humidityEvent.relative_humidity);
    ens160.update();

    unsigned long temperature = (unsigned long)(tempEvent.temperature * 100.);
    unsigned long humidity    = (unsigned long)(humidityEvent.relative_humidity * 100.);
    unsigned long pressure    = (unsigned long)(bmp.readPressure() * 100.);
    unsigned long ppm         = (unsigned long)(ens160.getEco2() * 100.);

    char msg[32];
    int len = snprintf(msg, sizeof(msg), "t:%lu;h:%lu;p:%lu;m:%lu;", temperature, humidity, pressure, ppm);

    node.send(DEST_NODE_ID, (const uint8_t*)msg, len);
}

void loop() {
    node.poll();
    if (char* line = lines.poll()) handle(line);

    if (run.active) {
        if (millis() - run.lastTx >= run.interval) sendOne();
    } else if (SENSOR_TX && millis() - lastTx >= TX_INTERVAL_MS) {
        lastTx = millis();
        sensorTx();
    }
}
