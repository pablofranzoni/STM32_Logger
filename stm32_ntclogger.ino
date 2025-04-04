void setup() {
/*
  Programa para STM32F103 (Blue Pill) que:
  - Mide temperatura con un sensor NTC cada 10 minutos
  - Guarda los datos en una tarjeta SD, un archivo por día (TEMP_LOG_MMDD.CSV)
  - Transmite datos almacenados cuando se solicita a través de RF24
  - Utiliza el RTC interno para controlar el tiempo
  - Conserva datos por hasta 360 días
*/

#include <SPI.h>
#include <SD.h>
#include <RF24.h>
#include <RTClock.h>
#include <Wire.h>

// Configuración de pines
#define NTC_PIN PA0         // Pin analógico para el sensor NTC
#define SD_CS_PIN PA4       // Pin CS para la tarjeta SD
#define RF24_CE_PIN PB0     // Pin CE para el módulo RF24
#define RF24_CSN_PIN PB1    // Pin CSN para el módulo RF24

// Configuración del sensor NTC
#define THERMISTOR_NOMINAL 10000   // Resistencia del termistor a 25°C
#define TEMPERATURE_NOMINAL 25     // Temperatura para resistencia nominal (°C)
#define B_COEFFICIENT 3950         // Coeficiente B del termistor
#define SERIES_RESISTOR 10000      // Valor de la resistencia en serie

// Configuración para almacenamiento
#define SAMPLES_PER_DAY 144        // 144 muestras por día (cada 10 minutos)
#define MAX_DAYS 360               // Máximo número de días a almacenar

// Dirección de comunicación RF24
const byte address[6] = "Node1";

// Comandos RF
#define CMD_REQUEST_DATA 1
#define CMD_SEND_DATA 2
#define CMD_LIST_FILES 3
#define CMD_FILE_LIST 4

// Variables globales
RF24 radio(RF24_CE_PIN, RF24_CSN_PIN);
RTClock rtc(RTCSEL_LSE);
File dataFile;
uint32_t sampleCount = 0;
char currentFilename[16];  // Para almacenar el nombre del archivo actual
bool sdAvailable = false;
uint32_t currentDay = 0;   // Para detectar cambios de día

// Estructura para fecha y hora
struct DateTime {
  uint16_t year;
  uint8_t month;
  uint8_t day;
  uint8_t hour;
  uint8_t minute;
  uint8_t second;
};

// Estructura para solicitudes RF
struct DataRequest {
  uint8_t command;
  uint16_t startIndex;
  uint16_t numSamples;
  char filename[16];  // Para solicitar archivo específico
};

// Estructura para respuestas RF
struct DataResponse {
  uint8_t command;
  uint16_t index;
  float temperature;
  uint32_t timestamp;
};

// Estructura para respuesta de lista de archivos
struct FileListResponse {
  uint8_t command;
  uint8_t fileCount;
  char filename[16];
};

void setup() {
  // Inicialización de serial para depuración
  Serial.begin(115200);
  Serial.println("Iniciando sistema de registro de temperatura...");
  
  // Inicializar SPI
  SPI.begin();
  
  // Inicializar tarjeta SD
  Serial.print("Inicializando SD card...");
  if (SD.begin(SD_CS_PIN)) {
    Serial.println("SD card inicializada.");
    sdAvailable = true;
  } else {
    Serial.println("Error inicializando SD card!");
  }
  
  // Inicializar Radio RF24
  Serial.print("Inicializando Radio RF24...");
  if (radio.begin()) {
    radio.openReadingPipe(1, address);
    radio.setPALevel(RF24_PA_LOW);
    radio.startListening();
    Serial.println("Radio RF24 inicializado.");
  } else {
    Serial.println("Error inicializando Radio RF24!");
  }
  
  // Inicializar RTC con fecha y hora inicial
  // Establecer fecha/hora inicial (01/01/2025 00:00:00)
  // Esto es solo un ejemplo, en producción deberías obtener la fecha/hora actual
  DateTime initialDateTime = {2025, 1, 1, 0, 0, 0};
  setDateTime(initialDateTime);
  
  // Configurar alarma para cada 10 minutos
  setupNextAlarm();
  
  // Inicializar el archivo actual
  updateCurrentFilename();
  
  Serial.println("Sistema inicializado y listo!");
}

void loop() {
  // Comprobar si hay solicitudes RF24
  checkForRFRequests();
  
  // Comprobar si es hora de tomar una medición (manejado por la interrupción del RTC)
  if (rtc.getAlarmFlag()) {
    rtc.clearAlarmFlag();
    
    // Verificar si ha cambiado el día
    DateTime now = getCurrentDateTime();
    uint32_t dayValue = now.year * 10000 + now.month * 100 + now.day;
    
    if (dayValue != currentDay) {
      currentDay = dayValue;
      updateCurrentFilename();
    }
    
    // Tomar una nueva medición
    takeMeasurement();
    
    // Configurar la próxima alarma
    setupNextAlarm();
  }
  
  // Pequeña pausa para ahorrar energía
  delay(100);
}

// Establecer fecha y hora
void setDateTime(DateTime dt) {
  // Calcular segundos desde el inicio del día
  uint32_t secondsOfDay = dt.hour * 3600 + dt.minute * 60 + dt.second;
  
  // Establecer tiempo en el RTC
  rtc.setClockTime(secondsOfDay);
  
  // Almacenar la fecha actual como referencia
  currentDay = dt.year * 10000 + dt.month * 100 + dt.day;
}

// Obtener fecha y hora actual
DateTime getCurrentDateTime() {
  DateTime dt;
  
  // Obtener tiempo del RTC (segundos desde medianoche)
  uint32_t secondsOfDay = rtc.getTime();
  
  // Calcular horas, minutos y segundos
  dt.hour = (secondsOfDay / 3600) % 24;
  dt.minute = (secondsOfDay / 60) % 60;
  dt.second = secondsOfDay % 60;
  
  // Extraer año, mes y día de la variable global currentDay
  dt.year = currentDay / 10000;
  dt.month = (currentDay / 100) % 100;
  dt.day = currentDay % 100;
  
  return dt;
}

// Actualizar el nombre del archivo actual basado en la fecha
void updateCurrentFilename() {
  DateTime now = getCurrentDateTime();
  sprintf(currentFilename, "TEMP_%02d%02d.CSV", now.month, now.day);
  
  // Crear el archivo si no existe
  if (!SD.exists(currentFilename) && sdAvailable) {
    dataFile = SD.open(currentFilename, FILE_WRITE);
    if (dataFile) {
      dataFile.println("Index,Timestamp,DateTime,Temperature");
      dataFile.close();
      Serial.print("Archivo nuevo creado: ");
      Serial.println(currentFilename);
      sampleCount = 0;
    } else {
      Serial.println("Error creando archivo nuevo.");
    }
  } else if (sdAvailable) {
    // Si el archivo existe, contar las líneas para continuar la numeración
    dataFile = SD.open(currentFilename, FILE_READ);
    if (dataFile) {
      // Saltar la primera línea (encabezado)
      dataFile.readStringUntil('\n');
      
      // Contar líneas
      sampleCount = 0;
      while (dataFile.available()) {
        dataFile.readStringUntil('\n');
        sampleCount++;
      }
      dataFile.close();
      Serial.print("Archivo existente: ");
      Serial.print(currentFilename);
      Serial.print(", muestras: ");
      Serial.println(sampleCount);
    }
  }
}

// Eliminar archivos antiguos si se supera el límite de días
void cleanupOldFiles() {
  if (!sdAvailable) return;
  
  DateTime now = getCurrentDateTime();
  uint16_t currentYear = now.year;
  uint8_t currentMonth = now.month;
  uint8_t currentDay = now.day;
  
  // Crear una matriz para almacenar nombres de archivos
  char fileNames[MAX_DAYS][16];
  int fileCount = 0;
  
  // Abrir el directorio raíz
  File root = SD.open("/");
  
  // Enumerar todos los archivos
  while (true) {
    File entry = root.openNextFile();
    if (!entry) break;
    
    // Verificar si es un archivo de temperatura
    if (strncmp(entry.name(), "TEMP_", 5) == 0 && 
        strlen(entry.name()) == 12 &&
        strcmp(entry.name() + 8, ".CSV") == 0) {
      
      // Copiar el nombre del archivo
      strncpy(fileNames[fileCount], entry.name(), 15);
      fileNames[fileCount][15] = '\0';
      fileCount++;
    }
    entry.close();
  }
  root.close();
  
  // Si hay más archivos que el límite, eliminar los más antiguos
  if (fileCount > MAX_DAYS) {
    // Ordenar nombres de archivos (simple bubble sort)
    for (int i = 0; i < fileCount - 1; i++) {
      for (int j = 0; j < fileCount - i - 1; j++) {
        if (strcmp(fileNames[j], fileNames[j + 1]) > 0) {
          char temp[16];
          strcpy(temp, fileNames[j]);
          strcpy(fileNames[j], fileNames[j + 1]);
          strcpy(fileNames[j + 1], temp);
        }
      }
    }
    
    // Eliminar los archivos más antiguos
    int filesToDelete = fileCount - MAX_DAYS;
    for (int i = 0; i < filesToDelete; i++) {
      SD.remove(fileNames[i]);
      Serial.print("Archivo antiguo eliminado: ");
      Serial.println(fileNames[i]);
    }
  }
}

// Configurar la próxima alarma (cada 10 minutos)
void setupNextAlarm() {
  // Obtener la hora actual
  uint32_t currentTime = rtc.getTime();
  
  // Calcular el próximo tiempo de alarma (actual + 10 minutos)
  uint32_t alarmTime = currentTime + 600;  // 10 minutos = 600 segundos
  
  // Ajustar para cambio de día
  if (alarmTime >= 86400) {  // 24*60*60 segundos en un día
    alarmTime -= 86400;
    
    // Incrementar el día
    DateTime dt = getCurrentDateTime();
    dt.day++;
    
    // Lógica simple para manejar fin de mes (no es preciso para todos los meses)
    if ((dt.month == 4 || dt.month == 6 || dt.month == 9 || dt.month == 11) && dt.day > 30) {
      dt.day = 1;
      dt.month++;
    } else if (dt.month == 2) {
      // Año bisiesto aproximado
      bool isLeapYear = (dt.year % 4 == 0 && dt.year % 100 != 0) || (dt.year % 400 == 0);
      if ((isLeapYear && dt.day > 29) || (!isLeapYear && dt.day > 28)) {
        dt.day = 1;
        dt.month++;
      }
    } else if (dt.day > 31) {
      dt.day = 1;
      dt.month++;
      if (dt.month > 12) {
        dt.month = 1;
        dt.year++;
      }
    }
    
    // Actualizar la fecha
    currentDay = dt.year * 10000 + dt.month * 100 + dt.day;
    
    // Verificar si hay que limpiar archivos antiguos
    cleanupOldFiles();
  }
  
  // Configurar la alarma
  rtc.setAlarmTime(alarmTime);
  rtc.attachAlarmInterrupt(NULL);  // No se necesita función de callback, solo establecer la bandera
}

// Formatear fecha y hora como cadena
String formatDateTime(DateTime dt) {
  char buffer[20];
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
          dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  return String(buffer);
}

// Tomar una medición y almacenarla
void takeMeasurement() {
  if (!sdAvailable || sampleCount >= SAMPLES_PER_DAY) {
    Serial.println("No se puede guardar más datos o SD no disponible");
    return;
  }
  
  // Leer el sensor NTC
  float temperature = readNTC();
  
  // Obtener timestamp y fecha/hora actual
  uint32_t timestamp = rtc.getTime();
  DateTime now = getCurrentDateTime();
  String dateTimeStr = formatDateTime(now);
  
  // Guardar datos en la SD
  dataFile = SD.open(currentFilename, FILE_WRITE);
  if (dataFile) {
    dataFile.print(sampleCount);
    dataFile.print(",");
    dataFile.print(timestamp);
    dataFile.print(",");
    dataFile.print(dateTimeStr);
    dataFile.print(",");
    dataFile.println(temperature);
    dataFile.close();
    
    Serial.print("Muestra guardada #");
    Serial.print(sampleCount);
    Serial.print(" en ");
    Serial.print(currentFilename);
    Serial.print(": ");
    Serial.print(temperature);
    Serial.println("°C");
    
    sampleCount++;
  } else {
    Serial.println("Error abriendo archivo para guardar datos");
  }
}

// Leer temperatura desde el sensor NTC
float readNTC() {
  // Leer valor analógico
  int analogValue = analogRead(NTC_PIN);
  
  // Convertir a resistencia
  float resistance = SERIES_RESISTOR / ((4095.0 / analogValue) - 1.0);
  
  // Convertir a temperatura usando la ecuación B
  float steinhart = resistance / THERMISTOR_NOMINAL;     // (R/Ro)
  steinhart = log(steinhart);                           // ln(R/Ro)
  steinhart /= B_COEFFICIENT;                           // 1/B * ln(R/Ro)
  steinhart += 1.0 / (TEMPERATURE_NOMINAL + 273.15);    // + (1/To)
  steinhart = 1.0 / steinhart;                          // Invertir
  float temperature = steinhart - 273.15;               // Convertir a °C
  
  return temperature;
}

// Comprobar si hay solicitudes RF24 y responder
void checkForRFRequests() {
  if (radio.available()) {
    DataRequest request;
    radio.read(&request, sizeof(request));
    
    // Procesar la solicitud
    if (request.command == CMD_REQUEST_DATA) {
      // Pausar la escucha para enviar datos
      radio.stopListening();
      
      Serial.print("Solicitud de datos recibida para archivo: ");
      Serial.print(request.filename);
      Serial.print(", Inicio: ");
      Serial.print(request.startIndex);
      Serial.print(", Cantidad: ");
      Serial.println(request.numSamples);
      
      // Verificar si el archivo existe
      if (!SD.exists(request.filename)) {
        Serial.println("Archivo no encontrado.");
        radio.startListening();
        return;
      }
      
      // Abrir archivo para lectura
      dataFile = SD.open(request.filename, FILE_READ);
      if (dataFile) {
        // Saltar encabezado
        dataFile.readStringUntil('\n');
        
        // Saltar hasta el índice de inicio
        for (uint16_t i = 0; i < request.startIndex; i++) {
          if (!dataFile.available()) break;
          dataFile.readStringUntil('\n');
        }
        
        // Leer y enviar las muestras solicitadas
        uint16_t counter = 0;
        while (dataFile.available() && counter < request.numSamples) {
          String line = dataFile.readStringUntil('\n');
          
          // Parsear la línea
          int idx1 = line.indexOf(',');
          int idx2 = line.indexOf(',', idx1 + 1);
          int idx3 = line.indexOf(',', idx2 + 1);
          
          DataResponse response;
          response.command = CMD_SEND_DATA;
          response.index = request.startIndex + counter;
          response.timestamp = line.substring(idx1 + 1, idx2).toInt();
          response.temperature = line.substring(idx3 + 1).toFloat();
          
          // Enviar respuesta
          radio.write(&response, sizeof(response));
          
          // Pequeña pausa para evitar saturar el receptor
          delay(10);
          counter++;
        }
        
        dataFile.close();
        Serial.print("Datos enviados: ");
        Serial.print(counter);
        Serial.println(" muestras.");
      } else {
        Serial.println("No se pudo abrir el archivo para leer.");
      }
      
      // Volver a modo escucha
      radio.startListening();
    }
    else if (request.command == CMD_LIST_FILES) {
      // Pausar la escucha para enviar datos
      radio.stopListening();
      
      Serial.println("Solicitud de lista de archivos recibida.");
      
      // Abrir directorio raíz
      File root = SD.open("/");
      if (root) {
        // Contador de archivos
        uint8_t fileCount = 0;
        
        // Enumerar todos los archivos
        while (true) {
          File entry = root.openNextFile();
          if (!entry) break;
          
          // Verificar si es un archivo de temperatura
          if (strncmp(entry.name(), "TEMP_", 5) == 0 && 
              strlen(entry.name()) == 12 &&
              strcmp(entry.name() + 8, ".CSV") == 0) {
            
            // Preparar respuesta
            FileListResponse response;
            response.command = CMD_FILE_LIST;
            response.fileCount = fileCount;
            strcpy(response.filename, entry.name());
            
            // Enviar respuesta
            radio.write(&response, sizeof(response));
            
            fileCount++;
            delay(10);  // Pequeña pausa
          }
          entry.close();
        }
        
        root.close();
        Serial.print("Lista de archivos enviada: ");
        Serial.print(fileCount);
        Serial.println(" archivos.");
      } else {
        Serial.println("No se pudo abrir el directorio raíz.");
      }
      
      // Volver a modo escucha
      radio.startListening();
    }
  }
}
