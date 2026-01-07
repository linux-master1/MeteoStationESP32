# MeteoStationESP32

## description
autonomous clock-weather-station based on esp-32-wroom and arduino ide.  
displays current time, weather forecast and air quality data.  

time is synchronized via ntp and shown on led matrix MAX7219.  
weather forecast for three days is displayed on oled screen.  
temperature, humidity and air quality index are measured by bme680 and shown with current date.  

wifi is configured automatically via access point and web interface.  
network settings can be reset using a physical button.

## pinout

### i2c bus (oled displays + bme680)
- sda -> gpio 21  
- scl -> gpio 22  

**i2c addresses**
- oled weather display: 0x3c  
- oled date and sensor display: 0x3d  
- bme680 sensor: 0x77  

### led matrix max7219 (spi)
- vcc -> vin (5v)  
- gnd -> gnd  
- din -> gpio 23  
- clk -> gpio 18  
- cs  -> gpio 5  

### potentiometer (brightness control)
- left pin  -> gnd  
- right pin -> 3.3v  
- middle pin -> gpio 32  

### wifi reset button
- one pin -> gpio 4  
- second pin -> gnd

# WARNING!

- THE WEATHER STATION OPERATES IN UTC+3 TIME ZONE
  
- FIND THE LONG LINE IN THE CODE CONTAINING "ВАШАШИРИНА" AND "ВАШАДОЛГОТА" AND REPLACE THEM WITH YOUR OWN GPS COORDINATES IN 00.00 FORMAT FOR WEATHER DATA

- WIFI ACCESS POINT PASSWORD: 12345678
