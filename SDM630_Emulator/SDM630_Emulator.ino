//#include <WiFi.h> //if using ESP32
#include <ESP8266WiFi.h>  //is using ESP8266
#include <PubSubClient.h>
#include <Wire.h>
#include <SoftwareSerial.h>

SoftwareSerial SoftSerial(4, 5);
WiFiClient espClient;
PubSubClient client(espClient);

/******************USER DEFINITIONS***************************/
#define DEBUG 0                                       //Set to 1 if you want debug prints :)
#define DEBUG_RS485 1

const char* ssid = "SSID";                    //WiFi SSID/Password combination
const char* password = "PASSWORD";

// Add your MQTT Broker IP address:
const char* mqtt_server = "IP-OF-YOUR-BROKER";
const char* mtqq_user = "MQTT-USERNAME";
const char* mtqq_password = "MQTT-PASSWORD";
const char* mtqq_device = "SDM630Emulator";
const char* mtqq_topic = "powersensor/sensor/#";        //the sensor to subscribe to (the # enables all sub-topics)
/******************END USER DEFINITIONS***********************/

uint8_t registers[160]; //this will hold the data up to and inclusive parameter 40 as per SDM630 manual. (4-bytes per parameter)

uint16_t calc_crc(uint8_t* data, uint8_t length) {
  uint16_t crc = 0xFFFF;
  for (int pos = 0; pos < length; pos++) {
    crc ^= data[pos];
    for (int i = 0; i < 8; i++) {
      if ((crc & 1) != 0) {
        crc >>= 1;
        crc ^= 0xA001;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc;
}

void handleInverter() 
{
  if (SoftSerial.available()>7)                                      //handle incomming requests
  {
    static uint8_t recbuffer[8]={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};

    unsigned char firstByte = SoftSerial.read();

    if(firstByte==0x01)
    {
      recbuffer[0]=0x01;
      for(int i=1;i<8;i++)
      {
        recbuffer[i] = SoftSerial.read();
      }
   
      if(DEBUG_RS485)
      {
        Serial.print(millis());
        Serial.print(" ");
        for (int i = 0; i < 8; i++) 
        {
          Serial.print(" ");
          if(recbuffer[i]<16)
            Serial.print("0");
          Serial.print(recbuffer[i], HEX);
        }
        Serial.println();
      }
  
      if (calc_crc(recbuffer, 8) != 0)                              //if CRC doesn't match, return
      {
        Serial.println("malformed packet");
        return;
      }
    }
    else
    {
      static uint8_t recbuffer2[13]={0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00};
      recbuffer2[0]=firstByte;
      while(SoftSerial.available()<12) {}
      for(int i=1;i<13;i++)
      {
        recbuffer2[i] = SoftSerial.read();
      }
   
      if(DEBUG_RS485)
      {
        Serial.print(millis());
        Serial.print("This weird one ");
        for (int i = 0; i < 13; i++) 
        {
          Serial.print(" ");
          if(recbuffer2[i]<16)
            Serial.print("0");
          Serial.print(recbuffer2[i], HEX);
        }
        Serial.println();
      }
      return;
    }
        
    uint8_t slaveadr = recbuffer[0];
    uint8_t functioncode = recbuffer[1];
    uint16_t address = (recbuffer[2] << 8) | recbuffer[3];
    uint16_t numRegisters = (recbuffer[4] << 8) | recbuffer[5];
 
    if(DEBUG_RS485)
    {
      Serial.print(" process register read ");
      Serial.print(slaveadr);
      Serial.print(" ");
      Serial.print(functioncode);
      Serial.print(" ");
      Serial.print(address);
      Serial.print(" ");
      Serial.println(numRegisters);
    }
    
    //build up the array for returning values
    uint8_t response[(numRegisters*2)+3+2]; //make space for the header and CRC - the registers are 16-bit values so they take up double space!
    for(int i=0;i<sizeof(response);i++)
      response[i]=0;

    //build up the header
    response[0]=0x01;                                               //set slave address
    response[1]=0x04;                                               //response type
    response[2]=numRegisters*2;                                       //remember the registers are 16-bit values

    //fill in the response from the local registers
    uint16_t responseP=0;
    for(int i=(address*2);i<((address*2)+(numRegisters*2));i++)                       //start at the address requested (multiply by 2 to get 16-bit values)
    {
      response[responseP+3]=registers[i];                           //copy to the response array - dont overwrite the header!
      responseP++;
    }

    uint16_t crc = calc_crc(response, (sizeof(response)-2));          //calculate CRC (don't calculate on the empty CRC fields)
    
    response[sizeof(response)-2] = crc & 0xFF;                      //insert CRC
    response[sizeof(response)-1] = (crc >> 8) & 0xFF;  
    
    if(DEBUG_RS485)
    {
      Serial.print("response");
      for (int i = 0; i < sizeof(response); i++) 
      {
        Serial.print(" ");
        if(response[i]<16)
          Serial.print("0");
        Serial.print(response[i], HEX);
      }
      Serial.println();
    }
    
    SoftSerial.write(response, sizeof(response));                   //Write the response!
    memset(recbuffer, 0, sizeof(recbuffer));                        //reset buffer for incoming message
  }
}

void setup() 
{
  Serial.begin(115200);
  setup_wifi();
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
  SoftSerial.begin(9600, SWSERIAL_8N1);
}

void setup_wifi() 
{
  if(DEBUG)
  {
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);
  }

  WiFi.begin(ssid, password);               // We start by connecting to a WiFi network

  while (WiFi.status() != WL_CONNECTED)     //waiting to connect...
  {
    delay(500);
    Serial.print(".");
  }
  if(DEBUG)
  {
    Serial.println("");
    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());
  }
}

union converter                             //Some C magic for converting a float into a byte-array ;)
{
  float val;
  unsigned char b[4];
}x;

void insertIntoRegisters(uint8_t startAddress)  
{
  registers[startAddress]   = x.b[3]; //and do a endianness conversion at the same time
  registers[startAddress+1] = x.b[2];
  registers[startAddress+2] = x.b[1];
  registers[startAddress+3] = x.b[0];
}

void callback(char* topic, byte* message, unsigned int length) 
{
  String messageTemp;                  
  for (int i = 0; i < length; i++) 
    messageTemp += (char)message[i];
  x.val=messageTemp.toFloat();  //save in union

  if(DEBUG)
  { 
    Serial.print("Message arrived on topic: ");
    Serial.print(topic);
    Serial.print(". Message: ");
    Serial.println(messageTemp.toFloat());
  }
  
  String topicc(topic);         //create a string for searching
  //the magic sauce of mapping the values to the SDM630 registers - NOTE: The MQTT topics are defined in the transmitter!
  
  if(topicc.indexOf("r_voltage/") > 0) insertIntoRegisters(0);
  if(topicc.indexOf("s_voltage/") > 0) insertIntoRegisters(4);
  if(topicc.indexOf("t_voltage/") > 0) insertIntoRegisters(8);
  
  if(topicc.indexOf("r_current/") > 0) insertIntoRegisters(12);
  if(topicc.indexOf("s_current/") > 0) insertIntoRegisters(16);
  if(topicc.indexOf("t_current/") > 0) insertIntoRegisters(20);

  if(topicc.indexOf("r_power/") > 0) insertIntoRegisters(24);
  if(topicc.indexOf("s_power/") > 0) insertIntoRegisters(28);
  if(topicc.indexOf("t_power/") > 0) insertIntoRegisters(32);

  if(topicc.indexOf("r_apparent_power/") > 0) insertIntoRegisters(36);
  if(topicc.indexOf("s_apparent_power/") > 0) insertIntoRegisters(40);
  if(topicc.indexOf("t_apparent_power/") > 0) insertIntoRegisters(44);

  if(topicc.indexOf("r_reactive_power/") > 0) insertIntoRegisters(48);
  if(topicc.indexOf("s_reactive_power/") > 0) insertIntoRegisters(52);
  if(topicc.indexOf("t_reactive_power/") > 0) insertIntoRegisters(56);

  if(topicc.indexOf("r_power_factor/") > 0) insertIntoRegisters(60);
  if(topicc.indexOf("s_power_factor/") > 0) insertIntoRegisters(64);
  if(topicc.indexOf("t_power_factor/") > 0) insertIntoRegisters(68);

  if(topicc.indexOf("r_phase_angle/") > 0) insertIntoRegisters(72);
  if(topicc.indexOf("s_phase_angle/") > 0) insertIntoRegisters(76);
  if(topicc.indexOf("t_phase_angle/") > 0) insertIntoRegisters(80);

  if(topicc.indexOf("total_power/") > 0)             insertIntoRegisters(104);
  if(topicc.indexOf("frequency/") > 0)               insertIntoRegisters(140);

  if(topicc.indexOf("import_active_energy/") > 0)    insertIntoRegisters(144);
  if(topicc.indexOf("export_active_energy/") > 0)    insertIntoRegisters(148);
  if(topicc.indexOf("import_reactive_energy/") > 0)  insertIntoRegisters(152);
  if(topicc.indexOf("export_reactive_energy/") > 0)  insertIntoRegisters(156);
}

void loop() 
{
  if (!client.connected()) 
  {
    while (!client.connected())                                         // Loop until we're reconnected
    {
      Serial.print("Attempting MQTT connection...");
      if (client.connect(mtqq_device,mtqq_user,mtqq_password))          // Attempt to connect
      {
        Serial.println("connected");
        client.subscribe(mtqq_topic);                                   // Subscribe
      } 
      else 
      {
        Serial.print("failed, rc=");
        Serial.print(client.state());
        Serial.println(" try again in 5 seconds");     
        delay(5000);                                                    // Wait 5 seconds before retrying
      }
    }
  }
  client.loop();

  handleInverter();                                                     // handle communication with the inverter
}
