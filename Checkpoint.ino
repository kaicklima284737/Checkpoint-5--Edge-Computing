/*
  Checkpoint 5 - Edge Computing
  Projeto: EcoShield IoT - Monitoramento de Estufa e Reservatorio
  VERSAO: HARDWARE REAL (DHT11)

  BIBLIOTECAS (instalar):
  - "DHT sensor library" (Adafruit)
  - "Adafruit Unified Sensor" (Adafruit)

  PLACA: ESP32 (core Arduino-ESP32 2.x ou 3.x)

  SENSORES:
  - DHT11 : temperatura e umidade   (GPIO 15)
  - LDR   : luminosidade            (GPIO 34, ADC)
  - Sensor Ultrassonico: distancia ate a agua   (TRIG 25, ECHO 18)

  ATUADORES:
  - LED    (GPIO 21)
  - Buzzer (GPIO 4) - um tom por problema, tocados em sequencia

  LDR: assume-se que o valor ADC SOBE quando ha MAIS luz.
  Se a montagem for inversa, troque ldrPct por (100 - ldrPct).
*/

#include <DHT.h>

// ---------- Pinos ----------
#define DHTPIN     15
#define DHTTYPE    DHT11
#define LDR_PIN    34
#define TRIG_PIN   25
#define ECHO_PIN   18
#define LED_ALERT  21
#define BUZZER_PIN 4

// ---------- Limites (liga / desliga = histerese) ----------
// Temperatura (C): alerta acima de 31, volta ao normal abaixo de 30
const float TEMP_LIGA = 31.0, TEMP_DESLIGA = 30.0;
// Umidade (%): alerta abaixo de 35, volta ao normal acima de 38
const float UMID_LIGA = 35.0, UMID_DESLIGA = 38.0;
// Luz (% do ADC): alerta acima de 60, volta ao normal abaixo de 55
const float LUZ_LIGA = 60.0, LUZ_DESLIGA = 55.0;
// Nivel de agua (%): alerta abaixo de 20, volta ao normal acima de 25
const float AGUA_LIGA = 20.0, AGUA_DESLIGA = 25.0;

// Reservatorio: distancia do sensor (no topo) ate a agua
const float DIST_CHEIO = 5.0;   // cm  -> 100%
const float DIST_VAZIO = 80.0;  // cm  -> 0%

// Filtro: 0.5 = responde rapido, 0.2 = mais suave
const float ALPHA = 0.5;

// Intervalo entre ciclos (DHT11 precisa de >= 1 s)
const unsigned long INTERVALO = 2000;

// Buzzer: cada problema tem um tom (ordem de execucao: agua, temp, umid, luz)
const int TOM_AGUA = 500, TOM_TEMP = 1000, TOM_UMID = 1500, TOM_LUZ = 2000;
const unsigned long T_TOM = 800;    // tempo tocando (ms)
const unsigned long T_PAUSA = 500;  // pausa entre tons (ms)

// ---------- Variaveis globais ----------
DHT dht(DHTPIN, DHTTYPE);

unsigned long ultimoCiclo = 0;

// Valores filtrados (NAN = ainda sem leitura)
float tempF = NAN, umidF = NAN, luzF = NAN, nivelF = NAN;

// Alertas ativos (guardam o estado entre ciclos por causa da histerese)
bool aTemp = false, aUmid = false, aLuz = false, aAgua = false;

// Controle do buzzer
int tomAtual = 3;              // indice do ultimo tom tocado (0 a 3)
bool tocando = false;
unsigned long inicioBuzzer = 0;

String statusAnterior = "";
bool ledPisca = false;

// ---------- Funcoes auxiliares ----------

// Media movel exponencial: suaviza ruido das leituras
float filtrar(float anterior, float novo) {
  if (isnan(anterior)) return novo;
  return anterior + ALPHA * (novo - anterior);
}

// Alerta que liga quando o valor passa ACIMA do limite
bool acima(bool ativo, float v, float liga, float desliga) {
  if (v > liga) return true;
  if (v < desliga) return false;
  return ativo;  // zona intermediaria: mantem o estado
}

// Alerta que liga quando o valor passa ABAIXO do limite
bool abaixo(bool ativo, float v, float liga, float desliga) {
  if (v < liga) return true;
  if (v > desliga) return false;
  return ativo;
}

// Retorna distancia em cm ou -1 se a leitura falhar
float lerDistancia() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duracao = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duracao == 0) return -1.0;

  float d = duracao * 0.0343 / 2.0;
  if (d < 2.0 || d > 400.0) return -1.0;
  return d;
}

// Toca um tom por problema ativo: 0,8 s toca, 0,5 s pausa, proximo problema.
// Com todos os problemas: agua -> temp -> umidade -> luz -> repete.
// Roda a cada volta do loop, sem usar delay.
void atualizarBuzzer() {
  bool ativo[4] = {aAgua, aTemp, aUmid, aLuz};
  const int tom[4] = {TOM_AGUA, TOM_TEMP, TOM_UMID, TOM_LUZ};

  // Sem problemas: silencio
  if (!(aAgua || aTemp || aUmid || aLuz)) {
    noTone(BUZZER_PIN);
    tocando = false;
    return;
  }

  unsigned long agora = millis();

  if (tocando) {
    // Fim do tom: inicia a pausa
    if (agora - inicioBuzzer >= T_TOM) {
      noTone(BUZZER_PIN);
      tocando = false;
      inicioBuzzer = agora;
    }
    return;
  }

  // Fim da pausa: procura o proximo problema ativo
  if (agora - inicioBuzzer >= T_PAUSA) {
    for (int i = 1; i <= 4; i++) {
      int j = (tomAtual + i) % 4;
      if (ativo[j]) {
        tomAtual = j;
        tone(BUZZER_PIN, tom[j]);
        tocando = true;
        inicioBuzzer = agora;
        return;
      }
    }
  }
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== EcoShield IoT iniciado ===");

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(LDR_PIN, INPUT);
  pinMode(LED_ALERT, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(TRIG_PIN, LOW);
  digitalWrite(LED_ALERT, LOW);

  dht.begin();
  analogReadResolution(12);  // ADC de 0 a 4095
}

// ---------- Loop ----------
void loop() {
  atualizarBuzzer();  // mantem a sequencia de tons rodando

  // Executa um ciclo a cada INTERVALO, sem travar o ESP32
  if (millis() - ultimoCiclo < INTERVALO) return;
  ultimoCiclo = millis();

  // 1. LEITURA DOS SENSORES
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  bool dhtOk = !isnan(t) && !isnan(h);

  float luzPct = analogRead(LDR_PIN) * 100.0 / 4095.0;

  float dist = lerDistancia();
  bool distOk = (dist >= 0);

  // 2. FILTRO (so atualiza se a leitura for valida)
  if (dhtOk) {
    tempF = filtrar(tempF, t);
    umidF = filtrar(umidF, h);
  }
  luzF = filtrar(luzF, luzPct);
  if (distOk) {
    float nivel = (DIST_VAZIO - dist) / (DIST_VAZIO - DIST_CHEIO) * 100.0;
    nivelF = filtrar(nivelF, constrain(nivel, 0.0, 100.0));
  }

  // 3. REGRAS COM HISTERESE
  if (dhtOk) {
    aTemp = acima(aTemp, tempF, TEMP_LIGA, TEMP_DESLIGA);
    aUmid = abaixo(aUmid, umidF, UMID_LIGA, UMID_DESLIGA);
  }
  aLuz = acima(aLuz, luzF, LUZ_LIGA, LUZ_DESLIGA);
  if (distOk) {
    aAgua = abaixo(aAgua, nivelF, AGUA_LIGA, AGUA_DESLIGA);
  }

  bool algumAlerta = aTemp || aUmid || aLuz || aAgua;
  bool incompleto = !dhtOk || !distOk;

  // 4. STATUS EM TEXTO (para detectar mudancas)
  String status = "";
  if (aAgua) status += "[AGUA BAIXA] ";
  if (aTemp) status += "[TEMP ALTA] ";
  if (aUmid) status += "[UMIDADE BAIXA] ";
  if (aLuz)  status += "[LUZ EXCESSIVA] ";
  if (status == "") status = "[OK] ";
  if (incompleto) status += "[SENSOR INVALIDO]";

  // 5. EXIBICAO
  Serial.println("\n==============================");
  if (dhtOk) {
    Serial.printf("Temperatura: %.1f C\n", tempF);
    Serial.printf("Umidade:     %.1f %%\n", umidF);
  } else {
    Serial.println("ERRO: leitura do DHT11 invalida.");
  }
  Serial.printf("Luminosidade: %.0f %%\n", luzF);
  if (distOk) {
    Serial.printf("Nivel de agua: %.0f %% (distancia %.1f cm)\n", nivelF, dist);
  } else {
    Serial.println("ERRO: leitura do ultrassonico invalida.");
  }
  Serial.print("STATUS: ");
  Serial.println(status);

  // Mostra a reacao do sistema quando algo muda
  if (status != statusAnterior) {
    Serial.println(">>> MUDANCA DE ESTADO <<<");
    Serial.print("    ANTES: ");
    Serial.println(statusAnterior == "" ? "(inicio)" : statusAnterior);
    Serial.print("    AGORA: ");
    Serial.println(status);
    statusAnterior = status;
  }

  // 6. LED (o buzzer e tratado em atualizarBuzzer)
  if (algumAlerta) {
    digitalWrite(LED_ALERT, HIGH);       // aceso = alerta
  } else if (incompleto) {
    ledPisca = !ledPisca;                // piscando = sensor invalido
    digitalWrite(LED_ALERT, ledPisca);
  } else {
    digitalWrite(LED_ALERT, LOW);
  }
}
