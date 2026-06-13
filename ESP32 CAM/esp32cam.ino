// ================================================================
//  ESP32-CAM MJPEG STREAM FOR YOLO
//  Board: AI Thinker ESP32-CAM
// ================================================================

#include <WebServer.h>
#include <WiFi.h>
#include <esp32cam.h>
#include <esp_camera.h>

// ================================================================
// WIFI
// ================================================================

// const char* WIFI_SSID =; 
// const char* WIFI_PASS =;

// ================================================================
// SERVER
// ================================================================

WebServer server(8080);

static auto streamRes = esp32cam::Resolution::find(640, 480);

#define STREAM_BOUNDARY "123456789000000000000987654321"

// ================================================================
// ROOT PAGE
// ================================================================

void handleRoot() {
  String ip = WiFi.localIP().toString();

  String html =
      "<html>"
      "<head>"
      "<meta charset='UTF-8'>"
      "<title>ESP32-CAM LIVE STREAM</title>"
      "</head>"
      "<body style='text-align:center;'>"
      "<h2>ESP32-CAM LIVE STREAM</h2>"
      "<p>IP: " + ip + ":8080</p>"
      "<p>Stream URL: http://" + ip + ":8080/stream</p>"
      "<p>Capture URL: http://" + ip + ":8080/capture</p>"
      "<img src='/stream' width='640'>"
      "</body>"
      "</html>";

  server.send(200, "text/html", html);
}

// ================================================================
// CAPTURE HANDLER
// Trả về 1 ảnh JPG duy nhất
// URL: /capture
// ================================================================

void handleCapture() {
  auto frame = esp32cam::capture();

  if (frame == nullptr) {
    Serial.println("[CAPTURE] Capture failed");
    server.send(500, "text/plain", "Capture failed");
    return;
  }

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Cache-Control", "no-store");
  server.sendHeader("Content-Type", "image/jpeg");
  server.sendHeader("Content-Length", String(frame->size()));

  WiFiClient client = server.client();
  frame->writeTo(client);

  frame.reset();

  Serial.println("[CAPTURE] OK");
}

// ================================================================
// STREAM HANDLER
// Trả về MJPEG stream liên tục
// URL: /stream
// ================================================================

void handleStream() {
  WiFiClient client = server.client();

  String response =
      "HTTP/1.1 200 OK\r\n"
      "Content-Type: multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY "\r\n"
      "Access-Control-Allow-Origin: *\r\n"
      "Cache-Control: no-cache\r\n"
      "Connection: close\r\n\r\n";

  client.print(response);

  Serial.println("[STREAM] Client connected");

  while (client.connected()) {
    auto frame = esp32cam::capture();

    if (frame == nullptr) {
      Serial.println("[STREAM] Capture failed");
      delay(10);
      continue;
    }

    String head =
        "--" STREAM_BOUNDARY "\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: " + String(frame->size()) + "\r\n\r\n";

    client.print(head);
    frame->writeTo(client);
    client.print("\r\n");

    frame.reset();

    delay(50);
  }

  Serial.println("[STREAM] Client disconnected");
}

// ================================================================
// SETUP
// ================================================================

void setup() {
  Serial.begin(115200);

  Serial.println();
  Serial.println("[BOOT] ESP32-CAM");

  // ==============================================================
  // CAMERA
  // ==============================================================

  {
    using namespace esp32cam;

    Config cfg;

    cfg.setPins(pins::AiThinker);
    cfg.setResolution(streamRes);
    cfg.setBufferCount(2);
    cfg.setJpeg(80);

    bool ok = Camera.begin(cfg);

    if (!ok) {
      Serial.println("[CAM] FAILED");
      while (true) {
        delay(1000);
      }
    }

    Serial.println("[CAM] OK");

    sensor_t* s = esp_camera_sensor_get();

    if (s != nullptr) {
      // Nếu ảnh đang đúng chiều thì giữ 0.
      // Nếu ảnh bị ngược/trái phải thì đổi lại.
      s->set_vflip(s, 1);
      s->set_hmirror(s, 1);
    }
  }

  // ==============================================================
  // WIFI
  // ==============================================================

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  Serial.print("[WiFi] Connecting");

  int retry = 0;

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    retry++;

    if (retry > 60) {
      Serial.println();
      Serial.println("[WiFi] Failed. Restarting...");
      delay(1000);
      ESP.restart();
    }
  }

  Serial.println();
  Serial.println("[WiFi] Connected!");

  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  Serial.println();
  Serial.println("Open browser:");
  Serial.print("http://");
  Serial.print(WiFi.localIP());
  Serial.println(":8080");

  Serial.println();
  Serial.println("Stream URL:");
  Serial.print("http://");
  Serial.print(WiFi.localIP());
  Serial.println(":8080/stream");

  Serial.println();
  Serial.println("Capture URL:");
  Serial.print("http://");
  Serial.print(WiFi.localIP());
  Serial.println(":8080/capture");

  // ==============================================================
  // ROUTES
  // ==============================================================

  server.on("/", HTTP_GET, handleRoot);
  server.on("/stream", HTTP_GET, handleStream);
  server.on("/capture", HTTP_GET, handleCapture);

  server.begin();

  Serial.println("[SERVER] Started");
}

// ================================================================
// LOOP
// ================================================================

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Mất kết nối! Đang khởi động lại...");
    delay(1000);
    ESP.restart();
  }
  server.handleClient();
}