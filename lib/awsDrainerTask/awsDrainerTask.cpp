#include <Arduino.h>
#include "aws_sender.h"
#include "FS.h"
#include "SD.h"
#include "mutexWatchdog.h"
#include "aws_sender.h"
#include "esp_task_wdt.h"
//#include "rgb_led.h"
#include "config.h"
#include "shared_tasks.h"
#include "status_feedback.h"

extern SemaphoreHandle_t sdMutex;
extern SemaphoreHandle_t wifiMutex;

bool isTempFileNonEmpty() {
  bool result = false;

  if (xSemaphoreTake(sdMutex, portMAX_DELAY)) {
    File file = SD.open("/temp2.csv", FILE_READ);
    if (!file) {
      Serial.println("temp2.csv is empty or not found.");
      if (file) file.close();
      esp_task_wdt_reset();
       xSemaphoreGive(sdMutex);
      vTaskDelay(30000 / portTICK_PERIOD_MS);
    }
    esp_task_wdt_reset();
    if (file) {
      int lineCount = 0;
      while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() > 10) lineCount++;
        esp_task_wdt_reset();
      }
      file.close();
      result = (lineCount > 1); // header + at least 1 data row
      Serial.println("Lines in temp2");
      Serial.print(result);
    }
    esp_task_wdt_reset();
    xSemaphoreGive(sdMutex);
  }

  return result;
}

bool shouldDrainTemp2() {
  bool temp2HasData = false;

  if (xSemaphoreTake(sdMutex, portMAX_DELAY) != pdTRUE) return false;

  File temp2File = SD.open("/temp2.csv", FILE_READ);

  if (temp2File) {
    while (temp2File.available()) {
      esp_task_wdt_reset();  // Prevent watchdog reset during long read
      String line = temp2File.readStringUntil('\n');
      line.trim();

      if (line.length() > 10 && !line.startsWith("Date,Time")) {
        temp2HasData = true;
        break;
      }
    }
    temp2File.close();
  } else {
    Serial.println("[shouldDrainTemp2] Failed to open /temp2.csv");
  }

  xSemaphoreGive(sdMutex);
  return temp2HasData;
}


void awsdrain() {
  esp_task_wdt_reset();

  if (!xSemaphoreTake(awsDrainerLock, pdMS_TO_TICKS(100))) {
    Serial.println("[awsdrain] Could not take awsDrainerLock.");
    return;
  }

  Serial.println("[awsdrain] Got lock. Starting drain.");

  bool wifiConnected = false;
  bool mqttConnected = false;

  if (xSemaphoreTake(wifiMutex, portMAX_DELAY) == pdTRUE) {
    esp_task_wdt_reset();
    if (WiFi.status() == WL_CONNECTED) {
      wifiConnected = true;
      int count = 0;
      if (!client.connected()&& count++ < 20) {
        Serial.println("[awsdrain] MQTT reconnecting...");
        if (initAWS()) {
          mqttConnected = true;
          Serial.println("[awsdrain] MQTT connected.");
        } else {
          Serial.println("[awsdrain] MQTT reconnect failed.");
          delay(100);
        }
      } else {
        mqttConnected = true;
      }

     // if (mqttConnected) client.loop();
    } else {
      Serial.println("[awsdrain] WiFi not connected.");
    }
    xSemaphoreGive(wifiMutex);
  }

  if (!wifiConnected || !mqttConnected) {
    Serial.println("[awsdrain] Aborting drain. WiFi or MQTT unavailable.");
    xSemaphoreGive(awsDrainerLock);
    return;
  }

  if (xSemaphoreTake(sdMutex, portMAX_DELAY) != pdTRUE) {
    Serial.println("[awsdrain] Failed to take sdMutex.");
    xSemaphoreGive(awsDrainerLock);
    return;
  }

  File temp2File = SD.open("/temp2.csv", FILE_READ);
  if (!temp2File || temp2File.size() == 0) {
    Serial.println("[awsdrain] temp2.csv is empty.");
    if (temp2File) temp2File.close();
    xSemaphoreGive(sdMutex);
    xSemaphoreGive(awsDrainerLock);
    return;
  }

  File temp3File = SD.open("/temp3.csv", FILE_APPEND);
  File sentFile  = SD.open("/sent.csv", FILE_APPEND);
  if (!temp3File || !sentFile) {
    Serial.println("[awsdrain] Failed to open temp3/sent.");
    if (temp3File) temp3File.close();
    if (sentFile) sentFile.close();
    temp2File.close();
    xSemaphoreGive(sdMutex);
    xSemaphoreGive(awsDrainerLock);
    return;
  }

  bool headerSkipped = false;

  while (temp2File.available()) {
    esp_task_wdt_reset();
    vTaskDelay(1);  // Allow FreeRTOS to run other tasks (avoid watchdog trigger)

    String line = temp2File.readStringUntil('\n');
    line.trim();
    if (line.isEmpty()) continue;

    if (line.startsWith("Date,Time")) {
      if (!headerSkipped) headerSkipped = true;
      continue;
    }

    int firstComma = line.indexOf(',');
    String date = line.substring(0, firstComma);
    String time = line.substring(firstComma + 1, line.indexOf(',', firstComma + 1));
    String values = line.substring(line.indexOf(',', firstComma + 1) + 1);

    int rssi = 0;
    if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      rssi = WiFi.RSSI();
      xSemaphoreGive(wifiMutex);
    }

    String payload = "{\"ts\":\"" + date + "T" + time + "\",\"id\":\"" + DEVICE_UID +
                     "\",\"Fid\":\"" + FirmwareID + "\",\"values\":[" + String(rssi) + ",";

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

    bool publishOK = false;
    if (xSemaphoreTake(wifiMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      publishOK = client.publish("esp32/topic", payload.c_str());
      xSemaphoreGive(wifiMutex);
    }

    if (publishOK) {
      Serial.println("Sent → " + payload);
      sentFile.println(line);
    } else {
      Serial.println("Retry Later → " + payload);
      temp3File.println(line);
    }

    delay(300);  // throttle
  }

  temp2File.close();
  temp3File.close();
  sentFile.close();

  // ------- Rebuild temp2.csv from temp3 --------
  File retryLines = SD.open("/temp3.csv", FILE_READ);
  File newTemp = SD.open("/temp2.csv", FILE_WRITE);
  bool headerWritten = false;

  if (retryLines && newTemp) {
    while (retryLines.available()) {
      esp_task_wdt_reset();
      vTaskDelay(1);  // Yield

      String line = retryLines.readStringUntil('\n');
      line.trim();
      if (line.isEmpty()) continue;

      if (line.startsWith("Date,Time")) {
        if (!headerWritten) {
          newTemp.println(line);
          headerWritten = true;
        }
      } else {
        newTemp.println(line);
      }
      vTaskDelay(1);
    }
  }

  if (retryLines) retryLines.close();
  if (newTemp) newTemp.close();
  SD.remove("/temp3.csv");

  client.disconnect();  // Clean MQTT disconnect
  delay(1000);  // Let TCP/TLS stack clear

  xSemaphoreGive(sdMutex);
  xSemaphoreGive(awsDrainerLock);

  Serial.println("[awsdrain] Completed drain and cleanup. and client disconnected");

}


void awsDrainerTask(void* parameter) {
  esp_task_wdt_add(NULL);  // Register with watchdog

  for (;;) {
    esp_task_wdt_reset();
    setSystemState(SYS_DRAIN_SEQUENCE);

    if (otainprogress) {
      Serial.println("[awsDrainerTask] OTA in progress. Skipping this cycle...");
      vTaskDelay(pdMS_TO_TICKS(10000));
      continue;
    }

    if (shouldDrainTemp2()) {
      Serial.println("[awsDrainerTask] Conditions met. Starting drain...");
      awsdrain();
    } else {
      Serial.println("[awsDrainerTask] Skipping drain: temp2.csv empty.");
    }

    vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
  }
}



