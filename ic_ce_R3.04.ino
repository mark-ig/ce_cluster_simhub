// ============================================================
//  IC CE200 Gauge Cluster Emulator — R3 (3/25)
//  Hardware: ESP32 + SN65HVD230 CAN transceiver
//  Protocol: J1939 @ 250 kbps
//
//  KNOWN UNKNOWNS:
//    - Brake air tank 1 & 2 gauges   (trying FEF3 / FEF5 / FF26)
//    - PRNDL display (RND421, top-left)
//    - Mileage / hours / instant MPG  (top-right DID)
//    - Electrical Fault* message
//    - Economy switch sporadic flash  (EF17 byte 4 bit 5 — see ECO note)
// ============================================================

#include <driver/twai.h>

// ── CAN pins ────────────────────────────────────────────────
#define CAN_TX GPIO_NUM_5
#define CAN_RX GPIO_NUM_4

// ── J1939 source addresses ──────────────────────────────────
#define SA_ENGINE  0x00   // Engine ECU
#define SA_ICU     0x21   // Instrument Control Unit
#define SA_ABS     0x0B   // ABS / brake controller

// ── Gauge state (all user-adjustable) ───────────────────────
uint16_t currentRPM   = 700;   // 0 – 4500
uint8_t  currentSpeed = 0;     // 0 – 160 km/h
int8_t   currentTemp  = 42;    // -40 – 210 °C
uint8_t  currentOilP  = 65;    // 0 – 200 PSI
uint8_t  currentOilL  = 100;   // 0 – 100 % (oil level; 100% = full = 0x6F raw)
float    currentVolts = 14.1f; // 0.0 – 32.0 V
uint8_t  currentFuel  = 100;   // 0 – 100 %

// Air brake pressures (PSI, 0–150)
uint8_t  currentAir1  = 120;   // primary tank
uint8_t  currentAir2  = 120;   // secondary tank

// ── PGN data buffers ─────────────────────────────────────────
uint8_t eec1[8]  = { 0xFF, 0xFF, 0xFF, 0x01, 0x19, 0xFF, 0xFF, 0xFF }; // EEC1  — RPM
uint8_t ccvs[8]  = { 0xFF, 0x00, 0x00, 0x01, 0x0F, 0xFF, 0xFF, 0xFF }; // CCVS  — speed; byte 3 = cruise set lamp
uint8_t et1[8]   = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // ET1   — coolant temp
uint8_t feef[8]  = { 0xFF, 0xFF, 0xFA, 0x70, 0xFF, 0xFF, 0xFF, 0xFF }; // FEEF  — [2]=oil level (0xFA=100%), [3]=oil pressure (0x70≈65psi)
uint8_t fef7[8]  = { 0x01, 0xFF, 0xFF, 0xFF, 0x1A, 0x01, 0xFF, 0xFF }; // FEF7  — battery voltage
uint8_t fefc[8]  = { 0xFA, 250,  0xFA, 0xFA, 0xFA, 0xFA, 0xFA, 0xFA }; // FEFC  — [1]=fuel level (250=100%)

uint8_t dm1[8]   = { 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF }; // DM1   — active faults
uint8_t dm1abs[8]= { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // DM1 from ABS (always clean)
uint8_t ef17[8]  = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x39, 0xB5 }; // EF17  — ICU lamp/signal frame

// Misc PGNs — currently broadcasting as-is
uint8_t fece[8]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t fee5[8]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t fee7[8]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t feea[8]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
// ── Driver Info Display (DID) PGN buffers ───────────────────
// FEE9 — Engine Hours (PGN 65257): bytes 0-3, 0.05 hr/bit. 0 hrs = 0x00000000
// FEE5 — Engine Hours/Revolutions (PGN 65253): bytes 0-3 = trip hours, 0.05 hr/bit
// FEE0 — High Resolution Vehicle Distance (PGN 65248): bytes 4-7, 0.005 km/bit
//         (total distance in bytes 4-7; trip in bytes 0-3)
// FEEA — Engine Hours, Revolutions (trip) — already declared above
// Note: all 0x00 = zero displayed. 0xFF bytes = "not available" = "data n/a" on DID.
uint8_t engHours[8] = { 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF }; // FEE9: 0.0 hrs total
uint8_t tripHours[8]= { 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF }; // FEE5: 0.0 trip hrs
uint8_t vehDist[8]  = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }; // FEE0: 0 km trip+total
// FEE8 from SA 0x1C — cluster requests this from body controller every cycle (EA1C)
// This is Engine Fluid Level/Pressure from a secondary ECU. Send all-0xFF = "not available"
// but we MUST broadcast it continuously from SA 0x1C to clear "data n/a" on the DID.
uint8_t fee8body[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t eflp[8]  = { 125,  125,  125,  125,  125,  125,  125,  0xFF }; // EFLP  — engine fluid levels
uint8_t eflp2[8] = { 126,  126,  126,  126,  126,  126,  126,  126  };
uint8_t feb3[8]  = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t eflp3[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t fef3[8]  = { 0x7D, 0x7D, 0x7D, 0x7D, 0x7D, 0x7D, 0x7D, 0x7D }; // FEF3 — air brake pressures (candidate)
uint8_t fefa[8]  = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t ebc1[8]  = { 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
uint8_t febc[8]  = { 0x9F, 0x9F, 0x9F, 0x9F, 0x9F, 0x9F, 0x9F, 0x9F };
uint8_t feae[8]  = { 0x9F, 0x9F, 0x9F, 0x9F, 0x9F, 0x9F, 0x9F, 0x9F };
uint8_t airHealth[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// Air brake candidate frames — values rebuilt by rebuildAir()
// FEF3 (standard J1939 PGN 65267): [0]=supply, [1]=primary, [2]=secondary; 0.5 kPa/bit
// FEF5 / airProp: Navistar proprietary variants; byte layout TBD — currently mirroring same values
uint8_t airFEF5[8] = { 0, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }; // rebuilt in rebuildAir()
uint8_t airProp[8] = { 0x02, 0, 0, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF }; // rebuilt in rebuildAir()

// FF26 — Navistar proprietary (air or body status)
uint8_t ff26[8]      = { 0x01, 0x00, 0x00, 0x00, 0xFE, 0x03, 0xFE, 0x03 };
uint8_t ff26_data[8] = { 0x01, 0x00, 0x01, 0x00, 0xFE, 0x03, 0xF4, 0x03 };

// ── Lamp / signal booleans ───────────────────────────────────
// DM1 lamps
bool lampMIL   = false;  // check engine
bool lampRSL   = false;  // stop engine
bool lampAWL   = false;  // warn engine
bool lampPL    = false;  // protect
bool lampMaint = false;  // maintenance wrench

// EF17 byte 2
bool lampWTS = false;  // wait to start
bool lampIDS = false;  // idle shutdown
bool lampPKB = true;   // park brake (on by default)
bool lampEXT = false;  // emergency exit

// EF17 byte 3
bool lampRNG  = false;  // range inhibited
bool lampDPFT = false;  // DPF high temp
bool lampDPFF = false;  // DPF filter full

// EF17 byte 4
bool lampSBL = false;  // seatbelt
bool lampRFL = false;  // red flasher
bool lampAFL = false;  // amber flasher
bool lampECO = false;  // economy mode  ← NOTE: cluster may self-flash; see advisory
bool lampLTN = false;  // left turn
bool lampHBM = false;  // high beam

// EF17 byte 5
bool lampRTN = false;  // right turn

// ── Frame builders ───────────────────────────────────────────

void rebuildDM1() {
  uint8_t b = 0x00;
  if (lampMIL)  b |= 0x40;
  if (lampRSL)  b |= 0x10;
  if (lampAWL)  b |= 0x04;
  if (lampPL)   b |= 0x01;
  dm1[0] = b;
  dm1[1] = 0xFF;
  if (lampMaint) {
    dm1[2] = 0x71;  dm1[3] = 0x0E;
    dm1[4] = (0x00 << 5) | 31;  dm1[5] = 0x01;
    dm1[6] = 0xFF;  dm1[7] = 0xFF;
  } else {
    dm1[2] = dm1[3] = dm1[4] = dm1[5] = dm1[6] = dm1[7] = 0xFF;
  }
}

void rebuildEF17() {
  ef17[0] = 0x02;
  ef17[1] = (lampWTS ? 0x01 : 0)
          | (lampIDS ? 0x02 : 0)
          | (lampPKB ? 0x40 : 0)
          | (lampEXT ? 0x80 : 0);
  ef17[2] = (lampRNG  ? 0x01 : 0)
          | (lampDPFT ? 0x10 : 0)
          | (lampDPFF ? 0x20 : 0);
  ef17[3] = (lampSBL ? 0x01 : 0)
          | (lampRFL ? 0x04 : 0)
          | (lampAFL ? 0x08 : 0)
          | (lampECO ? 0x20 : 0)
          | (lampLTN ? 0x40 : 0)
          | (lampHBM ? 0x80 : 0);
  ef17[4] = (lampRTN ? 0x01 : 0);
  // ef17[5] = rolling counter — written in loop()
  ef17[6] = 0x39;
  ef17[7] = 0xB5;
}

// FEEF byte layout (confirmed):
//   [0]     = reserved (0xFF)
//   [1]     = reserved (0xFF)
//   [2]     = oil level:    J1939 SPN 98, 0.4%/bit, offset 0.
//             100% = 250 raw = 0xFA.  0% = 0x00.
//             Encode: raw = round(level * 2.5)
//   [3]     = oil pressure: J1939 SPN 100, 4 kPa/bit.
//             1 PSI = 6.895 kPa → raw = round(psi * 6.895 / 4). 65 PSI → ~0x70
//   [6] bit7-6 = WTS lamp (mirrors EF17)
void rebuildFEEF() {
  uint8_t lvlRaw = (uint8_t)(currentOilL * 2.5f + 0.5f);   // 100% → 250 = 0xFA
  uint8_t psiRaw = (uint8_t)((currentOilP * 6.895f) / 4.0f + 0.5f);
  feef[2] = lvlRaw;
  feef[3] = psiRaw;
  feef[6] = (feef[6] & 0x3F) | (lampWTS ? 0x40 : 0x00);
}

// Air brake pressure encoding — IMPORTANT NOTE:
//   The cluster's FEF5 reply (FF FF FF 00 FF FF FF FF, byte 4=0x00) means it's computing
//   zero pressure internally regardless of what we send on FEF5. FEF5 may be cluster-outbound only.
//   FEF3 (SA_ABS) is the most likely inbound path. Trying multiple kPa/bit resolutions:
//     4 kPa/bit:  120 PSI → 827 kPa / 4  = 207 raw → 0xCF (overflows if >147 PSI)
//     8 kPa/bit:  120 PSI → 827 kPa / 8  = 103 raw → 0x67 (fits, reasonable needle)
//    16 kPa/bit:  120 PSI → 827 kPa / 16 =  52 raw → 0x34 (fits, maybe too low)
//   Currently using 8 kPa/bit as best guess. If needles don't move, try AR1RAW/AR2RAW to
//   send arbitrary raw values directly and observe the needle.
uint8_t airRaw1 = 0;  // raw encoded value for tank 1 (set by rebuildAir or AR1RAW command)
uint8_t airRaw2 = 0;  // raw encoded value for tank 2

void rebuildAir() {
  airRaw1 = (uint8_t)((currentAir1 * 6.895f) / 8.0f + 0.5f);  // 8 kPa/bit
  airRaw2 = (uint8_t)((currentAir2 * 6.895f) / 8.0f + 0.5f);
  // FEF3 — standard J1939 PGN 65267, from SA_ABS
  fef3[0] = airRaw1;  // supply pressure (mirror primary)
  fef3[1] = airRaw1;  // primary circuit
  fef3[2] = airRaw2;  // secondary circuit
  fef3[3] = fef3[4] = fef3[5] = fef3[6] = fef3[7] = 0xFF;
  // FEF5 Navistar proprietary (probably cluster-outbound; sending anyway)
  airFEF5[0] = airRaw1;
  airFEF5[1] = airRaw2;
  // EFFF Navistar proprietary air candidate
  airProp[1] = airRaw1;
  airProp[2] = airRaw2;
}

// Body controller (SA 0x1C) odometer — TP BAM for FEE8 (8 bytes).
// FEE8 = Engine Fluid Level/Pressure from secondary ECU. Cluster requests it from 0x1C every cycle.
// Payload: all 0x00 = "zero" for all fields. This clears "data n/a" on the DID.
// Note: TP BAM for a single 8-byte PGN is 1 packet (no BAM needed — fits in one frame).
// Direct single-frame response is correct here; BAM is only for >8 bytes.
void sendOdoFrame() {
  // Single frame — FEE8 fits in 8 bytes, no transport protocol needed
  sendJ1939from(0xFEE8, fee8body, 0x1C);
}

// FEF7 battery voltage:
//   bytes 4-5 hold a 16-bit value; unit = 0.05 V/bit, offset 0
//   Confirmed: 0x011A = 282 → 282 * 0.05 = 14.1 V
void rebuildFEF7() {
  uint16_t raw = (uint16_t)(currentVolts / 0.05f + 0.5f);
  fef7[4] = raw & 0xFF;
  fef7[5] = raw >> 8;
}

// FEFC fuel level:
//   byte 1 = percent × 2.5  (0–250 maps to 0–100 %)
//   Confirmed: 250 = 100 %  (byte 0 and others are unused / 0xFA)
void rebuildFEFC() {
  fefc[1] = (uint8_t)(currentFuel * 2.5f + 0.5f);
}

void rebuildEEC1() {
  uint16_t raw = (uint16_t)(currentRPM / 0.125f);
  eec1[3] = raw & 0xFF;
  eec1[4] = raw >> 8;
}

void rebuildCCVS() {
  uint16_t raw = (uint16_t)(currentSpeed * 256);
  ccvs[1] = raw & 0xFF;
  ccvs[2] = raw >> 8;
}

void rebuildET1() {
  et1[0] = (uint8_t)(currentTemp + 40);
}

// ── CAN transmit helpers ─────────────────────────────────────

// Standard broadcast (priority 6, any SA)
void sendJ1939(uint32_t pgn, uint8_t* data, uint8_t sa = SA_ENGINE) {
  twai_message_t msg = {};
  msg.identifier = (0x06UL << 26) | (pgn << 8) | sa;
  msg.extd = 1;
  msg.data_length_code = 8;
  memcpy(msg.data, data, 8);
  twai_transmit(&msg, pdMS_TO_TICKS(10));
}

// Peer-to-peer / proprietary (checks bus health first)
void sendJ1939from(uint32_t pgn, uint8_t* data, uint8_t sa) {
  twai_status_info_t st;
  twai_get_status_info(&st);
  if (st.state == TWAI_STATE_BUS_OFF) return;
  twai_message_t msg = {};
  msg.identifier = (0x06UL << 26) | (pgn << 8) | sa;
  msg.extd = 1;
  msg.data_length_code = 8;
  memcpy(msg.data, data, 8);
  twai_transmit(&msg, pdMS_TO_TICKS(10));
}

// ── Serial command parser ─────────────────────────────────────
String serialBuffer = "";

void printHelp() {
  Serial.println(F("┌─ COMMANDS ───────────────────────────────┐"));
  Serial.println(F("│ GAUGES                                    │"));
  Serial.println(F("│   RPM<n>   0-4500                         │"));
  Serial.println(F("│   SPD<n>   0-160 km/h                     │"));
  Serial.println(F("│   TMP<n>   -40 to 210 °C                  │"));
  Serial.println(F("│   OIL<n>   0-200 PSI (pressure)           │"));
  Serial.println(F("│   LVL<n>   0-100 % (oil level)            │"));
  Serial.println(F("│   VLT<n.n> 0.0-32.0 V  (e.g. VLT14.1)   │"));
  Serial.println(F("│   FUL<n>   0-100 %                        │"));
  Serial.println(F("│   AR1<n>   0-150 PSI (brake air tank 1)   │"));
  Serial.println(F("│   AR2<n>   0-150 PSI (brake air tank 2)   │"));
  Serial.println(F("│   AR1RAW=xx  direct raw hex (probe only)  │"));
  Serial.println(F("│   AR2RAW=xx  direct raw hex (probe only)  │"));
  Serial.println(F("│ DM1 LAMPS  (suffix 1=on 0=off)            │"));
  Serial.println(F("│   MIL RSL AWL PL MNT                      │"));
  Serial.println(F("│ EF17 LAMPS (suffix 1=on 0=off)            │"));
  Serial.println(F("│   WTS IDS PKB EXT                         │"));
  Serial.println(F("│   RNG DPT DPF                             │"));
  Serial.println(F("│   SBL RFL AFL ECO LTN HBM RTN             │"));
  Serial.println(F("│ RAW BYTE                                  │"));
  Serial.println(F("│   EF17Bn=xx  (n=1-8, xx=hex)             │"));
  Serial.println(F("│ UTIL                                      │"));
  Serial.println(F("│   STATUS   SENDDM1   HELP                 │"));
  Serial.println(F("└───────────────────────────────────────────┘"));
}

void printStatus() {
  Serial.println(F("─── STATUS ─────────────────────────────────"));
  Serial.printf("Gauges : RPM=%d  SPD=%d km/h  TMP=%d°C\n", currentRPM, currentSpeed, currentTemp);
  Serial.printf("         OIL=%d PSI  LVL=%d%%  VLT=%.1f V  FUL=%d%%\n", currentOilP, currentOilL, currentVolts, currentFuel);
  Serial.printf("         AIR1=%d PSI  AIR2=%d PSI\n", currentAir1, currentAir2);
  Serial.printf("DM1    : MIL=%d RSL=%d AWL=%d PL=%d MNT=%d\n", lampMIL, lampRSL, lampAWL, lampPL, lampMaint);
  Serial.printf("EF17b2 : WTS=%d IDS=%d PKB=%d EXT=%d\n", lampWTS, lampIDS, lampPKB, lampEXT);
  Serial.printf("EF17b3 : RNG=%d DPT=%d DPF=%d\n", lampRNG, lampDPFT, lampDPFF);
  Serial.printf("EF17b4 : SBL=%d RFL=%d AFL=%d ECO=%d LTN=%d HBM=%d\n", lampSBL, lampRFL, lampAFL, lampECO, lampLTN, lampHBM);
  Serial.printf("EF17b5 : RTN=%d\n", lampRTN);
  Serial.printf("EF17   : %02X %02X %02X %02X %02X %02X %02X %02X\n",
                ef17[0],ef17[1],ef17[2],ef17[3],ef17[4],ef17[5],ef17[6],ef17[7]);
  Serial.println(F("────────────────────────────────────────────"));
}

void processCommand(String cmd) {
  cmd.trim();
  cmd.toUpperCase();

  // ── Numeric gauge commands ──────────────────────────────────
  if (cmd.startsWith("RPM")) {
    int v = cmd.substring(3).toInt();
    if (v >= 0 && v <= 4500) { currentRPM = v; rebuildEEC1(); Serial.printf("OK RPM %d\n", v); }
    else Serial.println(F("ERR RPM 0-4500"));

  } else if (cmd.startsWith("SPD")) {
    int v = cmd.substring(3).toInt();
    if (v >= 0 && v <= 160) { currentSpeed = v; rebuildCCVS(); Serial.printf("OK SPD %d km/h\n", v); }
    else Serial.println(F("ERR SPD 0-160"));

  } else if (cmd.startsWith("TMP")) {
    int v = cmd.substring(3).toInt();
    if (v >= -40 && v <= 210) { currentTemp = v; rebuildET1(); Serial.printf("OK TMP %d C\n", v); }
    else Serial.println(F("ERR TMP -40 to 210"));

  } else if (cmd.startsWith("OIL")) {
    int v = cmd.substring(3).toInt();
    if (v >= 0 && v <= 200) { currentOilP = v; rebuildFEEF(); Serial.printf("OK OIL %d PSI\n", v); }
    else Serial.println(F("ERR OIL 0-200"));

  } else if (cmd.startsWith("LVL")) {
    int v = cmd.substring(3).toInt();
    if (v >= 0 && v <= 100) { currentOilL = v; rebuildFEEF(); Serial.printf("OK LVL %d%%\n", v); }
    else Serial.println(F("ERR LVL 0-100"));

  } else if (cmd.startsWith("VLT")) {
    float v = cmd.substring(3).toFloat();
    if (v >= 0.0f && v <= 32.0f) { currentVolts = v; rebuildFEF7(); Serial.printf("OK VLT %.2f V\n", v); }
    else Serial.println(F("ERR VLT 0.0-32.0"));

  } else if (cmd.startsWith("FUL")) {
    int v = cmd.substring(3).toInt();
    if (v >= 0 && v <= 100) { currentFuel = v; rebuildFEFC(); Serial.printf("OK FUL %d%%\n", v); }
    else Serial.println(F("ERR FUL 0-100"));

  } else if (cmd.startsWith("AR1")) {
    int v = cmd.substring(3).toInt();
    if (v >= 0 && v <= 150) { currentAir1 = v; rebuildAir(); Serial.printf("OK AR1 %d PSI\n", v); }
    else Serial.println(F("ERR AR1 0-150"));

  } else if (cmd.startsWith("AR2")) {
    int v = cmd.substring(3).toInt();
    if (v >= 0 && v <= 150) { currentAir2 = v; rebuildAir(); Serial.printf("OK AR2 %d PSI\n", v); }
    else Serial.println(F("ERR AR2 0-150"));

  // Raw air byte override — send a specific raw value directly to fef3 for needle probing
  // Usage: AR1RAW=xx  (hex, e.g. AR1RAW=67 for 0x67)
  } else if (cmd.startsWith("AR1RAW=")) {
    uint8_t v = (uint8_t)strtol(cmd.substring(7).c_str(), NULL, 16);
    airRaw1 = v; fef3[0] = v; fef3[1] = v; airFEF5[0] = v; airProp[1] = v;
    Serial.printf("OK AR1 raw=0x%02X\n", v);
  } else if (cmd.startsWith("AR2RAW=")) {
    uint8_t v = (uint8_t)strtol(cmd.substring(7).c_str(), NULL, 16);
    airRaw2 = v; fef3[2] = v; airFEF5[1] = v; airProp[2] = v;
    Serial.printf("OK AR2 raw=0x%02X\n", v);

  // ── DM1 lamps ──────────────────────────────────────────────
  } else if (cmd == "MIL1") { lampMIL=true;  rebuildDM1(); Serial.println(F("Check Engine ON")); }
    else if (cmd == "MIL0") { lampMIL=false; rebuildDM1(); Serial.println(F("Check Engine OFF")); }
    else if (cmd == "RSL1") { lampRSL=true;  rebuildDM1(); Serial.println(F("Stop Engine ON")); }
    else if (cmd == "RSL0") { lampRSL=false; rebuildDM1(); Serial.println(F("Stop Engine OFF")); }
    else if (cmd == "AWL1") { lampAWL=true;  rebuildDM1(); Serial.println(F("Warn Engine ON")); }
    else if (cmd == "AWL0") { lampAWL=false; rebuildDM1(); Serial.println(F("Warn Engine OFF")); }
    else if (cmd == "PL1")  { lampPL=true;   rebuildDM1(); Serial.println(F("Protect Lamp ON")); }
    else if (cmd == "PL0")  { lampPL=false;  rebuildDM1(); Serial.println(F("Protect Lamp OFF")); }
    else if (cmd == "MNT1") { lampMaint=true;  rebuildDM1(); Serial.println(F("Maintenance ON")); }
    else if (cmd == "MNT0") { lampMaint=false; rebuildDM1(); Serial.println(F("Maintenance OFF")); }

  // ── EF17 lamps ─────────────────────────────────────────────
    else if (cmd == "WTS1") { lampWTS=true;  rebuildEF17(); rebuildFEEF(); Serial.println(F("WTS ON")); }
    else if (cmd == "WTS0") { lampWTS=false; rebuildEF17(); rebuildFEEF(); Serial.println(F("WTS OFF")); }
    else if (cmd == "IDS1") { lampIDS=true;  rebuildEF17(); Serial.println(F("Idle Shutdown ON")); }
    else if (cmd == "IDS0") { lampIDS=false; rebuildEF17(); Serial.println(F("Idle Shutdown OFF")); }
    else if (cmd == "PKB1") { lampPKB=true;  rebuildEF17(); Serial.println(F("Park Brake ON")); }
    else if (cmd == "PKB0") { lampPKB=false; rebuildEF17(); Serial.println(F("Park Brake OFF")); }
    else if (cmd == "EXT1") { lampEXT=true;  rebuildEF17(); Serial.println(F("Emergency Exit OPEN")); }
    else if (cmd == "EXT0") { lampEXT=false; rebuildEF17(); Serial.println(F("Emergency Exit CLOSED")); }
    else if (cmd == "RNG1") { lampRNG=true;  rebuildEF17(); Serial.println(F("Range Inhibited ON")); }
    else if (cmd == "RNG0") { lampRNG=false; rebuildEF17(); Serial.println(F("Range Inhibited OFF")); }
    else if (cmd == "DPT1") { lampDPFT=true;  rebuildEF17(); Serial.println(F("DPF Temp ON")); }
    else if (cmd == "DPT0") { lampDPFT=false; rebuildEF17(); Serial.println(F("DPF Temp OFF")); }
    else if (cmd == "DPF1") { lampDPFF=true;  rebuildEF17(); Serial.println(F("DPF Full ON")); }
    else if (cmd == "DPF0") { lampDPFF=false; rebuildEF17(); Serial.println(F("DPF Full OFF")); }
    else if (cmd == "SBL1") { lampSBL=true;  rebuildEF17(); Serial.println(F("Seatbelt ON")); }
    else if (cmd == "SBL0") { lampSBL=false; rebuildEF17(); Serial.println(F("Seatbelt OFF")); }
    else if (cmd == "RFL1") { lampRFL=true;  rebuildEF17(); Serial.println(F("Red Flasher ON")); }
    else if (cmd == "RFL0") { lampRFL=false; rebuildEF17(); Serial.println(F("Red Flasher OFF")); }
    else if (cmd == "AFL1") { lampAFL=true;  rebuildEF17(); Serial.println(F("Amber Flasher ON")); }
    else if (cmd == "AFL0") { lampAFL=false; rebuildEF17(); Serial.println(F("Amber Flasher OFF")); }
    else if (cmd == "ECO1") { lampECO=true;  rebuildEF17(); Serial.println(F("Economy ON")); }
    else if (cmd == "ECO0") { lampECO=false; rebuildEF17(); Serial.println(F("Economy OFF")); }
    else if (cmd == "LTN1") { lampLTN=true;  rebuildEF17(); Serial.println(F("Left Turn ON")); }
    else if (cmd == "LTN0") { lampLTN=false; rebuildEF17(); Serial.println(F("Left Turn OFF")); }
    else if (cmd == "HBM1") { lampHBM=true;  rebuildEF17(); Serial.println(F("High Beam ON")); }
    else if (cmd == "HBM0") { lampHBM=false; rebuildEF17(); Serial.println(F("High Beam OFF")); }
    else if (cmd == "RTN1") { lampRTN=true;  rebuildEF17(); Serial.println(F("Right Turn ON")); }
    else if (cmd == "RTN0") { lampRTN=false; rebuildEF17(); Serial.println(F("Right Turn OFF")); }

  // ── Raw EF17 byte access ────────────────────────────────────
    else if (cmd.startsWith("EF17B")) {
      int byteNum = cmd.substring(5, 6).toInt();
      int eqPos   = cmd.indexOf('=');
      if (byteNum >= 1 && byteNum <= 8 && eqPos > 0) {
        uint8_t val = (uint8_t)strtol(cmd.substring(eqPos + 1).c_str(), NULL, 16);
        ef17[byteNum - 1] = val;
        Serial.printf("OK ef17[%d] = %02X\n", byteNum, val);
      } else Serial.println(F("ERR format: EF17Bn=xx (n=1-8, xx=hex)"));

  // ── Utilities ──────────────────────────────────────────────
    } else if (cmd == "SENDDM1") {
      sendJ1939(0x00FECA, dm1);
      Serial.println(F("OK DM1 sent"));
    } else if (cmd == "STATUS") {
      printStatus();
    } else if (cmd == "HELP") {
      printHelp();
    } else {
      Serial.printf("ERR unknown: %s\n", cmd.c_str());
    }
}

// ── Setup ─────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX, CAN_RX, TWAI_MODE_NORMAL);
  twai_timing_config_t  t = TWAI_TIMING_CONFIG_250KBITS();
  twai_filter_config_t  f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
  twai_driver_install(&g, &t, &f);
  twai_start();

  // Address claims
  auto sendClaim = [](uint8_t sa, uint8_t nameTag) {
    twai_message_t c = {};
    c.identifier = (0x06UL << 26) | (0x00EE00UL << 8) | sa;
    c.extd = 1; c.data_length_code = 8;
    uint8_t n[8] = { 0,0,0,0,0,0,0, nameTag };
    memcpy(c.data, n, 8);
    twai_transmit(&c, pdMS_TO_TICKS(50));
    delay(50);
  };
  sendClaim(SA_ENGINE, 0x80);   // Engine ECU
  sendClaim(SA_ABS,    0x81);   // ABS controller
  sendClaim(SA_ICU,    0x82);   // ICU
  sendClaim(0x1C,      0x83);   // Body controller (odometer source)

  // ABS DM3 — clear active faults on start
  uint8_t dm3[8] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
  sendJ1939(0xFECC, dm3, SA_ABS);
  delay(100);

  // Build all initial frames
  rebuildDM1();
  rebuildEF17();
  rebuildEEC1();
  rebuildCCVS();
  rebuildET1();
  rebuildFEEF();
  rebuildFEF7();
  rebuildFEFC();
  rebuildAir();

  delay(250);
  sendOdoFrame();  // immediate SA 0x1C presence on startup
  Serial.println(F("IC CE200 emulator R3 ready. Type HELP for commands."));
}

// ── Main loop ─────────────────────────────────────────────────
void loop() {
  uint32_t now = millis();

  // Serial input
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length() > 0) {
        processCommand(serialBuffer);
        serialBuffer = "";
      }
    } else {
      serialBuffer += c;
    }
  }

  // Inbound CAN — respond to PGN requests (PGN 0xEA00)
  twai_message_t rx;
  if (twai_receive(&rx, 0) == ESP_OK) {
    uint8_t  sa  = rx.identifier & 0xFF;
    uint32_t pgn = (rx.identifier >> 8) & 0x3FFFF;

    // Debug dump — comment out once cluster is stable
    Serial.printf("RX SA:%02X PGN:%05X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
      sa, pgn,
      rx.data[0],rx.data[1],rx.data[2],rx.data[3],
      rx.data[4],rx.data[5],rx.data[6],rx.data[7]);

    if ((pgn & 0xFF00) == 0xEA00) {
      uint32_t req = rx.data[0] | ((uint32_t)rx.data[1] << 8) | ((uint32_t)rx.data[2] << 16);
      uint8_t  dst = pgn & 0xFF;
      if (req == 0x00FEE8) {
        uint8_t replyFrom = (dst == 0x1C) ? 0x1C : SA_ENGINE;
        uint8_t* replyBuf = (dst == 0x1C) ? fee8body : eflp;
        sendJ1939from(0xFEE8, replyBuf, replyFrom);
      }
      if (req == 0x00FEE9) sendJ1939(0xFEE9, engHours,  SA_ENGINE);
      if (req == 0x00FEEA) {
        sendJ1939(0xFEEA, feea, SA_ENGINE);
        sendJ1939(0xFEEA, feea, SA_ICU);   // cluster sends EA21 requesting FEEA from SA_ICU specifically
      }
      if (req == 0x00FEE7) sendJ1939(0xFEE7, fee7,  SA_ENGINE);
      if (req == 0x00FEDC) sendJ1939(0xFEDC, eflp2, SA_ENGINE);
      if (req == 0x00FECE) sendJ1939(0xFECE, fece,  SA_ENGINE);
      if (req == 0x00FEF5) sendJ1939(0xFEF5, airFEF5, SA_ICU);
      if (req == 0x00FEB3) sendJ1939(0xFEB3, feb3,  SA_ENGINE);
      if (req == 0x00FEE5) sendJ1939(0xFEE5, fee5,  SA_ENGINE);
      if (req == 0x00FEEF) sendJ1939(0xFEEF, feef,  SA_ENGINE);
      if (req == 0x00FEFC) sendJ1939(0xFEFC, fefc,  SA_ICU);
      if (req == 0x00FEF7) sendJ1939(0xFEF7, fef7,  SA_ICU);
      if (req == 0x00FEF3) {
        sendJ1939(0xFEF3, fef3, SA_ABS);
        Serial.println(F("RX: FEF3 requested — responded from SA_ABS"));
      }
    }

    // ── TP Connection Management (FEC1) ────────────────────────
    // Cluster sends RTS (0xCC) to our ABS SA 0x0B wanting to send it multi-packet data.
    // We must respond with CTS (0x11) or the cluster will retry and log a comm fault.
    // pgn for peer-to-peer TP.CM = 0xEC00 | dst_sa, so pgn & 0xFF00 == 0xEC00 and pgn & 0xFF == SA_ABS
    if ((pgn & 0xFF00) == 0xEC00 && (pgn & 0xFF) == SA_ABS) {
      if (rx.data[0] == 0xCC) {  // RTS
        uint8_t numPackets = rx.data[3];
        uint32_t reqPgn = rx.data[5] | ((uint32_t)rx.data[6] << 8) | ((uint32_t)rx.data[7] << 16);
        // Send CTS — accept all packets
        uint8_t cts[8] = {
          0x11,           // CTS
          numPackets,     // accept all
          0x01,           // starting at packet 1
          0xFF, 0xFF,     // reserved
          (uint8_t)(reqPgn & 0xFF),
          (uint8_t)((reqPgn >> 8) & 0xFF),
          (uint8_t)((reqPgn >> 16) & 0xFF)
        };
        // Send CTS back to the cluster (SA 0x17) from SA_ABS
        uint8_t clusterSA = sa;
        twai_message_t ctsMsg = {};
        ctsMsg.identifier = (0x06UL << 26) | ((0xEC00UL | clusterSA) << 8) | SA_ABS;
        ctsMsg.extd = 1; ctsMsg.data_length_code = 8;
        memcpy(ctsMsg.data, cts, 8);
        twai_transmit(&ctsMsg, pdMS_TO_TICKS(10));
        Serial.printf("TX CTS to SA:%02X for PGN:%05X\n", clusterSA, reqPgn);
      }
      if (rx.data[0] == 0x13) {  // End of Message ACK — cluster finished sending
        Serial.println(F("RX: TP End-of-Message ACK from cluster"));
      }
    }
  }

  // ── 10 Hz broadcast ────────────────────────────────────────
  static uint32_t last10Hz = 0;
  if (now - last10Hz >= 100) {
    last10Hz = now;

    // Primary gauge PGNs (from engine ECU SA)
    sendJ1939(0xF004, eec1, SA_ENGINE);  // RPM
    sendJ1939(0xFEF1, ccvs, SA_ENGINE);  // Speed
    sendJ1939(0xFEEE, et1,  SA_ENGINE);  // Coolant temp
    sendJ1939(0xFEEF, feef, SA_ENGINE);  // Oil pressure
    sendJ1939(0xFEFC, fefc, SA_ICU);     // Fuel level
    sendJ1939(0xFEF7, fef7, SA_ICU);     // Battery voltage

    // Secondary / duplicate broadcasts (ICU SA)
    sendJ1939(0xF004, eec1, SA_ICU);
    sendJ1939(0xFEEE, et1,  SA_ICU);
    sendJ1939(0xFEF1, ccvs, SA_ICU);

    // Misc engine data
    sendJ1939(0xFEE8, eflp,     SA_ENGINE); // Engine fluid level (from engine ECU)
    sendJ1939(0xFEE8, fee8body, 0x1C);      // Engine fluid level (from body ctrl SA 0x1C — satisfies EA1C request)
    sendJ1939(0xFEEA, feea,     SA_ENGINE);
    sendJ1939(0xFEE7, fee7,     SA_ENGINE);
    sendJ1939(0xFEE9, engHours, SA_ENGINE); // Engine total hours
    sendJ1939(0xFEE5, tripHours,SA_ENGINE); // Trip hours
    sendJ1939(0xFEE0, vehDist,  SA_ENGINE); // Vehicle distance (trip + total)
    sendJ1939(0xFEDC, eflp2,    SA_ENGINE);
    sendJ1939(0xFECE, fece,     SA_ENGINE);
    sendJ1939(0xFEBC, febc,     SA_ENGINE);
    sendJ1939(0xFEAE, feae,     SA_ENGINE);

    // ABS / brake controller
    sendJ1939(0xFE5F, airHealth, SA_ABS);  // Air system health
    sendJ1939(0xF001, ebc1,      SA_ABS);  // Brake controller heartbeat
    sendJ1939(0xFEF3, fef3,      SA_ABS);  // Air pressure candidate (needle data)

    // Air brake pressure experiments — try commenting these blocks in/out
    sendJ1939(0xFEF5, airFEF5, SA_ICU);
    sendJ1939(0xEFFF, airProp,  SA_ICU);   // Navistar proprietary air candidate

    // FF26 Navistar body/air status — two variants; observe which one cluster uses
    sendJ1939(0xFF26, ff26,      SA_ICU);
    sendJ1939(0xFF26, ff26_data, SA_ICU);

    // Static ABS health frame
    uint8_t fe5f[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    sendJ1939(0xFE5F, fe5f, SA_ABS);

    // ICU response frame with rolling counter (byte 6 / index 5)
    static uint8_t icuCounter = 0;
    ef17[5] = icuCounter++;
    sendJ1939from(0x00EF17, ef17, SA_ICU);
    sendJ1939from(0xFEF5,   airFEF5, SA_ICU);
  }

  // ── 1 Hz broadcast ─────────────────────────────────────────
  static uint32_t last1Hz = 0;
  if (now - last1Hz >= 1000) {
    last1Hz = now;
    sendJ1939(0xFECA, dm1,    SA_ENGINE);   // DM1 engine
    sendJ1939(0xFECA, dm1abs, SA_ABS);      // DM1 ABS (always clean)
    uint8_t dm3[8] = { 0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF };
    sendJ1939(0xFECC, dm3,    SA_ABS);      // DM3 — clear brake faults
    sendOdoFrame();                          // keep SA 0x1C alive (clears "data n/a")
  }

  // ── 20 Hz ABS heartbeat ────────────────────────────────────
  static uint32_t lastABS = 0;
  if (now - lastABS >= 50) {
    lastABS = now;
    uint8_t absData[8] = { 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF };
    sendJ1939from(0xFDAB, absData, SA_ABS);
  }
}
