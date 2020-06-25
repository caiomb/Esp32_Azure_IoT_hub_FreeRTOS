#ifndef ESP32_AZURE_IOT_HUB_CONSTANTS_H
#define ESP32_AZURE_IOT_HUB_CONSTANTS_H

// TODO: provide correct SSID for WiFi
#define WIFI_SSID "NET_2GE5F156"
// TODO: provide correct password for WiFi
#define WIFI_PASS "65E5F156"

// TODO: provide correct connectiong string for Azure IoT Hub connection
#define AZURE_IOT_CONNECTION_STRING "HostName=cbraga-iot-hub.azure-devices.net;DeviceId=esp32_cbraga;SharedAccessKey=Ke8TrZiDULoJs8Jnr9XI0inZhb94gBsT7ejoPs/aw9I="

// Interval of Device to Cloud (D2C) publishing in seconds
#define TX_INTERVAL_SECOND 60

#endif