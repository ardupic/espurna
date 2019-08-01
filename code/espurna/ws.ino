/*

WEBSOCKET MODULE

Copyright (C) 2016-2019 by Xose Pérez <xose dot perez at gmail dot com>

*/

#if WEB_SUPPORT

#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Ticker.h>
#include <vector>
#include "libs/WebSocketIncommingBuffer.h"

AsyncWebSocket _ws("/ws");
Ticker _web_defer;

std::vector<ws_on_send_callback_f> _ws_on_send_callbacks;
std::vector<ws_on_action_callback_f> _ws_on_action_callbacks;
std::vector<ws_key_check_callback_f> _ws_key_check_callbacks;

// -----------------------------------------------------------------------------
// Private methods
// -----------------------------------------------------------------------------

typedef struct {
    IPAddress ip;
    unsigned long timestamp = 0;
} ws_ticket_t;
ws_ticket_t _ticket[WS_BUFFER_SIZE];

void _onAuth(AsyncWebServerRequest *request) {

    webLog(request);
    if (!webAuthenticate(request)) return request->requestAuthentication();

    IPAddress ip = request->client()->remoteIP();
    unsigned long now = millis();
    unsigned short index;
    for (index = 0; index < WS_BUFFER_SIZE; index++) {
        if (_ticket[index].ip == ip) break;
        if (_ticket[index].timestamp == 0) break;
        if (now - _ticket[index].timestamp > WS_TIMEOUT) break;
    }
    if (index == WS_BUFFER_SIZE) {
        request->send(429);
    } else {
        _ticket[index].ip = ip;
        _ticket[index].timestamp = now;
        request->send(200, "text/plain", "OK");
    }

}

bool _wsAuth(AsyncWebSocketClient * client) {

    IPAddress ip = client->remoteIP();
    unsigned long now = millis();
    unsigned short index = 0;

    for (index = 0; index < WS_BUFFER_SIZE; index++) {
        if ((_ticket[index].ip == ip) && (now - _ticket[index].timestamp < WS_TIMEOUT)) break;
    }

    if (index == WS_BUFFER_SIZE) {
        return false;
    }

    return true;

}

#if DEBUG_WEB_SUPPORT

bool wsDebugSend(const char* prefix, const char* message) {

    if (!wsConnected()) return false;

    const size_t len = strlen(message) + strlen(prefix)
        + strlen("{\"weblog\":}")
        + strlen("{\"message\":\"\"}")
        + (strlen(prefix) ? strlen("\",\"prefix\":\"\"") : 0);

    // via: https://arduinojson.org/v6/assistant/
    // we use 1 object for "weblog", 2nd one for "message". "prefix", optional
    StaticJsonDocument<JSON_OBJECT_SIZE(3)> doc;
    JsonObject weblog = doc.createNestedObject("weblog");

    weblog["message"] = message;
    if (prefix && (prefix[0] != '\0')) {
        weblog["prefix"] = prefix;
    }


    wsSend(doc, len);

    return true;
}
#endif

// -----------------------------------------------------------------------------

#if MQTT_SUPPORT
void _wsMQTTCallback(unsigned int type, const char * topic, const char * payload) {
    if (type == MQTT_CONNECT_EVENT) wsSend_P(PSTR("{\"mqttStatus\": true}"));
    if (type == MQTT_DISCONNECT_EVENT) wsSend_P(PSTR("{\"mqttStatus\": false}"));
}
#endif

bool _wsStore(const String& key, const String& value) {

    // HTTP port
    if (key == "webPort") {
        if ((value.toInt() == 0) || (value.toInt() == 80)) {
            return delSetting(key);
        }
    }

    if (value != getSetting(key)) {
        return setSetting(key, value);
    }

    return false;

}

bool _wsStore(const String& key, const JsonArray& values) {

    bool changed = false;

    unsigned char index = 0;
    for (auto value : values) {
        if (_wsStore(key + index, value.as<String>())) changed = true;
        index++;
    }

    // Delete further values
    for (unsigned char i=index; i<SETTINGS_MAX_LIST_COUNT; i++) {
        if (!delSetting(key, index)) break;
        changed = true;
    }

    return changed;

}

void _wsParse(AsyncWebSocketClient *client, uint8_t * payload, size_t length) {

    //DEBUG_MSG_P(PSTR("[WEBSOCKET] Parsing: %s\n"), length ? (char*) payload : "");

    // Get client ID
    uint32_t client_id = client->id();

    // We don't expect too much of a problem with low buffer size,
    // because 'payload' can be modified in-place to reference every kv pair
    DynamicJsonDocument doc(1024);
    DeserializationError error = deserializeJson(doc, payload);

    if (error) {
        DEBUG_MSG_P(PSTR("[WEBSOCKET] JSON parsing error: %s\n"), error.c_str());
        wsSend_P(client_id, PSTR("{\"message\": 3}"));
        return;
    }

    // Check actions -----------------------------------------------------------

    JsonObject root = doc.as<JsonObject>();

    const char* action = root["action"];
    if (action) {

        DEBUG_MSG_P(PSTR("[WEBSOCKET] Requested action: %s\n"), action);

        if (strcmp(action, "reboot") == 0) {
            deferredReset(100, CUSTOM_RESET_WEB);
            return;
        }

        if (strcmp(action, "reconnect") == 0) {
            _web_defer.once_ms(100, wifiDisconnect);
            return;
        }

        if (strcmp(action, "factory_reset") == 0) {
            DEBUG_MSG_P(PSTR("\n\nFACTORY RESET\n\n"));
            resetSettings();
            deferredReset(100, CUSTOM_RESET_FACTORY);
            return;
        }

        JsonObject data = root["data"];
        if (!data.isNull()) {

            // Callbacks
            for (unsigned char i = 0; i < _ws_on_action_callbacks.size(); i++) {
                (_ws_on_action_callbacks[i])(client_id, action, data);
            }

            // Restore configuration via websockets
            if (strcmp(action, "restore") == 0) {
                if (settingsRestoreJson(data)) {
                    wsSend_P(client_id, PSTR("{\"message\": 5}"));
                } else {
                    wsSend_P(client_id, PSTR("{\"message\": 4}"));
                }
            }

            return;

        }

    };

    // Check configuration -----------------------------------------------------

    JsonObject config = root["config"];
    if (!config.isNull()) {

        DEBUG_MSG_P(PSTR("[WEBSOCKET] Parsing configuration data\n"));

        String adminPass;
        bool save = false;

        for (auto kv: config) {

            bool changed = false;

            // Check if key has to be processed
            const String key(kv.key().c_str());
            bool found = false;
            for (auto& check : _ws_key_check_callbacks) {
                found |= check(key.c_str());
                // TODO: remove this to call all KeyCheckCallbacks with the
                // current key/value
                if (found) break;
            }

            JsonVariant value = kv.value();

            // Check password
            if (key == "adminPass") {
                if (!value.is<JsonArray>()) continue;
                auto values = value.as<JsonArray>();
                if (values.size() != 2) continue;
                if (values[0].as<String>().equals(values[1].as<String>())) {
                    String password = values[0].as<String>();
                    if (password.length() > 0) {
                        setSetting(key, password);
                        save = true;
                        wsSend_P(client_id, PSTR("{\"action\": \"reload\"}"));
                    }
                } else {
                    wsSend_P(client_id, PSTR("{\"message\": 7}"));
                }
                continue;
            }

            if (!found) {
                delSetting(key);
                continue;
            }

            // Store values
            if (value.is<JsonArray>()) {
                if (_wsStore(key, value.as<JsonArray>())) changed = true;
            } else {
                if (_wsStore(key, value.as<String>())) changed = true;
            }

            // Update flags if value has changed
            if (changed) {
                save = true;
            }

        }

        // Save settings
        if (save) {

            // Callbacks
            espurnaReload();

            // Persist settings
            saveSettings();

            wsSend_P(client_id, PSTR("{\"message\": 8}"));

        } else {

            wsSend_P(client_id, PSTR("{\"message\": 9}"));

        }

    }

}

void _wsUpdate(JsonObject& root) {
    root["heap"] = getFreeHeap();
    root["uptime"] = getUptime();
    root["rssi"] = WiFi.RSSI();
    root["loadaverage"] = systemLoadAverage();
    #if ADC_MODE_VALUE == ADC_VCC
        root["vcc"] = ESP.getVcc();
    #endif
    #if NTP_SUPPORT
        if (ntpSynced()) root["now"] = now();
    #endif
}

void _wsDoUpdate(bool reset = false) {
    static unsigned long last = millis();
    if (reset) {
        last = millis() + WS_UPDATE_INTERVAL;
        return;
    }

    if (millis() - last > WS_UPDATE_INTERVAL) {
        last = millis();
        wsSend(_wsUpdate);
    }
}


bool _wsKeyCheck(const char * key) {
    if (strncmp(key, "ws", 2) == 0) return true;
    if (strncmp(key, "admin", 5) == 0) return true;
    if (strncmp(key, "hostname", 8) == 0) return true;
    if (strncmp(key, "desc", 4) == 0) return true;
    if (strncmp(key, "webPort", 7) == 0) return true;
    return false;
}

void _wsOnStart(JsonObject& root) {
    root["webMode"] = WEB_MODE_NORMAL;

    root["app_name"] = APP_NAME;
    root["app_version"] = APP_VERSION;
    root["device"] = DEVICE;
    root["manufacturer"] = MANUFACTURER;
    #if defined(APP_REVISION)
        root["app_revision"] = APP_REVISION;
    #endif

    root["app_build"] = buildTime();
    root["chipid"] = getChipID();
    root["mac"] = WiFi.macAddress();
    root["bssid"] = wifiBSSID();
    root["channel"] = WiFi.channel();
    root["hostname"] = getSetting("hostname");
    root["desc"] = getSetting("desc");
    root["network"] = getNetwork();
    root["deviceip"] = getIP();
    root["sketch_size"] = ESP.getSketchSize();
    root["free_size"] = ESP.getFreeSketchSpace();
    root["sdk"] = ESP.getSdkVersion();
    root["core"] = getCoreVersion();

    root["btnDelay"] = getSetting("btnDelay", BUTTON_DBLCLICK_DELAY).toInt();
    root["webPort"] = getSetting("webPort", WEB_PORT).toInt();
    root["wsAuth"] = getSetting("wsAuth", WS_AUTHENTICATION).toInt() == 1;
    #if TERMINAL_SUPPORT
        root["cmdVisible"] = 1;
    #endif

    root["hbMode"] = getSetting("hbMode", HEARTBEAT_MODE).toInt();
    root["hbInterval"] = getSetting("hbInterval", HEARTBEAT_INTERVAL).toInt();

    _wsDoUpdate(true);

}

void wsSend(JsonDocument& source, size_t len) {
    if (!len) len = measureJson(source);
    AsyncWebSocketMessageBuffer* buffer = _ws.makeBuffer(len);

    if (buffer) {
        serializeJson(source, reinterpret_cast<char*>(buffer->get()), buffer->length() + 1);
        _ws.textAll(buffer);
    }
}

void wsSend(JsonObject& source, size_t len) {
    if (!len) len = measureJson(source);
    AsyncWebSocketMessageBuffer* buffer = _ws.makeBuffer(len);

    if (buffer) {
        serializeJson(source, reinterpret_cast<char*>(buffer->get()), buffer->length() + 1);
        _ws.textAll(buffer);
    }
}

void wsSend(uint32_t client_id, JsonObject& source, size_t len) {

    AsyncWebSocketClient* client = _ws.client(client_id);
    if (client == nullptr) return;

    if (!len) len = measureJson(source);
    AsyncWebSocketMessageBuffer* buffer = _ws.makeBuffer(len);

    if (buffer) {
        serializeJson(source, reinterpret_cast<char*>(buffer->get()), buffer->length() + 1);
        _ws.textAll(buffer);
    }
}

void _wsStart(uint32_t client_id) {
    #if USE_PASSWORD && WEB_FORCE_PASS_CHANGE
        bool changePassword = getAdminPass().equals(ADMIN_PASS);
    #else
        bool changePassword = false;
    #endif

    constexpr const size_t WS_START_JSON_BUFFER_MAX = 1024;

    DynamicJsonDocument doc(WS_START_JSON_BUFFER_MAX);
    JsonObject root = doc.as<JsonObject>();

    if (changePassword) {
        root["webMode"] = WEB_MODE_PASSWORD;
        wsSend(client_id, root);
        return;
    }

    for (auto& callback : _ws_on_send_callbacks) {
        callback(root);
    }

    wsSend(client_id, root);
}

void _wsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){

    if (type == WS_EVT_CONNECT) {

        client->_tempObject = nullptr;

        #ifndef NOWSAUTH
            if (!_wsAuth(client)) {
                wsSend_P(client->id(), PSTR("{\"message\": 10}"));
                DEBUG_MSG_P(PSTR("[WEBSOCKET] Validation check failed\n"));
                client->close();
                return;
            }
        #endif

        IPAddress ip = client->remoteIP();
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u connected, ip: %d.%d.%d.%d, url: %s\n"), client->id(), ip[0], ip[1], ip[2], ip[3], server->url());
        _wsStart(client->id());
        client->_tempObject = new WebSocketIncommingBuffer(_wsParse, true);
        wifiReconnectCheck();

    } else if(type == WS_EVT_DISCONNECT) {
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u disconnected\n"), client->id());
        if (client->_tempObject) {
            delete (WebSocketIncommingBuffer *) client->_tempObject;
        }
        wifiReconnectCheck();

    } else if(type == WS_EVT_ERROR) {
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u error(%u): %s\n"), client->id(), *((uint16_t*)arg), (char*)data);

    } else if(type == WS_EVT_PONG) {
        DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u pong(%u): %s\n"), client->id(), len, len ? (char*) data : "");

    } else if(type == WS_EVT_DATA) {
        //DEBUG_MSG_P(PSTR("[WEBSOCKET] #%u data(%u): %s\n"), client->id(), len, len ? (char*) data : "");
        WebSocketIncommingBuffer *buffer = (WebSocketIncommingBuffer *)client->_tempObject;
        AwsFrameInfo * info = (AwsFrameInfo*)arg;
        buffer->data_event(client, info, data, len);

    }

}

void _wsLoop() {
    if (!wsConnected()) return;
    _wsDoUpdate();
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

bool wsConnected() {
    return (_ws.count() > 0);
}

bool wsConnected(uint32_t client_id) {
    return _ws.hasClient(client_id);
}

void wsOnSendRegister(ws_on_send_callback_f callback) {
    _ws_on_send_callbacks.push_back(callback);
}

void wsKeyCheckRegister(ws_key_check_callback_f callback) {
    _ws_key_check_callbacks.push_back(callback);
}

void wsOnActionRegister(ws_on_action_callback_f callback) {
    _ws_on_action_callbacks.push_back(callback);
}

void wsSend(ws_on_send_callback_f callback) {

    constexpr const size_t WS_SEND_JSON_BUFFER_MAX = 1024;

    if (_ws.count() > 0) {
        DynamicJsonDocument doc(WS_SEND_JSON_BUFFER_MAX);
        JsonObject root = doc.as<JsonObject>();
        callback(root);
        wsSend(root);
    }

}

void wsSend(const char * payload) {
    if (_ws.count() > 0) {
        _ws.textAll(payload);
    }
}

void wsSend_P(PGM_P payload) {
    if (_ws.count() > 0) {
        char buffer[strlen_P(payload)];
        strcpy_P(buffer, payload);
        _ws.textAll(buffer);
    }
}

void wsSend(uint32_t client_id, ws_on_send_callback_f callback) {
    AsyncWebSocketClient* client = _ws.client(client_id);
    if (client == nullptr) return;

    constexpr const size_t WS_SEND_CB_JSON_BUFFER_MAX = 1024;

    DynamicJsonDocument doc(WS_SEND_CB_JSON_BUFFER_MAX);
    JsonObject root = doc.as<JsonObject>();
    callback(root);

    AsyncWebSocketMessageBuffer* buffer = _ws.makeBuffer(doc.size());

    if (buffer) {
        serializeJson(doc, reinterpret_cast<char*>(buffer->get()), buffer->length() + 1);
        client->text(buffer);
    }
}

void wsSend(uint32_t client_id, const char * payload) {
    _ws.text(client_id, payload);
}

void wsSend_P(uint32_t client_id, PGM_P payload) {
    char buffer[strlen_P(payload)];
    strcpy_P(buffer, payload);
    _ws.text(client_id, buffer);
}

void wsSetup() {

    _ws.onEvent(_wsEvent);
    webServer()->addHandler(&_ws);

    // CORS
    const String webDomain = getSetting("webDomain", WEB_REMOTE_DOMAIN);
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", webDomain);
    if (!webDomain.equals("*")) {
        DefaultHeaders::Instance().addHeader("Access-Control-Allow-Credentials", "true");
    }

    webServer()->on("/auth", HTTP_GET, _onAuth);

    #if MQTT_SUPPORT
        mqttRegister(_wsMQTTCallback);
    #endif

    wsOnSendRegister(_wsOnStart);
    wsKeyCheckRegister(_wsKeyCheck);
    espurnaRegisterLoop(_wsLoop);
}

#endif // WEB_SUPPORT
