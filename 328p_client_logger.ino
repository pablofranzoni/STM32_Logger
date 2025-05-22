/*
  Cliente RF24 para solicitar datos de temperatura al STM32F103
  Este cliente permite listar archivos y solicitar datos de archivos específicos
*/

#include <SPI.h>
#include <RF24.h>

// Configuración de pines
#define RF24_CE_PIN 9     // Pin CE para el módulo RF24
#define RF24_CSN_PIN 10   // Pin CSN para el módulo RF24

// Comandos RF
#define CMD_REQUEST_DATA 1
#define CMD_SEND_DATA 2
#define CMD_LIST_FILES 3
#define CMD_FILE_LIST 4
#define CMD_MODIFY_RTC 5     // Nuevo comando para modificar RTC
#define CMD_GET_RTC_INFO 6   // Nuevo comando para obtener info del RTC

#define CMD_LIST_START 0x24
#define CMD_LIST_END 0x25
#define MAX_FILES 20     // Máximo número de archivos a recibir

// Definiciones de claves para modificación RTC
#define RTC_KEY_MONTH 1   // Clave para modificar mes
#define RTC_KEY_DAY 2     // Clave para modificar día
#define RTC_KEY_HOUR 3    // Clave para modificar hora
#define RTC_KEY_MINUTE 4  // Clave para modificar minutos
#define RTC_KEY_SECOND 5  // Clave para modificar segundos
#define RTC_KEY_YEAR 6    // Clave para modificar año

#define MAX_FILENAME_LENGTH 13  // Longitud máxima para nombres de archivo

const byte addresses[][6] = {"Node1", "Node2"};

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
  uint8_t totalFiles;    
  char filename[MAX_FILENAME_LENGTH];     
};

RF24 radio(RF24_CE_PIN, RF24_CSN_PIN);

// Matriz para almacenar nombres de archivos
#define MAX_FILES 10
char fileList[MAX_FILES][MAX_FILENAME_LENGTH];
uint8_t fileCount = 0;

void setup() {
  Serial.begin(115200);
  Serial.println(F("Cliente RF24 para solicitar datos de temperatura (Archivos diarios)"));

  setupRadio();
}

void loop() {
  if (Serial.available() > 0) {
    // Usar un buffer estático para comandos
    static char cmdBuffer[32];
    uint8_t idx = 0;
    
    // Leer hasta encontrar un salto de línea o llenar el buffer
    while (Serial.available() > 0 && idx < 31) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') break;
      cmdBuffer[idx++] = c;
    }
    cmdBuffer[idx] = '\0'; // Terminar la cadena
    
    // Procesar el comando
    if (strncmp(cmdBuffer, "GET ", 4) == 0) {
      // Formato: GET filename startIndex numSamples
      // Ejemplo: GET TEMP_0101.CSV 0 10
      char* filename = strtok(cmdBuffer + 4, " ");
      char* startIdxStr = strtok(NULL, " ");
      char* numSamplesStr = strtok(NULL, " ");
      
      if (filename != NULL && startIdxStr != NULL && numSamplesStr != NULL) {
        uint16_t startIndex = atoi(startIdxStr);
        uint16_t numSamples = atoi(numSamplesStr);
        
        requestData(filename, startIndex, numSamples);
      } else {
        Serial.println(F("Formato incorrecto. Use: GET filename startIndex numSamples"));
      }
    } else if (strcmp(cmdBuffer, "LIST") == 0) {
      requestFileList();
    } else if (strncmp(cmdBuffer, "MOD ", 4) == 0) {
      // Formato: MOD key value
      // Ejemplo: MOD 1 10 (cambiar mes a octubre)
      char* keyStr = strtok(cmdBuffer + 4, " ");
      char* valueStr = strtok(NULL, " ");
      
      if (keyStr != NULL && valueStr != NULL) {
        uint8_t key = atoi(keyStr);
        uint8_t value = atoi(valueStr);
        
        // Verificar rango de key
        if (key >= RTC_KEY_MONTH && key <= RTC_KEY_YEAR) {
          modifyRTC(key, value);
        } else {
          Serial.println(F("Clave inválida. Debe ser:"));
          Serial.println(F("1=Mes, 2=Día, 3=Hora, 4=Minuto, 5=Segundo, 6=Año"));
        }
      } else {
        Serial.println(F("Formato incorrecto. Use: MOD key value"));
      }
    } else if (strcmp(cmdBuffer, "GETTIME") == 0) {
      // Obtener fecha/hora completa
      getCurrentDateTime();
    } else if (strncmp(cmdBuffer, "GETRTC ", 7) == 0) {
      // Formato: GETRTC key
      // Ejemplo: GETRTC 1 (obtener mes)
      uint8_t key = atoi(cmdBuffer + 7);
      
      // Verificar rango de key
      if (key >= RTC_KEY_MONTH && key <= RTC_KEY_YEAR) {
        uint8_t value = getRTCInfo(key);
        
        if (value != 0xFF) {
          // Nombre para la clave (sin usar String)
          Serial.print(F("Valor: "));
          switch(key) {
            case RTC_KEY_MONTH: Serial.print(F("Mes")); break;
            case RTC_KEY_DAY: Serial.print(F("Día")); break;
            case RTC_KEY_HOUR: Serial.print(F("Hora")); break;
            case RTC_KEY_MINUTE: Serial.print(F("Minuto")); break;
            case RTC_KEY_SECOND: Serial.print(F("Segundo")); break;
            case RTC_KEY_YEAR: Serial.print(F("Año (desde 2000)")); break;
            default: Serial.print(F("Desconocido")); break;
          }
          Serial.print(F(": "));
          Serial.println(value);
        }
      } else {
        Serial.println(F("Clave inválida. Debe ser:"));
        Serial.println(F("1=Mes, 2=Día, 3=Hora, 4=Minuto, 5=Segundo, 6=Año"));
      }
    } else if (strcmp(cmdBuffer, "HELP") == 0) {
      Serial.println(F("Comandos disponibles:"));
      Serial.println(F("GET filename startIdx numSamples - Solicita datos de temperatura"));
      Serial.println(F("LIST - Solicita lista de archivos disponibles"));
      Serial.println(F("MOD key value - Modifica configuración del RTC"));
      Serial.println(F("  key: 1=Mes, 2=Día, 3=Hora, 4=Min, 5=Seg, 6=Año"));
      Serial.println(F("  Ej: MOD 1 10 (cambiar mes a octubre)"));
      Serial.println(F("GETTIME - Obtiene fecha/hora completa"));
      Serial.println(F("GETRTC key - Obtiene dato específico del RTC"));
      Serial.println(F("HELP - Muestra esta ayuda"));
    } else {
      Serial.println(F("Comando desconocido. Escriba HELP para ver los comandos."));
    }
  }
  
  delay(100);
}

void setupRadio() {
  
  Serial.print("Inicializando Radio RF24...");
  if (radio.begin()) {
    
    radio.openWritingPipe(addresses[1]);
    radio.openReadingPipe(1, addresses[0]);
    
    radio.setPALevel(RF24_PA_MAX);
    radio.setRetries(3, 15);
    radio.setChannel(76);  // Ajusta según sea necesario
    radio.setDataRate(RF24_250KBPS);
    
    radio.startListening();

    Serial.println("Radio RF24 inicializado correctamente");
  } else {
    Serial.println("Error inicializando Radio RF24!");
    while (1); // Detener si hay error
  }

}

void requestFileList() {
  
 // Enviar solicitud de lista de archivos
  uint8_t requestCommand = CMD_FILE_LIST;
  radio.stopListening();
  bool sendSuccess = radio.write(&requestCommand, sizeof(requestCommand));
  
  if (sendSuccess) {
    Serial.println("Solicitud enviada correctamente, esperando respuesta...");
    
    // Cambiar a modo escucha
    delay(50);
    radio.startListening();
    
    // Variables para control de recepción
    unsigned long startTime = millis();
    bool timeout = false;
    int fileCount = 0;
    bool listStarted = false;
    bool listEnded = false;
    
    Serial.println("Esperando inicio de transmisión...");
    
    // Bucle principal de recepción
    while (!timeout && !listEnded) {
      // Verificar si hay datos disponibles
      if (radio.available()) {
        startTime = millis(); // Reiniciar timeout
        
        // Leer los datos disponibles en un buffer temporal primero
        uint8_t buffer[32];
        uint8_t bytesAvailable = radio.getPayloadSize();
        radio.read(buffer, bytesAvailable);
        
        // Procesar según el primer byte (comando)
        uint8_t command = buffer[0];
        
        if (command == CMD_LIST_START && !listStarted) {
          // Inicio de lista detectado
          Serial.println("Inicio de lista de archivos detectado");
          listStarted = true;
          
          // Limpiar el array de archivos
          for (int i = 0; i < MAX_FILES; i++) {
            fileList[i][0] = '\0';
          }
        } 
        else if (command == CMD_LIST_END) {
          // Fin de lista detectado
          Serial.println("Fin de lista de archivos detectado");
          listEnded = true;
        }
        else if (command == CMD_FILE_LIST && listStarted) {
          // Verificar tamaño correcto para evitar accesos inválidos
          if (bytesAvailable >= sizeof(FileListResponse)) {
            // Convertir el buffer a la estructura
            FileListResponse* response = (FileListResponse*)buffer;
            
            // Guardar el nombre del archivo en el array
            if (response->fileIndex < MAX_FILES) {
              strcpy(fileList[response->fileIndex], response->filename);
              
              Serial.print("Recibido archivo ");
              Serial.print(response->fileIndex + 1);
              Serial.print("/");
              Serial.print(response->totalFiles);
              Serial.print(": ");
              Serial.println(response->filename);
              
              // Actualizar la cuenta total de archivos
              if (response->fileIndex + 1 > fileCount) {
                fileCount = response->fileIndex + 1;
              }
            }
          } else {
            Serial.println("Error: Tamaño de payload incorrecto para FileListResponse");
          }
        } 
        else {
          // Datos desconocidos
          Serial.print("Datos desconocidos recibidos (");
          Serial.print(bytesAvailable);
          Serial.println(" bytes)");
          
          // Imprimir los bytes para debug
          for (int i = 0; i < bytesAvailable && i < 32; i++) {
            Serial.print(buffer[i], HEX);
            Serial.print(" ");
          }
          Serial.println();
        }
      } 
      else {
        // No hay datos disponibles
        delay(10);
        
        // Verificar timeout global (10 segundos)
        if (millis() - startTime > 10000) {
          Serial.println("Timeout alcanzado");
          timeout = true;
        }
      }
    }
    
    // Mostrar resumen
    Serial.print("Total de archivos recibidos: ");
    Serial.println(fileCount);
    
    if (fileCount > 0) {
      Serial.println("Lista de archivos:");
      for (int i = 0; i < fileCount; i++) {
        if (fileList[i][0] != '\0') {
          Serial.print(i + 1);
          Serial.print(". ");
          Serial.println(fileList[i]);
        } else {
          Serial.print(i + 1);
          Serial.println(". <No recibido>");
        }
      }
    }
    
    // Volver a modo TX cuando terminamos
    radio.stopListening();
  } 
  else {
    Serial.println("Error al enviar la solicitud");
  }

}

/**
 * Función para modificar la configuración del RTC
 * @param key Clave que indica qué parámetro modificar (RTC_KEY_*)
 * @param value Valor a establecer
 * @return true si la comunicación fue exitosa, false en caso contrario
 */
bool modifyRTC(uint8_t key, uint8_t value) {
  Serial.print(F("Enviando solicitud para modificar RTC - Clave: "));
  
  // Imprimir nombre para la clave (sin usar String)
  switch(key) {
    case RTC_KEY_MONTH: Serial.print(F("mes")); break;
    case RTC_KEY_DAY: Serial.print(F("dia")); break;
    case RTC_KEY_HOUR: Serial.print(F("hora")); break;
    case RTC_KEY_MINUTE: Serial.print(F("minuto")); break;
    case RTC_KEY_SECOND: Serial.print(F("segundo")); break;
    case RTC_KEY_YEAR: Serial.print(F("año")); break;
    default: Serial.print(F("desconocida")); break;
  }
  
  Serial.print(F(", Valor: "));
  Serial.println(value);
  
  // Preparar la solicitud
  DataRequest request;
  memset(&request, 0, sizeof(request)); // Limpiar la estructura
  request.command = CMD_MODIFY_RTC;
  request.key = key;
  request.value = value;
  
  // Cambiar a modo de transmisión
  radio.stopListening();
  
  // Enviar solicitud
  if (!radio.write(&request, sizeof(request))) {
    Serial.println(F("Error enviando solicitud"));
    radio.startListening();
    return false;
  }
  
  // Cambiar a modo de recepción
  radio.startListening();
  
  // Esperar respuesta
  unsigned long startTime = millis();
  while (!radio.available()) {
    if (millis() - startTime > 2000) { // Timeout de 2 segundos
      Serial.println(F("Tiempo de espera agotado"));
      return false;
    }
  }
  
  // Leer respuesta
  DataResponse response;
  radio.read(&response, sizeof(response));
  
  // Verificar que es la respuesta correcta
  if (response.command != CMD_MODIFY_RTC) {
    Serial.println(F("Respuesta incorrecta"));
    return false;
  }
  
  // Verificar resultado
  bool success = (response.value == 1);
  if (success) {
    Serial.println(F("RTC modificado correctamente"));
  } else {
    Serial.println(F("Error modificando RTC - Valor inválido"));
  }
  
  return success;
}

/**
 * Función para obtener información del RTC
 * @param key Clave que indica qué información obtener (RTC_KEY_*)
 * @return Valor obtenido, 0xFF si hay error
 */
uint8_t getRTCInfo(uint8_t key) {
  Serial.print(F("Solicitando información del RTC - "));
  
  // Imprimir nombre para la clave (sin usar String)
  switch(key) {
    case RTC_KEY_MONTH: Serial.println(F("mes")); break;
    case RTC_KEY_DAY: Serial.println(F("dia")); break;
    case RTC_KEY_HOUR: Serial.println(F("hora")); break;
    case RTC_KEY_MINUTE: Serial.println(F("minuto")); break;
    case RTC_KEY_SECOND: Serial.println(F("segundo")); break;
    case RTC_KEY_YEAR: Serial.println(F("año")); break;
    default: Serial.println(F("desconocida")); break;
  }

  // Preparar la solicitud
  DataRequest request;
  memset(&request, 0, sizeof(request)); // Limpiar la estructura
  request.command = CMD_GET_RTC_INFO;
  request.key = key;
  
  // Cambiar a modo de transmisión
  radio.stopListening();
  
  // Enviar solicitud
  if (!radio.write(&request, sizeof(request))) {
    Serial.println(F("Error enviando solicitud"));
    radio.startListening();
    return 0xFF;
  }
  
  // Cambiar a modo de recepción
  radio.startListening();
  
  // Esperar respuesta
  unsigned long startTime = millis();
  while (!radio.available()) {
    if (millis() - startTime > 2000) { // Timeout de 2 segundos
      Serial.println(F("Tiempo de espera agotado"));
      return 0xFF;
    }
  }
  
  // Leer respuesta
  DataResponse response;
  radio.read(&response, sizeof(response));
  
  // Verificar que es la respuesta correcta
  if (response.command != CMD_GET_RTC_INFO || response.key != key) {
    Serial.println(F("Respuesta incorrecta"));
    return 0xFF;
  }
  
  Serial.print(F("Valor recibido: "));
  Serial.println(response.value);
  
  return response.value;
}


/**
 * Función para obtener y mostrar la fecha/hora completa del servidor
 */
void getCurrentDateTime() {
  uint8_t year = getRTCInfo(RTC_KEY_YEAR);
  uint8_t month = getRTCInfo(RTC_KEY_MONTH);
  uint8_t day = getRTCInfo(RTC_KEY_DAY);
  uint8_t hour = getRTCInfo(RTC_KEY_HOUR);
  uint8_t minute = getRTCInfo(RTC_KEY_MINUTE);
  uint8_t second = getRTCInfo(RTC_KEY_SECOND);
  
  // Verificar si alguno de los valores es inválido
  if (year == 0xFF || month == 0xFF || day == 0xFF || 
      hour == 0xFF || minute == 0xFF || second == 0xFF) {
    Serial.println(F("Error obteniendo fecha/hora"));
    return;
  }
  
  // Mostrar fecha/hora completa
  char buffer[30];
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
          2000 + year, month, day, hour, minute, second);
  
  Serial.print(F("Fecha/hora actual: "));
  Serial.println(buffer);
}

void requestData(const char* filename, uint16_t startIndex, uint16_t numSamples) {
  Serial.print("Solicitando datos del archivo '");
  Serial.print(filename);
  Serial.print("' desde índice ");
  Serial.print(startIndex);
  Serial.print(" hasta ");
  Serial.println(startIndex + numSamples - 1);
  
  // Preparar solicitud
  DataRequest request;
  request.command = CMD_REQUEST_DATA;
  request.startIndex = startIndex;
  request.numSamples = numSamples;
  
  // IMPORTANTE: Asegurar que el string se copia correctamente
  memset(request.filename, 0, sizeof(request.filename)); // Limpiar el campo
  strncpy(request.filename, filename, sizeof(request.filename) - 1);
  
  // Enviar el comando primero para avisar al servidor
  uint8_t cmd = CMD_REQUEST_DATA;
    
  // Cambiar a modo TX
  radio.stopListening();
  
  // Enviar el comando inicial
  bool cmdSent = radio.write(&cmd, sizeof(cmd));
  
  if (!cmdSent) {
    Serial.println("Error enviando comando inicial");
    radio.startListening();
    return;
  }
    
  // Pequeña pausa para que el servidor se prepare
  delay(200);
  
  // Ahora enviar la solicitud completa
  bool requestSent = false;
  for (int attempt = 0; attempt < 3 && !requestSent; attempt++) {
    requestSent = radio.write(&request, sizeof(request));
    if (!requestSent) {
      Serial.println("Reintentando envío de solicitud...");
      delay(200);
    }
  }
    
  if (!requestSent) {
    Serial.println("Error enviando solicitud después de varios intentos");
    radio.startListening();
    return;
  }
  
  Serial.println("Solicitud enviada. Esperando respuesta...");
  
  // Cambiar a modo escucha
  radio.startListening();
    
  // Esperar respuestas
  unsigned long startTime = millis();
  bool timeout = false;
  uint16_t receivedCount = 0;
  
  Serial.println("Idx,Timestamp,Value0,Value1,Value2,Value3");
  
  while (!timeout && receivedCount < numSamples) {
    if (radio.available()) {
      DataResponse response;
      radio.read(&response, sizeof(response));
      
      if (response.command == CMD_SEND_DATA) {
        // Convertir timestamp a formato fecha/hora
        //unsigned long seconds = response.timestamp;
        //int hours = (seconds / 3600) % 24;
        //int minutes = (seconds / 60) % 60;
        //int secs = seconds % 60;
        
        //char timeStr[9];
        //sprintf(timeStr, "%02d:%02d:%02d", hours, minutes, secs);
        
        // Imprimir datos
        Serial.print(response.index);
        Serial.print(",");
        Serial.print(response.timestamp);
        Serial.print(",");
        Serial.print(response.value0);
        Serial.print(",");
        Serial.print(response.value1);
        Serial.print(",");
        Serial.print(response.value2);
        Serial.print(",");
        Serial.println(response.value3);
        
        receivedCount++;
        startTime = millis(); // Reiniciar el tiempo de espera
      }
    }
    
    // Comprobar timeout (10 segundos sin recibir nada)
    if (millis() - startTime > 10000) {
      timeout = true;
    }
  }
    
  if (timeout && receivedCount < numSamples) {
    Serial.print("Timeout. Se recibieron ");
    Serial.print(receivedCount);
    Serial.print(" de ");
    Serial.print(numSamples);
    Serial.println(" muestras.");
  } else {
    Serial.println("Todos los datos recibidos correctamente.");
  }
  
  // Asegurarse de volver a modo escucha
  radio.startListening();
  
}