// ------- Configure your modem type -------
#define TINY_GSM_MODEM_SIM7000
#define TINY_GSM_RX_BUFFER 1024
#define TINY_GSM_USE_GPRS true

// Debug to Serial
#define TINY_GSM_DEBUG Serial

#include <PubSubClient.h>
#include <TinyGsmClient.h>

// -------- Pins: adjust to your hardware --------
#define MODEM_TX 27        // ESP32 -> SIM7000 RX
#define MODEM_RX 26        // ESP32 <- SIM7000 TX
#define MODEM_PWRKEY 4     // Toggle to power on
#define MODEM_POWER_ON 23  // Some boards use a power enable pin; else ignore

// -------- APN / MQTT --------
const char* APN = "internet.comcel.com.co";
const char* BROKER = "broker.emqx.io";  // or "broker.hivemq.com"
const uint16_t PORT = 1883;             // 1883 = plain; 8883 for TLS (see note)

// Topics (change to yours)
const char* TOPIC_PUB = "output/01";
const char* TOPIC_SUB = "input/01";

// Client ID & LWT
const char* CLIENT_ID = "sim7000_1";
const char* LWT_TOPIC = "ec/dev/sim7000/status";
const char* LWT_MSG = "offline";

HardwareSerial SerialAT(1);
TinyGsm modem(SerialAT);
TinyGsmClient net(modem);  // For plain MQTT
// TinyGsmClientSecure net(modem);  // ← use this for TLS (port 8883)

// MQTT client
PubSubClient mqtt(net);

// Connection timing
unsigned long lastMqttReconnect = 0;
unsigned long lastPublish = 0;

// ---------- Helpers ----------
void mqttCallback(char* topic, byte* payload, unsigned int len) {
  Serial.print("[MQTT] Message on ");
  Serial.print(topic);
  Serial.print(": ");
  for (unsigned int i = 0; i < len; i++) Serial.write(payload[i]);
  Serial.println();

  // Example: simple echo to another topic
  mqtt.publish("ec/dev/sim7000/echo", payload, len);
}

bool mqttConnect() {
  Serial.print("[MQTT] Connecting to ");
  Serial.print(BROKER);
  Serial.print(":");
  Serial.println(PORT);
  mqtt.setServer(BROKER, PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setKeepAlive(60);     // seconds
  mqtt.setBufferSize(1024);  // for larger payloads if needed

  // Last-Will & Testament
  if (mqtt.connect(CLIENT_ID, LWT_TOPIC, 1, true, LWT_MSG)) {
    Serial.println("[MQTT] Connected");
    mqtt.publish(LWT_TOPIC, "online", true);  // retained online flag
    mqtt.subscribe(TOPIC_SUB, 1);             // QoS 1
    return true;
  }
  Serial.print("[MQTT] Failed, state=");
  Serial.println(mqtt.state());
  return false;
}

bool gprsConnectIfNeeded() {
  if (modem.isGprsConnected()) return true;
  Serial.println("[NET] Connecting to network (GPRS)...");
  if (!modem.gprsConnect(APN)) {
    Serial.println("[NET] GPRS connect failed");
    return false;
  }
  Serial.print("[NET] GPRS connected. IP: ");
  Serial.println(modem.localIP());
  return true;
}

void powerOnModem() {
  // Optional POWER_EN pin
  pinMode(MODEM_POWER_ON, OUTPUT);
  digitalWrite(MODEM_POWER_ON, HIGH);
  delay(100);

  // PWRKEY pulse: many SIM7000 boards require a 1–2 s LOW pulse
  pinMode(MODEM_PWRKEY, OUTPUT);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(1500);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(3000);  // wait for boot
}

void setup() {
  Serial.begin(115200);
  delay(200);

  Serial.println("\n=== ESP32 + SIM7000 MQTT (TinyGSM) ===");

  powerOnModem();

  // Start modem serial
  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(300);

  // Initialize modem
  Serial.println("[MODEM] Restarting...");
  if (!modem.restart()) {
    Serial.println("[MODEM] Restart failed, trying AT");
    modem.sendAT("");
    delay(1000);
  }

  // Optional: set SIM7000 to full functionality
  modem.sendAT("+CFUN=1");
  modem.waitResponse();

  // Optional: ensure SMS text mode disabled from interfering
  modem.sendAT("+CMEE=2");  // verbose errors
  modem.waitResponse();

  // Attach to network (auto RAT; works for GSM or LTE-M depending on coverage)
  Serial.println("[MODEM] Waiting for network...");
  if (!modem.waitForNetwork(60000L)) {
    Serial.println(
        "[MODEM] Network attach failed");  // still continue; sometimes GPRS
                                           // works anyway
  } else {
    Serial.println("[MODEM] Network OK");
  }

  // Bring up data
  gprsConnectIfNeeded();

  // Connect MQTT
  mqttConnect();
}

void loop() {
  // Keep MQTT alive
  if (mqtt.connected()) {
    mqtt.loop();
  } else {
    unsigned long now = millis();
    if (now - lastMqttReconnect > 5000) {
      lastMqttReconnect = now;
      // Ensure data link is up before MQTT retry
      if (gprsConnectIfNeeded()) mqttConnect();
    }
  }

  // Heartbeat publish every 15 seconds
  unsigned long now = millis();
  if (now - lastPublish > 15000 && mqtt.connected()) {
    lastPublish = now;

    // Build a small status payload
    String ip = modem.localIP().toString();
    int rssi = modem.getSignalQuality();  // in dBm (negative)
    String msg = String("{\"ip\":\"") + ip + "\",\"rssi\":" + rssi + "}";

    Serial.print("[PUB] ");
    Serial.println(msg);
    mqtt.publish(TOPIC_PUB, msg.c_str(), true);  // retained
  }
}
