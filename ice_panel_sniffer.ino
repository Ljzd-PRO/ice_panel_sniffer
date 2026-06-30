/*
  ESP32-C3 sniffer/control firmware for the ice-maker control panel.

  Hardware safety contract:
  - Default operation is INPUT only.
  - Active button simulation commands are direct-GPIO accepted-risk tests and
    should only be used as short manual pulses or explicit timed holds.
  - ADC raw counts are relative correlation data, not calibrated panel voltage.
  - Do not connect ESP32 GND to the ice-maker panel for the direct-floating
    test plan.

  CSV output:
    A,<us>,<p1_raw>,<p2_raw>,<p3_raw>,<p4_raw>,<p5_raw>,<mask>
    E,<us>,<mask>
    H,<us>,<mode>,<mask>
    P,<us>,<action>,<ms>

  mask bit assignment:
    bit0=P1(GPIO0), bit1=P2(GPIO1), bit2=P3(GPIO2),
    bit3=P4(GPIO3), bit4=P5(GPIO4)
*/

#include <Arduino.h>
#include "driver/gpio.h"

enum CaptureMode : uint8_t {
  MODE_ADC = 0,
  MODE_EDGE = 1,
};

static const uint32_t SERIAL_BAUD = 921600;

// Default mapping: P1->GPIO0, P2->GPIO1, P3->GPIO2, P4->GPIO3, P5->GPIO4.
// If GPIO2 causes boot issues, move P3 to another safe input and update this.
static const uint8_t PANEL_PINS[5] = {0, 1, 2, 3, 4};
static const char *PANEL_NAMES[5] = {"P1", "P2", "P3", "P4", "P5"};
static const uint8_t PIN_P1_INDEX = 0;
static const uint8_t PIN_P2_INDEX = 1;
static const uint8_t PIN_P3_INDEX = 2;

static CaptureMode mode = MODE_ADC;
static uint32_t adcIntervalUs = 1000;
static uint32_t heartbeatIntervalUs = 1000000;
static uint32_t lastAdcUs = 0;
static uint32_t lastHeartbeatUs = 0;
static uint8_t lastMask = 0xff;

static uint8_t readMask() {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < 5; i++) {
    if (digitalRead(PANEL_PINS[i]) == HIGH) {
      mask |= (1u << i);
    }
  }
  return mask;
}

static void restorePanelInputs() {
  for (uint8_t i = 0; i < 5; i++) {
    const gpio_num_t pin = static_cast<gpio_num_t>(PANEL_PINS[i]);
    gpio_set_level(pin, 0);
    gpio_set_direction(pin, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pin, GPIO_FLOATING);
    pinMode(PANEL_PINS[i], INPUT);
  }

  analogReadResolution(12);
  for (uint8_t i = 0; i < 5; i++) {
    analogSetPinAttenuation(PANEL_PINS[i], ADC_11db);
  }
}

static void configureInputsOnly() {
  for (uint8_t i = 0; i < 5; i++) {
    gpio_reset_pin(static_cast<gpio_num_t>(PANEL_PINS[i]));
  }
  restorePanelInputs();
}

static void printHeader() {
  Serial.println("# ice_panel_sniffer");
  Serial.println("# direct-floating plan: default inputs only; active SW2 commands are accepted-risk manual pulses");
  Serial.println("# netlist: P5-P1=LED5_ice_full, P5-P2=LED4_no_water, P1-P2=SW1_power, P1-P3=SW2_select, P3-P4=LED3_large, P2-P4=LED2_small, P1-P4=LED1_power");
  Serial.print("# pins:");
  for (uint8_t i = 0; i < 5; i++) {
    Serial.print(' ');
    Serial.print(PANEL_NAMES[i]);
    Serial.print("=GPIO");
    Serial.print(PANEL_PINS[i]);
  }
  Serial.println();
  Serial.println("# commands: mode adc | mode edge | adc_us <n> | heartbeat_us <n> | status | help");
  Serial.println("# active commands: sw1_weak [ms] | sw1_od [ms] | sw1_hold_od [ms] | sw1_pair_od [ms] | sw2_weak [ms] | sw2_od [ms] | sw2_hold_od [ms] | sw2_pair_od [ms] | release");
  Serial.println("# recommended active tests: sw2_od 80 for select, sw1_od 100 for power");
  Serial.println("# csv: A,us,p1_raw,p2_raw,p3_raw,p4_raw,p5_raw,mask");
  Serial.println("# csv: E,us,mask");
  Serial.println("# csv: H,us,mode,mask");
  Serial.println("# csv: P,us,action,ms");
}

static void printStatus() {
  Serial.print("# status mode=");
  Serial.print(mode == MODE_ADC ? "adc" : "edge");
  Serial.print(" adc_us=");
  Serial.print(adcIntervalUs);
  Serial.print(" heartbeat_us=");
  Serial.print(heartbeatIntervalUs);
  Serial.print(" mask=");
  Serial.println(readMask());
}

static void setMode(CaptureMode nextMode) {
  mode = nextMode;
  lastMask = readMask();
  lastAdcUs = micros();
  lastHeartbeatUs = lastAdcUs;
  Serial.print("# mode ");
  Serial.println(mode == MODE_ADC ? "adc" : "edge");
}

static uint32_t parseOptionalMs(const String &line, const char *command, uint32_t defaultMs, uint32_t maxMs) {
  const String cmd(command);
  if (line == cmd) {
    return defaultMs;
  }
  if (!line.startsWith(cmd + " ")) {
    return 0;
  }
  const uint32_t requested = static_cast<uint32_t>(line.substring(cmd.length() + 1).toInt());
  if (requested == 0) {
    return defaultMs;
  }
  if (requested > maxMs) {
    return maxMs;
  }
  return requested;
}

static void emitPulseEvent(const char *action, uint32_t pulseMs) {
  Serial.print("P,");
  Serial.print(micros());
  Serial.print(',');
  Serial.print(action);
  Serial.print(',');
  Serial.println(pulseMs);
}

static void pulseSw2Weak(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw2_weak_begin", pulseMs);

  const gpio_num_t p3 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P3_INDEX]);
  gpio_set_direction(p3, GPIO_MODE_INPUT);
  gpio_set_pull_mode(p3, GPIO_PULLDOWN_ONLY);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw2_weak_end", pulseMs);
}

static void pulseSw1Weak(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw1_weak_begin", pulseMs);

  const gpio_num_t p2 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P2_INDEX]);
  gpio_set_direction(p2, GPIO_MODE_INPUT);
  gpio_set_pull_mode(p2, GPIO_PULLDOWN_ONLY);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw1_weak_end", pulseMs);
}

static void pulseSw2OpenDrain(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw2_od_begin", pulseMs);

  const gpio_num_t p3 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P3_INDEX]);
  gpio_set_level(p3, 0);
  gpio_set_direction(p3, GPIO_MODE_OUTPUT_OD);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw2_od_end", pulseMs);
}

static void holdSw2OpenDrain(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw2_hold_od_begin", pulseMs);

  const gpio_num_t p3 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P3_INDEX]);
  gpio_set_level(p3, 0);
  gpio_set_direction(p3, GPIO_MODE_OUTPUT_OD);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw2_hold_od_end", pulseMs);
}

static void pulseSw1OpenDrain(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw1_od_begin", pulseMs);

  const gpio_num_t p2 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P2_INDEX]);
  gpio_set_level(p2, 0);
  gpio_set_direction(p2, GPIO_MODE_OUTPUT_OD);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw1_od_end", pulseMs);
}

static void holdSw1OpenDrain(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw1_hold_od_begin", pulseMs);

  const gpio_num_t p2 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P2_INDEX]);
  gpio_set_level(p2, 0);
  gpio_set_direction(p2, GPIO_MODE_OUTPUT_OD);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw1_hold_od_end", pulseMs);
}

static void pulseSw2PairOpenDrain(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw2_pair_od_begin", pulseMs);

  const gpio_num_t p1 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P1_INDEX]);
  const gpio_num_t p3 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P3_INDEX]);
  gpio_set_level(p1, 0);
  gpio_set_level(p3, 0);
  gpio_set_direction(p1, GPIO_MODE_OUTPUT_OD);
  gpio_set_direction(p3, GPIO_MODE_OUTPUT_OD);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw2_pair_od_end", pulseMs);
}

static void pulseSw1PairOpenDrain(uint32_t pulseMs) {
  restorePanelInputs();
  emitPulseEvent("sw1_pair_od_begin", pulseMs);

  const gpio_num_t p1 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P1_INDEX]);
  const gpio_num_t p2 = static_cast<gpio_num_t>(PANEL_PINS[PIN_P2_INDEX]);
  gpio_set_level(p1, 0);
  gpio_set_level(p2, 0);
  gpio_set_direction(p1, GPIO_MODE_OUTPUT_OD);
  gpio_set_direction(p2, GPIO_MODE_OUTPUT_OD);
  delay(pulseMs);

  restorePanelInputs();
  emitPulseEvent("sw1_pair_od_end", pulseMs);
}

static void processCommand(String line) {
  line.trim();
  line.toLowerCase();
  if (line.length() == 0) {
    return;
  }

  if (line == "mode adc" || line == "adc") {
    setMode(MODE_ADC);
  } else if (line == "mode edge" || line == "edge") {
    setMode(MODE_EDGE);
  } else if (line.startsWith("adc_us ")) {
    const uint32_t value = static_cast<uint32_t>(line.substring(7).toInt());
    if (value >= 200 && value <= 1000000) {
      adcIntervalUs = value;
      Serial.print("# adc_us ");
      Serial.println(adcIntervalUs);
    } else {
      Serial.println("# error adc_us out_of_range 200..1000000");
    }
  } else if (line.startsWith("heartbeat_us ")) {
    const uint32_t value = static_cast<uint32_t>(line.substring(13).toInt());
    if (value >= 100000 && value <= 10000000) {
      heartbeatIntervalUs = value;
      Serial.print("# heartbeat_us ");
      Serial.println(heartbeatIntervalUs);
    } else {
      Serial.println("# error heartbeat_us out_of_range 100000..10000000");
    }
  } else if (line == "status") {
    printStatus();
  } else if (line == "help") {
    printHeader();
  } else if (line == "release") {
    restorePanelInputs();
    Serial.println("# release inputs_floating");
  } else if (line == "sw1_weak" || line.startsWith("sw1_weak ")) {
    pulseSw1Weak(parseOptionalMs(line, "sw1_weak", 120, 500));
  } else if (line == "sw1_od" || line.startsWith("sw1_od ")) {
    pulseSw1OpenDrain(parseOptionalMs(line, "sw1_od", 100, 300));
  } else if (line == "sw1_hold_od" || line.startsWith("sw1_hold_od ")) {
    holdSw1OpenDrain(parseOptionalMs(line, "sw1_hold_od", 5000, 6000));
  } else if (line == "sw1_pair_od" || line.startsWith("sw1_pair_od ")) {
    pulseSw1PairOpenDrain(parseOptionalMs(line, "sw1_pair_od", 80, 200));
  } else if (line == "sw2_weak" || line.startsWith("sw2_weak ")) {
    pulseSw2Weak(parseOptionalMs(line, "sw2_weak", 120, 500));
  } else if (line == "sw2_od" || line.startsWith("sw2_od ")) {
    pulseSw2OpenDrain(parseOptionalMs(line, "sw2_od", 80, 250));
  } else if (line == "sw2_hold_od" || line.startsWith("sw2_hold_od ")) {
    holdSw2OpenDrain(parseOptionalMs(line, "sw2_hold_od", 5000, 6000));
  } else if (line == "sw2_pair_od" || line.startsWith("sw2_pair_od ")) {
    pulseSw2PairOpenDrain(parseOptionalMs(line, "sw2_pair_od", 60, 150));
  } else {
    Serial.print("# error unknown_command ");
    Serial.println(line);
  }
}

static void pollSerialCommands() {
  static String line;
  while (Serial.available() > 0) {
    const char ch = static_cast<char>(Serial.read());
    if (ch == '\n' || ch == '\r') {
      processCommand(line);
      line = "";
    } else if (line.length() < 96) {
      line += ch;
    }
  }
}

static void emitAdcRow(uint32_t nowUs) {
  const uint16_t p1 = analogRead(PANEL_PINS[0]);
  const uint16_t p2 = analogRead(PANEL_PINS[1]);
  const uint16_t p3 = analogRead(PANEL_PINS[2]);
  const uint16_t p4 = analogRead(PANEL_PINS[3]);
  const uint16_t p5 = analogRead(PANEL_PINS[4]);
  const uint8_t mask = readMask();

  Serial.print("A,");
  Serial.print(nowUs);
  Serial.print(',');
  Serial.print(p1);
  Serial.print(',');
  Serial.print(p2);
  Serial.print(',');
  Serial.print(p3);
  Serial.print(',');
  Serial.print(p4);
  Serial.print(',');
  Serial.print(p5);
  Serial.print(',');
  Serial.println(mask);
}

static void emitHeartbeat(uint32_t nowUs) {
  Serial.print("H,");
  Serial.print(nowUs);
  Serial.print(',');
  Serial.print(mode == MODE_ADC ? "adc" : "edge");
  Serial.print(',');
  Serial.println(readMask());
}

void setup() {
  configureInputsOnly();
  Serial.begin(SERIAL_BAUD);
  delay(500);
  printHeader();
  setMode(MODE_ADC);
}

void loop() {
  pollSerialCommands();

  const uint32_t nowUs = micros();
  if (mode == MODE_ADC) {
    if (static_cast<uint32_t>(nowUs - lastAdcUs) >= adcIntervalUs) {
      lastAdcUs = nowUs;
      emitAdcRow(nowUs);
    }
  } else {
    const uint8_t mask = readMask();
    if (mask != lastMask) {
      lastMask = mask;
      Serial.print("E,");
      Serial.print(nowUs);
      Serial.print(',');
      Serial.println(mask);
    }
  }

  if (static_cast<uint32_t>(nowUs - lastHeartbeatUs) >= heartbeatIntervalUs) {
    lastHeartbeatUs = nowUs;
    emitHeartbeat(nowUs);
  }
}
