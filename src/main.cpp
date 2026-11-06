#include <Arduino.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

#define SERVICE_UUID "BE28A6AE-FE7A-4394-8858-F48BD1D62026"
#define RX_UUID "4AF882CE-9582-4515-BEAB-1941400E169B" // phone -> ESP32
#define TX_UUID "718CEFEA-4CE7-4A32-BC46-8C24BC97BD7E" // ESP32 -> phone

BLECharacteristic *txChar;
volatile bool connected = false;

class ServerCb : public BLEServerCallbacks
{
  void onConnect(BLEServer *) override { connected = true; }
  void onDisconnect(BLEServer *) override
  {
    connected = false;
    BLEDevice::startAdvertising(); // allow reconnect
  }
};

class RxCb : public BLECharacteristicCallbacks
{
  void onWrite(BLECharacteristic *c) override
  {
    auto v = c->getValue();
    Serial.printf("From phone: %u bytes\n", (unsigned)v.length());
  }
};

void setup()
{
  Serial.begin(115200);

  BLEDevice::init("HopTalk-GW");
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

  BLEAdvertising *adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

void loop()
{
}
