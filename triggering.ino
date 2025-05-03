#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1  // Reset pin not used
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int trigPin = D5;      // HC-SR04 Trigger pin

const int controlPin = D7;   // Control pin (HIGH=ON, LOW=OFF)
bool lastState = LOW;        // Track previous state

void setup() {
  // Initialize pins
  pinMode(trigPin, OUTPUT);
  pinMode(controlPin, INPUT);
  digitalWrite(trigPin, LOW);
  
  // Initialize Serial
  Serial.begin(115200);
  
  // Initialize OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("OLED allocation failed"));
    for(;;); // Don't proceed, loop forever
  }
  
  // Show initial display
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println("Ultrasonic Control");
  display.println("System Ready");
  display.display();
  
  Serial.println("Ultrasonic Sensor Control Ready");
}

void updateDisplay(bool state) {
  display.clearDisplay();
  display.setCursor(0,0);
  display.println("Ultrasonic Control");
  display.println("------------------");
  
  if(state == HIGH) {
    display.println("Status: ACTIVE");
    display.println("Pulses transmitting");
    display.println(""); // Empty line for spacing
    display.println("Pest detected!!");
  } else {
    display.println("Status: INACTIVE");
    display.println("System standby");
    display.println(""); // Empty line for spacing
    display.println("NO Pest detected");
  }
  
  display.display();
}

void loop() {
  bool currentState = digitalRead(controlPin);
  
  // Update display and serial only when state changes
  if(currentState != lastState) {
    if(currentState == HIGH) {
      Serial.println("Sensor ACTIVE - Transmitting pulses");
    } else {
      Serial.println("Sensor INACTIVE");
    }
    updateDisplay(currentState);
    lastState = currentState;
  }

  if(currentState == HIGH) {
    // Send ultrasonic pulse
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);
    
    // HC-SR04 requires 60ms between measurements
    delay(60); 
  }
  // Small delay to prevent OLED flickering
  delay(10);
}
