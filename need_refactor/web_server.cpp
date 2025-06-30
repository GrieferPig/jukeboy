#include "web_server.h"
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <FS.h>
#include <LittleFS.h>     // Required for LittleFS interaction
#include "audio_player.h" // Include audio player functions
#include "esp_log.h"      // Added for ESP_LOGx macros

static const char *TAG = "WebServer"; // Added for ESP_LOGx

// WiFi AP credentials and DNS settings
const char *ssid_web = "test";
const uint8_t DNS_PORT_WEB = 53;
DNSServer dnsServer_web;
IPAddress apIP_web(192, 168, 4, 1); // The IP address of the ESP32 in AP mode
// const char *customDomain_web = "test.local"; // Not currently used

WebServer server_web(80); // Web server on port 80

// --- Audio Control Handlers ---
void handleAudioPlayNext()
{
    audio_play_next();
    server_web.send(200, "text/plain", "Play Next command sent.");
}

void handleAudioPlayPrevious()
{
    audio_play_previous();
    server_web.send(200, "text/plain", "Play Previous command sent.");
}

void handleAudioRestartTrack()
{
    audio_restart_track();
    server_web.send(200, "text/plain", "Restart Track command sent.");
}

void handleAudioJumpToPosition()
{
    if (server_web.hasArg("seconds"))
    {
        uint32_t seconds = server_web.arg("seconds").toInt();
        audio_jump_to_position_seconds(seconds);
        server_web.send(200, "text/plain", "Jump to Position command sent.");
    }
    else
    {
        server_web.send(400, "text/plain", "Missing 'seconds' parameter.");
    }
}

void handleAudioFastForward()
{
    if (server_web.hasArg("seconds"))
    {
        uint32_t seconds = server_web.arg("seconds").toInt();
        audio_fast_forward_seconds(seconds);
        server_web.send(200, "text/plain", "Fast Forward command sent.");
    }
    else
    {
        server_web.send(400, "text/plain", "Missing 'seconds' parameter.");
    }
}

void handleAudioRewind()
{
    if (server_web.hasArg("seconds"))
    {
        uint32_t seconds = server_web.arg("seconds").toInt();
        audio_rewind_seconds(seconds);
        server_web.send(200, "text/plain", "Rewind command sent.");
    }
    else
    {
        server_web.send(400, "text/plain", "Missing 'seconds' parameter.");
    }
}

void handleAudioSetGain()
{
    if (server_web.hasArg("gain"))
    {
        float gain = server_web.arg("gain").toFloat();
        audio_set_gain(gain);
        server_web.send(200, "text/plain", "Set Gain command sent.");
    }
    else
    {
        server_web.send(400, "text/plain", "Missing 'gain' parameter.");
    }
}

void handleAudioTogglePause()
{
    audio_toggle_pause();
    server_web.send(200, "text/plain", "Toggle Pause command sent.");
}

void handleAudioGetCurrentTrackInfo()
{
    audio_get_current_track_info();
    server_web.send(200, "text/plain", "Get Current Track Info command sent. Check Serial for details.");
}

void handleAudioGetPlaybackStatus()
{
    audio_get_playback_status();
    server_web.send(200, "text/plain", "Get Playback Status command sent. Check Serial for details.");
}

void handleAudioSetPlaybackMode()
{
    if (server_web.hasArg("mode"))
    {
        String modeStr = server_web.arg("mode");
        PlaybackMode modeToSet;
        bool validMode = false;

        if (modeStr.equalsIgnoreCase("loop"))
        {
            modeToSet = PLAYBACK_MODE_LOOP;
            validMode = true;
        }
        else if (modeStr.equalsIgnoreCase("shuffle"))
        {
            modeToSet = PLAYBACK_MODE_SHUFFLE;
            validMode = true;
        }

        if (validMode)
        {
            audio_set_playback_mode(modeToSet);
            server_web.send(200, "text/plain", "Playback mode command sent. Mode: " + modeStr);
        }
        else
        {
            server_web.send(400, "text/plain", "Invalid 'mode' parameter. Use 'loop' or 'shuffle'.");
        }
    }
    else
    {
        server_web.send(400, "text/plain", "Missing 'mode' parameter.");
    }
}
// --- End of Audio Control Handlers ---

// Task for handling web server and DNS
void webServerTask(void *pvParameter)
{
    ESP_LOGI(TAG, "Starting");

    WiFi.softAPConfig(apIP_web, apIP_web, IPAddress(255, 255, 255, 0));
    WiFi.softAP(ssid_web, NULL);
    delay(100); // Allow time for AP to start

    IPAddress ip = WiFi.softAPIP();
    ESP_LOGI(TAG, "AP IP address: %s", ip.toString().c_str());

    dnsServer_web.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer_web.start(DNS_PORT_WEB, "thejuke.box", ip);

    ESP_LOGI(TAG, "DNS Server started.");

    // Register audio control handlers
    server_web.on("/audio/play_next", HTTP_GET, handleAudioPlayNext);
    server_web.on("/audio/play_previous", HTTP_GET, handleAudioPlayPrevious);
    server_web.on("/audio/restart", HTTP_GET, handleAudioRestartTrack);
    server_web.on("/audio/jump", HTTP_GET, handleAudioJumpToPosition);
    server_web.on("/audio/ffwd", HTTP_GET, handleAudioFastForward);
    server_web.on("/audio/rewind", HTTP_GET, handleAudioRewind);
    server_web.on("/audio/set_gain", HTTP_GET, handleAudioSetGain);
    server_web.on("/audio/toggle_pause", HTTP_GET, handleAudioTogglePause);
    server_web.on("/audio/info", HTTP_GET, handleAudioGetCurrentTrackInfo);
    server_web.on("/audio/status", HTTP_GET, handleAudioGetPlaybackStatus);
    server_web.on("/audio/set_mode", HTTP_GET, handleAudioSetPlaybackMode);

    // Serve static files from LittleFS root
    server_web.serveStatic("/", LittleFS, "/");
    // Custom handler for listing files when a file is not found (or for root if index.html is missing)
    server_web.onNotFound([]()
                          {
        server_web.sendHeader("Location", String("http://thejuke.box/index.html"), true);
        server_web.send(302, "text/plain", ""); });
    server_web.begin();
    ESP_LOGI(TAG, "Web server started.");

    for (;;)
    { // Task loop
        dnsServer_web.processNextRequest();
        server_web.handleClient();
        vTaskDelay(pdMS_TO_TICKS(10)); // Yield for other tasks
    }
}
