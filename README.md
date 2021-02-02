ESP32 Sampling Audio using I2S ADC and piping it to a remote PC via UDP
========================
The demo samples audio using I2S. The sampled buffer is then transmitted to a PC via UDP socket. The audio is then played using aplay utility.

### Hardware Required
This example is able to run on any commonly available ESP32 development board. The ADC input should be connected to ADC1 Channel 0 pin. 

### Wireless Configuration
Setup your wireless SSID and password in sdkconfig file, via menuconfig, or use esp_wifi_set_config() API.

### PC Configuration
Make sure alsa is installed. If not, please install alsa package first:
`sudo apt-get install alsa-utils`
Use netcat (nc) to open a UDP port. Then pipe the raw values to aplay as shown below:
`nc -ul 7777 | aplay -r 16000 -f S16_BE`


