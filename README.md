[![Build Status](https://travis-ci.org/chrisstaite/ESP32-Dmx.svg?branch=master)](https://travis-ci.org/harryprayiv/ESP32-Dmx)

ESP32-Dmx
=========

DMX Transmit and Receive for ESP32 C3 with RDM responder
(client) and RDM controller support.  It should support all of the serial
interfaces on the devices.

This is the combination of DMXSerial2, DmxReceive and DmxSimple but
converted to use the UART for Tx rather than the bit-banging method
utilised by DmxSimple.

Limited testing has been done with the hardware people have lying around
on Serial1, on Teensy 3.2, 3.5 and LC on Arduino IDE 1.8.2.

|               | Teensy-LC        | Teensy 3.0| Teensy 3.1| Teensy 3.2       | Teensy 3.5       | Teensy 3.6|
|---------------|------------------|-----------|-----------|------------------|------------------|-----------|
| DMX Rx        |:heavy_check_mark:|:question: |:question: |:heavy_check_mark:|:heavy_check_mark:|:question: |
| DMX Tx        |:heavy_check_mark:|:question: |:question: |:heavy_check_mark:|:heavy_check_mark:|:question: |
| RDM Responder |:heavy_check_mark:|:question: |:question: |:heavy_check_mark:|:heavy_check_mark:|:question: |
| RDM Controller|:question:        |:question: |:question: |:heavy_check_mark:|:question:        |:question: |
