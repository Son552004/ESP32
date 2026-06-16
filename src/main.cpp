#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "MAX30100_PulseOximeter.h"

// ============================================================
// FREERTOS — 2 QUEUE GIAO TIẾP GIỮA 2 CORE
// ============================================================
struct SensorData {
    float bpm;
    int spo2;
};

typedef enum : uint8_t {
    CMD_SLEEP = 0,
    CMD_WAKEUP = 1,
    CMD_RESTART = 2,
    CMD_ALERT_ON = 3,
    CMD_ALERT_OFF = 4
} DeviceCmd;

static QueueHandle_t sensorQueue      = nullptr;
static QueueHandle_t cmdQueue         = nullptr;
static TaskHandle_t  sensorTaskHandle = nullptr;

// ============================================================
// CẤU HÌNH CHÂN
// ============================================================
#define BOOT_BUTTON_PIN 0

// Còi thụ động
#define BUZZER_PIN 25
#define BUZZER_PWM_CHANNEL 0
#define BUZZER_PWM_RESOLUTION 8
#define BUZZER_FREQ 2000   // Có thể đổi 1000, 1500, 2500 Hz

// ============================================================
// CẤU HÌNH HIVEMQ
// ============================================================
const char* mqtt_server    = "a610b199cda842b3a6207037ad778131.s1.eu.hivemq.cloud";
const int   mqtt_port      = 8883;
const char* mqtt_user      = "admin";
const char* mqtt_pass      = "01233256549Na";

const char* mqtt_topic     = "HealthData_Upload_2026";
const char* mqtt_cmd_topic = "HealthData_Command_2026";
const char* mqtt_ack_topic = "HealthData_Ack_2026";

WebServer        localServer(80);
Preferences      preferences;
PulseOximeter    pox;
WiFiClientSecure espClient;
PubSubClient     mqttClient(espClient);

String ssid = "", password = "", userId = "";
bool   isSetupMode = false;

// ============================================================
// HÀM ĐIỀU KHIỂN CÒI THỤ ĐỘNG
// ============================================================
void buzzerOn() {
    ledcWriteTone(BUZZER_PWM_CHANNEL, BUZZER_FREQ);
}

void buzzerOff() {
    ledcWriteTone(BUZZER_PWM_CHANNEL, 0);
}

// ============================================================
// BỘ LỌC — CHỈ DÙNG TRONG SensorTask
// ============================================================
struct MovingAverage {
    static const int SIZE = 5;
    float buf[SIZE] = {};
    int idx = 0;

    float apply(float v) {
        buf[idx] = v;
        idx = (idx + 1) % SIZE;

        float s = 0;
        for (int i = 0; i < SIZE; i++) s += buf[i];

        return s / SIZE;
    }

    void reset() {
        idx = 0;
        memset(buf, 0, sizeof(buf));
    }
};

struct KalmanBPM {
    float q = 0.01, r = 0.5, x = 0, p = 1;

    float apply(float m) {
        if (isnan(m) || m < 40 || m > 200) return x;

        if (x == 0) {
            x = m;
            return x;
        }

        p += q;
        float k = p / (p + r);
        x += k * (m - x);
        p = (1 - k) * p;

        return constrain(x, 40.f, 200.f);
    }

    void reset() {
        x = 0;
        p = 1;
    }
};

struct KalmanSpO2 {
    float q = 0.005, r = 0.5, x = 95, p = 1;

    float apply(float m) {
        if (isnan(m) || m < 70 || m > 100) return x;

        p += q;
        float k = p / (p + r);
        x += k * (m - x);
        p = (1 - k) * p;

        return constrain(x, 70.f, 100.f);
    }

    void reset() {
        x = 95;
        p = 1;
    }
};

// ============================================================
// MQTT CALLBACK — Core 0, KHÔNG đụng I2C
// ============================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String msg = "";

    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    Serial.println("[Core0] 📥 CMD: " + msg);

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, msg)) {
        Serial.println("[Core0] ❌ JSON command không hợp lệ");
        return;
    }

    String command   = doc["command"].as<String>();
    String commandId = doc["commandId"].as<String>();
    String reqUser   = doc["userId"].as<String>();

    if (reqUser != userId) {
        Serial.println("[Core0] ⚠️ Lệnh không đúng userId, bỏ qua");
        return;
    }

    // Gửi ACK về server
    StaticJsonDocument<200> ack;
    ack["commandId"] = commandId;
    ack["userId"]    = userId;
    ack["status"]    = "Dang xu ly...";

    char ackBuf[256];
    serializeJson(ack, ackBuf);
    mqttClient.publish(mqtt_ack_topic, ackBuf);

    DeviceCmd cmd;

    if (command == "SLEEP") {
        cmd = CMD_SLEEP;
        xQueueSend(cmdQueue, &cmd, 0);
    }
    else if (command == "WAKEUP") {
        cmd = CMD_WAKEUP;
        xQueueSend(cmdQueue, &cmd, 0);
    }
    else if (command == "RESTART") {
        cmd = CMD_RESTART;
        xQueueSend(cmdQueue, &cmd, 0);
    }
    else if (command == "ALERT_ON") {
        cmd = CMD_ALERT_ON;
        xQueueSend(cmdQueue, &cmd, 0);
    }
    else if (command == "ALERT_OFF") {
        cmd = CMD_ALERT_OFF;
        xQueueSend(cmdQueue, &cmd, 0);
    }
    else {
        Serial.println("[Core0] ⚠️ Lệnh không hỗ trợ: " + command);
    }
}

void reconnectMQTT() {
    int retry = 0;

    while (!mqttClient.connected() && WiFi.status() == WL_CONNECTED && retry < 5) {
        String id = "ESP32-Health-" + String(random(0xffff), HEX);

        if (mqttClient.connect(id.c_str(), mqtt_user, mqtt_pass)) {
            mqttClient.subscribe(mqtt_cmd_topic);
            Serial.println("[Core0] ✅ MQTT OK");
        } else {
            Serial.printf("[Core0] ❌ MQTT err=%d\n", mqttClient.state());
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            retry++;
        }
    }
}

void handleSetup() {
    if (!localServer.hasArg("plain")) return;

    DynamicJsonDocument doc(512);
    deserializeJson(doc, localServer.arg("plain"));

    preferences.begin("iot_app", false);
    preferences.putString("ssid",     doc["ssid"].as<String>());
    preferences.putString("password", doc["password"].as<String>());
    preferences.putString("userId",   doc["userId"].as<String>());
    preferences.end();

    localServer.send(200, "application/json", "{\"message\":\"OK\"}");

    delay(1500);
    ESP.restart();
}

// ============================================================
// CORE 0 — NETWORK TASK
// ============================================================
void NetworkTask(void* pvParameters) {
    if (!isSetupMode) {
        WiFi.begin(ssid.c_str(), password.c_str());
        Serial.print("[Core0] Kết nối WiFi");

        int att = 0;
        while (WiFi.status() != WL_CONNECTED && att < 20) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            Serial.print(".");
            att++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n[Core0] ✅ WiFi: " + WiFi.localIP().toString());
        } else {
            Serial.println("\n[Core0] ⚠️ WiFi thất bại, sẽ retry");
        }
    }

    for (;;) {
        if (isSetupMode) {
            localServer.handleClient();
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        if (WiFi.status() != WL_CONNECTED) {
            WiFi.reconnect();
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            continue;
        }

        if (!mqttClient.connected()) {
            reconnectMQTT();
        }

        mqttClient.loop();

        SensorData data;
        if (xQueueReceive(sensorQueue, &data, 0) == pdTRUE) {
            Serial.printf("[Core0] 📦 BPM=%.0f | SpO2=%d\n", data.bpm, data.spo2);

            if (mqttClient.connected()) {
                StaticJsonDocument<200> doc;

                doc["userId"] = userId;
                doc["bpm"]    = round(data.bpm);
                doc["spo2"]   = data.spo2;

                char buf[256];
                serializeJson(doc, buf);

                if (mqttClient.publish(mqtt_topic, buf)) {
                    Serial.println("[Core0] ☁️ Đã gửi BPM/SpO2 lên server!");
                }
            }
        }

        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// ============================================================
// CORE 1 — SENSOR TASK
// ============================================================
void SensorTask(void* pvParameters) {
    if (isSetupMode) {
        Serial.println("[Core1] Setup Mode — SensorTask suspended");
        vTaskSuspend(nullptr);
        return;
    }

    Wire.begin(21, 22);
    Wire.setClock(400000);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    Wire.beginTransmission(0x57);
    if (Wire.endTransmission() != 0) {
        Serial.println("[Core1] ❌ MAX30100 không phản hồi. Dừng task.");
        vTaskDelete(nullptr);
        return;
    }

    if (!pox.begin()) {
        Serial.println("[Core1] ❌ pox.begin() thất bại. Dừng task.");
        vTaskDelete(nullptr);
        return;
    }

    pox.setIRLedCurrent(MAX30100_LED_CURR_27_1MA);
    pox.setOnBeatDetectedCallback([](){});

    Serial.println("[Core1] ✅ MAX30100 khởi tạo xong");

    Serial.println("[Core1] ⏳ Warm-up 5 giây...");
    uint32_t warmupStart = millis();

    while (millis() - warmupStart < 5000) {
        pox.update();
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }

    Serial.println("[Core1] ✅ Warm-up xong");

    MovingAverage mavgBpm, mavgSpo2;
    KalmanBPM     kalBpm;
    KalmanSpO2    kalSpo2;

    uint32_t fingerStartTime   = 0;
    uint32_t fingerMissingTime = 0;
    bool     isFingerStable    = false;
    bool     isSleeping        = false;

    uint32_t lastQueueSend     = 0;
    uint32_t rawDebugTimer     = 0;
    uint32_t filtDebugTimer    = 0;

    uint32_t btnPressTime      = 0;
    bool     btnPressed        = false;

    // Trạng thái còi cảnh báo
    bool     isAlerting        = false;
    uint32_t buzzerTimer       = 0;
    bool     buzzerState       = false;

    for (;;) {
        // ====================================================
        // NÚT BOOT — GIỮ 3 GIÂY ĐỂ XÓA CẤU HÌNH WIFI/USER
        // ====================================================
        if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
            if (!btnPressed) {
                btnPressed = true;
                btnPressTime = millis();
            }
            else if (millis() - btnPressTime > 3000) {
                Serial.println("[Core1] ⚠️ Xóa cấu hình!");

                preferences.begin("iot_app", false);
                preferences.clear();
                preferences.end();

                vTaskDelay(500 / portTICK_PERIOD_MS);
                ESP.restart();
            }
        } else {
            btnPressed = false;
        }

        // ====================================================
        // NHẬN LỆNH TỪ SERVER QUA Core 0
        // ====================================================
        DeviceCmd cmd;
        if (xQueueReceive(cmdQueue, &cmd, 0) == pdTRUE) {
            if (cmd == CMD_SLEEP && !isSleeping) {
                pox.shutdown();

                isSleeping = true;
                isFingerStable = false;
                fingerStartTime = 0;
                fingerMissingTime = 0;

                mavgBpm.reset();
                mavgSpo2.reset();
                kalBpm.reset();
                kalSpo2.reset();

                xQueueReset(sensorQueue);

                Serial.println("[Core1] 💤 Sensor sleep");
            }

            else if (cmd == CMD_WAKEUP && isSleeping) {
                pox.resume();

                Serial.println("[Core1] ⏳ Warm-up lại 3 giây...");
                uint32_t wu = millis();

                while (millis() - wu < 3000) {
                    pox.update();
                    vTaskDelay(1 / portTICK_PERIOD_MS);
                }

                isSleeping = false;
                Serial.println("[Core1] ☀️ Sensor sẵn sàng");
            }

            else if (cmd == CMD_RESTART) {
                buzzerOff();
                vTaskDelay(300 / portTICK_PERIOD_MS);
                ESP.restart();
            }

            else if (cmd == CMD_ALERT_ON) {
                isAlerting = true;
                Serial.println("[Core1] 🚨 Bật cảnh báo còi thụ động");
            }

            else if (cmd == CMD_ALERT_OFF) {
                isAlerting = false;
                buzzerState = false;
                buzzerOff();
                Serial.println("[Core1] ✅ Tắt cảnh báo còi");
            }
        }

        // ====================================================
        // CÒI CẢNH BÁO THỤ ĐỘNG — BÍP BÍP KHÔNG BLOCKING
        // ====================================================
        if (isAlerting) {
            if (millis() - buzzerTimer >= 300) {
                buzzerTimer = millis();
                buzzerState = !buzzerState;

                if (buzzerState) {
                    buzzerOn();
                } else {
                    buzzerOff();
                }
            }
        } else {
            buzzerOff();
        }

        // ====================================================
        // NẾU SENSOR ĐANG SLEEP THÌ KHÔNG ĐỌC MAX30100
        // Nhưng còi vẫn có thể hoạt động nếu server gửi ALERT_ON
        // ====================================================
        if (isSleeping) {
            vTaskDelay(50 / portTICK_PERIOD_MS);
            continue;
        }

        // ====================================================
        // ĐỌC MAX30100
        // ====================================================
        pox.update();

        float rawBpm  = pox.getHeartRate();
        int   rawSpo2 = pox.getSpO2();

        if (!isfinite(rawBpm))  rawBpm  = 0;
        if (!isfinite(rawSpo2)) rawSpo2 = 0;

        if (millis() - rawDebugTimer > 2000) {
            Serial.printf("[Core1] 🔴 RAW BPM=%.1f | SpO2=%d\n", rawBpm, rawSpo2);
            rawDebugTimer = millis();
        }

        // ====================================================
        // PHÁT HIỆN NGÓN TAY
        // ====================================================
        if (rawSpo2 > 50) {
            fingerMissingTime = 0;

            if (fingerStartTime == 0) {
                fingerStartTime = millis();
                Serial.println("[Core1] 👆 Phát hiện ngón tay, chờ 5s ổn định...");
            }

            if (millis() - fingerStartTime > 5000 && !isFingerStable) {
                isFingerStable = true;
                Serial.println("[Core1] ✅ Tín hiệu ổn định");
            }
        } else {
            if (fingerMissingTime == 0) {
                fingerMissingTime = millis();
            }

            if (millis() - fingerMissingTime > 2000 && fingerStartTime != 0) {
                Serial.println("[Core1] 👋 Rút tay — reset");

                fingerStartTime = 0;
                isFingerStable = false;

                mavgBpm.reset();
                mavgSpo2.reset();
                kalBpm.reset();
                kalSpo2.reset();

                xQueueReset(sensorQueue);
            }
        }

        // ====================================================
        // LỌC DỮ LIỆU VÀ GỬI SANG Core 0
        // ====================================================
        if (isFingerStable && rawBpm > 30 && rawSpo2 > 70) {
            float fBpm = constrain(
                kalBpm.apply(mavgBpm.apply(rawBpm)),
                40.f,
                200.f
            );

            int fSpo2 = constrain(
                (int)kalSpo2.apply(mavgSpo2.apply(rawSpo2)),
                70,
                100
            );

            if (millis() - filtDebugTimer > 2000) {
                Serial.printf("[Core1] ✅ FILTERED BPM=%.1f | SpO2=%d\n", fBpm, fSpo2);
                filtDebugTimer = millis();
            }

            if (millis() - lastQueueSend >= 1000) {
                SensorData d = { fBpm, fSpo2 };

                if (xQueueSend(sensorQueue, &d, 0) != pdTRUE) {
                    Serial.println("[Core1] ⚠️ Queue đầy");
                }

                lastQueueSend = millis();
            }
        }

        vTaskDelay(1 / portTICK_PERIOD_MS);
    }
}

// ============================================================
// SETUP
// ============================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

    // Khởi tạo còi thụ động PWM
    ledcSetup(BUZZER_PWM_CHANNEL, BUZZER_FREQ, BUZZER_PWM_RESOLUTION);
    ledcAttachPin(BUZZER_PIN, BUZZER_PWM_CHANNEL);
    buzzerOff();

    sensorQueue = xQueueCreate(5, sizeof(SensorData));
    cmdQueue    = xQueueCreate(5, sizeof(DeviceCmd));

    if (!sensorQueue || !cmdQueue) {
        Serial.println("❌ FATAL: Không tạo được Queue!");
        ESP.restart();
    }

    espClient.setInsecure();

    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);

    preferences.begin("iot_app", true);
    ssid     = preferences.getString("ssid", "");
    password = preferences.getString("password", "");
    userId   = preferences.getString("userId", "");
    preferences.end();

    if (ssid == "" || userId == "") {
        isSetupMode = true;

        WiFi.softAP("ThietBi_YTe_01");

        localServer.on("/setup", HTTP_POST, handleSetup);
        localServer.begin();

        Serial.println("[Setup] ⚠️ Setup Mode — AP: ThietBi_YTe_01");
    } else {
        isSetupMode = false;
        Serial.println("[Setup] Normal Mode");
    }

    xTaskCreatePinnedToCore(
        SensorTask,
        "SensorTask",
        4096,
        NULL,
        2,
        &sensorTaskHandle,
        1
    );

    xTaskCreatePinnedToCore(
        NetworkTask,
        "NetworkTask",
        20480,
        NULL,
        1,
        NULL,
        0
    );

    Serial.println("[Setup] ✅ 2 task đã khởi động");
}

// Không dùng loop
void loop() {
    vTaskDelete(NULL);
}