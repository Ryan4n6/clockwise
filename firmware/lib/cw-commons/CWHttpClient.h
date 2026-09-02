#pragma once

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "DbgUdp.h"

struct ClockwiseHttpClient
{

  static ClockwiseHttpClient *getInstance()
  {
    static ClockwiseHttpClient base;
    return &base;
  }

  // Takes a base WiFiClient* so the caller can pass either a plain WiFiClient
  // (HTTP, no TLS handshake -> no big contiguous-heap demand) or a WiFiClientSecure
  // (HTTPS; the caller calls setInsecure() on it before passing).
  void httpGet(WiFiClient *client, const char *host, const char *path, const uint16_t port)
  {
    Serial.printf("[HTTP] GET request to '%s%s' on port %d\n", host, path, port);

    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Not connected");
      return;
    }

    client->setTimeout(10000);
    DBG((String("HG connect ") + host + ":" + port).c_str());
    if (!client->connect(host, port))
    {
      // Log host/port/heap on failure. A TCP/TLS connect can fail on an egress
      // block, a refuse/timeout, or (for HTTPS) too little contiguous heap for the
      // handshake. maxAlloc is the tell for the memory case.
      Serial.printf("[HTTP] CONNECT FAIL host=%s port=%d freeHeap=%d maxAlloc=%d\n",
                    host, port, (int)ESP.getFreeHeap(), (int)ESP.getMaxAllocHeap());
      Serial.println(F("Connection failed"));
      return;
    }
    DBG("HG connect OK");

    // Send HTTP request
    client->printf("GET %s HTTP/1.1\r\n", path);
    client->printf("Host: %s\r\n", host);
    client->println(F("Connection: close"));
    if (client->println() == 0)
    {
      Serial.println(F("Failed to send request"));
      client->stop();
      return;
    }

    // char arrCode[4];
    // memcpy(arrCode, status + 8, 3);  //HTTP/1.1 404 Not Found
    // arrCode[3] = 0;
    // uint16_t httpCode = atoi(arrCode);

    // Check HTTP status
    char status[32] = {0};
    client->readBytesUntil('\r', status, sizeof(status));
    DBG((String("HG status=[") + status + "]").c_str());

    if (strstr(status, "200 OK") == NULL)
    {
      Serial.print(F("Unexpected response: "));
      Serial.println(status);
      client->stop();
      return;
    }
    DBG("HG status 200 OK");

    // Skip HTTP headers
    char endOfHeaders[] = "\r\n\r\n";
    if (!client->find(endOfHeaders))
    {
      Serial.println(F("Invalid response"));
      client->stop();
      return;
    }
  }
};
