# homebridge-esp8266-fan

A Homebridge plugin and ESP8266 Arduino sketch to control a 3 speed Itho Daalderop CVE ECO RFT fan using a CC1101 RF board.

I documented this project on https://mcpforlife.com/2018/09/24/using-home-app-to-control-your-ventilation-system/

I forked the plugin but without touching it, it already works perfect. It has a convenient settings panel inside Homebridge to configure the IPaddress of the ESP board.

The plugin utilises  a WebSocket client, the sketch implements one. The Sketch contains the logic to use a CC1101 board. With the CC1101 it sends and receives RF packets which contain commands to the Itho CVE.  

