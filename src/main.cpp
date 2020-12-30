#include <Arduino.h>
#include <ESP8266WiFi.h>

#include <ArduinoJson.h>
#include <PubSubClient.h>

#include "autoblind.h"

#define BASE_TOPIC "homeassistant/light/autoblind"
#define AUTODISCOVER_TOPIC BASE_TOPIC "/config"
#define STATE_TOPIC   "homeassistant/autoblind/state"
#define COMMAND_TOPIC "homeassistant/autoblind/set"
#define DEVICE_UUID "c16af742-4724-11eb-b378-0242ac130002"

#define SCALEFACTOR 32  // bri 1-255 -> position 32-8160 (approx 2 full turns of stepper)

#define ORANGE 5
#define YELLOW 13
#define PINK 12
#define BLUE 14

const uint coils[4] = {BLUE, PINK, YELLOW, ORANGE};
uint position = 0;
uint encoderState;
uint lastEncoderState;
int stateChange;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

void publishAutodiscover() {
  while (!mqttClient.connected()) {
    mqttClient.connect("autoblind");
    delay(100);
  }
  String message;
  StaticJsonDocument<768> jsonMessage;
  jsonMessage["json_attributes_topic"] = BASE_TOPIC;
  jsonMessage["availability_topic"] = STATE_TOPIC;
  jsonMessage["name"] = "autoblind";
  jsonMessage["unique_id"] = DEVICE_UUID;
  jsonMessage["command_topic"] = COMMAND_TOPIC;
  jsonMessage["state_topic"] = STATE_TOPIC;
  jsonMessage["schema"] = "json";
  jsonMessage["brightness"] = true;
  jsonMessage["ct"] = false;
  jsonMessage["xy"] = false;
  JsonObject device = jsonMessage.createNestedObject("device");
  JsonArray identifiers = device.createNestedArray("identifiers");
  identifiers.add(DEVICE_UUID);
  device["name"] = "Autoblind";
  device["model"] = "Autoblind Mk 1";
  device["sw_version"] = "0.1a";
  device["manufacturer"] = "Strickland Electronics and Electronics Accessories";
  serializeJson(jsonMessage, message);

  mqttClient.publish(
    AUTODISCOVER_TOPIC,
    message.c_str(),
    true  // retained
  );
  // delay(1000); some people say we need to wait for pub but idk seems fine
  Serial.println("autodiscovery published");
}

void publishState() {
  String state;
  DynamicJsonDocument stateDoc(256);
  stateDoc["state"] = position > 0 ? "ON" : "OFF";
  // take ceiling
  int brightness = position / SCALEFACTOR + (position % SCALEFACTOR != 0);
  stateDoc["brightness"] = brightness > 0 ? brightness : 1;
  serializeJson(stateDoc, state);
  Serial.println(state);
  // TODO DRY reconnect logic
  while (!mqttClient.connected()) {
    mqttClient.connect("autoblind");
    delay(100);
  }
  mqttClient.publish(
    STATE_TOPIC,
    state.c_str(),
    false  // retained
  );
}

void runStepper(int requestedPosition) {
  uint newPosition;
  if (requestedPosition < 0) {
    // a request for a negative value parks at 0
    newPosition = 0;
  } else {
    newPosition = (uint) requestedPosition;
  }
  Serial.printf("reposition to %d\n", newPosition);
  while (newPosition != position) {
    uint step = position % 8;
    switch(step) {
      case 0:
        digitalWrite(BLUE,   HIGH);
        digitalWrite(PINK,   LOW);
        digitalWrite(YELLOW, LOW);
        digitalWrite(ORANGE, LOW);
        break;
      case 1:
        digitalWrite(BLUE,   HIGH);
        digitalWrite(PINK,   HIGH);
        digitalWrite(YELLOW, LOW);
        digitalWrite(ORANGE, LOW);
        break;
      case 2:
        digitalWrite(BLUE,   LOW);
        digitalWrite(PINK,   HIGH);
        digitalWrite(YELLOW, LOW);
        digitalWrite(ORANGE, LOW);
        break;
      case 3:
        digitalWrite(BLUE,   LOW);
        digitalWrite(PINK,   HIGH);
        digitalWrite(YELLOW, HIGH);
        digitalWrite(ORANGE, LOW);
        break;
      case 4:
        digitalWrite(BLUE,   LOW);
        digitalWrite(PINK,   LOW);
        digitalWrite(YELLOW, HIGH);
        digitalWrite(ORANGE, LOW);
        break;
      case 5:
        digitalWrite(BLUE,   LOW);
        digitalWrite(PINK,   LOW);
        digitalWrite(YELLOW, HIGH);
        digitalWrite(ORANGE, HIGH);
        break;
      case 6:
        digitalWrite(BLUE,   LOW);
        digitalWrite(PINK,   LOW);
        digitalWrite(YELLOW, LOW);
        digitalWrite(ORANGE, HIGH);
        break;
      case 7:
        digitalWrite(BLUE,   HIGH);
        digitalWrite(PINK,   LOW);
        digitalWrite(YELLOW, LOW);
        digitalWrite(ORANGE, HIGH);
        break;
    }
    if (newPosition > position) {
      position++;
    } else {
      position--;
    }
    delay(2);
  }
  for (uint8_t i=0; i < 4; ++i) {
    // all transistors off / coils pulled high
    digitalWrite(coils[i], LOW);
  }
  publishState();
  Serial.printf("moved to position %d\n", position);
}

void mqttCallback(char* topic, byte* payload, uint length) {
  Serial.printf("%s message received\n", topic);
  byte truncatedPayload[length];
  for (uint i = 0; i < length; i++) {
    truncatedPayload[i] = payload[i];
  }
  Serial.println((char*) truncatedPayload);
  DynamicJsonDocument jsonDoc(192);
  DeserializationError jsonError = deserializeJson(jsonDoc, truncatedPayload);
  if (jsonError) {
    Serial.println(jsonError.c_str());
  } else {
    if (strcmp(topic, STATE_TOPIC) == 0) {
      //publishState();  this gets abso spammed... i think i just update it not sub?
    } else if (strcmp(topic, COMMAND_TOPIC) == 0) {
      JsonVariant state = jsonDoc["state"];
      if (!state.isNull() && strcmp(state.as<char*>(), "OFF") == 0) {
        runStepper(0);
        publishState();
      } else {
        JsonVariant brightness = jsonDoc["brightness"];
        // spec is to return to previous position if bri is 0, so no action for us
        if (!brightness.isNull() && brightness.as<int>() > 0) {
          publishState();
          int brightness = jsonDoc["brightness"];
          runStepper(brightness * SCALEFACTOR);
        } else {
          publishState();
        }
      }
    }
  }
}

uint sampleAdc() {
  uint sum = 0;
  uint nSamples = 10;
  delay(50);  // give the cap time to charge all the way up
  for (uint i=0; i < nSamples; i++) {
    sum += analogRead(A0);
  }
  return sum / nSamples;
}

void setup() {
  Serial.begin(9600);
  for (uint8_t i=0; i < 4; ++i) {
    pinMode(coils[i], OUTPUT);
  }
  pinMode(LED_BUILTIN, OUTPUT);
  encoderState = sampleAdc();
  WiFi.begin(SSID, PASSWORD);
  digitalWrite(LED_BUILTIN, LOW);
  while (WiFi.status() != WL_CONNECTED) {
    delay(100);
  }
  digitalWrite(LED_BUILTIN, HIGH);
  mqttClient.setServer(MQTT_SERVER, 1883);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setKeepAlive(120);
  mqttClient.setBufferSize(1024);
}

void loop() {
  lastEncoderState = encoderState;
  encoderState = sampleAdc();
  stateChange = encoderState - lastEncoderState;
  // the rotary encoder makes a voltage divider for the ADC.
  // as the 2 LSB count up, the voltage goes down, and vice-versa:
  // ~750mV, ~550mV, ~350mV, ~0V
  // loops back around every 4 positions.
  // check for reasonable chunks of voltage to account for noise
  if (stateChange >= 500) {
    runStepper(position + 1024);
    encoderState = sampleAdc();
  } else if (stateChange <= -500) {
    runStepper(position - 1024);
    encoderState = sampleAdc();
  } else if (stateChange <= -100) {
    runStepper(position + 1024);
    encoderState = sampleAdc();
  } else if (stateChange >= 100) {
    runStepper(position - 1024);
    encoderState = sampleAdc();
  }
  while (!mqttClient.connected()){
    Serial.println("establishing mqtt connection");
    Serial.printf("state %d\n", mqttClient.state());
    delay(100);
    if (mqttClient.connect("autoblind")) {
      mqttClient.subscribe(STATE_TOPIC);  // status
      mqttClient.subscribe(COMMAND_TOPIC);  //command
      Serial.println("mqtt subscribed");
      publishAutodiscover();
      publishState();
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
      digitalWrite(LED_BUILTIN, HIGH);
      delay(100);
      digitalWrite(LED_BUILTIN, LOW);
      delay(100);
      digitalWrite(LED_BUILTIN, HIGH);
    }
  }
  mqttClient.loop();
  yield();
}