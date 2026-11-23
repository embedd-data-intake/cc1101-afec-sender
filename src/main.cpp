#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_AHTX0.h>
#include <Adafruit_BMP3XX.h>
#include <ScioSense_ENS16x.h>

#include "cc1101.h"
#include "hal/cc1101_hal_arduino.h"   // swap for your own HAL on other frameworks
#include "afec_node.h"
#include "afec_platform_arduino.h"

#include "config.h"

CC1101HalArduino hal(SPI, PIN_SCK, PIN_MISO, PIN_MOSI, PIN_CSN, 4000000, PIN_GDO0);
CC1101 radio;

Adafruit_BMP3XX bmp;
Adafruit_AHTX0 aht;
ENS160 ens160;

afec::Node node;
static uint32_t counter = 0;
static uint32_t lastTx  = 0;

void setup() {
    Serial.begin(115200);
    delay(500);

    CC1101::Config cfg;
    cfg.freqMHz    = RF_FREQ_MHZ;
    cfg.syncWord   = RF_SYNC_WORD;
    cfg.txPowerDbm = RF_TX_DBM;
    cfg.whitening  = true;
    cfg.hwCrc      = false;

    afec::NodeConfig nc; nc.id = NODE_ID; nc.bitrateBps = 4800;

    if (!radio.begin(hal, cfg)) {
        Serial.println("CC1101 not found - check wiring/power");
        // while (true) delay(1000);
    }
    if (USE_IRQ) {
        if (!radio.enableInterrupt()) Serial.println("IRQ setup failed, falling back to polling");
    }
    Serial.printf("CC1101 ready, VERSION=0x%02X, f=%.3f MHz\n", radio.version(), RF_FREQ_MHZ);


    Wire.begin();
    delay(1000);
    aht.begin();
    bmp.begin_I2C();
    ens160.begin(&Wire, ENS160_ADDR);
    ens160.startStandardMeasure();

    node.begin(radio, nc, afec::arduinoPlatform());
}

void loop() {
    node.poll();

    if (millis() - lastTx >= TX_INTERVAL_MS) {
        lastTx = millis();
        // char msg[32];
        // int len = snprintf(msg, sizeof(msg), "hello #%lu", (unsigned long)counter++);

        ens160.wait();
        sensors_event_t tempEvent, humidityEvent;
        aht.getEvent(&humidityEvent, &tempEvent);
        ens160.writeCompensation(tempEvent.temperature, humidityEvent.relative_humidity);
        ens160.update();

        unsigned long temperature = (unsigned long)(tempEvent.temperature * 100.);
        unsigned long humidity    = (unsigned long)(humidityEvent.relative_humidity * 100.);
        unsigned long pressure    = (unsigned long)(bmp.readPressure() * 100.);
        unsigned long ppm         = (unsigned long)(ens160.getEco2() * 100.);

        char msg[32];
        int len = snprintf(msg, sizeof(msg), "t:%lu;h:%lu;p:%lu;m:%lu;", temperature, humidity, pressure, ppm);

        afec::Result result = node.send(2, (const uint8_t*)msg, len);

        Serial.println(msg);
        switch (result)
        {
        case afec::Result::OK:
            Serial.println("OK");
            break;
        case afec::Result::DUTY_LIMIT:
            Serial.println("DUTY_LIMIT");
            break;
        case afec::Result::NO_ACK:
            Serial.println("NO_ACK");
            break;
        case afec::Result::TOO_LONG:
            Serial.println("TOO_LONG");
            break;
        case afec::Result::RADIO_ERR:
            Serial.println("RADIO_ERR");
            break;
        }
    }
}
