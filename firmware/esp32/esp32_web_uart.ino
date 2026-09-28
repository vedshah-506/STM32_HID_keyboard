#include <WiFi.h>
#include <WebServer.h>

HardwareSerial STM32Serial(2);
WebServer server(80);

const char *AP_SSID = "STM32_ESP32";
const char *AP_PASSWORD = "CHANGE_THIS_PASSWORD";

// ESP32 UART2 pins.
static const int STM32_RX_PIN = 16;
static const int STM32_TX_PIN = 17;

const char WEB_PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>STM32 HID Terminal</title>
</head>
<body>
  <h2>Send text to STM32</h2>
  <form action="/send" method="POST">
    <input name="message" maxlength="120" autofocus required>
    <button type="submit">Send</button>
  </form>
</body>
</html>
)rawliteral";

void handleRoot()
{
    server.send(200, "text/html", WEB_PAGE);
}

void handleSend()
{
    if (!server.hasArg("message"))
    {
        server.send(400, "text/plain", "Missing message");
        return;
    }

    String message = server.arg("message");

    if (message.length() == 0)
    {
        server.send(400, "text/plain", "Message cannot be empty");
        return;
    }

    if (message.length() > 120)
    {
        message = message.substring(0, 120);
    }

    Serial.print("Sending to STM32: ");
    Serial.println(message);

    STM32Serial.print(message);
    STM32Serial.print('\n');
    STM32Serial.flush();

    server.send(200, "text/html",
                "<p>Message sent to STM32.</p>"
                "<a href=\"/\">Send another message</a>");
}

void setup()
{
    Serial.begin(115200);

    STM32Serial.begin(115200, SERIAL_8N1,
                      STM32_RX_PIN, STM32_TX_PIN);

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    server.on("/", HTTP_GET, handleRoot);
    server.on("/send", HTTP_POST, handleSend);
    server.begin();

    Serial.println();
    Serial.println("ESP32 HID bridge started.");
    Serial.print("Connect to Wi-Fi network: ");
    Serial.println(AP_SSID);
    Serial.print("Open this address: http://");
    Serial.println(WiFi.softAPIP());
}

void loop()
{
    server.handleClient();
}
