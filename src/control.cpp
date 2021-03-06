/*
 * (C) Copyright 2020 Tillmann Heidsieck
 *
 * SPDX-License-Identifier: MIT
 *
 */
#include <Arduino.h>

#include <ArduinoJson.h>
#include <CertStoreBearSSL.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266WiFi.h>
#include <FS.h>
#include <LittleFS.h>
#include <time.h>

#include "control.h"
#include "rtcmem_map.h"
#include "updater.h"
#include "version.h"

#include "influxca.h"

#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"


void FirmwareControl::set_clock() {
    char buffer[64];

    configTzTime(TZ_INFO, "pool.ntp.org", "time.nist.gov");

    Serial.print(F("Waiting for NTP time sync: "));
    time_t now = time(nullptr);
    while (now < 8 * 3600 * 2) {
        yield();
        delay(500);
        Serial.print(F("."));
        now = time(nullptr);
    }
    Serial.println();

    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    Serial.print(F("Current time: "));
    strftime(buffer, 64, "%c", &timeinfo);
    Serial.println(buffer);
}

void FirmwareControl::update_config(const char *name) {
    Serial.print(F("Update configuration: "));
    Serial.println(name);

    DynamicJsonDocument doc(1024);

    String url = ctrl_url + "/" + name;
    if (!https->begin(*wifi_client, url))
        return;

    https->setUserAgent(F("ESP8266-OTA"));
    https->addHeader(F("X-chip-id"), chip_id);

    String filename;
    if (!strncasecmp(name, "global_config", 13)) {
        https->addHeader(F("X-global-config-version"), String(global_config_version));
        https->addHeader(F("X-global-config-key"), String(global_config_key));
        filename = String("/") + name + ".json";
    } else if (!strncasecmp(name, "local_config", 12)) {
        https->addHeader(F("X-config-version"), String(config_version));
        filename = F("/config.json");
    } else {
        https->end();
        return;
    }

    int http_code = https->GET();
    if (http_code < 0) {
        https->end();
        return;
    }

    /* Note: we do not use streaming API here! Because we try to be a little more
     * safe here
     * - First download new config
     * - Second test if config is JSON format
     * - Third write to file
     */
    String payload;
    if (http_code == HTTP_CODE_OK) {
        payload = https->getString();
        DeserializationError error = deserializeJson(doc, payload.c_str());
        if (error != DeserializationError::Ok) {
            Serial.printf("Could not load config file");
            https->end();
            return;
        }
    } else {
        Serial.print(https->getString());
        Serial.println();
        https->end();
        return;
    }
    https->end();

    File file = LittleFS.open(filename, "w");
    if (!file)
        return;

    file.write(payload.c_str(), payload.length());

    file.close();
}

void FirmwareControl::OTA() {
    if (ctrl_url.length() < 11) {
        Serial.println(F("Invalid CTRL_URL"));
        return;
    }

    BearSSL::WiFiClientSecure *wcs = new BearSSL::WiFiClientSecure;
    if (!wcs) {
        Serial.println(F("OOM: could not allocate WiFiClientSecure"));
        return;
    }

    wcs->setCertStore(&cert_store);

    String server = ctrl_url;
    server.remove(0, server.indexOf(":") + 3);
    bool mfln = wcs->probeMaxFragmentLength(server, 443, 1024);
    if (mfln) {
        Serial.println(F("MFLN supported"));
        wcs->setBufferSizes(1024, 1024);
    }

    wifi_client = wcs;

    https = new HTTPClient;
    if (!https) {
        Serial.println(F("OOM: could not allocate HTTPClient"));
        return;
    }

    https->setReuse(true);
    https->setTimeout(20000);

    ctrl_url += "/api/v1";

    Serial.print(F("control server: "));
    Serial.println(ctrl_url);

    update_config("global_config");
    update_config("local_config");

    Updater upd;
    upd.update(*https, ctrl_url + "/firmware", VERSION);
}

void FirmwareControl::go_online() {
    Serial.println(("going online ..."));
    WiFi.forceSleepBegin();
    delay(1);
    WiFi.forceSleepWake();
    delay(1);
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifi_ssid, wifi_pass);

    for (int i = 0; i < 1000; i++) {
        /* work around problems of WiFi status not always showing WL_CONNECTED */
        if ((WiFi.status() == WL_CONNECTED) || WiFi.localIP().isSet()) {
            this->online = true;
            break;
        }
        delay(100);
    }

    if (online) {
        set_clock();
    } else {
        Serial.println(F("Failed to go online"));
        return;
    }

    if (!ota_request) {
        uint8_t num_sensors;

        num_sensors = sensor_manager->get_num_sensors();
        influx = new InfluxDBClient(influx_url, influx_org, influx_bucket,
                                    influx_token, influxCA);
        HTTPOptions opt;
        opt.connectionReuse(true);
        opt.httpReadTimeout(10000);
        influx->setHTTPOptions(opt);
	// NOTE: check batchSize with respect to num_sensors if there are memory
	// consumption errors upcomming
        influx->setWriteOptions(WriteOptions().writePrecision(WritePrecision::S).batchSize(num_sensors).bufferSize(2*num_sensors));

        if (influx->validateConnection()) {
            Serial.print("Connected to InfluxDB: ");
            Serial.println(influx->getServerUrl());
        } else {
            Serial.print("InfluxDB connection failed: ");
            Serial.println(influx->getLastErrorMessage());
        }
    }
}

void FirmwareControl::deep_sleep() {
    Serial.print(F(" -> deep sleep for "));
    Serial.println(sleep_time_s);

    if (online)
        WiFi.mode(WIFI_SHUTDOWN, wss);

    ESP.deepSleepInstant(sleep_time_s * 1E6, WAKE_RF_DISABLED);
}

void FirmwareControl::read_global_config() {
    File file = LittleFS.open("/global_config.json", "r");
    if (!file)
        return;

    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, file);
    if (error != DeserializationError::Ok) {
        Serial.println(F("Could not global load config file"));
        return;
    }
    file.close();

    global_config_key = doc["global_config_key"] | "ABCDEF";
    global_config_version = doc["global_config_version"] | 0;

    wifi_ssid = doc["wifi_ssid"] | "NO SSID";
    wifi_pass = doc["wifi_pass"] | "NO PSK";
    ctrl_url  = doc["ctrl_url"]  | "https://example.com";
    int idx = ctrl_url.indexOf("/", 8);
    if (idx)
        ctrl_url.remove(idx);

    influx_url = strdup(doc["influx_url"] | "https://example.com");
    influx_token = strdup(doc["influx_token"] | "ABCDEFG");
    influx_bucket = strdup(doc["influx_bucket"] | "sensor_bucket");
    influx_org = strdup(doc["influx_org"] | "influx org");
}

void FirmwareControl::read_config() {
    File file = LittleFS.open("/config.json", "r");
    if (file) {
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, file);
        if (error != DeserializationError::Ok) {
            Serial.printf("Could not load config file");
            return;
        }
        file.close();

        sleep_time_s = doc["sleep_time_s"] | 60;
        device_name = doc["device_name"] | chip_id;
        config_version = doc["config_version"] | 0;

        JsonArray ja = doc["sensors"].as<JsonArray>();
        sensor_manager = new SensorManager(ja);
    } else {
        ota_request = true;
    }
}

void FirmwareControl::setup() {
    snprintf(chip_id, 11, "0x%08x", ESP.getChipId());

    Serial.printf("ESP8266 Firmware Version %s (%s)\n", VERSION, BUILD_DATE);
    Serial.printf("  Chip ID: %s\n", chip_id);
    Serial.printf("  CPU Freq: %hhu\n", ESP.getCpuFreqMHz());
    Serial.print(F("  Reset Reason: "));
    Serial.println(ESP.getResetReason());
    LittleFS.begin();

    wss = (WiFiState *)RTCMEM_WSS;

    int numCerts = cert_store.initCertStore(LittleFS, PSTR("/certs.idx"),
                                            PSTR("/certs.ar"));
    Serial.print(F("Number of CA certs read: "));
    Serial.println(numCerts);
    if (numCerts == 0)
        Serial.printf("No certs found\n");

    read_global_config();
    read_config();

    if (ESP.getResetReason() == F("Power On"))
        ota_request = true;

    if (ESP.getResetReason() == F("External System"))
        ota_request = true;
}

void FirmwareControl::loop() {
    if (!online && (go_online_request || ota_request)) {
        go_online();
    }

    if (online && ota_request) {
        OTA();
        ESP.reset();
    }

    if (ota_request)
        return;

    if (!sensor_manager->sensors_done())
        sensor_manager->loop();

    if (sensor_manager->sensors_done())
        go_online_request = sensor_manager->upload_requested();

    if (sensor_manager->sensors_done()) {
        if (online) {
            sensor_manager->publish(influx, &device_name, chip_id, VERSION);
            if (!influx->flushBuffer()) {
                Serial.print("InfluxDB flush failed: ");
                Serial.println(influx->getLastErrorMessage());
                Serial.print("Full buffer: ");
                Serial.println(influx->isBufferFull() ? "Yes" : "No");
            }
            deep_sleep();
        } else if (!sensor_manager->upload_requested()) {
            deep_sleep();
        }
    }
}

FirmwareControl::FirmwareControl() :
    influx_url(nullptr), influx_org(nullptr), influx_bucket(nullptr), influx_token(nullptr),
    sleep_time_s(600), go_online_request(false), online(false), wss(nullptr),
    sensor_manager(nullptr), influx(nullptr) {
}
