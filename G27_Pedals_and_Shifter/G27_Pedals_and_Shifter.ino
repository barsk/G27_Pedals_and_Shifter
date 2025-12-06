// G27_Pedals_and_Shifter.ino
// by Jason Duncan

// Partially adapted from the work done by isrtv.com forums members pascalh and xxValiumxx:
// http://www.isrtv.com/forums/topic/13189-diy-g25-shifter-interface-with-h-pattern-sequential-and-handbrake-modes/

#include <HID.h>
#include <EEPROM.h>
#include <Joystick.h>
#include "./src/Filter.h"

// LED PINS
#define RED_PIN    3
#define GREEN_PIN  5
#define BLUE_PIN   6

// PEDAL PINS - please note the comments of @eriknz in 
// https://github.com/n-e-y-s/G27_Pedals_and_Shifter/issues/8
// This is the wiring suggested by eriknz, which does not connect the pedal ground 
// to +5V (pedals need to be inverted in the calibration gui):
//
//| DB9 | Original | Harness | Description | Pro Micro   |
//|   1 | Black    | Black   | GND         | GND         |
//|   2 | Orange   | Yellow  | Throttle    | pin 18 (A0) |
//|   3 | White    | White   | Brake       | pin 19 (A1) |
//|   4 | Green    | Green   | Clutch      | pin 20 (A2) |
//|   5 |          |         |             |             |
//|   6 | connected in pedals plug to pin 9
//|   7 |          |         |             |             |
//|   8 |          |         |             |             |
//|   9 | Red      | Red     | +5V         | +5V         |
//
// This is the original wiring from @functionreturnfunction
//
//| DB9 | Original | Harness | Description | Pro Micro   |
//|   1 | Black    | Red     | +5v         | +5v         |
//|   2 | Orange   | Yellow  | Throttle    | pin 18 (A0) |
//|   3 | White    | White   | Brake       | pin 19 (A1) |
//|   4 | Green    | Green   | Clutch      | pin 20 (A2) |
//|   5 |          |         |             |             |
//|   6 | Red      | Black   | GND         | GND         |
//|   7 |          |         |             |             |
//|   8 |          |         |             |             |
//|   9 | Red      | Black   | GND         | GND         |
#define GAS_PIN    18
#define BRAKE_PIN  19
#define CLUTCH_PIN 20

// SHIFTER PINS
//| DB9 | Original | Harness | Shifter | Description             | Pro Micro   |
//|   1 | Purple   | Purple  |       1 | Button Clock            | pin 0       |
//|   2 | Grey     | Blue    |       7 | Button Data             | pin 1       |
//|   3 | Yellow   | Yellow  |       5 | Button !CS & !PL (Mode) | pin 4       |
//|   4 | Orange   | Orange  |       3 | Shifter X axis          | pin 8  (A8) |
//|   5 | White    | White   |       2 | SPI input               |             |
//|   6 | Black    | Black   |       8 | GND                     | GND         |
//|   7 | Red      | Red     |       6 | +5V                     | VCC         |
//|   8 | Green    | Green   |       4 | Shifter Y axis          | pin 9 (A9)  |
//|   9 | Red      | Red     |       1 | +5V                     | VCC         |
#define SHIFTER_CLOCK_PIN  0
#define SHIFTER_DATA_PIN   1
#define SHIFTER_MODE_PIN   4
#define SHIFTER_X_PIN      8
#define SHIFTER_Y_PIN      9

// BUTTON DEFINITIONS
#define BUTTON_REVERSE         1

#define BUTTON_RED_CENTERRIGHT 4
#define BUTTON_RED_CENTERLEFT  5
#define BUTTON_RED_RIGHT       6
#define BUTTON_RED_LEFT        7
#define BUTTON_BLACK_TOP       8
#define BUTTON_BLACK_RIGHT     9
#define BUTTON_BLACK_LEFT      10
#define BUTTON_BLACK_BOTTOM    11
#define BUTTON_DPAD_RIGHT      12
#define BUTTON_DPAD_LEFT       13
#define BUTTON_DPAD_BOTTOM     14
#define BUTTON_DPAD_TOP        15

// Gears are mapped to output 0-6

#define OUTPUT_BLACK_TOP       7
#define OUTPUT_BLACK_LEFT      8
#define OUTPUT_BLACK_RIGHT     9
#define OUTPUT_BLACK_BOTTOM    10
#define OUTPUT_DPAD_TOP        11
#define OUTPUT_DPAD_LEFT       12
#define OUTPUT_DPAD_RIGHT      13
#define OUTPUT_DPAD_BOTTOM     14
#define OUTPUT_RED_LEFT        15
#define OUTPUT_RED_CENTERLEFT  16
#define OUTPUT_RED_CENTERRIGHT 17
#define OUTPUT_RED_RIGHT       18

// MISC.
#define MAX_AXIS            1023
#define SIGNAL_SETTLE_DELAY 10

#define CALIB_DATA_MAGIC_NUMBER 0x27CA11B1 // change this when the struct definition changes

#define FLAG1_INVERT_BRAKE 0x1 // invert brake pedal
#define FLAG1_INVERT_GAS 0x2 // invert gas pedal
#define FLAG1_INVERT_CLUTCH 0x4 // invert clutch pedal
#define FLAG1_REVERSE_RIGHT_RED 0x8 // use right red button for reverse gear instead of the reverse sensor
#define FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z 0x10 // combine clutch and gas
#define FLAG1_COMBINE_Z_EQUALS_Z_MINUS_X 0x20 // combine clutch and gas

#define FLAG2_ENABLE_PEDALS 0x1
#define FLAG2_BRAKE_DEAD_ZONE (0x2|0x4|0x8) // deadZone_percent = ((flag & FLAG2_BRAKE_DEAD_ZONE) >> 1)*2
#define FLAG2_GAS_DEAD_ZONE (0x10|0x20|0x40) // deadZone_percent = ((flag & FLAG2_GAS_DEAD_ZONE) >> 4)*2

#define FLAG3_ENABLE_SHIFTER 0x1
#define FLAG3_CLUTCH_DEAD_ZONE (0x10|0x20|0x40) // deadZone_percent = ((flag & FLAG3_CLUTCH_DEAD_ZONE) >> 4)*2

Joystick_ G27(            // define Joystick parameters
  5,  
  JOYSTICK_TYPE_JOYSTICK,      // type Joystick
  19, 1,                       // Button Count, Hat Switch Count
  false, false, true,               // X, Y, Z
  true,true,false,                  // Rx, Ry, Rz
  false,false,false,false,false);   // Throttle, Rudder, Accelerator, Brake, Steering 

typedef struct CalibData
{
  /* magic number, used for testing valid EEPROM content */
  uint32_t calibID;
  /* bool whether to automatically calibrate the pedals at a power cycle or use a static calibration */
  uint8_t pedals_auto_calib; 
  /* flag1, see the FLAG1_* defines for the meaning of the bits */
  uint8_t flag1;
  /* flag2, see the FLAG2_* defines for the meaning of the bits */
  uint8_t flag2;
  /* flag3, see the FLAG3_* defines for the meaning of the bits */
  uint8_t flag3;
  /* size of median filter to filter pedal values */
  uint8_t pedal_median_size;
  /* size of median filter to filter shifter values */
  uint8_t shifter_median_size;
  /* only meaningful if pedals_auto_calib is 0 */
  uint16_t gasMin, gasMax, brakeMin, brakeMax, clutchMin, clutchMax;
  /* defines the neutral zone of the y shifter hysteresis */
  uint16_t shifter_y_neutralMin, shifter_y_neutralMax;
  /* defines the 246R zone of the y shifter hysteresis, should be less than shifter_y_neutralMin */
  uint16_t shifter_y_246R_gearZone;
  /* defines the 135 zone of the y shifter hysteresis, should be greater than shifter_y_neutralMax */
  uint16_t shifter_y_135_gearZone;
  /* threshold for gears 1 and 2 of the x shifter, should be less than shifter_x_56 */
  uint16_t shifter_x_12;
  /* threshold for gears 5,6 and R of the x shifter, should be greater than shifter_x_12 */
  uint16_t shifter_x_56;  
} CalibData;

/* this structure holds the calibration data stored in EEPROM */
typedef struct calibration
{
    /* the actual calibration struct */
    CalibData data;
    /* CRC checksum */
    uint32_t crc;
} Calibration;

static Calibration calibDefault = {
    {
        CALIB_DATA_MAGIC_NUMBER,
        1, /* pedals auto calib */
        0, /* flag1, everything on default */
        1, /* use pedals */
        1, /* use shifter */
        0, /* pedals median size */
        0, /* shifter median size */
        0,0,0,0,0,0, /* calibrated pedal values */
        1024/2 - 1024/10, 1024/2 + 1024/10, /* shifter_y_neutral */
        1024/2 - 1024/3, 1024/2 + 1024/3,   /* shifter_y_gearZone */
        1024/3, 1024*2/3 /* shifter_x */
    },
    0 /* crc will be set when written to EEPROM */
};
static Calibration calibration = calibDefault;

unsigned long crc(uint8_t *buffer, uint16_t length) {

  const unsigned long crc_table[16] = {
    0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac,
    0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
    0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c,
    0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c
  };

  unsigned long crc = ~0L;

  for (uint8_t index = 0 ; index < length ; ++index) {
    crc = crc_table[(crc ^ buffer[index]) & 0x0f] ^ (crc >> 4);
    crc = crc_table[(crc ^ (buffer[index] >> 4)) & 0x0f] ^ (crc >> 4);
    crc = ~crc;
  }
  return crc;
}

static int crcError = 0;
static int magicNumberError = 0;
static int lastCrcFromEEPROM = 0;
static int lastCrcFromContents = 0;

void loadEEPROM()
{
  Calibration fromEEPROM;
  EEPROM.get(0, fromEEPROM);
  
  uint32_t eeprom_crc = crc((uint8_t*)&fromEEPROM.data, sizeof(fromEEPROM.data));
  lastCrcFromEEPROM = fromEEPROM.crc;
  lastCrcFromContents = eeprom_crc;
  if( eeprom_crc == fromEEPROM.crc && fromEEPROM.data.calibID == CALIB_DATA_MAGIC_NUMBER )
  {
    calibration = fromEEPROM;
  } else
  {
    crcError += eeprom_crc != fromEEPROM.crc;
    magicNumberError += fromEEPROM.data.calibID != CALIB_DATA_MAGIC_NUMBER;
    calibration = calibDefault;
  }
}

// PEDAL CODE
typedef struct Pedal {
  byte pin;
  int min, max, cur, axis;
} Pedal;

static Pedal* gasPedal = 0;
static Pedal* brakePedal = 0;
static Pedal* clutchPedal = 0;

int axisValue(struct Pedal* input, int dead_zone) 
{
  int physicalRange = input->max - input->min;
  int delta = dead_zone == 0 ? 0 : ((int32_t)physicalRange*(int32_t)dead_zone/(int32_t)100);
  int max = input->max - delta;
  int min = input->min + delta;
  physicalRange = max - min;
  if (physicalRange == 0) {
    return 0;
  }

  int result = map(input->cur, min, max, 0, MAX_AXIS);

  if (result < 0) {
    return 0;
  }
  if (result > MAX_AXIS) {
    return MAX_AXIS;
  }
  return result;
}

void processPedal(struct Pedal* input, SignalFilter *flt, uint8_t filterSize, int dead_zone) 
{
  input->cur = apply_filter(flt, filterSize, analogRead(input->pin));

  if(calibration.data.pedals_auto_calib)
  {
    // calibrate, we want the highest this pedal has been
    input->max = input->cur > input->max ? input->cur : input->max;
    // same for lowest, but bottom out at current value rather than 0
    input->min = input->cur < input->min ? input->cur : input->min;
  }

  input->axis = axisValue(input, dead_zone);
}

void setXAxis(void* in) {
  Pedal* input = (Pedal*)in;
  G27.setRxAxis(input->axis);
}

void setYAxis(void* in) {
  Pedal* input = (Pedal*)in;
  G27.setRyAxis(input->axis);
}

void setZAxis(void* in) {
  Pedal* input = (Pedal*)in;
  G27.setZAxis(input->axis);
}

// SHIFTER CODE
int buttonTable[] = {
  // first four are unused
  0, 0, 0, 0,
  OUTPUT_RED_CENTERRIGHT,
  OUTPUT_RED_CENTERLEFT,
  OUTPUT_RED_RIGHT,
  OUTPUT_RED_LEFT,
  OUTPUT_BLACK_TOP,
  OUTPUT_BLACK_RIGHT,
  OUTPUT_BLACK_LEFT,
  OUTPUT_BLACK_BOTTOM,
  OUTPUT_DPAD_RIGHT,
  OUTPUT_DPAD_LEFT,
  OUTPUT_DPAD_BOTTOM,
  OUTPUT_DPAD_TOP
};

void waitForSignalToSettle() {
  delayMicroseconds(SIGNAL_SETTLE_DELAY);
}

void getButtonStates(int *ret) {
  digitalWrite(SHIFTER_MODE_PIN, LOW);    // Switch to parallel mode: digital inputs are read into shift register
  waitForSignalToSettle();
  digitalWrite(SHIFTER_MODE_PIN, HIGH);   // Switch to serial mode: one data bit is output on each clock falling edge

  for(int i = 0; i < 16; ++i) {           // Iteration over both 8 bit registers
    digitalWrite(SHIFTER_CLOCK_PIN, LOW);         // Generate clock falling edge
    waitForSignalToSettle();

    ret[i] = digitalRead(SHIFTER_DATA_PIN);

    digitalWrite(SHIFTER_CLOCK_PIN, HIGH);        // Generate clock rising edge
    waitForSignalToSettle();
  }
}

void getShifterPosition(int *ret) {
  ret[0] = analogRead(SHIFTER_X_PIN);
  ret[1] = analogRead(SHIFTER_Y_PIN);
}

int getCurrentGear(int shifterPosition[], int btns[]) {
  static int gear = 0;  // default to neutral
  uint16_t x = shifterPosition[0], y = shifterPosition[1];
  
  if (y < calibration.data.shifter_y_neutralMax && y > calibration.data.shifter_y_neutralMin)
  {
    gear = 0;
  }

  if (y > calibration.data.shifter_y_135_gearZone )
  {
    if( x <= calibration.data.shifter_x_12 )
    {
      if( gear != 3 ) /* avoid toggles between neighboring gears */
      {
        gear = 1;
      }
    } else if ( x >= calibration.data.shifter_x_56 )
    {
      if( gear != 3 ) /* avoid toggles between neighboring gears */
      {
        gear = 5;
      }
    } else
    {
      if( gear != 1 && gear != 5 ) /* avoid toggles between neighboring gears */
      {
        gear = 3;
      }
    }
  }
  if (y < calibration.data.shifter_y_246R_gearZone )
  {
    if( x <= calibration.data.shifter_x_12 )
    {
      if( gear != 4 ) /* avoid toggles between neighboring gears */
      {
        gear = 2;
      }
    } else if ( x >= calibration.data.shifter_x_56 )
    {
      uint8_t reverse = (calibration.data.flag1 & FLAG1_REVERSE_RIGHT_RED) ? btns[BUTTON_RED_RIGHT] : btns[BUTTON_REVERSE];
      if(reverse)
      {
        if( gear != 4 && gear != 6 ) /* avoid toggles between neighboring gears */
        {
          gear = 7;
        }
      } else
      {
        if( gear != 4 && gear != 7) /* avoid toggles between neighboring gears */
        {
          gear = 6;
        }
      }
    } else
    {
      if( gear != 2 && gear != 6  && gear != 7) /* avoid toggles between neighboring gears */
      {
        gear = 4;
      }
    }
  }
  // reset button if we are not in reverse gear (this was there in original code, not sure if it's needed)
  if (gear != 7) btns[BUTTON_REVERSE] = 0;

  return gear;
}

typedef struct DebugStruct
{
  uint8_t sizeInBytes;
  uint8_t calibButton;
  int16_t axisValues[5];
  CalibData calib;
  uint16_t numCrcErrors;
  uint16_t numMagicNumErrors;
  uint32_t profiling[4];
  uint16_t out_axis[3];
  uint32_t out_buttons;
} DebugStruct;

static DebugStruct debug = {0};

void setButtonStates(int buttons[], int gear) {
  // SHIFTER CODE
  debug.out_buttons = 0;
  
  // release virtual buttons for all gears
  for (byte i = 0; i < 7; ++i) {
    G27.setButton(i, LOW);
  }

  if (gear > 0) {
    G27.setButton(gear - 1, HIGH);
    debug.out_buttons |= 1 << (gear - 1);
  }

  for (byte i = BUTTON_RED_CENTERRIGHT; i <= BUTTON_BLACK_BOTTOM; ++i) {
    G27.setButton(buttonTable[i], buttons[i]);
    if( buttons[i] )
    {
      debug.out_buttons |= 1 << buttonTable[i];
    }
  }

  // Hat switch (D-Pad)
  byte hatSwitchState = 0;
  hatSwitchState |= (buttons[BUTTON_DPAD_TOP] << 0);
  hatSwitchState |= (buttons[BUTTON_DPAD_RIGHT] << 1);
  hatSwitchState |= (buttons[BUTTON_DPAD_BOTTOM] << 2);
  hatSwitchState |= (buttons[BUTTON_DPAD_LEFT] << 3);

  switch (hatSwitchState)
  {
  case 0B00000001:
    G27.setHatSwitch(0, 0); // up
    break;
  case 0B00000011:
    G27.setHatSwitch(0, 45); // up right
    break;
  case 0B00000010:
    G27.setHatSwitch(0, 90); // right
    break;
  case 0B00000110:
    G27.setHatSwitch(0, 135); // down right
    break;
  case 0B00000100:
    G27.setHatSwitch(0, 180); // down
    break;
  case 0B00001100:
    G27.setHatSwitch(0, 225); // down left
    break;
  case 0B00001000:
    G27.setHatSwitch(0, 270); // left
    break;
  case 0B00001001:
    G27.setHatSwitch(0, 315); // up left
    break;
  case 0B00000000:
    G27.setHatSwitch(0, -1); // no direction
  default:
    break;
  }
}

void set_dead_zone(uint8_t dz, int pedal, CalibData *calib)
{
    dz /= 2;
    switch(pedal)
    {
        case 0:
            calib->flag2 &= ~(FLAG2_GAS_DEAD_ZONE);
            calib->flag2 |= (dz) << 4;
            break;
        case 1:
            calib->flag2 &= ~(FLAG2_BRAKE_DEAD_ZONE);
            calib->flag2 |= (dz) << 1;
            break;
        case 2:
            calib->flag3 &= ~(FLAG3_CLUTCH_DEAD_ZONE);
            calib->flag3 |= (dz) << 4;
            break;
        default:
          break;
    }
}

#define CALIB_MIN(calibValue, curValue, minValue) if( (int)calibValue < 0 || (int)curValue < (int)minValue ) { calibValue = minValue = curValue; }
#define CALIB_MAX(calibValue, curValue, maxValue) if( (int)calibValue < 0 || (int)curValue > (int)maxValue ) { calibValue = maxValue = curValue; }
#define CALIB_RANGE(calibValue, curValue, minValue, maxValue) \
    { if( (int)calibValue < 0 ) { calibValue = minValue = maxValue = curValue; } \
      CALIB_MIN(calibValue, curValue, minValue); \
      CALIB_MAX(calibValue, curValue, maxValue); }

void calib(struct Pedal *gas, Pedal *brake, Pedal *clutch, int shifter_X, int shifter_Y, int calibButton) 
{
  static uint8_t printMode = 0;
  static enum COMMANDS {
      IDLE = 'i',
      RECORD_Y_SHIFTER_135 = 'u',
      RECORD_Y_SHIFTER_246 = 'b',
      RECORD_Y_SHIFTER_NEUTRAL = 'n',
      RECORD_X_SHIFTER_12 = 'l',
      RECORD_X_SHIFTER_56 = 'r',
      RECORD_GAS = 'G',
      RECORD_BRAKE = 'B',
      RECORD_CLUTCH = 'C',
      SET_PEDAL_AUTO_CALIBRATE = 'P',
      RESET_PEDAL_AUTO_CALIBRATE = 'p',
      SET_PEDALS_ENABLED = 'E',
      RESET_PEDALS_ENABLED = 'e',
      SET_SHIFTER_ENABLED = 'S',
      RESET_SHIFTER_ENABLED = 's',
      SET_GAS_INVERTED = 'Y',
      RESET_GAS_INVERTED = 'y',
      SET_BRAKE_INVERTED = 'X',
      RESET_BRAKE_INVERTED = 'x',
      SET_CLUTCH_INVERTED = 'Z',
      RESET_CLUTCH_INVERTED = 'z',
      SET_REVERSE_RIGHT_RED = 'Q',
      RESET_REVERSE_RIGHT_RED = 'q',
      SET_PEDAL_FILTSIZE_OFF = '0',
      SET_PEDAL_FILTSIZE_3 = '3',
      SET_PEDAL_FILTSIZE_5 = '5',
      SET_PEDAL_FILTSIZE_7 = '7',
      SET_PEDAL_FILTSIZE_9 = '9',
      SET_PEDAL_FILTSIZE_15 = 'f',
      SET_SHIFTER_FILTSIZE_OFF = '1',
      SET_SHIFTER_FILTSIZE_3 = '2',
      SET_SHIFTER_FILTSIZE_5 = '4',
      SET_SHIFTER_FILTSIZE_7 = '6',
      SET_SHIFTER_FILTSIZE_9 = '8',
      SET_SHIFTER_FILTSIZE_15 = 'F',      
      SET_COMBINE_Z_EQUAL_X_MINUS_Z = '+',
      SET_COMBINE_Z_EQUAL_Z_MINUS_X = '-',
      SET_COMBINE_Z_EQUAL_Z = '.',
      SET_DEAD_ZONE00 = '^',
      SET_DEAD_ZONE02 = '!',
      SET_DEAD_ZONE04 = '"',
      SET_DEAD_ZONE06 = ']',
      SET_DEAD_ZONE08 = '$',
      SET_DEAD_ZONE10 = '%',
      SET_DEAD_ZONE12 = '&',
      SET_DEAD_ZONE14 = '/',
      SELECT_BRAKE_DEAD_ZONE = '(',
      SELECT_CLUTCH_DEAD_ZONE = ')',
      SELECT_GAS_DEAD_ZONE = '[',
      SET_PRINT_MODE = 'O',
      RESET_PRINT_MODE = 'o',
      STORE_CALIB = 'w',
      DEFAULT_CALIB = 'c',
      EEPROM_CALIB = 'U'
  } currentMode = IDLE;
  static int calibValue = -1;
  static int deadZoneSelect = -1;
  if (Serial.available() > 0) 
  {
    char rx_byte = Serial.read();
    switch (rx_byte)
    {
      case IDLE:
      case RECORD_Y_SHIFTER_135:        
      case RECORD_Y_SHIFTER_246:
      case RECORD_Y_SHIFTER_NEUTRAL:
      case RECORD_X_SHIFTER_12:
      case RECORD_X_SHIFTER_56:
      case RECORD_GAS:
      case RECORD_BRAKE:
      case RECORD_CLUTCH: calibValue = -1; currentMode = (COMMANDS)rx_byte; break;
      
      case SET_PEDAL_AUTO_CALIBRATE: calibration.data.pedals_auto_calib = 1; break;
      case RESET_PEDAL_AUTO_CALIBRATE: calibration.data.pedals_auto_calib= 0; break;
      
      case SET_PEDALS_ENABLED: calibration.data.flag2 |= FLAG2_ENABLE_PEDALS; break;
      case RESET_PEDALS_ENABLED: calibration.data.flag2 &= ~FLAG2_ENABLE_PEDALS; break;
      
      case SET_SHIFTER_ENABLED: calibration.data.flag3 |= FLAG3_ENABLE_SHIFTER; break;
      case RESET_SHIFTER_ENABLED: calibration.data.flag3 &= ~FLAG3_ENABLE_SHIFTER; break;
      
      case SET_GAS_INVERTED: calibration.data.flag1 |= FLAG1_INVERT_GAS; break;
      case RESET_GAS_INVERTED: calibration.data.flag1 &= ~FLAG1_INVERT_GAS; break;
      case SET_BRAKE_INVERTED: calibration.data.flag1 |= FLAG1_INVERT_BRAKE; break;
      case RESET_BRAKE_INVERTED: calibration.data.flag1 &= ~FLAG1_INVERT_BRAKE; break;
      case SET_CLUTCH_INVERTED: calibration.data.flag1 |= FLAG1_INVERT_CLUTCH; break;
      case RESET_CLUTCH_INVERTED: calibration.data.flag1 &= ~FLAG1_INVERT_CLUTCH; break;
      case SET_REVERSE_RIGHT_RED: calibration.data.flag1 |= FLAG1_REVERSE_RIGHT_RED; break;
      case RESET_REVERSE_RIGHT_RED: calibration.data.flag1 &= ~FLAG1_REVERSE_RIGHT_RED; break;
      
      case SET_PEDAL_FILTSIZE_OFF: calibration.data.pedal_median_size = 0; break;
      case SET_PEDAL_FILTSIZE_3: calibration.data.pedal_median_size = 3; break;
      case SET_PEDAL_FILTSIZE_5: calibration.data.pedal_median_size = 5; break;
      case SET_PEDAL_FILTSIZE_7: calibration.data.pedal_median_size = 9; break;
      case SET_PEDAL_FILTSIZE_9: calibration.data.pedal_median_size = 15; break;
      case SET_PEDAL_FILTSIZE_15: calibration.data.pedal_median_size = 49; break;
      
      case SET_SHIFTER_FILTSIZE_OFF: calibration.data.shifter_median_size = 0; break;
      case SET_SHIFTER_FILTSIZE_3: calibration.data.shifter_median_size = 3; break; 
      case SET_SHIFTER_FILTSIZE_5: calibration.data.shifter_median_size = 5; break;
      case SET_SHIFTER_FILTSIZE_7: calibration.data.shifter_median_size = 9; break;
      case SET_SHIFTER_FILTSIZE_9: calibration.data.shifter_median_size = 15; break;
      case SET_SHIFTER_FILTSIZE_15: calibration.data.shifter_median_size = 49; break;
      
      case SET_COMBINE_Z_EQUAL_X_MINUS_Z: calibration.data.flag1 &= ~(FLAG1_COMBINE_Z_EQUALS_Z_MINUS_X); calibration.data.flag1 |= FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z; break;
      case SET_COMBINE_Z_EQUAL_Z_MINUS_X: calibration.data.flag1 &= ~(FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z); calibration.data.flag1 |= FLAG1_COMBINE_Z_EQUALS_Z_MINUS_X; break;
      case SET_COMBINE_Z_EQUAL_Z: calibration.data.flag1 &= ~(FLAG1_COMBINE_Z_EQUALS_Z_MINUS_X|FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z); break;
      case SET_DEAD_ZONE00: set_dead_zone(0, deadZoneSelect, &calibration.data); break;
      case SET_DEAD_ZONE02: set_dead_zone(2, deadZoneSelect, &calibration.data); break;
      case SET_DEAD_ZONE04: set_dead_zone(4, deadZoneSelect, &calibration.data); break;
      case SET_DEAD_ZONE06: set_dead_zone(6, deadZoneSelect, &calibration.data); break;
      case SET_DEAD_ZONE08: set_dead_zone(8, deadZoneSelect, &calibration.data); break;
      case SET_DEAD_ZONE10: set_dead_zone(10, deadZoneSelect, &calibration.data); break;
      case SET_DEAD_ZONE12: set_dead_zone(12, deadZoneSelect, &calibration.data); break;
      case SET_DEAD_ZONE14: set_dead_zone(14, deadZoneSelect, &calibration.data); break;
      case SELECT_BRAKE_DEAD_ZONE: deadZoneSelect = 1; break;
      case SELECT_CLUTCH_DEAD_ZONE: deadZoneSelect = 2; break;
      case SELECT_GAS_DEAD_ZONE: deadZoneSelect = 0; break;
      case SET_PRINT_MODE: printMode = 1; break;
      case RESET_PRINT_MODE: printMode = 0;; break;

      case STORE_CALIB:
        calibration.crc = crc((uint8_t*)&calibration.data, sizeof(calibration.data));
        EEPROM.put(0, calibration);
        break;
      case DEFAULT_CALIB:
        calibration = calibDefault;
        break;
      case EEPROM_CALIB:
        loadEEPROM();
        break;
    }
  }
  if(printMode)
  {
    debug.sizeInBytes = sizeof(debug);
    debug.calibButton = calibButton;
    debug.axisValues[0] = shifter_X;
    debug.axisValues[1] = shifter_Y;
    debug.axisValues[2] = gas->cur;
    debug.axisValues[3] = brake->cur;
    debug.axisValues[4] = clutch->cur;
    debug.calib = calibration.data;
    debug.numCrcErrors = crcError;
    debug.numMagicNumErrors = magicNumberError;
    debug.profiling[3] = micros();
    Serial.write((const byte*)&debug, sizeof(debug));    
    debug.profiling[0] = debug.profiling[3];
  }
  if(calibButton)
  {
    switch(currentMode)
    {
      case RECORD_Y_SHIFTER_135:
        CALIB_MIN(calibValue, shifter_Y, calibration.data.shifter_y_135_gearZone);
        break;
      case RECORD_Y_SHIFTER_246:
        CALIB_MAX(calibValue, shifter_Y, calibration.data.shifter_y_246R_gearZone);
        break;
      case RECORD_Y_SHIFTER_NEUTRAL:
        CALIB_RANGE(calibValue, shifter_Y, calibration.data.shifter_y_neutralMin, calibration.data.shifter_y_neutralMax);
        break;
      case RECORD_X_SHIFTER_12:
        CALIB_MAX(calibValue, shifter_X, calibration.data.shifter_x_12);
        break;
      case RECORD_X_SHIFTER_56:
        CALIB_MIN(calibValue, shifter_X, calibration.data.shifter_x_56);
        break;
      default:
        break;
    }
  }
  switch(currentMode)
  {
    case RECORD_GAS:
      CALIB_RANGE(calibValue, gas->cur, calibration.data.gasMin, calibration.data.gasMax);
      break;
    case RECORD_BRAKE:
      CALIB_RANGE(calibValue, brake->cur, calibration.data.brakeMin, calibration.data.brakeMax);
      break;
    case RECORD_CLUTCH:         
      CALIB_RANGE(calibValue, clutch->cur, calibration.data.clutchMin, calibration.data.clutchMax);
      break;
    default:
        break;
  }
}

void setup() {
  Serial.begin(460800);

  // lights
  pinMode(RED_PIN, OUTPUT);
  pinMode(GREEN_PIN, OUTPUT);
  pinMode(BLUE_PIN, OUTPUT);

  // shifter
  pinMode(SHIFTER_MODE_PIN, OUTPUT);
  pinMode(SHIFTER_CLOCK_PIN, OUTPUT);

  digitalWrite(SHIFTER_MODE_PIN, HIGH);
  digitalWrite(SHIFTER_CLOCK_PIN, HIGH);

  // pedals
  Pedal* gas = new Pedal();
  Pedal* brake = new Pedal();
  Pedal* clutch = new Pedal();

  gas->pin = GAS_PIN;
  brake->pin = BRAKE_PIN;
  clutch->pin = CLUTCH_PIN;

  loadEEPROM();

  if( !calibration.data.pedals_auto_calib )
  {
    gas->min = calibration.data.gasMin;
    gas->max = calibration.data.gasMax;

    brake->min = calibration.data.brakeMin;
    brake->max = calibration.data.brakeMax;

    clutch->min = calibration.data.clutchMin;
    clutch->max = calibration.data.clutchMax;
  } else
  {
    gas->min = brake->min = clutch->min = MAX_AXIS;
    gas->max = brake->max = clutch->max = 0;
  }

  gasPedal = gas;
  brakePedal = brake;
  clutchPedal = clutch;
}

SignalFilter signalFilters[5];

void loop() {
  debug.profiling[2] = micros();
  // pedals
  processPedal(gasPedal, &signalFilters[0], calibration.data.pedal_median_size, 
               ((calibration.data.flag2 & FLAG2_GAS_DEAD_ZONE) >> 4)*2);
  processPedal(brakePedal, &signalFilters[1], calibration.data.pedal_median_size,
               ((calibration.data.flag2 & FLAG2_BRAKE_DEAD_ZONE) >> 1)*2);
  processPedal(clutchPedal, &signalFilters[2], calibration.data.pedal_median_size,
               ((calibration.data.flag3 & FLAG3_CLUTCH_DEAD_ZONE) >> 4)*2);

  if(calibration.data.flag1 & FLAG1_INVERT_GAS )
  {
    Pedal* gas = (Pedal*)gasPedal;
    gas->axis = map(gas->axis, 0, MAX_AXIS, MAX_AXIS, 0);
  }
  if(calibration.data.flag1 & FLAG1_INVERT_BRAKE )
  {
    Pedal* brake = (Pedal*)brakePedal;
    brake->axis = map(brake->axis, 0, MAX_AXIS, MAX_AXIS, 0);
  }
  if(calibration.data.flag1 & FLAG1_INVERT_CLUTCH )
  {
    Pedal* clutch = (Pedal*)clutchPedal;
    clutch->axis = map(clutch->axis, 0, MAX_AXIS, MAX_AXIS, 0);
  }
  if(calibration.data.flag1 & FLAG1_COMBINE_Z_EQUALS_X_MINUS_Z)
  {
      Pedal *clutch = (Pedal*)clutchPedal;
      Pedal *gas = (Pedal*)gasPedal;
      clutch->axis = map(gas->axis - clutch->axis, -MAX_AXIS, MAX_AXIS, 0, MAX_AXIS);
  }
  if(calibration.data.flag1 & FLAG1_COMBINE_Z_EQUALS_Z_MINUS_X)
  {
      Pedal *clutch = (Pedal*)clutchPedal;
      Pedal *gas = (Pedal*)gasPedal;
      clutch->axis = map(clutch->axis - gas->axis, -MAX_AXIS, MAX_AXIS, 0, MAX_AXIS);
  }

  if(calibration.data.flag2 & FLAG2_ENABLE_PEDALS)
  {
    setXAxis(gasPedal);
    setYAxis(brakePedal);
    setZAxis(clutchPedal);
    debug.out_axis[0] = gasPedal->axis;
    debug.out_axis[1] = brakePedal->axis;
    debug.out_axis[2] = clutchPedal->axis;
  } else
  {
    debug.out_axis[0] = 0xffff;
    debug.out_axis[1] = 0xffff;
    debug.out_axis[2] = 0xffff;
  }
  
  // shifter
  int buttonStates[16];
  getButtonStates(buttonStates);
  
  int shifterPosition[2];
  getShifterPosition(shifterPosition);
  shifterPosition[0] = apply_filter(&signalFilters[3], calibration.data.shifter_median_size, shifterPosition[0]);
  shifterPosition[1] = apply_filter(&signalFilters[4], calibration.data.shifter_median_size, shifterPosition[1]);
  
  int gear = getCurrentGear(shifterPosition, buttonStates);

  if(calibration.data.flag3 & FLAG3_ENABLE_SHIFTER)
  {
    setButtonStates(buttonStates, gear);
  } else
  {
    debug.out_buttons = 0xffffffff;
  }

  calib(gasPedal, brakePedal, clutchPedal, shifterPosition[0], shifterPosition[1], buttonStates[BUTTON_RED_LEFT]);
  G27.sendState();
  debug.profiling[1] = micros();
}
