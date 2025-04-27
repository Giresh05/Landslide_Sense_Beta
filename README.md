# 📄 LandslideSense Documentation

## 🧠 About

**LandslideSense** is an early warning system for landslides.  
It uses ESP32 modules and sensors (vibration, soil moisture, rain) to monitor ground conditions 24/7.  
The sensor data is fed into a hybrid ML model (Autoencoder + LSTM) to detect abnormal patterns and predict potential landslides before they happen.

Even unfinished, the goal is simple: **save lives through tech**.

---

## 🚀 Project Features

- **ESP32 Sensor Nodes**: Continuous real-time data collection.
- **ESP-NOW Protocol**: Efficient, low-latency communication between nodes.
- **WebSocket Transmission**: Fast and reliable server updates.
- **Hybrid Machine Learning**:
  - Autoencoder → anomaly detection
  - LSTM → future trend prediction
- **Continuous Data Flow**: No "event-based" logging — even idle data is recorded for strong model training.

---

## 🛠️ Hardware Requirements

- ESP32 Dev Boards (multiple for a network)
- Sensors:
  - Vibration Sensor
  - Soil Moisture Sensor
  - Rain Sensor
- Power Sources (batteries or USB)
- WiFi access for WebSocket communication (optional, for server upload)

---

## 🧹 Software Requirements

- Arduino IDE (with ESP32 support installed)
- Libraries:
  - Wire
  - Adafruit_GFX
  - MAX30105 (for MAX30100/30102)
  - MPU9250_asukiaaa
  - TinyGPSPlus
  - BlynkSimpleEsp32
  - Any additional libraries for WebSocket Client (like `arduinoWebSockets`)

- Python 3 (for server-side ML model + WebSocket receiver)
- TensorFlow/Keras (for training/using Autoencoder + LSTM)

---

## ⚙️ Setup Guide

1. **Clone this repo**:

   ```bash
   git clone https://github.com/your_username/LandslideSense.git
   cd LandslideSense/docs
   ```

2. **Flash ESP32 Nodes**:
   - Upload the correct Arduino sketch to each node (sensor code provided).
   - Configure MAC addresses if needed for ESP-NOW.

3. **Connect Sensors**:
   - Vibration → Analog pin
   - Soil Moisture → Analog pin
   - Rain Sensor → Digital/Analog pin (depending on your sensor)

4. **Run WebSocket Server (optional)**:
   - Python script available for WebSocket data collection and logging.

5. **Train/Deploy ML Model**:
   - Use collected data to train Autoencoder + LSTM.
   - Deploy lightweight version if needed (for edge devices).

---

## 📊 Data Flow Diagram (Coming Soon)

*(Sensors → ESP32 → WebSocket → Server → ML Model → Alerts)*

---

## ✨ To-Do (WIP)

- [ ] Finalize hardware setup.
- [ ] Optimize WebSocket reliability.
- [ ] Fine-tune ML model.
- [ ] Build simple dashboard for live alerts.
- [ ] (Optional) Solar-powered nodes.

---

## 🤝 Contributions

- PRs, ideas, optimizations — all welcome.
- Open an issue if you find a bug or have a feature request.

---

## 📜 License

MIT License.  
Free to use, modify, and share — just give credits if you make something cool from it. ✨

---

# 🌋 Stay ahead of disasters with tech.
