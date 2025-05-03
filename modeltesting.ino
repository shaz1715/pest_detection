#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <arduinoFFT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define TRIGGER_PIN 27  // GPIO 27 used to signal ESP8266
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET     -1  // For I2C OLEDs
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define I2S_WS  15
#define I2S_SD  32
#define I2S_SCK 14

#define SAMPLE_RATE 16000
#define FFT_SIZE    512
#define MEL_BINS    40
#define FRAME_LEN   400
#define FRAME_STEP  160
#define NUM_FRAMES  79
#define NUM_MFCC    13

float fft_input[FFT_SIZE];
float fft_output[FFT_SIZE];
ArduinoFFT<float> FFT = ArduinoFFT<float>(fft_input, fft_output, FFT_SIZE, SAMPLE_RATE);
float mel_energies[MEL_BINS];

// ====== WiFi & Cloud Function Setup ======
const char* ssid = "JioFiber-RAkYy_EXT";
const char* password = "123456789";
const char* cloudFunctionUrl = "https://us-central1-pestdetectmodel.cloudfunctions.net/pestDetection";

// ====== Buffers ======
float audio_buffer[NUM_FRAMES * FRAME_STEP + (FRAME_LEN - FRAME_STEP)];
float mfcc_features[NUM_FRAMES][NUM_MFCC];

// ====== Normalization (from training) ======
float mfcc_mean[NUM_MFCC] = {
  2.1815, 0.3534, -1.0513, -1.1732, -0.9364,
  -0.4737, -0.0425, 0.3658, 0.6834, 0.9238,
  1.0759, 1.1613, 1.2196
};
float mfcc_std[NUM_MFCC] = {
  2.3027, 1.3462, 1.1120, 1.0437, 0.9816,
  0.9340, 0.8843, 0.8620, 0.8611, 0.8834,
  0.9081, 0.9453, 1.0133
};

// ====== Setup WiFi ======
void setupWiFi() {
  Serial.print("🔌 Connecting to WiFi: ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" ✅ Connected!");
  Serial.print("📶 IP Address: ");
  Serial.println(WiFi.localIP());
}

// ====== Setup I2S Mic ======
void setupI2SMic() {
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 256,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  i2s_zero_dma_buffer(I2S_NUM_0);
  Serial.println("🎙 I2S microphone initialized.");
}

// ====== Hamming Window ======
void applyHamming(float* frame, int len) {
  for (int i = 0; i < len; i++) {
    frame[i] *= 0.54 - 0.46 * cos((2 * PI * i) / (len - 1));
  }
}

// ====== Simple Mel Filterbank ======
void computeMelEnergies(float* spectrum, float* mel_out) {
  int start_bin = 1;
  int end_bin = FFT_SIZE / 2;

  for (int m = 0; m < MEL_BINS; m++) {
    float mel_energy = 0;
    int left = start_bin + (m * (end_bin - start_bin)) / MEL_BINS;
    int center = start_bin + ((m + 1) * (end_bin - start_bin)) / MEL_BINS;
    int right = start_bin + ((m + 2) * (end_bin - start_bin)) / MEL_BINS;

    for (int k = left; k < center; k++)
      mel_energy += spectrum[k] * (float)(k - left) / (center - left);
    for (int k = center; k < right; k++)
      mel_energy += spectrum[k] * (float)(right - k) / (right - center);

    mel_out[m] = mel_energy > 1e-6 ? logf(mel_energy) : -10.0f;
  }
}

// ====== DCT Type-II ======
void computeDCT(const float* input, float* output, int inSize, int outSize) {
  for (int k = 0; k < outSize; k++) {
    float sum = 0.0;
    for (int n = 0; n < inSize; n++) {
      sum += input[n] * cos(PI * k * (2 * n + 1) / (2.0 * inSize));
    }
    output[k] = sum;
  }
}

// ====== Get Audio Samples ======
void getSamples(float* buffer, int len) {
  int32_t sample = 0;
  size_t bytesRead;

  Serial.printf("🎧 Capturing %d audio samples...\n", len);
  for (int i = 0; i < len; i++) {
    if (i2s_read(I2S_NUM_0, &sample, sizeof(sample), &bytesRead, portMAX_DELAY) == ESP_OK && bytesRead == sizeof(sample)) {
      sample = sample >> 8;
      buffer[i] = (float)sample / (1 << 23);  // Normalize 24-bit to [-1, 1]
    } else {
      buffer[i] = 0.0f;
    }
  }

  Serial.println("🔍 First 20 samples:");
  for (int i = 0; i < 20; i++) {
    Serial.print(buffer[i], 5);
    Serial.print(i < 19 ? ", " : "\n");
  }
}

// ====== Normalize MFCCs ======
void normalizeMFCCs() {
  for (int i = 0; i < NUM_FRAMES; i++) {
    for (int j = 0; j < NUM_MFCC; j++) {
      mfcc_features[i][j] = (mfcc_features[i][j] - mfcc_mean[j]) / mfcc_std[j];
    }
  }

  int debugFrames[] = {0, 10, 78};
  for (int f = 0; f < 3; f++) {
    int frame = debugFrames[f];
    Serial.printf("🎯 Normalized MFCCs - Frame %d:\n", frame);
    for (int i = 0; i < NUM_MFCC; i++) {
      Serial.print(mfcc_features[frame][i], 4);
      Serial.print(i < NUM_MFCC - 1 ? ", " : "\n");
    }
  }
}

// ====== Map Class ID to Pest Name ======
const char* mapPestName(int id) {
  switch (id) {
    case 0: return "bombus terrestris";
    case 1: return "bradysia difformis";
    case 2: return "coccilena septempunctata";
    case 3: return "myzus persicae";
    case 4: return "nezara viridula";
    case 5: return "palomena prasina";
    case 6: return "trialeurodes vaporariorum";
    case 7: return "tuta absoluta";
    default: return "No pest detected";
  }
}
// ====== Display Result on OLED ======
void showOnOLED(const char* pestName, float confidence) {
  display.clearDisplay();
  display.setCursor(0, 0);

  if (strcmp(pestName, "No pest detected") == 0) {
    display.println("🐛 No pest detected");
  } else {
    display.print("Pest: ");
    display.println(pestName);
    display.print("Conf: ");
    display.print(confidence * 100, 1);
    display.println("%");
  }

  display.display();
}


// ====== Send MFCCs to Cloud Function ======
void sendToCloudFunction() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠ Not connected to WiFi.");
    return;
  }

  Serial.println("🌐 Sending MFCCs to Cloud Function...");

  WiFiClientSecure client;
  client.setInsecure();

  if (!client.connect("us-central1-pestdetectmodel.cloudfunctions.net", 443)) {
    Serial.println("❌ HTTPS connection failed.");
    return;
  }

  StaticJsonDocument<30000> doc;
  JsonArray mfccs = doc.createNestedArray("mfccs");
  for (int i = 0; i < NUM_FRAMES; i++) {
    JsonArray row = mfccs.createNestedArray();
    for (int j = 0; j < NUM_MFCC; j++) {
      row.add(mfcc_features[i][j]);
    }
  }

  String requestBody;
  serializeJson(doc, requestBody);

  client.println("POST /pestDetection HTTP/1.1");
  client.println("Host: us-central1-pestdetectmodel.cloudfunctions.net");
  client.println("Content-Type: application/json");
  client.print("Content-Length: ");
  client.println(requestBody.length());
  client.println();
  client.print(requestBody);

  while (client.connected()) {
    String line = client.readStringUntil('\n');
    if (line == "\r") break;
  }

  String response = client.readStringUntil('\n');
  StaticJsonDocument<512> resDoc;
  DeserializationError err = deserializeJson(resDoc, response);

  if (!err && resDoc.containsKey("predicted_class") && resDoc.containsKey("top_3")) {
    int predicted_class = resDoc["predicted_class"];
    float confidence = resDoc["confidence"] | 0.0;

    const char* pestName;

    if (predicted_class == 2 && confidence < 0.95) {
      pestName = "No pest detected";
      Serial.println("🐛 No pest detected.");
    } else {
      pestName = mapPestName(predicted_class);
      digitalWrite(TRIGGER_PIN, HIGH); // 🔼 Signal ESP8266
      delay(2000); // Keep it HIGH for 2 sec (adjust as needed)
      digitalWrite(TRIGGER_PIN, LOW);  // 🔽 Reset

      Serial.printf("✅ Predicted Class: %d\n", predicted_class);
      Serial.printf("📈 Confidence: %.4f\n", confidence);
      Serial.printf("🐛 Pest Name: %s\n", pestName);
    }

    showOnOLED(pestName, confidence);


    Serial.println("🔝 Top 3 Predictions:");
    JsonArray top3 = resDoc["top_3"];
    for (int i = 0; i < top3.size(); i++) {
      int cls = top3[i];
      Serial.printf("  %d: %s\n", cls, mapPestName(cls));
    }
  } else {
    Serial.println("⚠ Failed to parse response.");
    Serial.println("🔁 Raw response: " + response);
  }
}


// ====== Arduino Setup ======
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(TRIGGER_PIN, OUTPUT);
  digitalWrite(TRIGGER_PIN, LOW);  // default to LOW
  Serial.println("🚀 Starting Pest Detection System...");
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("❌ OLED init failed"));
    while (true);
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("🌿 Pest Detection");
  display.display();
  delay(1000);

  setupWiFi();
  setupI2SMic();
}

// ====== Arduino Loop ======
void loop() {
  Serial.println("🎤 Capturing audio...");
  getSamples(audio_buffer, NUM_FRAMES * FRAME_STEP + (FRAME_LEN - FRAME_STEP));

  Serial.println("🔬 Extracting MFCCs...");
  for (int frame = 0; frame < NUM_FRAMES; frame++) {
    float frame_data[FRAME_LEN];
    int offset = frame * FRAME_STEP;
    memcpy(frame_data, audio_buffer + offset, sizeof(float) * FRAME_LEN);
    applyHamming(frame_data, FRAME_LEN);

    for (int i = 0; i < FFT_SIZE; i++) {
      fft_input[i] = (i < FRAME_LEN) ? frame_data[i] : 0.0;
      fft_output[i] = 0.0;
    }

    FFT.compute(FFT_FORWARD);
    FFT.complexToMagnitude();

    for (int i = 0; i < FFT_SIZE; i++) {
      fft_output[i] = fft_input[i];  // FFT magnitude now stored in input
    }

    computeMelEnergies(fft_output, mel_energies);
    computeDCT(mel_energies, mfcc_features[frame], MEL_BINS, NUM_MFCC);
  }

  normalizeMFCCs();
  sendToCloudFunction();

  delay(5000);  // Wait before next capture
}
