// Includes
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Wire.h>               // Needed for I2C communication
#include <MPU9250_asukiaaa.h> // Use the asukiaaa library
// #include <driver/adc.h>    // Explicit ADC driver include REMOVED

// --- Configuration ---
const int MAX_NODES = 5;
// --- Node MAC Addresses ---  // Ensure these are correct for your specific ESP32s
uint8_t nodeMacs[MAX_NODES][ESP_NOW_ETH_ALEN] = {
    {0xF4, 0x65, 0x0B, 0x55, 0x2A, 0x74}, // Node 1 MAC
    {0x10, 0x06, 0x1C, 0x82, 0x70, 0x54}, // Node 2 MAC
    {0xF0, 0x24, 0xF9, 0x5A, 0xC8, 0xD4}, // Node 3 MAC
    {0x5C, 0x01, 0x3B, 0x4E, 0x08, 0xFC}, // Node 4 MAC
    {0xF0, 0x24, 0xF9, 0x5A, 0x4A, 0xC4}  // Node 5 MAC
};
uint8_t broadcastMac[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // ESP-NOW Broadcast address

// --- Sensor Pin Definitions ---
const int RAIN_SENSOR_PIN = 34;
const int SOIL_MOISTURE_PIN = 35;

// --- MPU9250 Object (asukiaaa library) ---
MPU9250_asukiaaa mpu; // Use the default constructor. Assumes Wire will be initialized.

// Timings, Thresholds, Channel
const unsigned long DATA_INTERVAL = 5000;   // Send sensor data every 5 seconds
const unsigned long PROBE_INTERVAL = 7000; // Probe higher nodes every 7 seconds
const int FAILOVER_THRESHOLD = 3;       // How many consecutive DATA send failures trigger failover
const int WIFI_CHANNEL = 1;
const int FAILBACK_PROBE_THRESHOLD = 2; // Backup mechanism via probing

// --- Node State ---
typedef enum { ROLE_SENDER, ROLE_RECEIVER } node_role_t;
int myNodeId = 0;
node_role_t myRole = ROLE_SENDER; // Default state before setup determines role
int targetNodeId = 0;
int currentProbeTargetId = 0;
unsigned long lastDataTime = 0;
unsigned long lastProbeTime = 0;
int consecutiveSendFailures = 0;
int lastFailedTargetId = 0;
int consecutiveSuccessfulProbesToHigher = 0;

// --- Message Structure ---
typedef enum { MSG_TYPE_DATA, MSG_TYPE_PROBE, MSG_TYPE_ANNOUNCE_AR } message_type_t;

// Structure to hold sensor data
typedef struct __attribute__((packed)) struct_sensor_data {
    int rain_analog;         // Value from analogRead (floating on 3,4,5)
    int soil_moisture_analog;// Value from analogRead (floating on 3,4,5)
    float acc_x; // g's
    float acc_y; // g's
    float acc_z; // g's
    float gyro_x; // dps
    float gyro_y; // dps
    float gyro_z; // dps
} struct_sensor_data;

// Main message structure embedding sensor data
typedef struct __attribute__((packed)) struct_message {
    message_type_t type;
    uint8_t senderId;
    struct_sensor_data sensor_data;
} struct_message;

struct_message messageToSend; // Reusable buffer for sending messages

// --- Helper Functions ---
void printMacAddress(const uint8_t *mac_addr) {
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    Serial.print(macStr);
}

int getNodeIdFromMac(const uint8_t *mac_addr) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (memcmp(mac_addr, nodeMacs[i], ESP_NOW_ETH_ALEN) == 0) {
            return i + 1; // Node IDs are 1-based
        }
    }
    return 0; // Not found
}

// --- Forward Declarations ---
void becomeActiveReceiver();
void becomeSender(int newTargetId);
void findNewTargetAndFailover();
bool readSensors(struct_sensor_data &s_data); // Forward declare readSensors

// --- Sensor Reading Function (Always read analog, check MPU) ---
bool readSensors(struct_sensor_data &s_data) {
    // --- Always Read Analog Sensors ---
    s_data.rain_analog = analogRead(RAIN_SENSOR_PIN);
    s_data.soil_moisture_analog = analogRead(SOIL_MOISTURE_PIN);

    // --- Attempt to Read MPU9250 ---
    bool accel_read_ok = true;
    bool gyro_read_ok = true;

    if (mpu.accelUpdate() != 0) {
        // Log failure periodically
        static unsigned long lastMpuFailLog = 0;
        if (myNodeId != 0 && (millis() - lastMpuFailLog > DATA_INTERVAL * 2)) { // Avoid logging before ID known
             Serial.printf("!!! [%d] Failed MPU accel update (sensor likely not present).\n", myNodeId);
             lastMpuFailLog = millis();
        }
        s_data.acc_x = s_data.acc_y = s_data.acc_z = 0.0f;
        accel_read_ok = false;
    } else {
        s_data.acc_x = mpu.accelX(); s_data.acc_y = mpu.accelY(); s_data.acc_z = mpu.accelZ();
    }

    if (mpu.gyroUpdate() != 0) {
        // Log failure periodically
        static unsigned long lastGyroFailLog = 0;
         if (myNodeId != 0 && (millis() - lastGyroFailLog > DATA_INTERVAL * 2)) {
            Serial.printf("!!! [%d] Failed MPU gyro update (sensor likely not present).\n", myNodeId);
            lastGyroFailLog = millis();
         }
        s_data.gyro_x = s_data.gyro_y = s_data.gyro_z = 0.0f;
        gyro_read_ok = false;
    } else {
        s_data.gyro_x = mpu.gyroX(); s_data.gyro_y = mpu.gyroY(); s_data.gyro_z = mpu.gyroZ();
    }
    return true; // Always return true as per requirement
}


// --- Core Logic Functions ---
void becomeActiveReceiver() {
    if (myRole == ROLE_RECEIVER) return;

    Serial.println("**************************************");
    Serial.printf(">>> [%d] Transitioning to ACTIVE RECEIVER role.\n", myNodeId);
    Serial.println("**************************************");
    myRole = ROLE_RECEIVER; targetNodeId = 0;
    consecutiveSendFailures = 0; lastFailedTargetId = 0; consecutiveSuccessfulProbesToHigher = 0;
    lastDataTime = millis() - DATA_INTERVAL; lastProbeTime = millis() - PROBE_INTERVAL;

    Serial.printf(">>> [%d] Broadcasting ANNOUNCE_AR.\n", myNodeId);
    messageToSend.type = MSG_TYPE_ANNOUNCE_AR; messageToSend.senderId = myNodeId;
    esp_err_t result = esp_now_send(broadcastMac, (uint8_t *)&messageToSend, sizeof(messageToSend));
    if (result != ESP_OK) { Serial.printf("!!! [%d] Failed ANNOUNCE_AR broadcast: %s\n", myNodeId, esp_err_to_name(result)); }
}

void becomeSender(int newTargetId) {
    if (newTargetId == myNodeId) { Serial.printf("!!! [%d] Logic Error: Target self (%d). Preventing.\n", myNodeId, newTargetId); if (myNodeId != 1) newTargetId = 1; else { becomeActiveReceiver(); return; } }
    if (newTargetId <= 0 || (newTargetId > 4 && newTargetId != 0) ) { Serial.printf("!!! [%d] Logic Error: Target invalid node %d. Defaulting to 1.\n", myNodeId, newTargetId); newTargetId = 1; if (newTargetId == myNodeId) { becomeActiveReceiver(); return;} }
    if (myRole == ROLE_SENDER && targetNodeId == newTargetId) return;

    Serial.println("**************************************");
    Serial.printf(">>> [%d] Transitioning to SENDER role. Targeting Node %d for DATA.\n", myNodeId, newTargetId);
    Serial.println("**************************************");
    myRole = ROLE_SENDER; targetNodeId = newTargetId;
    consecutiveSendFailures = 0; lastFailedTargetId = 0; consecutiveSuccessfulProbesToHigher = 0;
    lastDataTime = millis() - DATA_INTERVAL; lastProbeTime = millis() - PROBE_INTERVAL;
}

void findNewTargetAndFailover() {
    int failedNodeId = lastFailedTargetId;
    if (failedNodeId == 0) { Serial.printf("!!! [%d] FAILOVER Triggered without known failed target. Resetting state.\n", myNodeId); consecutiveSendFailures = 0; lastFailedTargetId = 0; if (myNodeId != 1) becomeSender(1); else becomeActiveReceiver(); return; }

    Serial.printf("!!! [%d] FAILOVER Triggered: Node %d seems down (failed %d times).\n", myNodeId, failedNodeId, consecutiveSendFailures);
    targetNodeId = 0;

    int expectedNewAR = 0;
    for (int potentialAR = failedNodeId + 1; potentialAR <= 4; ++potentialAR) { expectedNewAR = potentialAR; break; }

    if (expectedNewAR == 0) {
         if (myNodeId == 5) { Serial.printf("    [%d] Node 4 failed, no lower AR. Stopping DATA sends.\n", myNodeId); myRole = ROLE_SENDER; targetNodeId = 0; consecutiveSendFailures = 0; lastFailedTargetId = 0; }
         else { Serial.printf("    [%d] CRITICAL ERROR: No expected AR after Node %d failed? Becoming AR.\n", myNodeId, failedNodeId); if (myNodeId <= 4) becomeActiveReceiver(); else becomeSender(1); }
         return;
    }

    if (myNodeId == expectedNewAR) { Serial.printf("    [%d] My turn! Becoming Active Receiver.\n", myNodeId); becomeActiveReceiver(); }
    else if (myNodeId > expectedNewAR || myNodeId == 5) { Serial.printf("    [%d] Node %d should be AR. Targeting it.\n", myNodeId, expectedNewAR); becomeSender(expectedNewAR); }
    else { Serial.printf("    [%d] LOGIC WARNING: My ID (%d) < expectedNewAR (%d) during failover from %d.\n", myNodeId, myNodeId, expectedNewAR, failedNodeId); becomeSender(expectedNewAR); }
}

// --- ESP-NOW Callbacks ---
void OnDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
    int remoteNodeId = getNodeIdFromMac(mac_addr); if (remoteNodeId == 0) return;
    bool isProbeCallback = (remoteNodeId == currentProbeTargetId);
    if (isProbeCallback) {
        currentProbeTargetId = 0; bool isHigherNode = (remoteNodeId < myNodeId);
        if (status == ESP_NOW_SEND_SUCCESS) {
            if (myRole == ROLE_RECEIVER && isHigherNode) {
                consecutiveSuccessfulProbesToHigher++;
                if (consecutiveSuccessfulProbesToHigher >= FAILBACK_PROBE_THRESHOLD) { Serial.printf("    [CB %d] AR confirmed Node %d stable via PROBE! Yielding role (backup).\n", myNodeId, remoteNodeId); becomeSender(remoteNodeId); }
            } else if (myRole == ROLE_SENDER && isHigherNode && (targetNodeId == 0 || targetNodeId > remoteNodeId)) {
                if (targetNodeId != remoteNodeId) { Serial.printf("    [CB %d] Sender detected Higher Node %d via PROBE! Switching target from %d (backup).\n", myNodeId, remoteNodeId, targetNodeId); becomeSender(remoteNodeId); }
                else { if(consecutiveSendFailures > 0) { consecutiveSendFailures = 0; lastFailedTargetId = 0; } }
            } else { if (myRole == ROLE_RECEIVER && !isHigherNode) { if (consecutiveSuccessfulProbesToHigher > 0) { consecutiveSuccessfulProbesToHigher = 0; } } }
        } else { if (myRole == ROLE_RECEIVER && isHigherNode) { if (consecutiveSuccessfulProbesToHigher > 0) { consecutiveSuccessfulProbesToHigher = 0; } } }
    } else {
        if (targetNodeId == 0 || remoteNodeId != targetNodeId) return;
        if (status == ESP_NOW_SEND_SUCCESS) { if (consecutiveSendFailures > 0) { Serial.printf("    [CB %d] Connection to Target Node %d restored.\n", myNodeId, targetNodeId); } consecutiveSendFailures = 0; lastFailedTargetId = 0; }
        else { Serial.printf("!!! [CB %d] Data NACK/Timeout for Target Node %d.\n", myNodeId, targetNodeId); if (targetNodeId == lastFailedTargetId || lastFailedTargetId == 0) { consecutiveSendFailures++; lastFailedTargetId = targetNodeId; } else { consecutiveSendFailures = 1; lastFailedTargetId = targetNodeId; } Serial.printf("    [CB %d] Consecutive failures for Node %d: %d/%d\n", myNodeId, targetNodeId, consecutiveSendFailures, FAILOVER_THRESHOLD); if (consecutiveSendFailures >= FAILOVER_THRESHOLD) { findNewTargetAndFailover(); } }
    }
}

void OnDataRecv(const esp_now_recv_info_t * esp_info, const uint8_t *incomingData, int len) {
    const uint8_t *mac_addr = esp_info->src_addr; int senderNodeId = getNodeIdFromMac(mac_addr); if (senderNodeId == 0) return;

    if (myRole == ROLE_RECEIVER && senderNodeId < myNodeId && senderNodeId >= 1 && senderNodeId <= 4 ) { Serial.printf("<- [%d AR] Yielding! Detected msg from HIGHER priority Node %d! Becoming Sender.\n", myNodeId, senderNodeId); becomeSender(senderNodeId); return; }

    if (len != sizeof(struct_message)) { Serial.printf("<- [CB %d] Corrupted packet (len %d != %d) from Node %d\n", myNodeId, len, sizeof(struct_message), senderNodeId); return; }
    struct_message msg; memcpy(&msg, incomingData, sizeof(msg));
    if (msg.senderId != senderNodeId) { Serial.printf("<- [CB %d] WARN: Packet MAC (%d) != header senderId (%d)\n", myNodeId, senderNodeId, msg.senderId); }

    switch (msg.type) {
        case MSG_TYPE_ANNOUNCE_AR: { int announcedAR = msg.senderId; Serial.printf("<- [%d %s] Received ANNOUNCE_AR from Node %d.\n", myNodeId, (myRole == ROLE_RECEIVER ? "AR" : "SENDER"), announcedAR); if (myRole == ROLE_SENDER) { if (announcedAR >= 1 && announcedAR <= 4 && (targetNodeId == 0 || announcedAR < targetNodeId)) { if (targetNodeId != announcedAR) { Serial.printf("    [%d Sender] Announcer %d > target %d. Switching target.\n", myNodeId, announcedAR, targetNodeId); becomeSender(announcedAR); } } } break; }
        case MSG_TYPE_DATA: {
            if (myRole == ROLE_RECEIVER) {
                Serial.println("----------------------------------------");
                Serial.printf("<- [AR %d] Received SENSOR DATA from Node %d:\n", myNodeId, senderNodeId);
                Serial.printf("   Rain Analog: %d\n", msg.sensor_data.rain_analog); // Raw value printed
                Serial.printf("   Soil Analog: %d\n", msg.sensor_data.soil_moisture_analog); // Raw value printed
                bool mpuDataValid = !(msg.sensor_data.acc_x == 0.0f && msg.sensor_data.acc_y == 0.0f && msg.sensor_data.acc_z == 0.0f && msg.sensor_data.gyro_x == 0.0f && msg.sensor_data.gyro_y == 0.0f && msg.sensor_data.gyro_z == 0.0f);
                if (mpuDataValid) {
                    Serial.printf("   Acc (X,Y,Z): %.3f, %.3f, %.3f g\n", msg.sensor_data.acc_x, msg.sensor_data.acc_y, msg.sensor_data.acc_z);
                    Serial.printf("   Gyro(X,Y,Z): %.2f, %.2f, %.2f dps\n", msg.sensor_data.gyro_x, msg.sensor_data.gyro_y, msg.sensor_data.gyro_z);
                } else {
                    if (senderNodeId >= 3 && senderNodeId <= 5) { Serial.println("   MPU Data: [Sensor not present/read failed on sender]"); }
                    else { Serial.println("   MPU Data: [Read Failure or Sensor Error on sender]"); }
                }
                Serial.println("----------------------------------------");
            } else { Serial.printf("<- [SENDER %d] *** UNEXPECTED *** DATA received from Node %d. Target: %d. Ignoring.\n", myNodeId, senderNodeId, targetNodeId); }
            break;
        }
        case MSG_TYPE_PROBE: { break; }
        default: { Serial.printf("<- [%s %d] Received unknown message type %d from Node %d\n", (myRole == ROLE_RECEIVER ? "AR" : "SENDER"), myNodeId, msg.type, senderNodeId); break; }
    }
}

// --- Setup ---
void setup() {
    Serial.begin(115200);
    while (!Serial);
    Serial.println("\n\n=====================================");
    Serial.println("ESP-NOW HA Sensor Network (vHA-Sensor-asukiaaa-SendJunkAnalog)"); // Version name
    Serial.println("=====================================");

    WiFi.mode(WIFI_STA);

    // --- Analog Pin / ADC Configuration --- // REMOVED explicit config
    Serial.println("Using default analogRead() configuration.");

    // --- MPU9250 Initialization ---
    Wire.begin();
    mpu.beginAccel();
    Serial.println("MPU Accelerometer Initialized (assuming success).");
    mpu.beginGyro();
    Serial.println("MPU Gyroscope Initialized (assuming success).");
    delay(100);

    // --- Network Setup ---
    uint8_t myMac[ESP_NOW_ETH_ALEN]; esp_wifi_get_mac(WIFI_IF_STA, myMac); myNodeId = getNodeIdFromMac(myMac);
    Serial.print("Device MAC Address: "); printMacAddress(myMac); Serial.println();
    if (myNodeId == 0) { Serial.println("!!! FATAL: MAC not found. Halting."); while (true) delay(1000); }
    Serial.printf("Identified as Node %d\n", myNodeId);

    // --- Initial sensor read check ---
    if (myNodeId == 1 || myNodeId == 2) { // Assuming sensors only on 1 & 2
         struct_sensor_data initial_read; readSensors(initial_read);
        bool mpu_init_ok = !(initial_read.acc_x == 0.0f && initial_read.acc_y == 0.0f && initial_read.acc_z == 0.0f);
        if (!mpu_init_ok) { Serial.println("!!! WARNING: Initial MPU read failed on this node!"); }
        else { Serial.println("   Initial sensor read successful on this node."); }
    } else {
        Serial.println("   No MPU sensor expected on this node - will read floating analog pins.");
    }

    // --- Continue Network Setup ---
    if(esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE) == ESP_OK) { Serial.printf("WiFi Channel set to %d\n", WIFI_CHANNEL); }
    else { Serial.printf("!!! Failed to set WiFi Channel to %d.\n", WIFI_CHANNEL); }
    if (esp_now_init() != ESP_OK) { Serial.println("!!! FATAL: ESP-NOW init failed. Restarting..."); delay(1000); ESP.restart(); }
    Serial.println("ESP-NOW Initialized Successfully.");
    esp_now_register_send_cb(OnDataSent);
    if (esp_now_register_recv_cb(OnDataRecv) != ESP_OK) { Serial.println("!!! FATAL: ESP-NOW recv CB failed. Restarting..."); delay(1000); ESP.restart(); }
    Serial.println("ESP-NOW Callbacks Registered.");
    // Add Broadcast Peer
    esp_now_peer_info_t peerInfo = {}; memcpy(peerInfo.peer_addr, broadcastMac, ESP_NOW_ETH_ALEN); peerInfo.channel = WIFI_CHANNEL; peerInfo.encrypt = false; peerInfo.ifidx = WIFI_IF_STA;
    if (esp_now_add_peer(&peerInfo) != ESP_OK && esp_now_is_peer_exist(broadcastMac) == false){ Serial.println("!!! Failed to add broadcast peer"); }
    else { Serial.println("   > Added broadcast peer (or it already existed)"); }
    // Add Unicast Peers
    Serial.println("Adding Unicast ESP-NOW Peers:"); int peers_added = 0;
    for (int i = 0; i < MAX_NODES; ++i) {
        if ((i + 1) != myNodeId) {
            esp_now_peer_info_t unicastPeerInfo = {}; memcpy(unicastPeerInfo.peer_addr, nodeMacs[i], ESP_NOW_ETH_ALEN); unicastPeerInfo.channel = WIFI_CHANNEL; unicastPeerInfo.encrypt = false; unicastPeerInfo.ifidx = WIFI_IF_STA;
            esp_err_t addStatus = esp_now_add_peer(&unicastPeerInfo);
            if (addStatus != ESP_OK && addStatus != ESP_ERR_ESPNOW_EXIST) { Serial.printf("   !!! Failed to add peer: Node %d (%s)\n", i + 1, esp_err_to_name(addStatus)); }
            else { if (addStatus == ESP_OK) { Serial.printf("   > Added unicast peer: Node %d\n", i + 1); peers_added++; } else { Serial.printf("   * Unicast peer already exists: Node %d\n", i + 1); } }
        }
    }
     Serial.printf("Finished adding peers. Added %d new unicast peers.\n", peers_added);
    // Set Initial Role
    if (myNodeId == 1) { becomeActiveReceiver(); } else { becomeSender(1); }
    Serial.println("--- Setup Complete --- Entering Main Loop ---");
}

// --- Main Loop ---
void loop() {
    unsigned long now = millis();

    // Action 1: Data Generation / Sending
    if (now - lastDataTime >= DATA_INTERVAL) {
        lastDataTime = now;
        if (myRole == ROLE_RECEIVER) {
            Serial.printf("-> [AR %d] Generating local data. Timestamp: %lu\n", myNodeId, now);
             struct_sensor_data localSensorData;
             readSensors(localSensorData);
             Serial.println("   AR Local Sensor Readings:");
             Serial.printf("   Rain Analog: %d\n", localSensorData.rain_analog); // Raw value
             Serial.printf("   Soil Analog: %d\n", localSensorData.soil_moisture_analog); // Raw value
             Serial.printf("   Acc (X,Y,Z): %.3f, %.3f, %.3f g\n", localSensorData.acc_x, localSensorData.acc_y, localSensorData.acc_z);
             Serial.printf("   Gyro(X,Y,Z): %.2f, %.2f, %.2f dps\n", localSensorData.gyro_x, localSensorData.gyro_y, localSensorData.gyro_z);
             // TODO: Process local sensor data for the AR
        } else { // ROLE_SENDER
            if (targetNodeId != 0) {
                messageToSend.type = MSG_TYPE_DATA; messageToSend.senderId = myNodeId;
                readSensors(messageToSend.sensor_data); // Read whatever is available
                uint8_t *targetMac = nodeMacs[targetNodeId - 1];
                esp_err_t result = esp_now_send(targetMac, (uint8_t *)&messageToSend, sizeof(messageToSend));
                if (result != ESP_OK) { Serial.printf("!!! [%d] esp_now_send(DATA to %d) failed immediately: %s\n", myNodeId, targetNodeId, esp_err_to_name(result)); }
            }
        }
    }

    // Action 2: Probing Node 1
    if (currentProbeTargetId == 0 && (now - lastProbeTime >= PROBE_INTERVAL)) {
       lastProbeTime = now; int nodeToProbe = 0; if (myNodeId > 1) nodeToProbe = 1;
       if (nodeToProbe != 0) {
           messageToSend.type = MSG_TYPE_PROBE; messageToSend.senderId = myNodeId;
           uint8_t *probeTargetMac = nodeMacs[nodeToProbe - 1];
           esp_err_t result = esp_now_send(probeTargetMac, (uint8_t *)&messageToSend, sizeof(messageToSend));
           if (result == ESP_OK) { currentProbeTargetId = nodeToProbe; } else { currentProbeTargetId = 0; }
       }
    }
    // delay(1);
}