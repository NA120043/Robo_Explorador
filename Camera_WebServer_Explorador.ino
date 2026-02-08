#include "esp_camera.h"
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

// Defina o nome da rede Wi-Fi e a senha
const char* ssid = "LRNR7912.2.4GHz";
const char* password = "lonary7912";

// Defina o IP estático, o gateway e a máscara de sub-rede
IPAddress local_IP(10, 0, 0, 254);  // Substitua pelo IP desejado
IPAddress gateway(10, 0, 0, 2);
IPAddress subnet(255, 255, 255, 0);

// Pinos dos motores
int motorA1 = 12;  // IN1
int motorA2 = 13;  // IN2
int motorB1 = 14;  // IN3
int motorB2 = 15;  // IN4

// Servidor para controle dos motores
WiFiServer motorServer(8080);

// Fila para comunicação entre threads
QueueHandle_t commandQueue;

// Selecione o modelo da câmera
#define CAMERA_MODEL_AI_THINKER // Utilize este modelo para a câmera OV2640
#include "camera_pins.h"

void startCameraServer();

// Protótipos das funções
void motorControlTask(void *parameter);
void handleCommand(String cmd);

// Variáveis globais para controle dos gatilhos (acessadas apenas pela thread de controle de motores)
volatile bool R1 = false;
volatile bool L1 = false;
volatile bool R2 = false;
volatile bool L2 = false;

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("Inicializando...");

  // Configurar pinos dos motores
  pinMode(motorA1, OUTPUT);
  pinMode(motorA2, OUTPUT);
  pinMode(motorB1, OUTPUT);
  pinMode(motorB2, OUTPUT);
  
  // Garantir que motores começam parados
  digitalWrite(motorA1, LOW);
  digitalWrite(motorA2, LOW);
  digitalWrite(motorB1, LOW);
  digitalWrite(motorB2, LOW);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_VGA;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_VGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  Serial.println("Câmera inicializada com sucesso");

  // Configurar IP estático
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("Configuração de IP estático falhou");
  }

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);
  Serial.print("Conectando-se ao WiFi");

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 20000) { // Tentativa por 20 segundos
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("");
    Serial.println("Falha na conexão WiFi");
    return;
  }

  Serial.println("");
  Serial.println("Conectado ao WiFi");

  startCameraServer();
  
  // Iniciar servidor para controle dos motores
  motorServer.begin();
  
  // Criar fila para comandos (tamanho máximo de 10 comandos)
  commandQueue = xQueueCreate(10, sizeof(char[50]));
  
  // Criar task para controle dos motores
  xTaskCreatePinnedToCore(
    motorControlTask,    // Função da tarefa
    "MotorControlTask",  // Nome da tarefa
    4096,               // Tamanho da pilha
    NULL,               // Parâmetros
    1,                  // Prioridade
    NULL,               // Handle da tarefa
    0                   // Core (0 ou 1)
  );
  
  Serial.print("Câmera pronta! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' para conectar");
  
  Serial.print("Controle de motores na porta: ");
  Serial.println(WiFi.localIP());
  Serial.println("Envie comandos TCP para a porta 8080");
}

void loop() {
  // Thread principal apenas escuta conexões e envia comandos para a fila
  WiFiClient client = motorServer.available();
  
  if (client) {
    Serial.println("Novo cliente conectado para controle de motores");
    
    while (client.connected()) {
      if (client.available()) {
        String message = client.readStringUntil('\n');
        message.trim();
        Serial.println("Comando recebido na thread principal: " + message);
        
        // Enviar comando para a fila (thread de controle de motores)
        char cmdBuffer[50];
        message.toCharArray(cmdBuffer, sizeof(cmdBuffer));
        if (xQueueSend(commandQueue, &cmdBuffer, portMAX_DELAY) == pdTRUE) {
          // Responder ao cliente
          client.println("OK: Comando recebido");
        } else {
          client.println("ERRO: Fila de comandos cheia");
        }
      }
    }
    
    client.stop();
    Serial.println("Cliente desconectado");
  }
  
  delay(10);
}

// Task para controle dos motores (roda em thread separada)
void motorControlTask(void *parameter) {
  char cmdBuffer[50];
  
  Serial.println("Task de controle de motores iniciada");
  
  while (true) {
    // Aguardar comando da fila
    if (xQueueReceive(commandQueue, &cmdBuffer, portMAX_DELAY) == pdTRUE) {
      String command = String(cmdBuffer);
      handleCommand(command);
    }
    
    // Pequena pausa para evitar uso excessivo da CPU
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void handleCommand(String cmd) {
  cmd.toUpperCase();

  // Controle dos gatilhos
  if (cmd == "GATILHO R2: 1.0") {
    R2 = true;
  } 
  else if (cmd == "GATILHO R2: -1.0") {
    R2 = false;
  }
  else if (cmd == "GATILHO L2: 1.0") {
    L2 = true;
  }
  else if (cmd == "GATILHO L2: -1.0") {
    L2 = false;
  }

  // Controle dos Botões
  if (cmd == "BOTãO R1: APERTADO") {
    R1 = true;
  } 
  else if (cmd == "BOTãO R1: SOLTO") {
    R1 = false;
  }
  else if (cmd == "BOTãO L1: APERTADO") {
    L1 = true;
  }
  else if (cmd == "BOTãO L1: SOLTO") {
    L1 = false;
  }
  
  // Controle de movimento baseado nos gatilhos
  if (R2 && L2) {
    // Ambos gatilhos ativados - Frente
    digitalWrite(motorA1, HIGH);
    digitalWrite(motorA2, LOW);
    digitalWrite(motorB1, HIGH);
    digitalWrite(motorB2, LOW);
    Serial.println("Thread Motor: Movendo FRENTE (gatilhos)");
  }
  else if (R2 && !L2) {
    // Gatilhos da direita ativado - Direita
    digitalWrite(motorA1, HIGH);
    digitalWrite(motorA2, LOW);
    digitalWrite(motorB1, LOW);
    digitalWrite(motorB2, LOW);
    Serial.println("Thread Motor: Movendo DIREITA (gatilhos)");
  }
   else if (!R2 && L2) {
    // Gatilhos da esquerda ativado - Esquerda
    digitalWrite(motorA1, LOW);
    digitalWrite(motorA2, LOW);
    digitalWrite(motorB1, HIGH);
    digitalWrite(motorB2, LOW);
    Serial.println("Thread Motor: Movendo ESQUERDA (gatilhos)");
  }

  // Controle de movimento baseado nos Botões
  if (R1 && L1) {
    // Ambos botões ativados - Trás
    digitalWrite(motorA1, LOW);
    digitalWrite(motorA2, HIGH);
    digitalWrite(motorB1, LOW);
    digitalWrite(motorB2, HIGH);
    Serial.println("Thread Motor: Movendo TRÁS (botões)");
  }
  else if (R1 && !L1) {
    // Botão da direita ativado - Direita
    digitalWrite(motorA1, LOW);
    digitalWrite(motorA2, HIGH);
    digitalWrite(motorB1, LOW);
    digitalWrite(motorB2, LOW);
    Serial.println("Thread Motor: Movendo DIREITA (botão)");
  }
   else if (!R1 && L1) {
    // Botão da esquerda ativado - Esquerda
    digitalWrite(motorA1, LOW);
    digitalWrite(motorA2, LOW);
    digitalWrite(motorB1, LOW);
    digitalWrite(motorB2, HIGH);
    Serial.println("Thread Motor: Movendo ESQUERDA (botão)");
  }

  if (!R2 && !L2 && !R1 && !L1) {
    // Ambos gatilhos desativados - Parar
    digitalWrite(motorA1, LOW);
    digitalWrite(motorA2, LOW);
    digitalWrite(motorB1, LOW);
    digitalWrite(motorB2, LOW);
    Serial.println("Thread Motor: PARANDO motores");
  }
}