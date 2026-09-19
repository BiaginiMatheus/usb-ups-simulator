/*
  UPS.ino — Nobreak "virtual" para o QNAP TS-419P II
  Arduino Pro Micro (ATmega32U4) compilado SEMPRE como "Arduino Leonardo" (VID/PID 2341:8036)
  Biblioteca: abratchik/HIDPowerDevice

  Diferenças para o exemplo original da biblioteca:
   1. Sem leitura analógica de SoC. Não existe bateria real para medir — o estado de
      carga é uma SIMULAÇÃO por software, integrada a partir do tempo em que a rede
      elétrica esteve ausente.
   2. Polaridade do detector de rede INVERTIDA (falha segura). Ver AC_PRESENT_LEVEL.
   3. bCapacityMode = 1 (mWh) por padrão — o modo 2 (%) é exibido como 0% em
      notebooks Windows que já têm bateria interna (issue #11 da biblioteca).
   4. Base de tempo em millis(), sem delay() bloqueante no caminho da medição.
   5. BelowRemainingCapacityLimit habilitado — é o flag que vira "LB" (low battery)
      no NUT e é o que efetivamente dispara o shutdown no NAS.

  A placa começa SEMPRE com a bateria cheia (BATT_INITIAL_SOC). Dar boot em 0%
  faria o QNAP desligar assim que enxergasse o dispositivo.
*/

#include <HIDPowerDevice.h>

// ============================ CONFIGURAÇÃO ============================

#define DEBUG_SERIAL          0        // 0 obrigatorio na board 'UPS NUT': sem CDC nao ha Serial

// ---- Pinos ----
#define ACDETECTPIN           4        // saída do optoacoplador (presença de rede)
#define RUNSTATUSPIN          5        // LED de heartbeat (pisca 1 Hz)
#define COMMLOSTPIN           10       // HIGH quando o envio de relatório falha

// ---- Polaridade do detector de rede ----
// O PC817 vai CONDUZIR enquanto houver rede (LED alimentado pelo carregador 5V
// ligado na tomada da rede, nunca no nobreak). Conduzindo, ele puxa o pino para GND.
//   rede presente      -> transistor conduz -> pino LOW
//   rede ausente       -> transistor aberto -> pull-up interno -> pino HIGH
//   fio solto / opto queimado -> também HIGH -> lido como "caiu a energia"
// Alarme falso é seguro; alarme que nunca dispara não é.
#define AC_PRESENT_LEVEL      LOW

// Em BANCADA, sem o opto montado, isto significa:
//   pino 4 flutuando  = SEM rede (descarregando)
//   pino 4 no GND     = COM rede (carregando)
#define AC_DEBOUNCE_MS        300UL

// ---- Modelo de bateria simulada ----
// Calibrado pelo teste de bancada com o Siera UPS500 (18/09/2026): 40 min ate o
// apito com ~20W, escalado para os ~30W do NAS e descontado o efeito Peukert ->
// apito real esperado em ~22 min. Com o LB em 90% do timer, 1000 s poe o NAS
// comecando a desligar em ~15 min, 7 min antes do nobreak avisar.
#define BATT_RUNTIME_FULL_S   1000UL    // autonomia a 100% de carga, em segundos
#define BATT_RECHARGE_S       3600UL   // tempo de 0% a 100%, em segundos
#define BATT_INITIAL_SOC      100.0f   // carga no boot

// ---- Limiares de alarme (em %) ----
// Deliberadamente SEPARADOS de iWarnCapacityLimit / iRemnCapacityLimit.
// Aquelas variáveis são features HID graváveis: o host (Windows, e possivelmente
// o QNAP) pode sobrescrevê-las via HID_SET_REPORT, e aí o alarme passa a depender
// de um valor que não é nosso. Os flags de status usam estes #define.
#define SOC_WARN_PCT          20
#define SOC_LOW_PCT           10

// ---- Relatórios ----
#define MINUPDATEINTERVAL_MS  26000UL  // envio periódico mesmo sem mudança

// ============================ ESTADO HID ============================

const char STRING_DEVICECHEMISTRY[] PROGMEM = "PbAc";
const char STRING_OEMVENDOR[]       PROGMEM = "Matheus";
const char STRING_SERIAL[]          PROGMEM = "UPS10";

const byte bDeviceChemistry = IDEVICECHEMISTRY;
const byte bOEMVendor       = IOEMVENDOR;

PresentStatus iPresentStatus = {}, iPreviousStatus = {};

byte bRechargable  = 1;

// 2 = porcentagem, 1 = mWh.
// Mantenha 1 para testar no notebook. No QNAP tanto faz: FullChargeCapacity = 100,
// então o NUT lê battery.charge como 0..100 nos dois modos.
byte bCapacityMode = 1;

// Parâmetros físicos
const uint16_t iConfigVoltage = 1380;
uint16_t iVoltage = 1300,        iPrevVoltage = 0;
uint16_t iRunTimeToEmpty = 0,    iPrevRunTimeToEmpty = 0;
uint16_t iAvgTimeToFull  = (uint16_t)BATT_RECHARGE_S;
uint16_t iAvgTimeToEmpty = (uint16_t)BATT_RUNTIME_FULL_S;

// Limite de tempo restante que marca RemainingTimeLimitExpired.
// O descritor HID aceita 120..1380 s. Tem que ser BEM menor que BATT_RUNTIME_FULL_S,
// senão o flag nasce ativo e o NAS desliga na primeira queda.
uint16_t iRemainTimeLimit = 120;

int16_t  iDelayBe4Reboot   = -1;
int16_t  iDelayBe4ShutDown = -1;   // o host escreve aqui para pedir desligamento
uint16_t iManufacturerDate = 0;
byte     iAudibleAlarmCtrl = 1;    // 1 Disabled, 2 Enabled, 3 Muted

// Compatibilidade ACPI
const byte iDesignCapacity = 100;
byte iWarnCapacityLimit    = 20;   // aviso em 20%
byte iRemnCapacityLimit    = 10;   // crítico em 10%  -> vira LB no NUT
const byte bCapacityGranularity1 = 1;
const byte bCapacityGranularity2 = 1;
byte iFullChargeCapacity   = 100;

byte iRemaining = (byte)BATT_INITIAL_SOC, iPrevRemaining = 0;

int iRes = 0;

// ============================ ESTADO INTERNO ============================

float         fSoC        = BATT_INITIAL_SOC;   // 0.0 .. 100.0
bool          bACPresent  = true;               // valor já filtrado
bool          bACRaw      = true;               // última leitura crua
unsigned long ulACChange  = 0;
unsigned long ulLastTick  = 0;
unsigned long ulLastSend  = 0;
unsigned long ulLastBlink = 0;
bool          bLedState   = false;

// ============================ SETUP ============================

void setup() {

#if DEBUG_SERIAL
  Serial.begin(57600);
#endif

  PowerDevice.begin();
  PowerDevice.setSerial(STRING_SERIAL);   // vira o nome da porta

#if DEBUG_SERIAL
  PowerDevice.setOutput(Serial);
#endif

  pinMode(ACDETECTPIN, INPUT_PULLUP);
  pinMode(RUNSTATUSPIN, OUTPUT);
  pinMode(COMMLOSTPIN, OUTPUT);

  // leitura inicial sem esperar o debounce
  bACRaw = bACPresent = (digitalRead(ACDETECTPIN) == AC_PRESENT_LEVEL);

  PowerDevice.setFeature(HID_PD_PRESENTSTATUS,    &iPresentStatus,   sizeof(iPresentStatus));

  PowerDevice.setFeature(HID_PD_RUNTIMETOEMPTY,   &iRunTimeToEmpty,  sizeof(iRunTimeToEmpty));
  PowerDevice.setFeature(HID_PD_AVERAGETIME2FULL, &iAvgTimeToFull,   sizeof(iAvgTimeToFull));
  PowerDevice.setFeature(HID_PD_AVERAGETIME2EMPTY,&iAvgTimeToEmpty,  sizeof(iAvgTimeToEmpty));
  PowerDevice.setFeature(HID_PD_REMAINTIMELIMIT,  &iRemainTimeLimit, sizeof(iRemainTimeLimit));
  PowerDevice.setFeature(HID_PD_DELAYBE4REBOOT,   &iDelayBe4Reboot,  sizeof(iDelayBe4Reboot));
  PowerDevice.setFeature(HID_PD_DELAYBE4SHUTDOWN, &iDelayBe4ShutDown,sizeof(iDelayBe4ShutDown));

  PowerDevice.setFeature(HID_PD_RECHARGEABLE,     &bRechargable,     sizeof(bRechargable));
  PowerDevice.setFeature(HID_PD_CAPACITYMODE,     &bCapacityMode,    sizeof(bCapacityMode));
  PowerDevice.setFeature(HID_PD_CONFIGVOLTAGE,    &iConfigVoltage,   sizeof(iConfigVoltage));
  PowerDevice.setFeature(HID_PD_VOLTAGE,          &iVoltage,         sizeof(iVoltage));

  PowerDevice.setStringFeature(HID_PD_IDEVICECHEMISTRY, &bDeviceChemistry, STRING_DEVICECHEMISTRY);
  PowerDevice.setStringFeature(HID_PD_IOEMINFORMATION,  &bOEMVendor,       STRING_OEMVENDOR);

  PowerDevice.setFeature(HID_PD_AUDIBLEALARMCTRL, &iAudibleAlarmCtrl, sizeof(iAudibleAlarmCtrl));

  PowerDevice.setFeature(HID_PD_DESIGNCAPACITY,   &iDesignCapacity,  sizeof(iDesignCapacity));
  PowerDevice.setFeature(HID_PD_FULLCHRGECAPACITY,&iFullChargeCapacity, sizeof(iFullChargeCapacity));
  PowerDevice.setFeature(HID_PD_REMAININGCAPACITY,&iRemaining,       sizeof(iRemaining));
  PowerDevice.setFeature(HID_PD_WARNCAPACITYLIMIT,&iWarnCapacityLimit, sizeof(iWarnCapacityLimit));
  PowerDevice.setFeature(HID_PD_REMNCAPACITYLIMIT,&iRemnCapacityLimit, sizeof(iRemnCapacityLimit));
  PowerDevice.setFeature(HID_PD_CPCTYGRANULARITY1,&bCapacityGranularity1, sizeof(bCapacityGranularity1));
  PowerDevice.setFeature(HID_PD_CPCTYGRANULARITY2,&bCapacityGranularity2, sizeof(bCapacityGranularity2));

  uint16_t year = 2026, month = 9, day = 16;
  iManufacturerDate = (year - 1980) * 512 + month * 32 + day;
  PowerDevice.setFeature(HID_PD_MANUFACTUREDATE, &iManufacturerDate, sizeof(iManufacturerDate));

  ulLastTick = ulLastSend = ulLastBlink = ulACChange = millis();
}

// ============================ LOOP ============================

void loop() {

  unsigned long now = millis();

  // ---------- 1. Detector de rede, com debounce ----------
  bool bRaw = (digitalRead(ACDETECTPIN) == AC_PRESENT_LEVEL);
  if (bRaw != bACRaw) {
    bACRaw     = bRaw;
    ulACChange = now;
  }
  if ((now - ulACChange) >= AC_DEBOUNCE_MS) {
    bACPresent = bACRaw;
  }

  // ---------- 2. Integração da carga simulada ----------
  unsigned long dt = now - ulLastTick;
  if (dt >= 250UL) {                       // passo mínimo de 250 ms
    ulLastTick = now;
    float fSec = dt / 1000.0f;

    if (bACPresent) {
      fSoC += 100.0f * fSec / (float)BATT_RECHARGE_S;
      if (fSoC > 100.0f) fSoC = 100.0f;
    } else {
      fSoC -= 100.0f * fSec / (float)BATT_RUNTIME_FULL_S;
      if (fSoC < 0.0f) fSoC = 0.0f;
    }
  }

  iRemaining      = (byte)(fSoC + 0.5f);
  iRunTimeToEmpty = (uint16_t)((float)BATT_RUNTIME_FULL_S * fSoC / 100.0f);

  // O host pode gravar nestas duas features (HID_SET_REPORT escreve direto na
  // variável). O Windows faz isso: foi visto mexendo em iRemnCapacityLimit para
  // 88, 85, 81... Reimpomos todo ciclo, para que o valor que o NUT lê como
  // battery.charge.low no NAS seja sempre o nosso.
  iWarnCapacityLimit = SOC_WARN_PCT;
  iRemnCapacityLimit = SOC_LOW_PCT;

  // ---------- 3. PresentStatus ----------
  bool bFull        = (fSoC >= 99.5f);
  bool bDischarging = !bACPresent;

  iPresentStatus.ACPresent     = bACPresent;
  iPresentStatus.Charging      = (bACPresent && !bFull);
  iPresentStatus.Discharging   = bDischarging;
  iPresentStatus.FullyCharged  = bFull;
  iPresentStatus.FullyDischarged = (iRemaining == 0);
  iPresentStatus.BatteryPresent  = 1;

  // Este é o flag que o NUT traduz como LB (low battery) e que faz o NAS desligar.
  // Usa o limiar interno, não a feature gravável pelo host.
  iPresentStatus.BelowRemainingCapacityLimit = (iRemaining <= SOC_LOW_PCT);

  iPresentStatus.RemainingTimeLimitExpired =
      (bDischarging && iRunTimeToEmpty < iRemainTimeLimit);

  // Desligamento pedido pelo host (o QNAP escreve em DelayBeforeShutdown)
  iPresentStatus.ShutdownRequested = (iDelayBe4ShutDown > 0);

  iPresentStatus.ShutdownImminent =
      (iPresentStatus.ShutdownRequested || iPresentStatus.RemainingTimeLimitExpired);

  // ---------- 4. Heartbeat ----------
  if ((now - ulLastBlink) >= 1000UL) {
    ulLastBlink = now;
    bLedState = !bLedState;
    digitalWrite(RUNSTATUSPIN, bLedState ? HIGH : LOW);
  }

  // ---------- 5. Envio de relatórios ----------
  bool bChanged = (iPresentStatus != iPreviousStatus)
               || (iRemaining != iPrevRemaining)
               || (iRunTimeToEmpty != iPrevRunTimeToEmpty);

  if (bChanged || (now - ulLastSend) >= MINUPDATEINTERVAL_MS) {

    PowerDevice.sendReport(HID_PD_REMAININGCAPACITY, &iRemaining, sizeof(iRemaining));
    if (bDischarging)
      PowerDevice.sendReport(HID_PD_RUNTIMETOEMPTY, &iRunTimeToEmpty, sizeof(iRunTimeToEmpty));
    iRes = PowerDevice.sendReport(HID_PD_PRESENTSTATUS, &iPresentStatus, sizeof(iPresentStatus));

    digitalWrite(COMMLOSTPIN, (iRes < 0) ? HIGH : LOW);

    ulLastSend          = now;
    iPreviousStatus     = iPresentStatus;
    iPrevRemaining      = iRemaining;
    iPrevRunTimeToEmpty = iRunTimeToEmpty;

#if DEBUG_SERIAL
    Serial.print(F("AC="));   Serial.print(bACPresent ? 1 : 0);
    Serial.print(F(" SoC=")); Serial.print(iRemaining);
    Serial.print(F("% RT=")); Serial.print(iRunTimeToEmpty);
    Serial.print(F("s ST=0x"));
    Serial.print((uint16_t)iPresentStatus, HEX);
    Serial.print(F(" res="));  Serial.print(iRes);
    Serial.print(F(" dly="));  Serial.print(iDelayBe4ShutDown);
    // wl/rl revelam se o host sobrescreveu os limites. Devem ficar em 20 e 10.
    Serial.print(F(" wl="));   Serial.print(iWarnCapacityLimit);
    Serial.print(F(" rl="));   Serial.println(iRemnCapacityLimit);
#endif
  }

  delay(20);
}
