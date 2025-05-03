# acoustic-pest-detection
Trains an ML model to detect and identify pests based on their audio. 
Trained a model on tensorflow with audio dataset obtained from Kaggle.
Extracted MFCCs for the model to be trained on.
Deployed model onto ESP32, used Arduino IDE to deploy and detect the pest, print predicted pest and confidence on OLED.
ESP32 sends high signal to ESP8226 depending on status of pest.
ESP8226 triggers ultrasonic sensor to deter the pest if detected, prints on OLED correspondingly.
HCSR04 (ultrasonic sensor) deters pest by emitting high frequency sound.
