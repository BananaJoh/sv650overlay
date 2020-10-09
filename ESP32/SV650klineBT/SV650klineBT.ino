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
#define K_STRBUF_SIZE        20
#define K_DATASTR_SIZE      300


//__________Global Variables__________//
BluetoothSerial SerialBT;

uint32_t time_ms           = 0;                                       // Timing variables
uint32_t init_start_ms     = 0;
uint32_t last_frame_end_ms = 0;
uint32_t watchdog_ms       = 0;
uint8_t k_outCntr          = 0;                                       // Counter and buffer variables
uint8_t k_inCntr           = 0;
uint8_t k_inByte           = 0;
uint8_t k_chksm            = 0;
uint8_t k_size             = 0;

enum k_states {                                                       // K-Line state
  INIT,
  START_SESSION,
  ACTIVE
} k_state = INIT;

char k_strbuf[K_STRBUF_SIZE]   = {0};                                 // Buffer for int to ascii conversions
char k_datastr[K_DATASTR_SIZE] = {0};                                 // Buffer for data string to send via Bluetooth

byte K_CMD_START_COM[5] = {                                           // Start sequence
  0x81, 0x12, 0xF1, 0x81, 0x05
};

byte K_CMD_READ_ALL_SENS[7] = {                                       // Request sensor data command
  0x80, 0x12, 0xF1, 0x02, 0x21, 0x08, 0xAE
};


//__________Setup Function__________//
void setup() {
  pinMode(LED_GPIO, OUTPUT);
  pinMode(K_TX_GPIO, OUTPUT);
  digitalWrite(LED_GPIO, LOW);

  // Initialize Bluetooth
  while(!SerialBT.begin("ESP_SV", false)) {
    delay(500);
  }

  // Wait for Bluetooth client
  while(!SerialBT.hasClient()) {
    digitalWrite(LED_GPIO, !digitalRead(LED_GPIO));
    delay(500);
  }
}


//__________Main Loop Function__________//
void loop() {
  int8_t ret;

  if(SerialBT.available()) {                                          // Reset on command
    if(SerialBT.read() == 'R') {
      ESP.restart();
    }
  }

  digitalWrite(LED_GPIO, SerialBT.hasClient());

  switch(k_state) {
    case INIT:
      if(k_init() > 0) {                                              // Perform K-Line fast init
        k_state = START_SESSION;                                      // Go to START_SESSION state on success
      }
      break;

    case START_SESSION:
      ret = k_transmit(K_CMD_START_COM, 5);                           // Send K-Line start sequence
      if(ret > 0) {                                                   // Go to ACTIVE state on success
        k_state = ACTIVE;
      } else if(ret < 0) {                                            // Go back to INIT state on error
        k_state = INIT;
      }
      break;

    case ACTIVE:
      ret = k_transmit(K_CMD_READ_ALL_SENS, 7);                       // Send sensor data request and process answer
      if(ret > 0) {                                                   // Send data via Bluetooth on success
        if(SerialBT.hasClient()) {
          SerialBT.println(k_datastr);
        }
      } else if(ret < 0) {                                            // Go back to START_SESSION state on error (receive timeout)
        k_state = START_SESSION;
      }
      break;

    default:
      k_state = INIT;
  }
}


//__________Transmission Variable Reset Function__________//
void end_of_frame_reset() {
  k_outCntr = 0;
  k_inCntr = 0;
  k_inByte = 0;
  k_chksm = 0;
  k_size = 0;
  watchdog_ms = millis();
  last_frame_end_ms = millis();
}


//__________K-Line Initialization Function__________//
int8_t k_init() {
  while(Serial2.available()) {                                        // Empty Buffer
    Serial2.read();
  }
  Serial2.end();

  time_ms = millis();                                                 // Get current time
  if(init_start_ms == 0) {
    init_start_ms = millis();                                         // Save transmission start time
  }
  if(time_ms < init_start_ms + T_01) {                                // T_01 high (initial delay)
    digitalWrite(K_TX_GPIO, HIGH);
  } else if((time_ms >= init_start_ms + T_01) && (time_ms < init_start_ms + T_01 + T_02)) {                      // T_02 low (25 ms pulse)
    digitalWrite(K_TX_GPIO, LOW);
  } else if((time_ms >= init_start_ms + T_01 + T_02) && (time_ms < init_start_ms + T_01 + T_02 + T_02)) {        // T_02 high (25 ms pulse)
    digitalWrite(K_TX_GPIO, HIGH);
  } else if(time_ms >= init_start_ms + T_01 + T_02 + T_02) {          // Start serial communication
    Serial2.begin(K_SERIAL_BAUD);
    end_of_frame_reset();
    init_start_ms = 0;
    last_frame_end_ms = 0;                                            // Skip post frame delay
    return 1;
  }
  return 0;
}


//__________K-Line Transmit Function__________//
int8_t k_transmit(byte *command, byte bytes) {
  time_ms = millis();
  if((k_outCntr < bytes) && (time_ms >= last_frame_end_ms + T_04)) {  // Wait before sending next frame
    Serial2.write(command[k_outCntr]);                                // Send next byte
    k_outCntr++;
  }
  if(Serial2.available()) {                                           // Receive
    watchdog_ms = millis();                                           // Reset watchdog
    k_inByte = Serial2.read();
    k_inCntr++;

    if(k_inCntr > k_outCntr) {
      if(k_inCntr == k_outCntr + 1) {                                 // (k_inCntr <= k_outCntr) -> ACKs of command being sent
        memset(k_datastr, 0, sizeof(k_datastr));                      // Empty data string buffer before appending first data byte
      } else if(k_inCntr == k_outCntr + 4) {                          // Retrieve message size field
        k_size = k_inByte;
      }

      memset(k_strbuf, 0, sizeof(k_strbuf));                          // Convert data to csv string
      itoa(k_inByte, k_strbuf, 10);
      strcat(k_datastr, k_strbuf);
      strcat(k_datastr, ",");

      if(k_inCntr < k_outCntr + k_size + 5) {                         // Collect data for checksum calculation
        k_chksm = k_chksm + k_inByte;
      } else if(k_inCntr == k_outCntr + k_size + 5) {                 // End of frame -> check if checksum ok
        int8_t ret = 0;                                               // Return 0 (continue) if checksum faulty
        if(k_chksm == k_inByte) {
          ret = 1;                                                    // Return 1 (success) if ok
        }
        end_of_frame_reset();
        return ret;
      }
    }
  } else {                                                            // Watchdog timeout
    time_ms = millis();
    if(time_ms > watchdog_ms + T_03) {
      return -1;                                                      // Return -1 on receive timeout
    }
  }
  return 0;                                                           // Return 0 (continue) if not done yet
}


// Other K-Line commands:
// 80 12 F1 06 A5 05 20 00 70 00 C3 -> Reset learned ISC values
