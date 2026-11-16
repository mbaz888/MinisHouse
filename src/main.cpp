#include <Arduino.h>
#include <esp_arduino_version.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Update.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>

#include "secrets.h"

// -------------------------------------------------------------------------
// CONFIGURACIÓN DE PINES Y HARDWARE
// -------------------------------------------------------------------------
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// the LED is at pin  GPIO10
#define LED_PIN 10

#define ONE_WIRE_BUS 4        // Pin datos sensores DS18B20
#define HEATER1_PIN  3        // Pin Gate Mosfet 1
#define HEATER2_PIN  2        // Pin Gate Mosfet 2
#define PRESENCE1_PIN  6      // Pin sensor de presencia 1
#define PRESENCE2_PIN  7      // Pin sensor de presencia 2

// Configuración OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Configuración Sensores
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DeviceAddress sensor1Addr, sensor2Addr; // Direcciones detectadas dinámicamente

// Configuración Web Server
WebServer server(80);

// Configuración Preferencias (almacenamiento NVS)
Preferences preferences;



// -------------------------------------------------------------------------
// VARIABLES GLOBALES Y ESTADO
// -------------------------------------------------------------------------
float currentTemp1 = 0.0;
float currentTemp2 = 0.0;
int targetTemp1 = 30;     // Temperatura objetivo inicial
int targetTemp2 = 30;     // Temperatura objetivo inicial
bool systemEnabled = false;   // Estado global del sistema (ON/OFF)
bool heater1Enabled = false;  // Estado del calentador 1 (ON/OFF)
bool heater2Enabled = false;  // Estado del calentador 2 (ON/OFF)
int threshold = 1;          // Umbral de histéresis para evitar ciclos rápidos

// Configuración PWM (Arduino-ESP32 core 3.x: ledcAttach/ledcWrite/ledcRead trabajan directamente sobre el pin)
const int freq = 1000;
const int resolution = 8; // 8 bit (0-255)
bool heating1 = false;
bool heating2 = false;
bool presenceSensor1 = true;
bool presenceSensor2 = false;

// Tiempo acumulado de calentamiento (se muestra en la web y se reinicia
// únicamente con una acción manual sobre el calentador o el sistema)
long heatingTime1 = 0;
long heatingTime2 = 0;

// Tiempo de calentamiento continuo (controla MAX_HEATING_TIME y se reinicia
// cada vez que el calentador deja de calentar, aunque siga habilitado)
long continuousHeatingTime1 = 0;
long continuousHeatingTime2 = 0;

// Umbral de tiempo máximo de calentamiento continuo (30 minutos en milisegundos)
const long MAX_HEATING_TIME = 30 * 60 * 1000; // 30 min = 1,800,000 ms
bool heatingTime1Warning = false;
bool heatingTime2Warning = false;

// Temporizadores para no bloquear el loop
unsigned long lastTempRequest = 0;
const unsigned long tempInterval = 1000; // Leer temperatura cada 1s
unsigned long lastOledUpdate = 0;
const unsigned long oledInterval = 500;  // Actualizar pantalla cada 500ms

// -------------------------------------------------------------------------
// INTERFAZ WEB (HTML/CSS/JS)
// -------------------------------------------------------------------------
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE HTML><html>
<head>
  <meta charset="UTF-8">
  <title>MinisHouse Heater Control</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial, sans-serif; background-color: #121212; color: #e0e0e0; text-align: center; }
    h2 { color: #03a9f4; }
    .card { background-color: #1e1e1e; max-width: 500px; margin: 20px auto; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.5); }
    .sensor-val { font-size: 2.5rem; font-weight: bold; color: #ff9800; }
    .input-group { margin: 15px 0; }
    input[type=number] { padding: 10px; width: 80px; font-size: 1.2rem; border-radius: 5px; border: none; }
    button { padding: 10px 20px; font-size: 1rem; cursor: pointer; background-color: #03a9f4; color: white; border: none; border-radius: 5px; margin: 5px; }
    button.save { background-color: #4CAF50; }
    button.off { background-color: #f44336; }
    .switch-container { margin: 20px; }
    /* OTA Style */
    #ota-section { margin-top: 30px; border-top: 1px solid #444; padding-top: 20px; }
    /* Cat Icon */
    .cat-icon { font-size: 2rem; display: inline-block; vertical-align: middle; margin-left: 10px; }
    /* Warning Style */
    .warning-msg { background-color: #ff9800; color: #000; padding: 10px; border-radius: 5px; margin: 10px 0; font-weight: bold; display: none; }
    .warning-icon { color: #ff9800; font-size: 1.5rem; }
  </style>
</head>
<body>
  <h2>Calefaccion MinisHouse</h2>
  
  <div class="card">
    <div class="switch-container">
      <span id="sysStatus" style="font-weight:bold; color:red">OFF</span>
      <br><br>
      <button id="btnToggle" onclick="toggleSystem()">ENCENDER / APAGAR</button>
    </div>
  </div>

  <div class="card">
    <h3>Calentador 1 <span class="cat-icon" id="cat1" style="display:none;">&#128049;</span></h3>
    <div class="warning-msg" id="warning1">
      <span class="warning-icon">⚠️</span> ADVERTENCIA: Calentamiento continuo > 30 minutos!
    </div>
    <div class="sensor-val"><span id="t1" style="color: #ff9800;">-- &deg;C</span></div>
    <div class="input-group">
      Objetivo: <input type="number" id="set1" step="1" value="30">
    </div>
    <div class="pwm-val"> <span id="heater1"></span>  Tiempo: <span id="time1">0</span></div>
    <div class="switch-container">
      <span id="heater1Status" style="font-weight:bold;">--</span>
      <br>
      <button id="btnHeater1" onclick="toggleHeater1()">Habilitar/Deshabilitar</button>
    </div>
  </div>

  <div class="card">
    <h3>Calentador 2 <span class="cat-icon" id="cat2" style="display:none;">&#128049;</span></h3>
    <div class="warning-msg" id="warning2">
      <span class="warning-icon">⚠️</span> ADVERTENCIA: Calentamiento continuo > 30 minutos!!
    </div>
    <div class="sensor-val"><span id="t2" style="color: #ff9800;">-- &deg;C</span></div>
    <div class="input-group">
      Objetivo: <input type="number" id="set2" step="1" value="30">
    </div>
    <div class="pwm-val"> <span id="heater2"></span>  Tiempo: <span id="time2">0</span></div>
    <div class="switch-container">
      <span id="heater2Status" style="font-weight:bold;">--</span>
      <br>
      <button id="btnHeater2" onclick="toggleHeater2()">Habilitar/Deshabilitar</button>
    </div>
  </div>

  <div class="card">
    <button class="save" onclick="saveSettings()">Guardar Cambios</button>
  </div>

  <div class="card" id="ota-section">
    <h3>Actualizar Firmware (OTA)</h3>
    <form method='POST' action='/update' enctype='multipart/form-data'>
      <input type='file' name='update'>
      <button type='submit'>Subir .bin</button>
    </form>
  </div>

<script>
  //setInterval(getData, 5000);
  getData(); // Llamada inicial

  function getData() {
    fetch('/status').then(response => response.json()).then(data => {

      const t1Element = document.getElementById("t1");
      const t2Element = document.getElementById("t2");
      t1Element.innerHTML = data.t1.toFixed(1) + " &deg;C";
      t2Element.innerHTML = data.t2.toFixed(1) + " &deg;C";
      document.getElementById("set1").value = data.target1.toFixed(1);
      document.getElementById("set2").value = data.target2.toFixed(1);
      //document.getElementById("heater1").innerHTML = data.heater1 ? "ON" : "OFF";
      //document.getElementById("heater2").innerHTML = data.heater2 ? "ON" : "OFF";
      document.getElementById("time1").innerHTML = data.time1;
      document.getElementById("time2").innerHTML = data.time2;

      const statusSpan = document.getElementById("sysStatus");
      const btn = document.getElementById("btnToggle");
      if(data.enabled) {
        statusSpan.innerHTML = "ENCENDIDO";
        statusSpan.style.color = "#4CAF50";
        btn.classList.add("off");
      } else {
        statusSpan.innerHTML = "APAGADO";
        statusSpan.style.color = "#f44336";
        btn.classList.remove("off");
      }

      // Actualizar color de t1 según si está calentando
      if(data.heater1) {
        t1Element.style.color = "#f44336"; // Rojo - calentando
      } else {
        t1Element.style.color = "#2196F3"; // Azul - no calentando
      }

      // Actualizar color de t2 según si está calentando
      if(data.heater2) {
        t2Element.style.color = "#f44336"; // Rojo - calentando
      } else {
        t2Element.style.color = "#2196F3"; // Azul - no calentando
      }

      // Actualizar estado Calentador 1
      const heater1StatusSpan = document.getElementById("heater1Status");
      const btnHeater1 = document.getElementById("btnHeater1");
      if(data.heater1Enabled) {
        heater1StatusSpan.innerHTML = "HABILITADO";
        heater1StatusSpan.style.color = "#4CAF50";
        btnHeater1.classList.add("off");
      } else {
        heater1StatusSpan.innerHTML = "DESHABILITADO";
        heater1StatusSpan.style.color = "#f44336";
        btnHeater1.classList.remove("off");
      }

      // Actualizar estado Calentador 2
      const heater2StatusSpan = document.getElementById("heater2Status");
      const btnHeater2 = document.getElementById("btnHeater2");
      if(data.heater2Enabled) {
        heater2StatusSpan.innerHTML = "HABILITADO";
        heater2StatusSpan.style.color = "#4CAF50";
        btnHeater2.classList.add("off");
      } else {
        heater2StatusSpan.innerHTML = "DESHABILITADO";
        heater2StatusSpan.style.color = "#f44336";
        btnHeater2.classList.remove("off");
      }

      // Actualizar icono de gato para presencia
      const cat1 = document.getElementById("cat1");
      const cat2 = document.getElementById("cat2");
      cat1.style.display = data.presenceSensor1 ? "inline-block" : "none";
      cat2.style.display = data.presenceSensor2 ? "inline-block" : "none";

      // Actualizar advertencias de tiempo de calentamiento
      const warning1 = document.getElementById("warning1");
      const warning2 = document.getElementById("warning2");
      warning1.style.display = data.warning1 ? "block" : "none";
      warning2.style.display = data.warning2 ? "block" : "none";
    });
  }

  function saveSettings() {
    const s1 = document.getElementById("set1").value;
    const s2 = document.getElementById("set2").value;
    fetch('/set?t1=' + s1 + '&t2=' + s2).then(res => alert("Guardado!"));
    location.reload();
  }

  function toggleSystem() {
    fetch('/toggle').then(res => getData()); // Refrescar inmediato
    location.reload();
  }

  function toggleHeater1() {
    fetch('/toggleHeater1').then(res => getData()); // Refrescar inmediato
    location.reload();
  }

  function toggleHeater2() {
    fetch('/toggleHeater2').then(res => getData()); // Refrescar inmediato
    location.reload();
  }

</script>
</body>
</html>
)rawliteral";

// -------------------------------------------------------------------------
// FUNCIONES DE CONTROL (LÓGICA TÉRMICA)
// -------------------------------------------------------------------------

// Función simple de control proporcional para el PWM
bool calculateHeating(float current, int target, bool heating) {
  if (!systemEnabled || current == -127.0) return false; // Seguridad

  //Serial.printf("Calefacción : %s\n", heating ? "ON" : "OFF");
  //Serial.printf("Temperatura actual: %.2f °C, Objetivo: %d °C\n", current, target); 
  if (heating) {
    
    if (current >= target + threshold) {
      //Serial.println("Apagando calefacción");
      return false; // Apagar si supera el objetivo + umbral
    }
    //Serial.println("Manteniendo calefacción");
    return true;
  } else {
    if (current <= target - threshold) {
      //Serial.println("Encendiendo calefacción");
      return true; // Encender si está por debajo del objetivo - umbral
    }
    //Serial.println("Calefacción apagada");
    return false;
    
  }


  // float error = target - current;
  
  // // Banda Proporcional:
  // // Si falta mucho (> 5 grados), 100% potencia.
  // // Si falta poco, reducir potencia proporcionalmente.
  // if (error > 5.0) return 255;
  // if (error > 0) {
  //   // Escala el error de 0-5 grados a 0-255 PWM
  //   return (int)(error * 51.0); // 51 * 5 ~= 255
  // }
  //return 0;
}

// -------------------------------------------------------------------------
// HANDLERS DEL SERVIDOR WEB
// -------------------------------------------------------------------------

void handleRoot() {
  server.send(200, "text/html", index_html);
}

void handleStatus() {
  // Devolver JSON con los datos actuales para AJAX
  String json = "{";
  json += "\"fw\":\"" ESP_ARDUINO_VERSION_STR "\",";
  json += "\"t1\":" + String(currentTemp1) + ",";
  json += "\"t2\":" + String(currentTemp2) + ",";
  json += "\"target1\":" + String(targetTemp1) + ",";
  json += "\"target2\":" + String(targetTemp2) + ",";
  json += "\"heater1\":" + String(heating1 && heater1Enabled ? "true" : "false") + ",";
  json += "\"heater2\":" + String(heating2 && heater2Enabled ? "true" : "false") + ",";
  json += "\"enabled\":" + String(systemEnabled ? "true" : "false") + ",";
  json += "\"heater1Enabled\":" + String(heater1Enabled ? "true" : "false") + ",";
  json += "\"heater2Enabled\":" + String(heater2Enabled ? "true" : "false") + ",";
  json += "\"presenceSensor1\":" + String(presenceSensor1 ? "true" : "false") + ",";
  json += "\"presenceSensor2\":" + String(presenceSensor2 ? "true" : "false");

  // Convert time to string with format mm ss
  int min1 = heatingTime1 / 1000 / 60;
  int hour1 = heatingTime1 / 1000 / 3600;
  int min2 = heatingTime2 / 1000 / 60;
  int hour2 = heatingTime2 / 1000 / 3600;
  json += ",\"time1\":\"" + String(hour1) + "h" + String(min1 % 60) + "m\"";
  json += ",\"time2\":\"" + String(hour2) + "h" + String(min2 % 60) + "m\"";
  json += ",\"warning1\":" + String(heatingTime1Warning ? "true" : "false");
  json += ",\"warning2\":" + String(heatingTime2Warning ? "true" : "false");

  json += "}";

  server.send(200, "application/json", json);
}

void handleSet() {
  Serial.println("Actualizando temperaturas objetivo");
  if (server.hasArg("t1")) targetTemp1 = server.arg("t1").toInt();
  if (server.hasArg("t2")) targetTemp2 = server.arg("t2").toInt();

  // Guardar en NVS
  preferences.begin("minis_house", false);
  preferences.putUInt("temperature1", targetTemp1);
  preferences.putUInt("temperature2", targetTemp2);
  preferences.end();

  server.send(200, "text/plain", "OK");

}

void handleToggle() {
  Serial.println("Toggling system state");
  systemEnabled = !systemEnabled;

  // Calcular PWM basado en las temperaturas leídas
  heating1 = calculateHeating(currentTemp1, targetTemp1, heating1);
  heating2 = calculateHeating(currentTemp2, targetTemp2, heating2);

  // Acción manual: reiniciar tiempos acumulados y continuos, y retirar avisos
  heatingTime1 = 0;
  heatingTime2 = 0;
  continuousHeatingTime1 = 0;
  continuousHeatingTime2 = 0;
  heatingTime1Warning = false;
  heatingTime2Warning = false;

  if (heater1Enabled && heating1)
    ledcWrite(HEATER1_PIN, 255);
  else
    ledcWrite(HEATER1_PIN, 0);

  if (heater2Enabled && heating2)
    ledcWrite(HEATER2_PIN, 255);
  else
    ledcWrite(HEATER2_PIN, 0);

  preferences.begin("minis_house", false);
  preferences.putBool("system_enabled", systemEnabled); 
  preferences.end();

  server.send(200, "text/plain", systemEnabled ? "ON" : "OFF");
}

void handleToggleHeater1() {
  Serial.println("Toggling heater 1 state");
  heater1Enabled = !heater1Enabled;

  // Acción manual: reiniciar el tiempo acumulado y la racha continua,
  // y retirar la advertencia inmediatamente
  heatingTime1 = 0;
  continuousHeatingTime1 = 0;
  heatingTime1Warning = false;

  // Aplicar inmediatamente el cambio
  heating1 = calculateHeating(currentTemp1, targetTemp1, heating1);
  
  if (heater1Enabled && heating1)
    ledcWrite(HEATER1_PIN, 255);
  else
    ledcWrite(HEATER1_PIN, 0);

  // Guardar en NVS
  preferences.begin("minis_house", false);
  preferences.putBool("heater1_enabled", heater1Enabled);
  preferences.end();

  server.send(200, "text/plain", heater1Enabled ? "ON" : "OFF");
}

void handleToggleHeater2() {
  Serial.println("Toggling heater 2 state");
  heater2Enabled = !heater2Enabled;

  // Acción manual: reiniciar el tiempo acumulado y la racha continua,
  // y retirar la advertencia inmediatamente
  heatingTime2 = 0;
  continuousHeatingTime2 = 0;
  heatingTime2Warning = false;

  // Aplicar inmediatamente el cambio
  heating2 = calculateHeating(currentTemp2, targetTemp2, heating2);
  
  if (heater2Enabled && heating2)
    ledcWrite(HEATER2_PIN, 255);
  else
    ledcWrite(HEATER2_PIN, 0);

  // Guardar en NVS
  preferences.begin("minis_house", false);
  preferences.putBool("heater2_enabled", heater2Enabled);
  preferences.end();

  server.send(200, "text/plain", heater2Enabled ? "ON" : "OFF");
}

// -------------------------------------------------------------------------
// SETUP
// -------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);

  pinMode(LED_PIN, OUTPUT);
  pinMode(PRESENCE1_PIN, INPUT_PULLUP);
  pinMode(PRESENCE2_PIN, INPUT_PULLUP);   

  digitalWrite(LED_PIN, HIGH);   // flash LED
  delay(200);
  digitalWrite(LED_PIN, LOW);  
  delay(200);                     
  digitalWrite(LED_PIN, HIGH);   
  delay(200); 
  digitalWrite(LED_PIN, LOW);  
  delay(200);


  Serial.println();
  Serial.println("Iniciando calentadores MinisHouse...");  

  // Cargar temperaturas objetivo desde NVS
  preferences.begin("minis_house", false);
  targetTemp1 = preferences.getUInt("temperature1", 30);
  targetTemp2 = preferences.getUInt("temperature2", 30);
  systemEnabled = preferences.getBool("system_enabled", false);
  heater1Enabled = preferences.getBool("heater1_enabled", false);
  heater2Enabled = preferences.getBool("heater2_enabled", false);
  preferences.end();
  Serial.print("Temperaturas objetivo cargadas: T1=");
  Serial.print(targetTemp1);  
  Serial.print(" T2=");
  Serial.println(targetTemp2);


  //1. Iniciar OLED
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) { 
    Serial.println(F("Fallo OLED"));
  } else {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(WHITE);
    display.setCursor(0,0);
    display.println("Iniciando Sistema...");
    display.display();
  }

 
  // 2. Iniciar Sensores
  sensors.begin();
  int deviceCount = sensors.getDeviceCount();
  Serial.print("Sensores encontrados: ");
  Serial.println(deviceCount);
  
  // Intentar obtener direcciones automáticamente
  if(deviceCount >= 1) sensors.getAddress(sensor1Addr, 0);
  if(deviceCount >= 2) sensors.getAddress(sensor2Addr, 1);

  Serial.print("Sensor 1 Address: ");
  for (uint8_t i = 0; i < 8; i++) {
    Serial.print(sensor1Addr[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
  Serial.print("Sensor 2 Address: ");
  for (uint8_t i = 0; i < 8; i++) {
    Serial.print(sensor2Addr[i], HEX);
    Serial.print(" ");
  }   
  Serial.println();
  
  // IMPORTANTE: Hacer lecturas asíncronas para no bloquear el servidor web
  sensors.setWaitForConversion(false); 
  sensors.requestTemperatures(); 
  lastTempRequest = millis();

  // 3. Configurar PWM (MOSFETs) - API Arduino-ESP32 3.x (ledcAttach reemplaza a ledcSetup + ledcAttachPin)
  ledcAttach(HEATER1_PIN, freq, resolution);
  ledcAttach(HEATER2_PIN, freq, resolution);

  // 4. Conectar WiFi
  WiFi.begin(ssid, password);
  display.setCursor(0, 15);
  display.println("Conectando WiFi...");
  display.display();


  int tries = 20;
  while (WiFi.status() != WL_CONNECTED ) {
    Serial.print(".");
    digitalWrite(LED_PIN, HIGH);   // flash LED
    delay(200);
    digitalWrite(LED_PIN, LOW);  
    delay(200);      

  }
  
  Serial.println("");
  Serial.println("WiFi conectado.");
  Serial.println("IP: " + WiFi.localIP().toString());

  // 5. Rutas Web Server
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/set", handleSet);
  server.on("/toggle", handleToggle);
  server.on("/toggleHeater1", handleToggleHeater1);
  server.on("/toggleHeater2", handleToggleHeater2);

  // Manejo de OTA (Actualización de Firmware)
  server.on("/update", HTTP_POST, []() {
    // server.sendHeader("Connection", "close");
    // server.send(200, "text/plain", (Update.hasError()) ? "FAIL, Restaring" : "OK, Restarting");
    // ESP.restart();

          // handler when file upload finishes
    if (Update.hasError()) {
        server.send(200, F("text/html"), String(F("<META http-equiv=\"refresh\" content=\"5;URL=/\">Update error: ")) + String(Update.errorString()));
    } else {
        server.client().setNoDelay(true);
        server.send(200, PSTR("text/html"), String(F("<META http-equiv=\"refresh\" content=\"15;URL=/\">Update Success! Rebooting...")));
        delay(100);
        server.client().stop();
        ESP.restart();
    }
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Serial.printf("Update: %s\n", upload.filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { 
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
        Update.printError(Serial);
      }
    } else if (upload.status == UPLOAD_FILE_END) {
      if (Update.end(true)) { 
        Serial.printf("Success: %u\nRebooting...\n", upload.totalSize);
      } else {
        Update.printError(Serial);
      }
    }
  });

  server.begin();
}

// -------------------------------------------------------------------------
// LOOP PRINCIPAL
// -------------------------------------------------------------------------
void loop() {

  server.handleClient();
  unsigned long currentMillis = millis();

#ifdef SENSOR_SEARCH
  byte i;
  byte addr[8];
  Serial.print("Bus de sensores: ");
  if (!oneWire.search(addr)) {
    Serial.println(" No more addresses.");
    Serial.println();
    oneWire.reset_search();
    delay(250);
    return;
  }
  Serial.print(" ROM =");
  for (i = 0; i < 8; i++) {
    Serial.write(' ');
    Serial.print(addr[i], HEX);
  }
 #endif

  // 1. Lectura de Sensores (No bloqueante)
  // DS18B20 necesita ~750ms para 12-bit. Leemos cada 1s.
  if (currentMillis - lastTempRequest >= tempInterval) {
    // Leer valores de la petición anterior
    float t1 = sensors.getTempC(sensor1Addr);
    float t2 = sensors.getTempC(sensor2Addr);
    
    // Filtrar errores (-127 es desconectado)
    if(t1 > -100) currentTemp1 = t1; 
    if(t2 > -100) currentTemp2 = t2;

    //Serial.print("Temperaturas: H1="); Serial.print(currentTemp1); Serial.print(" H2="); Serial.println(currentTemp2);

    // Lanzar nueva petición para la siguiente vuelta
    sensors.requestTemperatures();
    lastTempRequest = currentMillis;

    // Calcular PWM basado en las temperaturas leídas
    heating1 = calculateHeating(currentTemp1, targetTemp1, heating1);
    heating2 = calculateHeating(currentTemp2, targetTemp2, heating2);

    // HEATER 1: Control y monitorización de tiempo
    if (heater1Enabled && heating1) {
      ledcWrite(HEATER1_PIN, 255);
      heatingTime1 += tempInterval;            // Acumulado para la web
      continuousHeatingTime1 += tempInterval;  // Racha continua para el límite

      // Verificar si se excede el tiempo máximo de calentamiento continuo
      if (continuousHeatingTime1 >= MAX_HEATING_TIME) {
        if (!heatingTime1Warning) {
          heatingTime1Warning = true;
          Serial.println("⚠️  ADVERTENCIA: Calentador 1 ha estado funcionando continuamente por más de 30 minutos!");
        }
      }
    } else {
      ledcWrite(HEATER1_PIN, 0);
      // La racha continua se rompe al dejar de calentar, pero el tiempo
      // acumulado se conserva hasta una acción manual
      continuousHeatingTime1 = 0;
      heatingTime1Warning = false;
      if (!heater1Enabled) heatingTime1 = 0; // Deshabilitado manualmente: reiniciar acumulado
    }

    // HEATER 2: Control y monitorización de tiempo
    if (heater2Enabled && heating2) {
      ledcWrite(HEATER2_PIN, 255);
      heatingTime2 += tempInterval;            // Acumulado para la web
      continuousHeatingTime2 += tempInterval;  // Racha continua para el límite

      // Verificar si se excede el tiempo máximo de calentamiento continuo
      if (continuousHeatingTime2 >= MAX_HEATING_TIME) {
        if (!heatingTime2Warning) {
          heatingTime2Warning = true;
          Serial.println("⚠️  ADVERTENCIA: Calentador 2 ha estado funcionando continuamente por más de 30 minutos!");
        }
      }
    } else {
      ledcWrite(HEATER2_PIN, 0);
      // La racha continua se rompe al dejar de calentar, pero el tiempo
      // acumulado se conserva hasta una acción manual
      continuousHeatingTime2 = 0;
      heatingTime2Warning = false;
      if (!heater2Enabled) heatingTime2 = 0; // Deshabilitado manualmente: reiniciar acumulado
    }
  }

  // 2. Actualizar Pantalla OLED
  if (currentMillis - lastOledUpdate >= oledInterval) {
    lastOledUpdate = currentMillis;
    
    display.clearDisplay();
    
    // Cabecera: IP y Estado
    display.setTextSize(1);
    display.setCursor(0,0);
    display.print(WiFi.localIP());
    display.setCursor(95,0);
    display.print(systemEnabled ? "ON" : "OFF");
    display.drawLine(0, 10, 128, 10, WHITE);

    // Cuerpo: Temperaturas
    display.setCursor(0, 15);
    display.print("H1: "); display.print(currentTemp1, 1); 
    display.print("/"); display.print(targetTemp1, 0);
    
    // Barra de potencia visual H1
    int bar1 = map(ledcRead(HEATER1_PIN), 0, 255, 0, 40);
    display.drawRect(80, 15, 42, 6, WHITE);
    display.fillRect(81, 16, bar1, 4, WHITE);

    display.setCursor(0, 35);
    display.print("H2: "); display.print(currentTemp2, 1); 
    display.print("/"); display.print(targetTemp2, 0);

    // Barra de potencia visual H2
    int bar2 = map(ledcRead(HEATER2_PIN), 0, 255, 0, 40);
    display.drawRect(80, 35, 42, 6, WHITE);
    display.fillRect(81, 36, bar2, 4, WHITE);

    display.display();

    if (digitalRead(PRESENCE1_PIN) == LOW) {
      Serial.println("Presencia 1 detectada");
      presenceSensor1 = true;
    } else {
      presenceSensor1 = false;
    }

    if (digitalRead(PRESENCE2_PIN) == LOW) {
      Serial.println("Presencia 2 detectada");
      presenceSensor2 = true;
    } else {
      presenceSensor2 = false;
    }
  }
}
