// Claude Cube loader: the smallest firmware that joins Wi-Fi and accepts an update at /update.
// The full firmware is too big to OTA over itself, so updates go full -> loader -> full
// (tools/flash.sh). Falls back to the ClaudeCube access point if Wi-Fi doesn't come up.
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>

ESP8266WebServer server(80);
ESP8266HTTPUpdateServer updater;

void setup() {
  pinMode(5, OUTPUT);
  digitalWrite(5, HIGH);  // backlight off (active low): the panel holds no picture here
  WiFi.mode(WIFI_STA);
  WiFi.hostname("claudecube");
  WiFi.begin();
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) { WiFi.mode(WIFI_AP); WiFi.softAP("ClaudeCube"); }
  server.on("/status", []() { server.send(200, "application/json", "{\"fw\":\"claude-cube-loader\"}"); });
  server.on("/", []() { server.sendHeader("Location", "/update"); server.send(302); });
  updater.setup(&server, "/update");
  server.begin();
}

void loop() { server.handleClient(); }
