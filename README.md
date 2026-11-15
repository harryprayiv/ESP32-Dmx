[![Build Status](https://travis-ci.org/chrisstaite/ESP32-Dmx.svg?branch=master)](https://travis-ci.org/harryprayiv/ESP32-Dmx)

ESP32-Dmx
=========

DMX Transmit and Receive for ESP32 C3 with RDM responder
(client) and RDM controller support.  It should support all of the serial
interfaces on the devices.

This is the combination of DMXSerial2, DmxReceive and DmxSimple but
converted to use the UART for Tx rather than the bit-banging method
utilised by DmxSimple.

