#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Arduino.h>
#include <Preferences.h>
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/rsa.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/base64.h"

// Sensors
const int PIN_BUZZER    = 14;  // D14
const int PIN_PIR       = 13;  // D13
const int PIN_LED_GREEN = 26;  // D26
const int PIN_LED_RED   = 25;  // D25

// Wi-Fi Credentials
const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

// MQTT Broker
const char* mqtt_server = "broker.mqtt-dashboard.com";
const char* rsa_public_key_topic     = "IPB/IoT/AlarmProject/Crypto/PublicKey/";
const char* personal_notifications_sub = "IPB/IoT/AlarmProject/Notifications/Personal/";

WiFiClient espClient;
PubSubClient client(espClient);
unsigned long lastKeyPublish = 0;

// RSA Key
char global_public_pem[1024];
mbedtls_pk_context global_pk;

// Persistent state (NVS)
Preferences preferences;
String currentPin = "123456";

// Alarm state
bool armed = false;                     // starts off
bool alarming = false;                  // latched: stays on until disarmed by PIN
bool buzzActive = false;                // current buzzer/LED phase (non-blocking blink)
unsigned long lastBuzzToggle = 0;
const unsigned long BUZZ_INTERVAL_MS = 200;

// RSA helpers 

void generateAndStoreRSAKeys() {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_pk_init(&global_pk);

    const char *pers = "esp32_alarm_rsa";
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                          (const unsigned char *)pers, strlen(pers));

    mbedtls_pk_setup(&global_pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(global_pk);

    // node-rsa (sense-rsa) cifra com OAEP-SHA1 por padrao; o ESP32 precisa
    // decifrar com o mesmo esquema, senao da MBEDTLS_ERR_RSA_INVALID_PADDING (-0x4100).
    mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);

    Serial.println("Generating RSA keypair...");
    if (mbedtls_rsa_gen_key(rsa, mbedtls_ctr_drbg_random, &ctr_drbg, 2048, 65537) == 0) {
        memset(global_public_pem, 0, sizeof(global_public_pem));
        mbedtls_pk_write_pubkey_pem(&global_pk,
                                    (unsigned char *)global_public_pem,
                                    sizeof(global_public_pem));
        Serial.println("RSA keypair ready.");
    } else {
        Serial.println("RSA generation failed.");
    }

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
}

String rsa_decrypt(const String& encryptedBase64) {
    unsigned char encrypted[256];
    size_t decoded_len = 0;

    int rc = mbedtls_base64_decode(encrypted, sizeof(encrypted), &decoded_len,
                                   (const unsigned char*)encryptedBase64.c_str(),
                                   encryptedBase64.length());
    if (rc != 0) {
        Serial.printf("Base64 decode failed: -0x%X\n", -rc);
        return "";
    }

    unsigned char decrypted[256];
    size_t decrypted_len = 0;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                          (const unsigned char*)"rsa_decrypt", 11);

    int ret = mbedtls_pk_decrypt(
        &global_pk,
        encrypted, decoded_len,
        decrypted, &decrypted_len, sizeof(decrypted),
        mbedtls_ctr_drbg_random, &ctr_drbg
    );

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    if (ret != 0) {
        Serial.printf("RSA decrypt failed: -0x%X\n", -ret);
        return "";
    }

    decrypted[decrypted_len] = '\0';
    return String((char*)decrypted);
}

// WiFi / MQTT

void setup_wifi() {
    delay(10);
    Serial.println();
    Serial.print("Connecting to ");
    Serial.println(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
        Serial.print(".");
    }
    randomSeed(micros());

    Serial.println();
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
}

void reconnect() {
    while (!client.connected()) {
        Serial.print("Attempting MQTT connection... ");
        String clientId = "IPB-IoT-Alarm-" + String((uint32_t)ESP.getEfuseMac(), HEX);
        if (client.connect(clientId.c_str())) {
            Serial.println("connected.");
            client.subscribe(personal_notifications_sub, 1);
            Serial.printf("Subscribed to %s\n", personal_notifications_sub);
        } else {
            Serial.printf("failed rc=%d, retry in 5s\n", client.state());
            delay(5000);
        }
    }
}

// Application logic

void publishResponse(const char* topic, JsonDocument& resp) {
    char buf[256];
    size_t n = serializeJson(resp, buf, sizeof(buf));
    client.publish(topic, (uint8_t*)buf, n, false);
    Serial.printf("Response -> %s : %.*s\n", topic, (int)n, buf);
}

void persistPin(const String& pin) {
    preferences.begin("alarm", false);
    preferences.putString("pin", pin);
    preferences.end();
}

String responseTopicFor(const String& uuid) {
    return String("IPB/IoT/AlarmProject/Notifications/Personal/") + uuid;
}

void handleLogin(const String& pin, const String& uuid) {
    JsonDocument resp;
    resp["action"] = "login_response";
    resp["uuid"]   = uuid;

    if (pin == currentPin) {
        armed = !armed;
        if (!armed) {
            // Disarming silences any ongoing alarm right away.
            alarming = false;
            buzzActive = false;
            digitalWrite(PIN_BUZZER,  LOW);
            digitalWrite(PIN_LED_RED, LOW);
        }
        resp["success"] = true;
        resp["armed"]   = armed;
        Serial.printf("Login OK. armed=%s\n", armed ? "true" : "false");
    } else {
        resp["success"] = false;
        resp["message"] = "Invalid PIN";
        Serial.println("Login FAILED.");
    }
    publishResponse(responseTopicFor(uuid).c_str(), resp);
}

void handleUpdatePassword(const String& oldPwd, const String& newPwd,
                          const String& uuid) {
    JsonDocument resp;
    resp["action"] = "update_password_response";
    resp["uuid"]   = uuid;

    if (oldPwd != currentPin) {
        resp["success"] = false;
        resp["message"] = "Senha antiga incorreta";
        publishResponse(responseTopicFor(uuid).c_str(), resp);
        Serial.println("Password update REJECTED (old pin mismatch).");
        return;
    }

    if (newPwd.length() == 0 || newPwd.length() > 6) {
        resp["success"] = false;
        resp["message"] = "Nova senha invalida";
        publishResponse(responseTopicFor(uuid).c_str(), resp);
        return;
    }

    currentPin = newPwd;
    persistPin(currentPin);

    resp["success"] = true;
    publishResponse(responseTopicFor(uuid).c_str(), resp);
    Serial.println("Password updated and saved to NVS.");
}

void callback(char* topic, byte* payload, unsigned int length) {
    String message;
    message.reserve(length);
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

    Serial.printf("[%s] %s\n", topic, message.c_str());

    String decrypted = rsa_decrypt(String((char*)payload, length));
    if (decrypted.length() == 0) {
        Serial.println("Decrypt failed.");
        return;
    }
    Serial.print("Decrypted: ");
    Serial.println(decrypted);

    JsonDocument doc;
    if (deserializeJson(doc, decrypted)) {
        Serial.println("Outer JSON parse failed.");
        return;
    }

    const char* action = doc["action"];
    String uuid = doc["uuid"].as<String>();
    if (!action) {
        Serial.println("Missing action.");
        return;
    }

    if (strcmp(action, "login") == 0) {
        handleLogin(doc["pin"].as<String>(), uuid);
    } else if (strcmp(action, "update_password") == 0) {
        handleUpdatePassword(doc["old_password"].as<String>(),
                             doc["new_password"].as<String>(),
                             uuid);
    } else {
        Serial.printf("Unknown action: %s\n", action);
    }
}

// Alarm

// Non-blocking buzzer/LED blink. Called every loop() while alarming so MQTT
// (and therefore the disarm login) keeps being processed.
void updateAlarm() {
    if (millis() - lastBuzzToggle >= BUZZ_INTERVAL_MS) {
        lastBuzzToggle = millis();
        buzzActive = !buzzActive;
        digitalWrite(PIN_BUZZER,  buzzActive ? HIGH : LOW);
        digitalWrite(PIN_LED_RED, buzzActive ? HIGH : LOW);
    }
}

// Arduino entrypoints

void setup() {
    Serial.begin(115200);

    pinMode(PIN_PIR,       INPUT);
    pinMode(PIN_BUZZER,    OUTPUT);
    pinMode(PIN_LED_GREEN, OUTPUT);
    pinMode(PIN_LED_RED,   OUTPUT);
    digitalWrite(PIN_BUZZER,    LOW);
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED,   LOW);

    digitalWrite(PIN_LED_GREEN, HIGH);   // booting

    preferences.begin("alarm", true);
    currentPin = preferences.getString("pin", "123456");
    preferences.end();
    Serial.printf("PIN loaded from NVS: %s\n", currentPin.c_str());

    generateAndStoreRSAKeys();
    setup_wifi();

    client.setServer(mqtt_server, 1883);
    client.setCallback(callback);
    client.setBufferSize(2048);

    digitalWrite(PIN_LED_GREEN, LOW);
    Serial.println("Ready. System ARMED.");
}

void loop() {
    if (!client.connected()) reconnect();
    client.loop();

    // Motion while armed latches the alarm ON; it stays on until a valid PIN
    // disarms the system (handled in handleLogin).
    if (armed && digitalRead(PIN_PIR) == HIGH) {
        alarming = true;
    }

    if (alarming) {
        digitalWrite(PIN_LED_GREEN, LOW);
        updateAlarm();                  // keep buzzing (non-blocking)
    } else if (armed) {
        digitalWrite(PIN_LED_GREEN, LOW);
        digitalWrite(PIN_LED_RED,   HIGH);
    } else {
        digitalWrite(PIN_LED_GREEN, HIGH);
        digitalWrite(PIN_LED_RED,   LOW);
    }

    unsigned long now = millis();
    if (now - lastKeyPublish > 5000) {
        lastKeyPublish = now;
        client.publish(rsa_public_key_topic, global_public_pem, 1);
    }
}
