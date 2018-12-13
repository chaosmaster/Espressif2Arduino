#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>

#include "rboot-api.h"

#define MAGIC_V1 0xE9
#define MAGIC_V2 0xEA
#define UPGRADE_FLAG_START 0x01
#define UPGRADE_FLAG_FINISH 0x02
#define SECTOR_SIZE 4096
#define BUFFER_SIZE 1024
#define TIMEOUT 5000

byte buffer[BUFFER_SIZE] __attribute__((aligned(4))) = {0};
byte bootrom[SECTOR_SIZE] __attribute__((aligned(4))) = {0};
byte _blink = 0;

enum FlashMode
{
  MODE_UNKNOWN,
  MODE_FLASH_ROM1,
  MODE_FLASH_ROM2
};


//USER EDITABLE VARABLES HERE
#define DELAY 100        //ms to blink/delay for
#define STATUS_GPIO 13  //gpio to toggle as status indicator
#define RETRY 3         //number of times to retry

#define URL_QIO_ROM_2 "http://example.org/e2t.rom1" //Change this to point to where you are serving the files
#define URL_QIO_ROM_3 "http://thehackbox.org/tasmota/sonoff-basic.bin" //Can be any ESP-image that will be flashed to 0x00000 (including bootloader) should be < 500K

#define URL_DIO_ROM_2 URL_QIO_ROM_2
#define URL_DIO_ROM_3 URL_QIO_ROM_3

//Uncomment to provide fixed credentials - otherwise will try to use credentials saved by sonoff device
//#define WIFI_SSID "SSID"
//#define WIFI_PASSWORD "Password"

void setup()
{
  Serial.begin(115200);
  Serial.print("\nInitalizing...");
  if(STATUS_GPIO)
  {
    pinMode(STATUS_GPIO, OUTPUT);
  }

  //blink our status LED while we wait for serial to come up
  for(int i=0;i<100;i++)
  {
    blink(); 
    delay(DELAY);
  }
  digitalWrite(STATUS_GPIO, HIGH);
  Serial.println("Done");

  uint8_t upgrade = determineUpgradeMode();
  if(upgrade == MODE_FLASH_ROM1 || MODE_FLASH_ROM2)
  {
      connectToWiFiBlocking(upgrade);
      digitalWrite(STATUS_GPIO, LOW);
  }
  
  Serial.printf("Flash Mode: ");
  FlashMode_t mode = ESP.getFlashChipMode();
  Serial.printf("%d\n", mode);
  
  if (upgrade == MODE_FLASH_ROM1)
    flashRom1(mode);
  else if(upgrade == MODE_FLASH_ROM2)
    flashRom2(mode);
  else
    Serial.println("Flash Mode not recognized");
}

uint8_t determineUpgradeMode()
{
  Serial.printf("Current rom: ");
  const rboot_config bootconf = rboot_get_config();
  uint8_t rom = bootconf.current_rom + 1;

  Serial.printf("%d\n", rom);

  Serial.printf("Rom 1 magic byte: ");
  uint32_t rom_1_start_address = 0x002000;
  byte magic = 0;
  ESP.flashRead(rom_1_start_address, (uint32_t*)&magic, 1);
  Serial.printf("0x%02X\n", magic);

  uint8_t mode = MODE_UNKNOWN;
  if (rom == 1 && magic == MAGIC_V2)
    mode = MODE_FLASH_ROM2;
  else if (rom == 2 && magic == MAGIC_V2)
    mode = MODE_FLASH_ROM1;
  
  Serial.printf("Reflashing rom: %d\n", mode);
  return mode;
}

void connectToWiFiBlocking(uint8_t mode)
{
  
  Serial.printf("Connecting to Wifi...");
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
  WiFi.mode(WIFI_STA);

#ifdef WIFI_SSID
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
#endif
#ifndef WIFI_SSID
  WiFi.begin();
#endif
  while (WiFi.status() != WL_CONNECTED)
  {
    blink();
    delay(DELAY*2);
  }

  Serial.printf("\tSSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("\tPassword: %s\n", WiFi.psk().c_str());
  //WiFi.persistent(true);
  Serial.print("Done\n");
  IPAddress ip = WiFi.localIP();
  Serial.printf("\t%d.%d.%d.%d\n", ip[0], ip[1], ip[2], ip[3]);
}

void flashRom1(FlashMode_t mode)
{
  bool result = downloadRomToFlash(
    1,                //Rom 1
    true,             //Bootloader is being updated
    0xE9,             //Standard Arduino Magic
    0x000000,         //Write to 0x0 since we are replacing the bootloader
    0x100000,         //Stop before end of ram
    0,                //Erase Sector from 0 to
    130,              //Sector 130 (not inclusive)
    (mode == FM_DIO) ? URL_DIO_ROM_3 : URL_QIO_ROM_3,
    RETRY             //Retry Count
  );

  ESP.restart(); //restart regardless of success
}

//Download special rom.
void flashRom2(FlashMode_t mode)
{
  //system_upgrade_flag_set(UPGRADE_FLAG_START);
  bool result = downloadRomToFlash(
    2,                //Rom 2
    false,            //Bootloader is not being updated
    0xEA,             //V2 Espressif Magic
    0x082000,         //Not replacing bootloader
    0x100000,         //Stop before end of ram
    128,              //From middle of flash
    256,              //End of flash
    (mode == FM_DIO) ? URL_DIO_ROM_2 : URL_QIO_ROM_2,
    RETRY             //Retry Count
  );
  
  if(result)
  {
    rboot_set_current_rom(1);
    ESP.restart();
  }
  else
  {
    ESP.restart();
  }
}

//Assumes bootloader must be in first SECTOR_SIZE bytes.
bool downloadRomToFlash(byte rom, byte bootloader, byte magic, uint32_t start_address, uint32_t end_address, uint16_t erase_sectior_start, uint16_t erase_sector_end, const char * url, uint8_t retry_limit)
{
  uint8_t retry_counter = 0;
  while(retry_counter < retry_limit)
  {
    uint16_t erase_start = erase_sectior_start;
    uint32_t write_address = start_address;
    uint8_t header[4] = { 0 };
    bootrom[SECTOR_SIZE] = { 0 };
    buffer[BUFFER_SIZE] = { 0 };

    Serial.printf("Flashing rom %d (retry:%d): %s\n", rom, retry_counter, url);
    HTTPClient http;
    http.begin(url);
    http.useHTTP10(true);
    http.setTimeout(TIMEOUT);

    //Response Code Check
    uint16_t httpCode = http.GET();
    Serial.printf("HTTP response Code: %d\n", httpCode);
    if(httpCode != HTTP_CODE_OK)
    {
      Serial.println("Invalid response Code - retry");
      retry_counter++;
      continue;
    }

    //Length Check (at least one sector)
    uint32_t len = http.getSize();
    Serial.printf("HTTP response length: %d\n", len);
    if(len < SECTOR_SIZE)
    {
      Serial.println("Length too short - retry");
      retry_counter++;
      continue;
    }

    if(len > (end_address-start_address))
    {
      Serial.println("Length exceeds flash size - retry");
      retry_counter++;
      continue;
    }

    //Confirm magic byte
    WiFiClient* stream = http.getStreamPtr();
    stream->peekBytes(&header[0],4);
    Serial.printf("Magic byte from stream: 0x%02X\n", header[0]);
    if(header[0] != magic)
    {
      Serial.println("Invalid magic byte - retry");
      retry_counter++;
      continue;
    }

    if(bootloader)
    { 
      Serial.printf("Downloading %d byte bootloader", sizeof(bootrom));
      size_t size = stream->available();
      while(size < sizeof(bootrom))
      {
        blink();
        size = stream->available();
      }
      int c = stream->readBytes(bootrom, sizeof(bootrom));

      //Skip the bootloader section for the moment..
      erase_start++;
      write_address += SECTOR_SIZE;
      len -= SECTOR_SIZE;
      Serial.printf(".Done\n");
    }

    Serial.printf("Erasing flash sectors %d-%d", erase_start, erase_sector_end);
    for (uint16_t i = erase_start; i < erase_sector_end; i++)
    {
      ESP.flashEraseSector(i);
      blink();
    }  
    Serial.printf("Done\n");
    
    Serial.printf("Downloading rom to 0x%06X-0x%06X in %d byte blocks", write_address, write_address+len, sizeof(buffer));
    //Serial.println();
    while(len > 0)
    {
      size_t size = stream->available();
      if(size >= sizeof(buffer) || size == len) 
      {
        memset(buffer, 0xFF, sizeof(buffer));
        int c = stream->readBytes(buffer, ((size > sizeof(buffer)) ? sizeof(buffer) : size));
        //Serial.printf("address=0x%06X, bytes=%d, len=%d\n", write_address, c, len);
        ESP.flashWrite(write_address, (uint32_t*)&buffer[0], c);
        write_address +=c; //increment next write address
        len -= c; //decremeant remaining bytes
      }
      blink();
      //delay(100);
    }
    http.end();
    Serial.println("Done");

    if(bootloader)
    {
      uint16_t max_sector = ESP.getFlashChipRealSize() / SECTOR_SIZE;
      
      Serial.printf("Erasing bootloader sector 0");
      ESP.flashEraseSector(0);
      Serial.printf("..Done\n");
      
      Serial.printf("Writing bootloader to 0x%06X-0x%06X", 0, SECTOR_SIZE);
      ESP.flashWrite(0, (uint32_t*)&bootrom[0], SECTOR_SIZE);
      Serial.printf("..Done\n");

      uint32_t sketch_end = 0x82000 + ESP.getSketchSize();
      uint16_t sketch_sector = sketch_end / SECTOR_SIZE;
      Serial.printf("Erasing flash sectors %d-%d", sketch_sector, max_sector);
      for (uint16_t i = sketch_sector; i < max_sector; i++)
      {
        ESP.flashEraseSector(i);
        blink();
      }  

      Serial.printf("Done\n");

    }


    return true;
  }
  Serial.println("Retry counter exceeded - giving up");
  return false;
}


void blink()
{
  if(STATUS_GPIO)
  {
     if(_blink)
      digitalWrite(STATUS_GPIO, LOW);
    else
      digitalWrite(STATUS_GPIO, HIGH);
      
      _blink ^= 0xFF;
  }
  Serial.print(".");
  yield(); // reset watchdog
}

void loop()
{
  //delay(100);
}
