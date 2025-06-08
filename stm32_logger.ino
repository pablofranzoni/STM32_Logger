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
#include <STM32RTC.h>  
#include <Wire.h>

// Configuración de pines
#define ADC_0 PA0         // Pin analógico lectura
#define ADC_1 PA1         // Pin analógico lectura
#define ADC_2 PA2         // Pin analógico lectura
#define ADC_3 PA3         // Pin analógico lectura

#define SD_CS_PIN PA4
#define RF24_CE_PIN PB13     // Pin CE para el módulo RF24
#define RF24_CSN_PIN PB12    // Pin CSN para el módulo RF24

// Configuración del sensor NTC
//#define THERMISTOR_NOMINAL 10000   // Resistencia del termistor a 25°C
//#define TEMPERATURE_NOMINAL 25     // Temperatura para resistencia nominal (°C)
//#define B_COEFFICIENT 3950         // Coeficiente B del termistor
//#define SERIES_RESISTOR 10000      // Valor de la resistencia en serie

// Comandos RF
#define CMD_REQUEST_DATA 1
#define CMD_SEND_DATA 2
#define CMD_LIST_FILES 3
#define CMD_FILE_LIST 4
#define CMD_MODIFY_RTC 5     // Nuevo comando para modificar RTC
#define CMD_GET_RTC_INFO 6   // Nuevo comando para obtener info del RTC

#define MAX_FILES 50
#define CMD_LIST_START 0x24
#define CMD_LIST_END 0x25

// Configuración para almacenamiento
#define MAX_DAYS 360               // Máximo número de días a almacenar
// Configuración para almacenamiento (ahora variable según intervalo)
#define MINUTES_PER_DAY 1440      // 1440 minutos en un día (24h * 60min)
#define MAX_FILENAME_LENGTH 13  // Longitud máxima para nombres de archivo

#define RTC_KEY_MONTH 1   // Clave para modificar mes
#define RTC_KEY_DAY 2     // Clave para modificar día
#define RTC_KEY_HOUR 3    // Clave para modificar hora
#define RTC_KEY_MINUTE 4  // Clave para modificar minutos
#define RTC_KEY_SECOND 5  // Clave para modificar segundos
#define RTC_KEY_YEAR 6    // Clave para modificar año

uint16_t maxSamplesPerDay;        // Se calculará según el intervalo
uint16_t sampleCount = 0;         // Contador de muestras
uint8_t samplingInterval = 10;    // Valor por defecto (10 minutos), se puede cambiar

const byte addresses[][6] = {"Node1", "Node2"};

char fileNames[MAX_FILES][13];
uint8_t fileCount = 0;

// Variable para controlar si ya se han escaneado los archivos
bool filesScanned = false;

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
struct __attribute__((packed)) DataRequest {
  uint8_t command;
  uint16_t startIndex;
  uint16_t numSamples;
  char filename[MAX_FILENAME_LENGTH]; // Nombre del archivo
  uint8_t key;          // Clave para modificación (RTC_KEY_*)
  uint8_t value;        // Valor a establecer
};

// Estructura para respuestas RF
struct __attribute__((packed)) DataResponse {
  uint8_t command;
  uint16_t index;
  uint16_t value0;
  uint16_t value1;
  uint16_t value2;
  uint16_t value3;
  char timestamp[17];  //un caracter mas para el caracter nulo \0
  uint8_t key;          // Clave para respuesta (RTC_KEY_*)
  uint8_t value;        // Valor obtenido
};

// Estructura para respuesta de lista de archivos
struct __attribute__((packed)) FileListResponse {
  uint8_t command;
  uint8_t fileIndex;
  uint8_t totalFiles;    // Total de archivos en la lista
  char filename[13];
};


// Función para manejar evento de alarma en STM32RTC
void alarmMatch(void *data) {
  alarmTriggered = true;
}

void setup() {

  // Inicialización de serial para depuración
  Serial.begin(115200);
  Serial.println(F("Iniciando sistema de registro de temperatura..."));
  
  setupRadio();

  setupSDCard();
  
  // Inicializar RTC con fecha y hora inicial
  // Configurar el RTC para usar LSE (cristal externo)
  bool reset_bkup = false;
  rtc.setClockSource(STM32RTC::LSE_CLOCK);
  rtc.begin(reset_bkup);
  //rtc.getDate(&weekDay, &day, &month, &year);
  //rtc.getTime(&hours, &minutes, &seconds, &subs);
  /*
  if (reset_bkup) {
    Serial.printf("reset the date to %02d/%02d/%02d\n", day, month, year);
  } else {
    Serial.printf("date from the BackUp %02d/%02d/%02d\n", day, month, year);
  }
  */
  
  // Establecer fecha/hora inicial (01/01/2025 00:00:00)
  // Esto es solo un ejemplo, en producción deberías obtener la fecha/hora actual
  //DateTime initialDateTime = {2025, 1, 1, 0, 0, 0};
  //setDateTime(initialDateTime);
  
  // Configurar intervalo inicial (ej. 5 minutos)
  setSamplingInterval(5);

  // Configurar callback para alarma
  rtc.attachInterrupt(alarmMatch);
  
  // Configurar alarma para cada 10 minutos
  setupNextAlarm();
  
  // Inicializar el archivo actual
  updateCurrentFilename();
  
  Serial.println(F("Sistema inicializado y listo!"));
}

void loop() {
  // Comprobar si hay solicitudes RF24
  waitForCommand();
  
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

void setupSDCard() {
  // Inicializar tarjeta SD
  Serial.print(F("Inicializando SD card..."));
  if (SD.begin(SD_CS_PIN)) {
    Serial.println(F("SD card inicializada."));
    sdAvailable = true;
  } else {
    Serial.println(F("Error inicializando SD card!"));
  }
}

void setupRadio() {
  // Inicializar Radio RF24
  Serial.print(F("Inicializando Radio RF24..."));
  //lo nuevo
  if (radio.begin()) {
    
    // Servidor escribe en "Node1" y lee de "Node2"
    radio.openWritingPipe(addresses[0]);     // Escribe a Node2 (cliente)
    radio.openReadingPipe(1, addresses[1]);  // Lee de Node1 (solicitudes del cliente)
    
    radio.setPALevel(RF24_PA_MAX);
    radio.setRetries(3, 15);
    radio.setChannel(76);  // Mismo canal que el cliente
    radio.setDataRate(RF24_250KBPS);
    
    radio.startListening();

    Serial.println(F("Radio RF24 inicializado."));
  } else {
    Serial.println(F("Error inicializando Radio RF24!"));
  }

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
      dataFile.println("Index,DateTime,Value0,Value1,Value2,Value3");
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
  char buffer[17]; // Reducido el tamaño ya que necesitamos menos caracteres
  sprintf(buffer, "%02d-%02d-%02d %02d:%02d", 
          dt.year % 100, dt.month, dt.day, dt.hour, dt.minute);
  return String(buffer);
}

void takeMeasurement() {
  // Calcular el máximo de muestras por día basado en el intervalo actual
  maxSamplesPerDay = MINUTES_PER_DAY / samplingInterval;
  
  if (!sdAvailable) {
    Serial.println("Error: Tarjeta SD no disponible");
    return;
  }
  
  // Verificar si ha cambiado el día o hemos excedido el máximo de muestras
  DateTime now = getCurrentDateTime();
  char newFilename[16];
  sprintf(newFilename, "TMP_%02d%02d.CSV", now.month, now.day);
  
  // Si el nombre del archivo ha cambiado (nuevo día) o excedimos el máximo
  if (strcmp(newFilename, currentFilename) != 0 || sampleCount >= maxSamplesPerDay) {
    strcpy(currentFilename, newFilename);
    updateCurrentFilename();
    sampleCount = 0;  // Reiniciar contador
    
    Serial.print("Nuevo archivo creado. Máximo de muestras: ");
    Serial.println(maxSamplesPerDay);
  }
  
  // Leer el sensor NTC (siempre se hace la medición)
  //float temperature = readNTC();
  int analogValue0 = analogRead(ADC_0);
  int analogValue1 = analogRead(ADC_1);
  int analogValue2 = analogRead(ADC_2);
  int analogValue3 = analogRead(ADC_3);
  
  // Obtener timestamp y fecha/hora actual
  String dateTimeStr = formatDateTime(now);
  
  // Guardar datos en la SD
  dataFile = SD.open(currentFilename, FILE_WRITE);
  if (dataFile) {
    dataFile.print(sampleCount);
    dataFile.print(",");
    dataFile.print(dateTimeStr);
    dataFile.print(",");
    dataFile.print(analogValue0);
    dataFile.print(",");
    dataFile.print(analogValue1);
    dataFile.print(",");
    dataFile.print(analogValue2);
    dataFile.print(",");
    dataFile.println(analogValue3);
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
    Serial.print(analogValue0);
    Serial.print(",");
    Serial.print(analogValue1);
    Serial.print(",");
    Serial.print(analogValue2);
    Serial.print(",");
    Serial.print(analogValue3);
    Serial.println();

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
/*
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
*/

/**
 * Función para modificar la configuración del RTC según clave y valor
 * @param key Clave que indica qué parámetro modificar
 * @param value Valor a establecer
 * @return true si se realizó el cambio correctamente, false en caso contrario
 */
bool modifyRTC(uint8_t key, uint8_t value) {
  DateTime current = getCurrentDateTime();
  bool valid = true;
  
  Serial.print("Modificando RTC - Clave: ");
  Serial.print(key);
  Serial.print(", Valor: ");
  Serial.println(value);
  
  switch(key) {
    case RTC_KEY_MONTH:
      if (value >= 1 && value <= 12) {
        current.month = value;
      } else {
        valid = false;
        Serial.println("Valor inválido para mes (debe ser 1-12)");
      }
      break;
      
    case RTC_KEY_DAY:
    {
      // Validación básica de días por mes (no considera años bisiestos)
      uint8_t maxDays = 31;
      if (current.month == 4 || current.month == 6 || 
          current.month == 9 || current.month == 11) {
        maxDays = 30;
      } else if (current.month == 2) {
        // Febrero: lógica simple para año bisiesto
        maxDays = ((current.year % 4 == 0 && current.year % 100 != 0) || 
                  (current.year % 400 == 0)) ? 29 : 28;
      }
      
      if (value >= 1 && value <= maxDays) {
        current.day = value;
      } else {
        valid = false;
        Serial.print("Valor inválido para día (debe ser 1-");
        Serial.print(maxDays);
        Serial.println(")");
      }
    }
      break;
      
    case RTC_KEY_HOUR:
      if (value < 24) {
        current.hour = value;
      } else {
        valid = false;
        Serial.println("Valor inválido para hora (debe ser 0-23)");
      }
      break;
      
    case RTC_KEY_MINUTE:
      if (value < 60) {
        current.minute = value;
      } else {
        valid = false;
        Serial.println("Valor inválido para minuto (debe ser 0-59)");
      }
      break;
      
    case RTC_KEY_SECOND:
      if (value < 60) {
        current.second = value;
      } else {
        valid = false;
        Serial.println("Valor inválido para segundo (debe ser 0-59)");
      }
      break;
      
    case RTC_KEY_YEAR:
      // Asumimos que el valor representa años desde 2000 para simplificar
      if (value <= 99) {
        current.year = 2000 + value;
      } else {
        valid = false;
        Serial.println("Valor inválido para año (debe ser 0-99, representa 2000-2099)");
      }
      break;
      
    default:
      valid = false;
      Serial.println("Clave de RTC desconocida");
      break;
  }
  
  if (valid) {
    // Si la validación fue exitosa, actualizar el RTC
    setDateTime(current);
    
    // Actualizar el nombre de archivo si cambió el día o mes
    if (key == RTC_KEY_MONTH || key == RTC_KEY_DAY) {
      updateCurrentFilename();
    }
    
    Serial.println("RTC actualizado correctamente");
    
    // Mostrar la nueva fecha y hora
    DateTime updated = getCurrentDateTime();
    Serial.print("Nueva fecha/hora: ");
    Serial.println(formatDateTime(updated));
  }
  
  return valid;
}

/**
 * Función para obtener información del RTC según la clave solicitada
 * @param key Clave que indica qué información obtener
 * @return Valor solicitado según la clave
 */
uint8_t getRTCInfo(uint8_t key) {
  DateTime current = getCurrentDateTime();
  
  switch(key) {
    case RTC_KEY_MONTH:
      return current.month;
    case RTC_KEY_DAY:
      return current.day;
    case RTC_KEY_HOUR:
      return current.hour;
    case RTC_KEY_MINUTE:
      return current.minute;
    case RTC_KEY_SECOND:
      return current.second;
    case RTC_KEY_YEAR:
      return current.year - 2000; // Devolver años desde 2000
    default:
      return 0xFF; // Valor inválido
  }
}

/**
 * Función para enviar información del RTC al cliente
 * @param request Solicitud recibida con la clave de información a obtener
 */
void sendRTCInfo(DataRequest request) {
  DataResponse response;
  response.command = CMD_GET_RTC_INFO;
  response.key = request.key;
  response.value = getRTCInfo(request.key);
  
  // Detener escucha para poder enviar
  radio.stopListening();
  
  // Pequeña pausa para estabilizar el radio
  delay(10);
  
  // Enviar respuesta
  if (radio.write(&response, sizeof(response))) {
    Serial.print("Información RTC enviada - Clave: ");
    Serial.print(response.key);
    Serial.print(", Valor: ");
    Serial.println(response.value);
  } else {
    Serial.println("Error enviando información RTC");
  }
  
  // Volver a modo escucha
  radio.startListening();
}

void waitForCommand() {
  
  // Verificar si hay datos disponibles
  if (radio.available()) {
    // Leer comando
    DataRequest request;
    radio.read(&request, sizeof(request));
    
    Serial.print("Comando recibido: ");
    Serial.println(request.command);
    
    // Procesar comando
    if (request.command == CMD_FILE_LIST) {
      Serial.println("Comando de listar archivos recibido");
      
      // Escanear archivos nuevamente para asegurar actualización
      scanFiles();
      
      // Dar tiempo para que el cliente cambie a modo recepción
      delay(100);
      
      // Enviar la lista de archivos
      sendFileList();
    }
    else if (request.command == CMD_REQUEST_DATA) {
      // Enviar el contenido del archivo
      sendFile(request);
    }
    else if (request.command == CMD_MODIFY_RTC) {
      // Modificar la configuración del RTC
      bool success = modifyRTC(request.key, request.value);
      
      // Enviar respuesta de confirmación
      DataResponse response;
      response.command = CMD_MODIFY_RTC;
      response.key = request.key;
      response.value = success ? 1 : 0; // 1 = éxito, 0 = error
      
      // Detener escucha para poder enviar
      radio.stopListening();
      
      // Pequeña pausa para estabilizar el radio
      delay(10);
      
      // Enviar confirmación
      if (radio.write(&response, sizeof(response))) {
        Serial.println("Confirmación de modificación RTC enviada");
      } else {
        Serial.println("Error enviando confirmación");
      }
      
      // Volver a modo escucha
      radio.startListening();
    }
    else if (request.command == CMD_GET_RTC_INFO) {
      // Enviar información del RTC
      sendRTCInfo(request);
    }
    else {
      Serial.println("Comando desconocido");
    }
  }
  
  // Pequeña pausa
  delay(10);
  
}

// Escanear archivos y almacenarlos en el array
void scanFiles() {
  // Resetear contador
  fileCount = 0;
  
  // Abrir directorio raíz
  File root = SD.open("/");
  if (!root) {
    Serial.println("Error: No se pudo abrir el directorio raíz");
    return;
  }
  
  Serial.println(F("Escaneando archivos disponibles..."));
  
  // Recorrer todos los archivos
  while (fileCount < MAX_FILES) {
    File entry = root.openNextFile();
    if (!entry) break;
    
    // Verificar si es un archivo de temperatura
    if (strncmp(entry.name(), "TMP_", 4) == 0 && 
        strlen(entry.name()) == 12 &&
        strcmp(entry.name() + 8, ".CSV") == 0) {
      
      // Almacenar nombre en el array
      strcpy(fileNames[fileCount], entry.name());
      
      Serial.print(F("Archivo encontrado: "));
      Serial.println(fileNames[fileCount]);
      
      fileCount++;
    }
    entry.close();
  }
  
  root.close();
  
  Serial.print(F("Total de archivos encontrados: "));
  Serial.println(fileCount);
  
  filesScanned = true;
}

void sendFileList() {
  // Cambiar a modo TX
  radio.stopListening();
  
  // Si no hay archivos, terminar
  if (fileCount == 0) {
    Serial.println(F("No hay archivos para enviar"));
    // Enviar mensaje de fin de lista
    uint8_t endMsg = CMD_LIST_END;
    radio.write(&endMsg, sizeof(endMsg));
    radio.startListening();
    return;
  }
  
  // Enviar mensaje de inicio de lista
  uint8_t startMsg = CMD_LIST_START;
  bool success = radio.write(&startMsg, sizeof(startMsg));
  
  if (!success) {
    Serial.println(F("Error al enviar señal de inicio"));
    radio.startListening();
    return;
  }
  
  Serial.println(F("Señal de inicio enviada"));
  delay(200);  // Dar tiempo al cliente
  
  // Enviar cada archivo desde el array
  FileListResponse response;
  response.command = CMD_FILE_LIST;
  response.totalFiles = fileCount;
  
  for (uint8_t i = 0; i < fileCount; i++) {
    // Preparar respuesta
    response.fileIndex = i;
    strcpy(response.filename, fileNames[i]);
    
    // Enviar respuesta
    Serial.print("Enviando archivo ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.print(fileCount);
    Serial.print(": ");
    Serial.print(response.filename);
    
    bool txSuccess = radio.write(&response, sizeof(response));
    
    if (txSuccess) {
      Serial.println(" - OK");
    } else {
      Serial.println(" - FALLO");
    }
    
    delay(250);  // Pausa entre envíos
  }
  
  // Enviar mensaje de fin de lista
  uint8_t endMsg = CMD_LIST_END;
  radio.write(&endMsg, sizeof(endMsg));
  
  Serial.println("Lista de archivos enviada completamente");
  
  // Volver a modo recepción
  radio.startListening();
}

void sendFile(DataRequest request) {
  Serial.println("Procesando solicitud de datos...");
  
  // Pausar la escucha para enviar datos
  radio.stopListening();

  // Asegurar que el nombre del archivo termine con null
  request.filename[MAX_FILENAME_LENGTH - 1] = '\0';
  
  Serial.print(F("Solicitud de datos recibida para archivo: "));
  Serial.print(request.filename);
  Serial.print(F(", Inicio: "));
  Serial.print(request.startIndex);
  Serial.print(F(", Cantidad: "));
  Serial.println(request.numSamples);
  
  // Verificar si el archivo existe
  if (!SD.exists(request.filename)) {
    Serial.println(F("Archivo no encontrado."));
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
      int idx4 = line.indexOf(',', idx3 + 1);
      int idx5 = line.indexOf(',', idx4 + 1);
          
      DataResponse response;
      response.command = CMD_SEND_DATA;
      response.index = request.startIndex + counter;
      //response.timestamp = line.substring(idx1 + 1, idx2);
      String tempTimestamp = line.substring(idx1 + 1, idx2);
      strcpy(response.timestamp, tempTimestamp.c_str());
      response.value0 = line.substring(idx2 + 1, idx3).toInt();
      response.value1 = line.substring(idx3 + 1, idx4).toInt();
      response.value2 = line.substring(idx4 + 1, idx5).toInt();
      response.value3 = line.substring(idx5 + 1).toInt();
      
      // Enviar respuesta
      radio.write(&response, sizeof(response));
          
      // Pequeña pausa para evitar saturar el receptor
      delay(10);
      counter++;
    }
        
    dataFile.close();
    Serial.print(F("Datos enviados: "));
    Serial.print(counter);
    Serial.println(F(" muestras."));
  } else {
    Serial.println(F("No se pudo abrir el archivo para leer."));
  }
      
  // Volver a modo escucha
  radio.startListening();
  
}