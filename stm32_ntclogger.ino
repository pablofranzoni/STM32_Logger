/*
  Programa para STM32F103 (Blue Pill) que:
  - Mide temperatura con un sensor NTC cada 10 minutos
  - Guarda los datos en una tarjeta SD, un archivo por día (TMP_MMDD.CSV)
  - Transmite datos almacenados cuando se solicita a través de RF24
  - Utiliza el RTC interno para controlar el tiempo
  - Conserva datos por hasta 360 días
*/

#include <SPI.h>
#include <SD.h>
#include <RF24.h>
#include <STM32RTC.h>  // Nueva librería RTC
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
#define MAX_DAYS 360               // Máximo número de días a almacenar
// Configuración para almacenamiento (ahora variable según intervalo)
#define MINUTES_PER_DAY 1440      // 1440 minutos en un día (24h * 60min)
uint16_t maxSamplesPerDay;        // Se calculará según el intervalo
uint16_t sampleCount = 0;         // Contador de muestras
uint8_t samplingInterval = 10;    // Valor por defecto (10 minutos), se puede cambiar

// Dirección de comunicación RF24
const byte address[6] = "Node1";

// Comandos RF
#define CMD_REQUEST_DATA 1
#define CMD_SEND_DATA 2
#define CMD_LIST_FILES 3
#define CMD_FILE_LIST 4

// Variables globales
RF24 radio(RF24_CE_PIN, RF24_CSN_PIN);
STM32RTC& rtc = STM32RTC::getInstance();  // Obtener instancia del RTC
File dataFile;
char currentFilename[16];  // Para almacenar el nombre del archivo actual
bool sdAvailable = false;
bool alarmTriggered = false;  // Para controlar cuando se debe tomar una medición
uint32_t lastAlarmTime = 0;   // Para controlar la alarma manualmente

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

// Función para manejar evento de alarma en STM32RTC
void alarmMatch(void *data) {
  alarmTriggered = true;
}

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
  // Configurar el RTC para usar LSE (cristal externo)
  rtc.begin(STM32RTC::LSE_CLOCK);
  
  // Establecer fecha/hora inicial (01/01/2025 00:00:00)
  // Esto es solo un ejemplo, en producción deberías obtener la fecha/hora actual
  DateTime initialDateTime = {2025, 1, 1, 0, 0, 0};
  setDateTime(initialDateTime);
  
  // Configurar intervalo inicial (ej. 5 minutos)
  setSamplingInterval(5);

  // Configurar callback para alarma
  rtc.attachInterrupt(alarmMatch);
  
  // Configurar alarma para cada 10 minutos
  setupNextAlarm();
  
  // Inicializar el archivo actual
  updateCurrentFilename();
  
  Serial.println("Sistema inicializado y listo!");
}

void loop() {
  // Comprobar si hay solicitudes RF24
  checkForRFRequests();
  
  // Comprobar si es hora de tomar una medición (manejado por la alarma)
  if (alarmTriggered) {
    alarmTriggered = false;
    
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
  // Establecer hora y fecha en el RTC
  rtc.setHours(dt.hour);
  rtc.setMinutes(dt.minute);
  rtc.setSeconds(dt.second);
  rtc.setDay(dt.day);
  rtc.setMonth(dt.month);
  rtc.setYear(dt.year - 2000);  // STM32RTC usa años desde 2000
}

// Obtener fecha y hora actual
DateTime getCurrentDateTime() {
  DateTime dt;
  
  // Obtener fecha y hora del RTC
  dt.hour = rtc.getHours();
  dt.minute = rtc.getMinutes();
  dt.second = rtc.getSeconds();
  dt.day = rtc.getDay();
  dt.month = rtc.getMonth();
  dt.year = rtc.getYear() + 2000;  // STM32RTC devuelve años desde 2000
  
  return dt;
}

// Actualizar el nombre del archivo actual basado en la fecha
void updateCurrentFilename() {
  DateTime now = getCurrentDateTime();
  sprintf(currentFilename, "TMP_%02d%02d.CSV", now.month, now.day);
  Serial.println(currentFilename);
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
    if (strncmp(entry.name(), "TMP_", 4) == 0 && 
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

// Obtener timestamp (segundos desde medianoche)
uint32_t getTimestampSeconds() {
  uint8_t hour = rtc.getHours();
  uint8_t minute = rtc.getMinutes();
  uint8_t second = rtc.getSeconds();
  
  return hour * 3600 + minute * 60 + second;
}

// Configurar la próxima alarma (cada 10 minutos)
void setupNextAlarm() {
  DateTime now = getCurrentDateTime();
  
  // Calcular el próximo tiempo de alarma (actual + 10 minutos)
  uint8_t alarmMinute = (now.minute + samplingInterval) % 60;
  uint8_t alarmHour = now.hour;
  
  // Ajustar hora si los minutos dan la vuelta
  if (alarmMinute < now.minute) {
    alarmHour = (alarmHour + 1) % 24;
    
    // Si la hora da la vuelta, podría ser un nuevo día
    if (alarmHour < now.hour) {
      // Verificar si hay que limpiar archivos antiguos
      cleanupOldFiles();
      // La fecha cambiará automáticamente por el RTC
    }
  }
  
  // Configurar la alarma para la próxima medición
  rtc.setAlarmDay(rtc.getDay());
  rtc.setAlarmHours(alarmHour);
  rtc.setAlarmMinutes(alarmMinute);
  rtc.setAlarmSeconds(0);
  rtc.enableAlarm(STM32RTC::MATCH_HHMMSS);
  
  // Guardar el tiempo de la alarma para respaldo
  lastAlarmTime = alarmHour * 3600 + alarmMinute * 60;
  
  Serial.print("Próxima alarma configurada para: ");
  Serial.print(alarmHour);
  Serial.print(":");
  Serial.print(alarmMinute);
  Serial.println(":00");
}

// Formatear fecha y hora como cadena
String formatDateTime(DateTime dt) {
  char buffer[20];
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
          dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
  return String(buffer);
}


void takeMeasurement() {
  // Calcular el máximo de muestras por día basado en el intervalo actual
  maxSamplesPerDay = MINUTES_PER_DAY / samplingInterval;
  
  if (!sdAvailable || sampleCount >= maxSamplesPerDay) {
    Serial.println("No se puede guardar más datos o SD no disponible");
    return;
  }
  
  // Verificar si ha cambiado el día
  DateTime now = getCurrentDateTime();
  char newFilename[16];
  sprintf(newFilename, "TMP_%02d%02d.CSV", now.month, now.day);
  
  // Si el nombre del archivo ha cambiado, actualizar y reiniciar contador
  if (strcmp(newFilename, currentFilename) != 0) {
    strcpy(currentFilename, newFilename);
    updateCurrentFilename();
    sampleCount = 0;  // Reiniciar contador al cambiar de día
    Serial.print("Nuevo archivo creado. Máximo de muestras: ");
    Serial.println(maxSamplesPerDay);
  }
  
  // Leer el sensor NTC
  float temperature = readNTC();
  
  // Obtener timestamp y fecha/hora actual
  uint32_t timestamp = getTimestampSeconds();
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
    
    Serial.print("Muestra #");
    Serial.print(sampleCount);
    Serial.print("/");
    Serial.print(maxSamplesPerDay);
    Serial.print(" en ");
    Serial.print(currentFilename);
    Serial.print(" (Int: ");
    Serial.print(samplingInterval);
    Serial.print("min): ");
    Serial.print(temperature);
    Serial.println("°C");
    
    sampleCount++;
  } else {
    Serial.println("Error abriendo archivo para guardar datos");
  }
}

/**
 * Establece el nuevo intervalo de muestreo (1-60 minutos)
 * @param interval Nuevo intervalo en minutos
 */
void setSamplingInterval(uint8_t interval) {
  if(interval >= 1 && interval <= 60) {
    samplingInterval = interval;
    maxSamplesPerDay = MINUTES_PER_DAY / samplingInterval;
    sampleCount = 0; // Reiniciar contador al cambiar intervalo
    
    Serial.print("Nuevo intervalo de muestreo: ");
    Serial.print(samplingInterval);
    Serial.print(" minutos. Máx muestras/día: ");
    Serial.println(maxSamplesPerDay);
  } else {
    Serial.println("Error: Intervalo debe ser 1-60 minutos");
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
          if (strncmp(entry.name(), "TMP_", 4) == 0 && 
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