//__________Includes__________//
#include "BluetoothSerial.h"


//__________Defines__________//
#define LED_GPIO              2        // LED GPIO pin

#define K_SERIAL_BAUD     10400        // K-Line baudrate
#define K_TX_GPIO            17        // K-Line Tx GPIO pin
#define T_01               6000        // Initial delay ms
#define T_02                 25        // Fast init pulse time ms
#define T_03                500        // Watchdog timeout ms
#define T_04                200        // Post frame delay ms
#define K_STR_BUFFER_SIZE    20
#define K_BUFFER_SIZE       300


//__________Global Variables__________//
BluetoothSerial SerialBT;

uint32_t time_ms = 0;                                            // Timing variables
uint32_t init_start_ms = 0;
uint32_t last_frame_end_ms = 0;
uint32_t watchdog_ms = 0;
uint8_t k_outCntr = 0;                                           // Counter and buffer variables
uint8_t k_inCntr = 0;
uint8_t k_inByte = 0;
uint8_t k_chksm = 0;
uint8_t k_size = 0;
uint8_t k_mode = 0;
boolean k_initialized = false;                                   // K-Line initialized flag
char k_str[K_STR_BUFFER_SIZE] = {0};
char k_buffer[K_BUFFER_SIZE] = {0};

byte K_START_COM[5] = {                                          // Start sequence
  0x81, 0x12, 0xF1, 0x81, 0x05
};

byte K_READ_ALL_SENS[7] = {                                      // Request sensor data command
  0x80, 0x12, 0xF1, 0x02, 0x21, 0x08, 0xAE
};

uint16_t rpm_tmp = 0;
uint16_t iap = 0;


//__________Setup Function__________//
void setup() {
  pinMode(LED_GPIO, OUTPUT);
  pinMode(K_TX_GPIO, OUTPUT);

  while(!SerialBT.begin("ESP_SV", false)) {                      // Bluetooth init error
    digitalWrite(LED_GPIO, HIGH);
    delay(1000);
  }
  digitalWrite(LED_GPIO, LOW);
  delay(5000);                                                   // Wait some seconds for ECU to start up
  while(!SerialBT.hasClient()) {                                 // Wait for Bluetooth client
    digitalWrite(LED_GPIO, !digitalRead(LED_GPIO));
    delay(500);
  }
  digitalWrite(LED_GPIO, HIGH);
}


//__________Main Loop Function__________//
void loop() {
  if(k_mode == 0) {
    if(k_transmit(K_START_COM, 5)) {                             // Start sequence for K-Line fast init
      k_mode++;
    }
  } else if(k_mode == 1) {
    if(k_transmit(K_READ_ALL_SENS, 7)) {                         // Send sensor data request and process answer
      if(SerialBT.hasClient()) {
        SerialBT.println(k_buffer);
      }
    }
  }
}


//__________K-Line Transmit Function__________//
boolean k_transmit(byte *function, byte num) {
  if(!k_initialized) {
    while(Serial2.available()) {                                 // Empty Buffer
      Serial2.read();
    }
    Serial2.end();

    time_ms = millis();                                          // Get current time
    if(init_start_ms == 0) {
      init_start_ms = millis();                                  // Save transmission start time
    }
    if(time_ms < init_start_ms + T_01) {                         // T_01 high (initial delay)
      digitalWrite(K_TX_GPIO, HIGH);
    } else if((time_ms >= init_start_ms + T_01) && (time_ms < init_start_ms + T_01 + T_02)) {                      // T_02 low (25 ms pulse)
      digitalWrite(K_TX_GPIO, LOW);
    } else if((time_ms >= init_start_ms + T_01 + T_02) && (time_ms < init_start_ms + T_01 + T_02 + T_02)) {        // T_02 high (25 ms pulse)
      digitalWrite(K_TX_GPIO, HIGH);
    } else if(time_ms >= init_start_ms + T_01 + T_02 + T_02) {   // Start serial communication
      Serial2.begin(K_SERIAL_BAUD);
      memset(k_buffer, 0, sizeof(k_buffer));
      k_mode = 0;
      k_outCntr = 0;
      k_inCntr = 0;
      k_inByte = 0;
      k_chksm = 0;
      k_size = 0;
      init_start_ms = 0;
      watchdog_ms = millis();
      last_frame_end_ms = 0;
      k_initialized = true;
    }
  } else {                                                       // k_initialized
    time_ms = millis();
    if((k_outCntr < num) && (time_ms >= last_frame_end_ms + T_04)) {  // Wait before sending next frame
      Serial2.write(function[k_outCntr]);                        // Send
      k_outCntr++;
    }
    if(Serial2.available()) {                                    // Receive
      watchdog_ms = millis();                                    // Reset watchdog
      k_inByte = Serial2.read();
      k_inCntr++;

      if(k_inCntr < k_outCntr) {                                 // Command is being sent
        memset(k_buffer, 0, sizeof(k_buffer));
      }

      memset(k_str, 0, sizeof(k_str));
      itoa(k_inByte, k_str, 10);
      strcat(k_buffer, k_str);
      strcat(k_buffer, ",");

      if(k_inCntr == k_outCntr + 4) {                            // Retrieve message size
        k_size = k_inByte;
      }
      if((k_inCntr > k_outCntr) && (k_inCntr < k_size + 5 + k_outCntr)) {
        k_chksm = k_chksm + k_inByte;
      }
      if(k_inCntr == k_size + 5 + k_outCntr) {                   // Return true and reset if checksum ok
        if(k_chksm == k_inByte) {
          k_outCntr = 0;
          k_inCntr = 0;
          k_inByte = 0;
          k_chksm = 0;
          k_size = 0;
          watchdog_ms = millis();                                // Reset watchdog
          last_frame_end_ms = millis();                          // Wait before sending again
          return true;
        }
      }
    } else {                                                     // Watchdog timeout
      time_ms = millis();
      if(time_ms > watchdog_ms + T_03) {
        k_initialized = false;
      }
    }
  }
  return false;
}


// Other K-Line commands:
// 80 12 F1 06 A5 05 20 00 70 00 C3 -> Reset learned ISC values
