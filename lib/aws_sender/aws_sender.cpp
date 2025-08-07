#include "aws_sender.h"
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include "FS.h"
#include "SD.h"
//#include "rgb_led.h"
#include "esp_task_wdt.h"
#include <certs.h>
#include "config.h"
#include "shared_tasks.h"
#include "status_feedback.h"


extern SemaphoreHandle_t sdMutex;
extern SemaphoreHandle_t wifiMutex;

const char* aws_endpoint = "a1j6n3szeyh3xa-ats.iot.ap-south-1.amazonaws.com";
const int aws_port = 8883;
const char* thingName = "poc-001";



bool initAWS() {
  net.setCACert(rootCA);
  net.setCertificate(cert);
  net.setPrivateKey(privateKey);
  client.setServer(aws_endpoint, aws_port);
  int count =0;
  while (!client.connected()&& count++<20) {
    esp_task_wdt_reset();
    if (client.connect(thingName)) {
      Serial.println(" Connected to AWS IoT");
      esp_task_wdt_reset();
      return true; 
    } else {
      Serial.print(" AWS MQTT Connect failed: ");
      Serial.println(client.state());
      esp_task_wdt_reset();
      delay(200);
      return false;
    }
  }
}



void awsSenderTaskprocess() {
  esp_task_wdt_reset();  // Initial feed

  // 🔒 Prevent clash with awsdrain()
  if (!xSemaphoreTake(awsDrainerLock, pdMS_TO_TICKS(100))) {
    Serial.println("[AWS Sender] Skipping — awsDrainer is running.");
    return;
  }

  bool wifiConnected = false;
  bool mqttConnected = false;

  // -------- Wi-Fi and MQTT Checks --------
  if (xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
    esp_task_wdt_reset();

    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[AWS Sender] WiFi disconnected.");
    } else {
      Serial.println("[AWS_SENDER FN] WiFi connected.");
      wifiConnected = true;

      if (!client.connected()) {
        Serial.println("[AWS Sender] MQTT reconnecting...");
        esp_task_wdt_reset();
        if (initAWS()) {
          mqttConnected = true;
          Serial.println("[AWS Sender] MQTT connected.");
        } else {
          Serial.println("[AWS Sender] MQTT reconnect failed.");
        }
      } else {
        mqttConnected = true;
      }

      // ✅ Call client.loop() only when connected
      if (mqttConnected) client.loop();
    }

    xSemaphoreGive(wifiMutex);
  } else {
    Serial.println("[AWS_SENDER FN] Could not take wifiMutex.");
  }

  // -------- SD Access Start --------
  if (xSemaphoreTake(sdMutex, portMAX_DELAY) != pdTRUE) {
    Serial.println("[AWS Sender] Failed to take sdMutex.");
    xSemaphoreGive(awsDrainerLock);
    return;
  }

  File tempFile = SD.open("/temp.csv", FILE_READ);
  if (!tempFile || tempFile.size() == 0) {
    Serial.println("[AWS Sender] temp.csv is empty or not found.");
    if (tempFile) tempFile.close();
    xSemaphoreGive(sdMutex);
    xSemaphoreGive(awsDrainerLock);
    vTaskDelay(30000 / portTICK_PERIOD_MS);
    return;
  }

  File temp2File = SD.open("/temp2.csv", FILE_APPEND);
  File sentFile = SD.open("/sent.csv", FILE_APPEND);
  if (!temp2File || !sentFile) {
    Serial.println("[AWS Sender] File open failed (temp2/sent).");
    if (temp2File) temp2File.close();
    if (sentFile) sentFile.close();
    tempFile.close();
    xSemaphoreGive(sdMutex);
    xSemaphoreGive(awsDrainerLock);
    vTaskDelay(30000 / portTICK_PERIOD_MS);
    return;
  }

  bool headerSkipped = false;

  while (tempFile.available()) {
    esp_task_wdt_reset();
    String line = tempFile.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;

    if (line.startsWith("Date,Time,KWH1")) {
      if (!headerSkipped) {
        temp2File.println(line);
        headerSkipped = true;
      }
      continue;
    }

    if (!wifiConnected || !mqttConnected) {
      // Save to temp2 if not connected
      temp2File.println(line);
      Serial.println("Retry Later 1→ " + line);
      continue;
    }

    // ---------- Parse and Build Payload ----------
    int firstComma = line.indexOf(',');
    String date = line.substring(0, firstComma);
    String time = line.substring(firstComma + 1, line.indexOf(',', firstComma + 1));
    String values = line.substring(line.indexOf(',', firstComma + 1) + 1);

    int rssi = 0;
    if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      rssi = WiFi.RSSI();
      xSemaphoreGive(wifiMutex);
    }

    // ✅ Standard MQTT Payload Format
    String payload = "{\"TS\":\"" + date + "T" + time + "\",\"DID\":\"" + DEVICE_UID +
                     "\",\"FID\":\"" + FirmwareID + "\",\"values\":[" + String(rssi) + ",";

    for (int i = 0; i < 36; i++) {
      int commaIndex = values.indexOf(',');
      String val = (i < 35) ? values.substring(0, commaIndex) : values;
      val.trim();
      if (val == "" || val == "NaN" || val == "null") val = "N";
      payload += val;
      if (i < 35) payload += ",";
      values = values.substring(commaIndex + 1);
    }
    payload += "]}";

    Serial.println("---- Payload ----");
    Serial.println(payload);
    Serial.print("Payload size: ");
    Serial.println(payload.length());
    Serial.println("-----------------");

    // -------- Publish to AWS --------
    bool publishOK = false;
    if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      esp_task_wdt_reset();
      publishOK = client.publish("esp32/topic", payload.c_str());
      xSemaphoreGive(wifiMutex);
    }

    if (publishOK) {
      Serial.println("Sent → " + payload);
      setSystemState(SYS_DATA_SENT);
      sentFile.println(line);
      sentFile.flush();
      setSystemState(SYS_NORMAL);
    } else {
      Serial.println("Retry Later → " + payload);
      temp2File.println(line);
      //temp2File.flush();
      setSystemState(SYS_NORMAL);
    }

    delay(300);  // throttle MQTT bursts
  }

  tempFile.close();
  temp2File.close();
  sentFile.close();
  SD.remove("/temp.csv");
  xSemaphoreGive(sdMutex);
  xSemaphoreGive(awsDrainerLock);
  client.disconnect();
  Serial.println("[AWS SENDER] : Sending Done, Cleanup Done, AWS Client removed");
  vTaskDelay(3000 / portTICK_PERIOD_MS);
}



void awsSenderTask(void *parameter) {
  while (true) {
    //ulTaskNotifyTake(pdTRUE, portMAX_DELAY); //Wait for Modbus task to trigger
    int sendTrigger;
    if (xQueueReceive(senderQueue, &sendTrigger, portMAX_DELAY) == pdTRUE) {
      esp_task_wdt_reset();
      Serial.println("[awsSenderTask] Received trigger from RS485 task");
    Serial.println("[AWS Task] Notified, sending data...");
    esp_task_wdt_reset();
    awsSenderTaskprocess();
    esp_task_wdt_reset();
    }
  }
}

