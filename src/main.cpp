#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "MAX30100_PulseOximeter.h"

#define BOOT_BUTTON_PIN 0 

// ==========================================
// 🌟 CẤU HÌNH HIVEMQ CLOUD BẢO MẬT
// ==========================================
const char* mqtt_server = "a610b199cda842b3a6207037ad778131.s1.eu.hivemq.cloud"; 
const int mqtt_port = 8883;
const char* mqtt_user = "admin";
const char* mqtt_pass = "01233256549Na";
const char* mqtt_topic = "HealthData_Upload_2026"; 
const char* mqtt_cmd_topic = "HealthData_Command_2026"; 

WebServer localServer(80);
Preferences preferences;
PulseOximeter pox;

WiFiClientSecure espClient; 
PubSubClient mqttClient(espClient);

String ssid = "";
String password = "";
String userId = "";

uint32_t tsLastReport = 0;
bool isSetupMode = false;
bool isDeviceActive = true;  // 💤 Chế độ ngủ đông
uint32_t buttonPressTime = 0;
bool isButtonPressed = false;

float sharedBpm = 0;
int sharedSpo2 = 0;

// ==========================================
// ✅ PRE-FILTER: MOVING AVERAGE (LỌC TRƯỚC KALMAN)
// ==========================================
struct MovingAverage {
    static const int SIZE = 5;
    float buffer[SIZE] = {0};
    int index = 0;
    
    float apply(float value) {
        buffer[index] = value;
        index = (index + 1) % SIZE;
        
        float sum = 0;
        for (int i = 0; i < SIZE; i++) {
            sum += buffer[i];
        }
        return sum / SIZE;
    }
    
    void reset() {
        index = 0;
        for (int i = 0; i < SIZE; i++) {
            buffer[i] = 0;
        }
    }
};

MovingAverage mavgBpm;
MovingAverage mavgSpo2;

// ==========================================
// ✅ STRUCT KALMAN FILTER (CẢI THIỆN)
// ==========================================
struct KalmanFilterBPM {
    float q = 0.003;      // ✅ Giảm từ 0.01 (tin process model hơn)
    float r = 0.8;        // ✅ Tăng từ 0.1 (tin sensor ít hơn)
    float x = 0.0;        // State
    float p = 1.0;        // Error
    
    void setParams(float process, float measurement) {
        q = constrain(process, 0.0001, 0.05);
        r = constrain(measurement, 0.1, 3.0);
    }
    
    float apply(float measurement) {
        // ✅ Validation input
        if (isnan(measurement) || measurement < 40 || measurement > 200) {
            return x;
        }
        
        // ✅ Initialize on first valid reading
        if (x == 0.0) {
            x = measurement;
            return x;
        }
        
        // Kalman update
        p = p + q;
        float k = p / (p + r);
        x = x + k * (measurement - x);
        p = (1 - k) * p;
        
        return constrain(x, 40.0, 200.0);
    }
    
    void reset() {
        x = 0.0;
        p = 1.0;
    }
};

struct KalmanFilterSpO2 {
    float q = 0.001;      // ✅ Giảm từ 0.005
    float r = 1.0;        // ✅ Tăng từ 0.15 (DỰA MẠN hơn)
    float x = 95.0;
    float p = 1.0;
    
    void setParams(float process, float measurement) {
        q = constrain(process, 0.0001, 0.01);
        r = constrain(measurement, 0.1, 2.0);
    }
    
    float apply(float measurement) {
        // ✅ Validation input
        if (isnan(measurement) || measurement < 70 || measurement > 100) {
            return x;
        }
        
        // Kalman update
        p = p + q;
        float k = p / (p + r);
        x = x + k * (measurement - x);
        p = (1 - k) * p;
        
        return constrain(x, 70.0, 100.0);
    }
    
    void reset() {
        x = 95.0;
        p = 1.0;
    }
};

KalmanFilterBPM kalmanBPM;
KalmanFilterSpO2 kalmanSPO2;

// ==========================================
// HÀM NHẬN LỆNH TỪ XA
// ==========================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    Serial.print("📥 [Lệnh từ xa] Nhận được: ");
    Serial.println(message);

    if (message == "RESTART") {
        Serial.println("🔄 HỆ THỐNG ĐANG KHỞI ĐỘNG LẠI THEO LỆNH TỪ XA...");
        delay(1000);
        ESP.restart(); 
    } 
    else if (message == "SLEEP") {
        isDeviceActive = false;
        pox.shutdown(); // Tắt đèn LED của cảm biến MAX30100 cho đỡ tốn điện
        Serial.println("💤 THIẾT BỊ ĐÃ VÀO CHẾ ĐỘ NGỦ ĐÔNG...");
    } 
    else if (message == "WAKEUP") {
        isDeviceActive = true;
        pox.resume(); // Bật lại đèn LED cảm biến
        Serial.println("☀️ THIẾT BỊ ĐÃ THỨC DẬY, SẴN SÀNG ĐO...");
    }
}

// ==========================================
// HÀM KẾT NỐI LẠI MQTT
// ==========================================
void reconnectMQTT() {
    while (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        Serial.print("🔐 Đang bắt tay bảo mật SSL...");
        String clientId = "ESP32Client-HealthIoT-";
        clientId += String(random(0xffff), HEX);
        
        if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
            Serial.println("✅ Đã kết nối HiveMQ!");
            mqttClient.subscribe(mqtt_cmd_topic);
        } else {
            Serial.print("❌ Lỗi (Mã: ");
            Serial.print(mqttClient.state());
            Serial.println("), thử lại sau 3s...");
            delay(3000);
        }
    }
}

void handleSetup() {
    if (localServer.hasArg("plain")) {
        String body = localServer.arg("plain");
        DynamicJsonDocument doc(512);
        deserializeJson(doc, body);

        preferences.begin("iot_app", false);
        preferences.putString("ssid", doc["ssid"].as<String>());
        preferences.putString("password", doc["password"].as<String>());
        preferences.putString("userId", doc["userId"].as<String>());
        preferences.end();

        localServer.send(200, "application/json", "{\"message\":\"Cài đặt thành công!\"}");
        delay(2000);
        ESP.restart(); 
    }
}

void onBeatDetected() {}

// ==============================================================================
// TASK MẠNG CHẠY ĐỘC LẬP TRÊN CORE 0
// ==============================================================================
void NetworkTask(void *pvParameters) {
    for (;;) { 
        if (isSetupMode) {
            localServer.handleClient(); 
        } else {
            if (WiFi.status() == WL_CONNECTED) {
                if (!mqttClient.connected()) {
                    reconnectMQTT();
                }
                mqttClient.loop(); 

                if (millis() - tsLastReport > 1000) {
                    Serial.print("⏳ BPM: ");
                    Serial.print((int)sharedBpm);
                    Serial.print(" bpm | SpO2: ");
                    Serial.print(sharedSpo2);
                    Serial.println(" %");

                    if (mqttClient.connected()) {
                        // ✅ CHỈ GỬI NẾU DỮ LIỆU HỢP LỆ
                        if (sharedBpm >= 40 && sharedBpm <= 200 && sharedSpo2 >= 70 && sharedSpo2 <= 100) {
                            StaticJsonDocument<200> doc;
                            doc["userId"] = userId;
                            doc["bpm"] = round(sharedBpm); 
                            doc["spo2"] = sharedSpo2;
                            doc["isDrowsy"] = false; 

                            char jsonBuffer[512];
                            serializeJson(doc, jsonBuffer);

                            if (mqttClient.publish(mqtt_topic, jsonBuffer)) {
                                Serial.println("   ☁️ Đã gửi lên Server!");
                            }
                        }
                    }
                    tsLastReport = millis();
                }
            } else {
                WiFi.reconnect();
                vTaskDelay(2000 / portTICK_PERIOD_MS); 
            }
        }
        vTaskDelay(10 / portTICK_PERIOD_MS); 
    }
}

// ==========================================
// HÀM KHỞI ĐỘNG (SETUP)
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Wire.begin(21, 22);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    
    espClient.setInsecure(); 
    
    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);

    preferences.begin("iot_app", true);
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("password", "");
    userId = preferences.getString("userId", "");
    preferences.end();

    if (ssid == "" || userId == "") {
        isSetupMode = true;
        Serial.println("\n⚠️ Chưa có cấu hình! Bật chế độ Setup");
        WiFi.softAP("ThietBi_YTe_01"); 
        localServer.on("/setup", HTTP_POST, handleSetup);
        localServer.begin();
    } else {
        isSetupMode = false;
        Serial.println("\n✅ Kết nối Wi-Fi: " + ssid);
        WiFi.begin(ssid.c_str(), password.c_str());
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500); Serial.print("."); attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n✅ Wi-Fi OK!");
            
            Wire.beginTransmission(0x57);
            byte error = Wire.endTransmission();
            
            if (error == 0) {
                if (pox.begin()) {
                    pox.setIRLedCurrent(MAX30100_LED_CURR_24MA); 
                    pox.setOnBeatDetectedCallback(onBeatDetected);
                    Serial.println("✅ MAX30100 OK!");
                }
            } else {
                Serial.println("❌ Không tìm thấy sensor!");
                while(1);
            }
        }
    }

    xTaskCreatePinnedToCore(NetworkTask, "NetworkTask", 10000, NULL, 1, NULL, 0);
}

// ==============================================================================
// VÒNG LẶP CHÍNH CHẠY TRÊN CORE 1 (Lấy mẫu & Lọc)
// ==============================================================================
uint32_t fingerStartTime = 0;
uint32_t fingerMissingTime = 0;
bool isFingerStable = false;

// ✅ DEBUG TIMER
static uint32_t rawDebugTimer = 0;
static uint32_t filterDebugTimer = 0;

void loop() {
    // 1. Quản lý nút Reset
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) { 
        if (!isButtonPressed) {
            isButtonPressed = true;
            buttonPressTime = millis(); 
        } else if (millis() - buttonPressTime > 3000) { 
            Serial.println("\n⚠️ ĐÃ XÓA TOÀN BỘ CẤU HÌNH!");
            preferences.begin("iot_app", false);
            preferences.clear();
            preferences.end();
            delay(1000);
            ESP.restart(); 
        }
    } else {
        isButtonPressed = false; 
    }

    // 2. Lấy mẫu từ sensor
    if (!isSetupMode && isDeviceActive) {
        pox.update(); 
        
        float rawBpm = pox.getHeartRate();
        int rawSpo2 = pox.getSpO2();

        // ✅ VALIDATION: Kiểm tra NaN/Inf
        if (isnan(rawBpm) || isinf(rawBpm) || isnan(rawSpo2) || isinf(rawSpo2)) {
            return;
        }

        // ✅ BOUNDS CHECK: Giới hạn giá trị
        rawBpm = constrain(rawBpm, 40, 200);
        rawSpo2 = constrain(rawSpo2, 0, 100);

        // ✅ DEBUG: In giá trị thô (mỗi 2s)
        if (millis() - rawDebugTimer > 2000) {
            Serial.printf("🔴 RAW: BPM=%d, SpO2=%d\n",
                (int)rawBpm, rawSpo2);
            rawDebugTimer = millis();
        }

        // ==========================================
        // 🌟 FINGER DETECTION (Cải thiện ngưỡng)
        // ==========================================
        // ✅ ĐỔI: SpO2 > 20 → SpO2 > 70 (ngưỡng hợp lý)
        if (rawSpo2 > 70) {
            fingerMissingTime = 0;
            
            if (fingerStartTime == 0) {
                fingerStartTime = millis();
                Serial.println("👆 Phát hiện ngón tay, chờ 3s...");
            }
            
            if (millis() - fingerStartTime > 3000) {
                isFingerStable = true;
            }
            
        } else {
            if (fingerMissingTime == 0) {
                fingerMissingTime = millis();
            }
            
            if (millis() - fingerMissingTime > 2000) {
                if (fingerStartTime != 0) {
                    Serial.println("👋 Rút tay - Reset bộ lọc");
                }
                
                fingerStartTime = 0;
                isFingerStable = false;
                sharedBpm = 0;
                sharedSpo2 = 0;
                
                // ✅ RESET ĐÚNG CÁCH (dùng reset function)
                kalmanBPM.reset();
                kalmanSPO2.reset();
                mavgBpm.reset();
                mavgSpo2.reset();
            }
        }

        // ==========================================
        // 🌟 LỌC DỮ LIỆU (Chỉ khi ổn định)
        // ==========================================
        if (isFingerStable) {
            // ✅ STEP 1: Moving Average (pre-filter)
            float filteredBpm = mavgBpm.apply(rawBpm);
            float filteredSpo2 = mavgSpo2.apply((float)rawSpo2);
            
            // ✅ STEP 2: Kalman Filter
            float tempBpm = kalmanBPM.apply(filteredBpm);
            int tempSpo2 = (int)kalmanSPO2.apply(filteredSpo2);
            
            // ✅ STEP 3: Final bounds check
            tempBpm = constrain(tempBpm, 40, 200);
            tempSpo2 = constrain(tempSpo2, 70, 100);
            
            sharedBpm = tempBpm;
            sharedSpo2 = tempSpo2;
            
            // ✅ DEBUG: In dữ liệu lọc (mỗi 2s)
            if (millis() - filterDebugTimer > 2000) {
                Serial.printf("✅ FILTERED: BPM=%.1f | SpO2=%d | MAVGbpm=%.1f, MAVGspo2=%.1f\n",
                    tempBpm, tempSpo2, filteredBpm, filteredSpo2);
                filterDebugTimer = millis();
            }
        }
    }
}