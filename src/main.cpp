#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <vector>

#define SERVICE_UUID "BE28A6AE-FE7A-4394-8858-F48BD1D62026"
#define RX_UUID "4AF882CE-9582-4515-BEAB-1941400E169B" // phone -> ESP32
#define TX_UUID "718CEFEA-4CE7-4A32-BC46-8C24BC97BD7E" // ESP32 -> phone

using namespace std;

vector<String> activeUserIds;
vector<String> stagedUserIds;

vector<vector<uint8_t>> messageFromMobileApp;
vector<uint8_t> messageFromLoRaModule;

bool isSendMode = true;

BLECharacteristic *txChar;
volatile bool connected = false;

class ServerCb : public BLEServerCallbacks
{
  void onConnect(BLEServer *server) override
  {
    connected = true;
    Serial.println("BLE client connected");
  }

  void onDisconnect(BLEServer *server) override
  {
    connected = false;
    Serial.println("BLE client disconnected");
    delay(500);
    BLEDevice::startAdvertising();
    Serial.println("Advertising restarted");
  }
};

bool receivingSnapshot = false;

bool isValidIdPrefix(const String &id)
{
  if (id.length() != 16)
    return false;

  for (int i = 0; i < id.length(); i++)
  {
    const char ch = id[i];
    const bool isHex =
        (ch >= '0' && ch <= '9') ||
        (ch >= 'a' && ch <= 'f') ||
        (ch >= 'A' && ch <= 'F');

    if (!isHex)
      return false;
  }
  return true;
}

bool hasReceiverId(const uint8_t *id8)
{
  String hex = "";
  for (int i = 0; i < 8; i++)
  {
    if (id8[i] < 16)
    {
      hex += "0";

      hex += String(id8[i], HEX);
    }
  }

  for (const String &id : activeUserIds)
  {
    if (id.equalsIgnoreCase(hex))
      return true;
  }

  return false;
}

class RxCb : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *c) override
  {
    uint8_t *data = c->getData();
    size_t length = c->getLength();

    if (length == 0)
      return;

    // 0x01 message packet
    if (data[0] == 0x01)
    {
      if (length < 25)
      {
        Serial.println("LoRa packet too short");
        return;
      }

      vector<uint8_t> singlePacket(data, data + length);
      messageFromMobileApp.push_back(singlePacket);
      Serial.printf("LoRa message packet received: %u bytes\n", (unsigned)length);
      return;
    }

    const String value = c->getValue();

    if (value.length() == 0)
      return;

    // Initial snapshot begins: discard any incomplete staging list.
    if (value.length() == 1 && (uint8_t)value[0] == 0xF0)
    {
      stagedUserIds.clear();
      receivingSnapshot = true;
      Serial.println("User ID snapshot started");
      return;
    }

    // Initial snapshot ends: replace the active list atomically.
    if (value.length() == 1 && (uint8_t)value[0] == 0xF1)
    {
      if (!receivingSnapshot)
        return;

      activeUserIds = stagedUserIds;
      stagedUserIds.clear();
      receivingSnapshot = false;

      Serial.printf("User ID snapshot committed: %u IDs\n",
                    (unsigned)activeUserIds.size());
      return;
    }

    // During the initial snapshot, each write is one 16-character ID prefix.
    if (receivingSnapshot)
    {
      const String idPrefix(value.c_str());
      if (!isValidIdPrefix(idPrefix))
      {
        Serial.println("Invalid ID prefix in snapshot");
        return;
      }

      bool alreadyPresent = false;
      for (const String &id : stagedUserIds)
      {
        if (id == idPrefix)
        {
          alreadyPresent = true;
          break;
        }
      }
      if (!alreadyPresent)
        stagedUserIds.push_back(idPrefix);
      return;
    }

    const uint8_t operation = (uint8_t)value[0];
    const String idPrefix(value.substring(1).c_str());

    if (!isValidIdPrefix(idPrefix))
      return;

    if (operation == 0xA1)
    {
      for (const String &id : activeUserIds)
      {
        if (id == idPrefix)
        {
          Serial.printf("ID already in list: %s\n", idPrefix.c_str());
          return;
        }
      }
      activeUserIds.push_back(idPrefix);
      Serial.printf("ID added: %s\n", idPrefix.c_str());
    }

    else if (operation == 0xA2)
    {
      for (auto it = activeUserIds.begin(); it != activeUserIds.end(); ++it)
      {
        if (*it == idPrefix)
        {
          activeUserIds.erase(it);
          Serial.printf("ID removed: %s\n", idPrefix.c_str());
          return;
        }
      }
      Serial.printf("ID to remove was not found: %s\n", idPrefix.c_str());
    }
    else if (operation == 0x01)
    {
      Serial.printf("Message: %s\n", value.c_str());
    }
    else
    {
      Serial.printf("Unknown ID operation: 0x%02X\n", operation);
    }
  }
};

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("HopTalk starting...");

  BLEDevice::init("HopTalk-GW");
  Serial.println("BLE initialized as HopTalk-GW");
  BLEDevice::setMTU(247);

  BLEServer *server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());

  BLEService *svc = server->createService(SERVICE_UUID);

  BLECharacteristic *rx = svc->createCharacteristic(
      RX_UUID,
      BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCb());

  txChar = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  txChar->addDescriptor(new BLE2902());

  svc->start();
  Serial.println("BLE service started");

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.println("Advertising started; waiting for a phone connection");
}

void loop()
{
  if(!messageFromMobileApp.empty()) {
    for(const vector<uint8_t> &packet : messageFromMobileApp) {
      Serial.printf("Sent packet of size: %u bytes\n", (unsigned)packet.size());
        for (uint8_t b : packet) {
            Serial.printf("%02X ", b);
        }
        Serial.println();
    }
  }

  delay(2000);
}
