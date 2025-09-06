// ESP32 <-> SIM7000 (UART)
// Wiring: SIM7000 TX -> ESP32 GPIO26 (RX1)
//         SIM7000 RX -> ESP32 GPIO27 (TX1)
//         GNDs common. Power SIM7000 properly.
// Serial1 @ 9600 as requested.

#include <Arduino.h>

HardwareSerial MODEM(1);  // Use UART1

// Pins
static const int MODEM_RX = 26;
static const int MODEM_TX = 27;

// Helpers
String readResponse(uint32_t timeout_ms = 1500) {
  uint32_t start = millis();
  String resp;
  while (millis() - start < timeout_ms) {
    while (MODEM.available()) {
      char c = (char)MODEM.read();
      resp += c;
    }
    if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) break;
    delay(10);
  }
  return resp;
}

String atCommand(const String& cmd, uint32_t wait_ms = 1500,
                 bool echo = false) {
  // Send with CRLF
  MODEM.flush();
  MODEM.print(cmd);
  MODEM.print("\r\n");
  if (echo) {
    Serial.print("[TX] ");
    Serial.println(cmd);
  }
  String resp = readResponse(wait_ms);
  if (echo) {
    Serial.print("[RX] ");
    Serial.println(resp);
  }
  return resp;
}

bool waitForOK(const String& resp) {
  return resp.indexOf("\nOK") >= 0 || resp.endsWith("OK") ||
         resp.indexOf("OK\r\n") >= 0;
}

// ===== Checks =====

bool modemAlive() {
  for (int i = 0; i < 5; ++i) {
    String r = atCommand("AT", 500, true);
    if (waitForOK(r)) return true;
    delay(200);
  }
  return false;
}

// Returns true and fills operatorName/mode if registered
bool getOperator(String& operatorName, int& mode) {
  // AT+COPS?  -> +COPS: <mode>,<format>,<oper>,<AcT>
  // <oper> is quoted when format=0 (long alpha)
  String r = atCommand("AT+COPS?", 1500, true);
  if (!waitForOK(r)) return false;

  int p = r.indexOf("+COPS:");
  if (p < 0) return false;

  // crude parse: +COPS: 0,0,"Carrier",7
  // extract the quoted name if present
  int q1 = r.indexOf('\"', p);
  int q2 = r.indexOf('\"', q1 + 1);
  if (q1 >= 0 && q2 > q1) {
    operatorName = r.substring(q1 + 1, q2);
  } else {
    operatorName = "";
  }

  // Try to parse AcT (last number)
  // Find last comma after "+COPS:"
  int lastComma = r.lastIndexOf(',');
  if (lastComma > p) {
    // Next token until \r or \n
    int end = r.indexOf('\n', lastComma);
    if (end < 0) end = r.length();
    String actStr = r.substring(lastComma + 1, end);
    actStr.trim();
    mode = actStr.toInt();  // 7=E-UTRAN (LTE), 9=Cat-M, etc., vendor-specific
                            // variants exist
  } else {
    mode = -1;
  }

  // If there's any operator name or valid pattern, consider carrier detected
  return true;
}

// Packet service attach? (data availability)
bool isPacketAttached() {
  // AT+CGATT? -> +CGATT: 1 (attached)
  String r = atCommand("AT+CGATT?", 1500, true);
  if (!waitForOK(r)) return false;
  return r.indexOf("+CGATT: 1") >= 0;
}

// Optional signal check
int getCSQ() {
  // AT+CSQ -> +CSQ: <rssi>,<ber>
  String r = atCommand("AT+CSQ", 1500, true);
  if (!waitForOK(r)) return -1;
  int p = r.indexOf("+CSQ:");
  if (p < 0) return -1;
  int comma = r.indexOf(',', p);
  if (comma < 0) return -1;
  String rssiStr = r.substring(p + 5, comma);  // after "+CSQ:"
  rssiStr.trim();
  return rssiStr.toInt();  // 0..31, 99=unknown
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n=== ESP32 <-> SIM7000 Carrier & Data Check ===");
  Serial.println("Init UART1 @ 9600 (RX=26, TX=27)...");

  MODEM.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(600);

  // Wake echo on (optional)
  atCommand("ATE1", 500, true);
  // Verbose errors (optional)
  atCommand("AT+CMEE=2", 500, true);

  // Check modem presence
  if (!modemAlive()) {
    Serial.println("ERROR: No response to AT. Check power, wiring, and port.");
    return;
  }
  Serial.println("Modem responded to AT.");

  // SIM ready?
  String cpin = atCommand("AT+CPIN?", 1500, true);
  if (cpin.indexOf("READY") < 0) {
    Serial.println("Warning: SIM not ready or PIN required.");
  } else {
    Serial.println("SIM is READY.");
  }

  // Signal (optional)
  int csq = getCSQ();
  if (csq >= 0 && csq != 99) {
    Serial.print("Signal RSSI index: ");
    Serial.println(csq);
  } else {
    Serial.println("Signal unknown or weak (CSQ=99).");
  }

  // Carrier detection
  String oper;
  int act = -1;
  bool hasCarrier = getOperator(oper, act);
  if (hasCarrier) {
    Serial.print("Carrier detected: ");
    if (oper.length())
      Serial.print(oper);
    else
      Serial.print("(unknown)");
    Serial.print(" | AcT: ");
    Serial.println(act);
  } else {
    Serial.println("No carrier detected (COPS? failed).");
  }

  // Packet service availability (attached?)
  bool attached = isPacketAttached();
  Serial.print("Packet service attached (CGATT): ");
  Serial.println(attached ? "YES" : "NO");

  Serial.println("=== Done ===");
}

void loop() {
  // Optionally poll periodically
  static uint32_t t0 = millis();
  if (millis() - t0 > 10000) {  // every 10s
    t0 = millis();

    bool attached = isPacketAttached();
    Serial.print("[Periodic] Data attached: ");
    Serial.println(attached ? "YES" : "NO");

    String oper;
    int act = -1;
    if (getOperator(oper, act)) {
      Serial.print("[Periodic] Carrier: ");
      Serial.print(oper.length() ? oper : "(unknown)");
      Serial.print(" | AcT: ");
      Serial.println(act);
    }
  }

  // Non-blocking pass-through (optional debug)
  while (Serial.available()) {
    MODEM.write(Serial.read());
  }
  while (MODEM.available()) {
    Serial.write(MODEM.read());
  }
}
