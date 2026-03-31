#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Wire.h>
#include "MAX30100_PulseOximeter.h"

#define BOOT_BUTTON_PIN 0 

// ==========================================
// CẤU HÌNH MQTT BROKER
// ==========================================
const char* mqtt_server = "broker.emqx.io";
const int mqtt_port = 1883;
const char* mqtt_topic = "DoAnIoT_HealthData_Upload_2026"; 

WebServer localServer(80);
Preferences preferences;
PulseOximeter pox;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

String ssid = "";
String password = "";
const char* userId = "69ca2edc4158e18d50df80f4";  // Thay bằng userID thực của bạn

uint32_t tsLastReport = 0;
bool isSetupMode = false;
bool isPoxInitialized = false; // ⭐ NEW: Cờ kiểm tra xem MAX30100 đã khởi tạo chưa

uint32_t buttonPressTime = 0;
bool isButtonPressed = false;

// ==========================================
// HÀM KẾT NỐI LẠI MQTT KHI BỊ RỚT MẠNG
// ==========================================
void reconnectMQTT() {
    while (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
        Serial.print("Đang kết nối MQTT Broker...");
        String clientId = "ESP32Client-HealthIoT-";
        clientId += String(random(0xffff), HEX);
        
        if (mqttClient.connect(clientId.c_str())) {
            Serial.println("✅ Đã kết nối MQTT!");
        } else {
            Serial.print("❌ Lỗi, thử lại sau 2 giây...");
            delay(2000);
        }
    }
}

// ==========================================
// HÀM NHẬN CẤU HÌNH TỪ APP QUA AP MODE
// ==========================================
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

        localServer.send(200, "application/json", "{\"message\":\"Cài đặt thành công! Thiết bị đang khởi động lại...\"}");
        
        Serial.println("✅ Đã nhận cấu hình từ App. Đang khởi động lại mạch...");
        delay(2000);
        ESP.restart(); 
    }
}

void onBeatDetected() {
    Serial.println("❤️ Phát hiện nhịp đập...");
}

// ==========================================
// HÀM KHỞI ĐỘNG (SETUP)
// ==========================================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Wire.begin(21, 22);
    pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
    mqttClient.setServer(mqtt_server, mqtt_port);

    preferences.begin("iot_app", true);
    ssid = preferences.getString("ssid", "");
    password = preferences.getString("password", "");
    preferences.end();

    if (ssid == "") {
        isSetupMode = true;
        Serial.println("\n⚠️ Chưa có cấu hình! Bật chế độ Cài đặt qua App.");
        WiFi.softAP("ThietBi_YTe_01"); 
        localServer.on("/setup", HTTP_POST, handleSetup);
        localServer.begin();
    } else {
        isSetupMode = false;
        Serial.println("\n✅ Đang kết nối Wi-Fi: " + ssid);
        WiFi.begin(ssid.c_str(), password.c_str());
        
        int attempts = 0;
        while (WiFi.status() != WL_CONNECTED && attempts < 20) {
            delay(500); Serial.print("."); attempts++;
        }

        if (WiFi.status() == WL_CONNECTED) {
            Serial.println("\n✅ Đã kết nối Wi-Fi thành công!");
            
            // =====================================================
            // ⭐ PHẦN SỬA: KIỂM TRA & KHỞI TẠO MAX30100 CHÍNH XÁC
            // =====================================================
            Serial.println("\n--- BẮT ĐẦU TEST MAX30100 ---");
            Serial.println("Đang quét tín hiệu điện I2C...");
            
            // 1️⃣ Quét I2C xem cảm biến có kết nối không
            Wire.beginTransmission(0x57);
            byte error = Wire.endTransmission();
            
            if (error == 0) {
                Serial.println("✅ Tín hiệu điện TỐT! ESP32 đã nhìn thấy cảm biến.");
                Serial.println("Đang khởi tạo thuật toán đo...");
                
                // 2️⃣ Khởi tạo pox
                if (!pox.begin()) {
                    Serial.println("❌ LỖI: Thuật toán MAX30100 khởi tạo thất bại!");
                    Serial.println("Thiết bị dừng lại.");
                    while(1); // ⭐ DỪNG NGAY KHI LỖI!
                } else {
                    Serial.println("✅ Khởi tạo thành công!");
                    pox.setIRLedCurrent(MAX30100_LED_CURR_24MA);
                    pox.setOnBeatDetectedCallback(onBeatDetected);
                    
                    // 3️⃣ Chờ người đặt ngón tay vào
                    Serial.println("\n⏳ Chờ 3 giây để bạn đặt ngón tay vào cảm biến...");
                    for (int i = 3; i > 0; i--) {
                        Serial.print(i);
                        Serial.println(" giây...");
                        delay(1000);
                    }
                    
                    Serial.println("\n🔴 Đang đo... Hãy giữ yên ngón tay!");
                    isPoxInitialized = true; // ⭐ Đánh dấu đã khởi tạo thành công
                }
            } else {
                Serial.println("❌ LỖI ĐIỆN ÁP: ESP32 bị mù, không thấy cảm biến!");
                Serial.println("👉 HƯỚNG XỬ LÝ: Hãy cắm dây VIN của cảm biến sang chân 5V (hoặc VBUS/VIN) của ESP32.");
                Serial.println("Thiết bị dừng lại.");
                while(1); // ⭐ DỪNG NGAY KHI KHÔNG TÌM THẤY CẢMBIẾN!
            }
            // =====================================================

        } else {
            Serial.println("\n⚠️ Mất mạng! Nếu bạn đã đổi Wi-Fi phòng, hãy NHẤN GIỮ NÚT BOOT 3 GIÂY để reset mạch!");
        }
    }
}

// ==========================================
// VÒNG LẶP CHÍNH (LOOP)
// ==========================================
void loop() {
    if (digitalRead(BOOT_BUTTON_PIN) == LOW) { 
        if (!isButtonPressed) {
            isButtonPressed = true;
            buttonPressTime = millis(); 
        } else if (millis() - buttonPressTime > 3000) { 
            Serial.println("\n⚠️ ĐÃ XÓA TOÀN BỘ CẤU HÌNH! Đang khởi động lại...");
            preferences.begin("iot_app", false);
            preferences.clear();
            preferences.end();
            delay(1000);
            ESP.restart(); 
        }
    } else {
        isButtonPressed = false; 
    }

    if (isSetupMode) {
        localServer.handleClient(); 
    } else {
        if (!mqttClient.connected() && WiFi.status() == WL_CONNECTED) {
            reconnectMQTT();
        }
        mqttClient.loop(); 
        
        // ⭐ CHỈ CẬP NHẬT POX NẾU ĐÃ KHỞI TẠO THÀNH CÔNG
        if (isPoxInitialized) {
            pox.update(); 

            // =====================================================
            // IN KẾT QUẢ VÀ GỬI MQTT MỖI 1 GIÂY
            // =====================================================
            if (millis() - tsLastReport > 1000) {
                float bpm = pox.getHeartRate();
                int spo2 = pox.getSpO2();

                // Luôn in ra Serial Monitor
                Serial.print("Nhịp tim: ");
                Serial.print(bpm);
                Serial.print(" bpm  |  SpO2: ");
                Serial.print(spo2);
                Serial.println(" %");

                // Chỉ gửi nếu thông số hợp lệ
                if (WiFi.status() == WL_CONNECTED && mqttClient.connected()) {
                    if (bpm >= 0 && spo2 >= 0) {
                        StaticJsonDocument<200> doc;
                        doc["userId"] = userId;
                        doc["bpm"] = round(bpm); 
                        doc["spo2"] = spo2;
                        doc["isDrowsy"] = false; 

                        char jsonBuffer[512];
                        serializeJson(doc, jsonBuffer);

                        if (mqttClient.publish(mqtt_topic, jsonBuffer)) {
                            Serial.println("  ☁️ [MQTT] Đã gửi lên Server!");
                        }
                    }
                } else if (WiFi.status() != WL_CONNECTED) {
                    WiFi.reconnect(); 
                }
                
                tsLastReport = millis();
            }
        } else {
            // Nếu chưa khởi tạo xong, chỉ duy trì WiFi
            Serial.println("⚠️ MAX30100 chưa sẵn sàng. Hãy kiểm tra kết nối cảm biến.");
            delay(2000);
        }
    }
}