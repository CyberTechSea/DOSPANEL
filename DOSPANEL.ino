/*
 * DOSPANEL.ino  v5.0
 * Pannello di controllo per retrocomputer DOS
 * Display: JC3248W535 (ESP32-S3, 480x320, touch)
 * 4 pagine navigabili con touch bordo sx/dx
 *
 * Connessioni MAX3232 → ESP32-S3:
 *   MAX3232 TX  →  GPIO 18
 *   MAX3232 GND →  GND
 *   MAX3232 VCC →  3.3V
 */

#include <JC3248W535.h>
#include <math.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <WebServer.h>
#include <SD.h>
#include <SPI.h>
#include <AudioFileSourceSD.h>
#include <AudioGeneratorMP3.h>
#include <AudioGeneratorWAV.h>
#include <AudioOutputI2S.h>

// -------------------------------------------------------
// DISPLAY + TOUCH
// -------------------------------------------------------
JC3248W535_Display display;
JC3248W535_Touch   touch;
Arduino_Canvas* gfx = nullptr;

#define SCR_W  480
#define SCR_H  320

// -------------------------------------------------------
// WIFI — modifica con le tue credenziali
// -------------------------------------------------------
#define WIFI_SSID  "NomeReteWiFi"
#define WIFI_PASS  "PasswordWiFi"

WebServer        httpServer(80);
WebSocketsServer wsServer(81);
bool wifiOk = false;
unsigned long lastWsBroadcast = 0;

// -------------------------------------------------------
// MICROSD — pin dallo schematico JC3248W535
// -------------------------------------------------------
#define SD_CS   10   // TF_CS
#define SD_MOSI 11   // MCU_MOSI
#define SD_CLK  12   // TF_CLK
#define SD_MISO 13   // MCU_MISO
#define LOG_INTERVAL_SEC 60
bool sdOk = false;
unsigned long lastLogTime = 0;
SPIClass sdSPI(HSPI);  // usa bus HSPI separato dal display
uint8_t currentPage = 0;
#define NUM_PAGES    5
#define TOUCH_ZONE   60

// -------------------------------------------------------
// AUDIO PLAYER — I2S dallo schematico JC3248W535
// -------------------------------------------------------
#define I2S_BCLK   42
#define I2S_LRCLK  2
#define I2S_DOUT   41
#define AUDIO_DIR  "/AUDIO"
#define MAX_TRACKS 32

// Buffer forma d'onda per visualizzazione
#define WAVE_SAMPLES 120
int16_t waveBuf[WAVE_SAMPLES] = {0};
uint8_t waveBufIdx = 0;

AudioFileSourceSD* audioSource = nullptr;
AudioGeneratorMP3* mp3Gen      = nullptr;
AudioGeneratorWAV* wavGen      = nullptr;
AudioOutputI2S*    audioOut    = nullptr;

struct Track { char name[32]; bool isMP3; };
Track    playlist[MAX_TRACKS];
uint8_t  trackCount    = 0;
int8_t   currentTrack  = -1;
bool     isPlaying     = false;
bool     isPaused      = false;
float    audioVolume   = 0.5f;
unsigned long trackStartTime = 0;

void loadPlaylist(){
  trackCount=0; currentTrack=-1;
  if(!sdOk) return;
  File dir=SD.open(AUDIO_DIR);
  if(!dir){ Serial.println("Cartella /AUDIO non trovata"); return; }
  File f=dir.openNextFile();
  while(f && trackCount<MAX_TRACKS){
    if(!f.isDirectory()){
      String nm=String(f.name()); nm.toUpperCase();
      if(nm.endsWith(".MP3")||nm.endsWith(".WAV")){
        String orig=String(f.name());
        orig.toCharArray(playlist[trackCount].name,32);
        playlist[trackCount].isMP3=nm.endsWith(".MP3");
        trackCount++;
      }
    }
    f.close(); f=dir.openNextFile();
  }
  dir.close();
  Serial.printf("Playlist: %d tracce\n",trackCount);
}

void audioStop(){
  if(mp3Gen){ if(mp3Gen->isRunning()) mp3Gen->stop(); delete mp3Gen; mp3Gen=nullptr; }
  if(wavGen){ if(wavGen->isRunning()) wavGen->stop(); delete wavGen; wavGen=nullptr; }
  if(audioSource){ delete audioSource; audioSource=nullptr; }
  isPlaying=false; isPaused=false;
}

void audioPlay(int8_t idx){
  if(idx<0||idx>=trackCount||!audioOut) return;
  audioStop();
  currentTrack=idx;
  String path=String(AUDIO_DIR)+"/"+String(playlist[idx].name);
  Serial.println("Play: "+path);
  audioSource=new AudioFileSourceSD(path.c_str());
  if(playlist[idx].isMP3){
    mp3Gen=new AudioGeneratorMP3();
    mp3Gen->begin(audioSource,audioOut);
  } else {
    wavGen=new AudioGeneratorWAV();
    wavGen->begin(audioSource,audioOut);
  }
  isPlaying=true; isPaused=false; trackStartTime=millis();
}

void audioNext(){ if(trackCount>0) audioPlay((currentTrack+1)%trackCount); }
void audioPrev(){ if(trackCount>0) audioPlay((currentTrack-1+trackCount)%trackCount); }

void audioUpdate(){
  if(!isPlaying||isPaused||!audioOut) return;
  if(millis()-trackStartTime < 2000) return;

  bool running=false;
  if(mp3Gen&&mp3Gen->isRunning()){
    running=mp3Gen->loop();
    // Campiona il livello audio per la forma d'onda
    // Usiamo il tempo come proxy sinusoidale per simulare
    // quando non abbiamo accesso diretto ai campioni decodificati
  }
  if(wavGen&&wavGen->isRunning()) running=wavGen->loop();

  if(!running&&isPlaying){
    isPlaying=false;
    audioNext();
  }
}

// Aggiorna buffer forma d'onda (chiamato ogni frame)
void updateWaveform(){
  if(!isPlaying||isPaused){
    // Decadimento quando fermo
    for(int i=0;i<WAVE_SAMPLES;i++) waveBuf[i]=waveBuf[i]*0.9f;
    return;
  }
  // Genera forma d'onda pseudo-casuale basata sul tempo
  // che simula l'audio in modo realistico
  unsigned long t=millis();
  float base=sinf(t*0.003f)*0.6f +
             sinf(t*0.007f)*0.3f +
             sinf(t*0.013f)*0.2f;
  // Shift buffer
  for(int i=WAVE_SAMPLES-1;i>0;i--) waveBuf[i]=waveBuf[i-1];
  // Nuovo campione con rumore
  float noise=(random(100)-50)/200.0f;
  waveBuf[0]=(int16_t)((base+noise)*audioVolume*100.0f);
}

// -------------------------------------------------------
// COLORI RGB565
// -------------------------------------------------------
#define COL_BG       0x0000
#define COL_WHITE    0xFFFF
#define COL_GREEN    0x07E0
#define COL_YELLOW   0xFFE0
#define COL_RED      0xF800
#define COL_CYAN     0x07FF
#define COL_GRAY     0x7BEF
#define COL_DKGREEN  0x03E0
#define COL_AMBER    0xFD20
#define COL_DKRED    0x4000

// -------------------------------------------------------
// SERIALE
// -------------------------------------------------------
#define RX_PIN      18
#define SERIAL_BAUD 9600

// -------------------------------------------------------
// DATI RICEVUTI DAL PC
// -------------------------------------------------------
struct DosData {
  // Pagina 1
  uint16_t ram_kb;
  uint8_t  hh, mm, ss;
  uint8_t  dd, mo;
  uint16_t yy;
  uint8_t  dos_major, dos_minor;
  uint8_t  drive_active;
  // Pagina 2
  uint16_t cpu_type;
  char     bios_info[16];
  char     drives[64];
  char     vol_label[16];
  // Pagina 3
  uint8_t  kbd_caps, kbd_num, kbd_scroll;
  uint8_t  mouse_present;
  uint16_t mouse_x, mouse_y;
  uint8_t  mouse_btn;
  uint16_t com_baud;
  uint16_t open_files;
  bool     valid;
};

DosData data;
String  rxBuffer = "";
unsigned long lastReceived = 0;
bool connected = false;

// -------------------------------------------------------
// SPACE INVADERS — SPRITE
// -------------------------------------------------------
const uint8_t ALIEN_A[8][11] = {
  {0,0,1,0,0,0,0,0,1,0,0},
  {0,0,0,1,0,0,0,1,0,0,0},
  {0,0,1,1,1,1,1,1,1,0,0},
  {0,1,1,0,1,1,1,0,1,1,0},
  {1,1,1,1,1,1,1,1,1,1,1},
  {1,0,1,1,1,1,1,1,1,0,1},
  {1,0,1,0,0,0,0,0,1,0,1},
  {0,0,0,1,1,0,1,1,0,0,0},
};

const uint8_t ALIEN_B[8][11] = {
  {0,0,1,0,0,0,0,0,1,0,0},
  {1,0,0,1,0,0,0,1,0,0,1},
  {1,0,1,1,1,1,1,1,1,0,1},
  {1,1,1,0,1,1,1,0,1,1,1},
  {1,1,1,1,1,1,1,1,1,1,1},
  {0,1,1,1,1,1,1,1,1,1,0},
  {0,0,1,0,0,0,0,0,1,0,0},
  {0,1,0,0,0,0,0,0,0,1,0},
};

const uint8_t SHIP[4][11] = {
  {0,0,0,0,0,1,0,0,0,0,0},
  {0,0,0,1,1,1,1,1,0,0,0},
  {0,1,1,1,1,1,1,1,1,1,0},
  {1,1,1,1,1,1,1,1,1,1,1},
};

const uint8_t SHIELD_SPRITE[4][6] = {
  {0,1,1,1,1,0},
  {1,1,1,1,1,1},
  {1,1,0,0,1,1},
  {1,1,0,0,1,1},
};

// -------------------------------------------------------
// SPACE INVADERS — COSTANTI
// -------------------------------------------------------
#define SI_SCALE    2
#define AREA_X      8
#define AREA_Y      190
#define AREA_W      464
#define AREA_H      96
#define ALIEN_W     (11 * SI_SCALE)
#define ALIEN_H     (8  * SI_SCALE)
#define ALIEN_GAP_X 4
#define ALIEN_GAP_Y 4
#define SI_COLS     8
#define SI_ROWS     2
#define MAX_ALIENS        (SI_COLS * SI_ROWS)
#define MAX_ALIEN_BULLETS 8
#define MAX_EXPLOSIONS    6
#define NUM_SHIELDS       4

// -------------------------------------------------------
// SPACE INVADERS — STRUTTURE
// -------------------------------------------------------
struct Alien     { int8_t c, r; bool alive; };
struct Bullet    { int16_t x, y; bool active; };
struct Explosion { int16_t x, y; uint8_t t; };
struct Shield    { int16_t x, y; uint8_t pixels[4][6]; };

Alien     aliens[MAX_ALIENS];
Bullet    playerBullet;
Bullet    alienBullets[MAX_ALIEN_BULLETS];
Explosion explosions[MAX_EXPLOSIONS];
Shield    shields[NUM_SHIELDS];

int16_t  alienOffsetX    = 0;
int8_t   alienDir         = 1;
bool     alienDropPending = false;
uint8_t  alienFrame       = 0;
uint16_t alienMoveTimer   = 0;
int16_t  shipX;
uint16_t siScore          = 0;
uint8_t  siLives          = 3;
unsigned long lastSIUpdate = 0;

// -------------------------------------------------------
// UTILITY SPRITE
// -------------------------------------------------------
void drawSprite11x8(const uint8_t sprite[8][11], int ax, int ay, uint16_t color) {
  for (int r = 0; r < 8; r++)
    for (int c = 0; c < 11; c++)
      if (sprite[r][c])
        gfx->fillRect(ax + c*SI_SCALE, ay + r*SI_SCALE, SI_SCALE, SI_SCALE, color);
}

void drawSprite11x4(const uint8_t sprite[4][11], int ax, int ay, uint16_t color) {
  for (int r = 0; r < 4; r++)
    for (int c = 0; c < 11; c++)
      if (sprite[r][c])
        gfx->fillRect(ax + c*SI_SCALE, ay + r*SI_SCALE, SI_SCALE, SI_SCALE, color);
}

int alienScreenX(int col) { return AREA_X + alienOffsetX + col*(ALIEN_W+ALIEN_GAP_X); }
int alienScreenY(int row) { return AREA_Y + 12 + row*(ALIEN_H+ALIEN_GAP_Y); }

// -------------------------------------------------------
// SPACE INVADERS — INIT
// -------------------------------------------------------
void initSI() {
  for (int r = 0; r < SI_ROWS; r++)
    for (int c = 0; c < SI_COLS; c++)
      aliens[r*SI_COLS+c] = {(int8_t)c, (int8_t)r, true};

  alienOffsetX = 0; alienDir = 1; alienDropPending = false;
  alienFrame = 0;   alienMoveTimer = 0;
  shipX = AREA_W / 2;
  playerBullet.active = false;
  for (int i = 0; i < MAX_ALIEN_BULLETS; i++) alienBullets[i].active = false;
  for (int i = 0; i < MAX_EXPLOSIONS;   i++) explosions[i].t = 0;

  int shieldXpos[NUM_SHIELDS] = {60, 160, 260, 360};
  for (int s = 0; s < NUM_SHIELDS; s++) {
    shields[s].x = shieldXpos[s];
    shields[s].y = AREA_H - 30;
    memcpy(shields[s].pixels, SHIELD_SPRITE, sizeof(SHIELD_SPRITE));
  }
}

void addExplosion(int x, int y) {
  for (int i = 0; i < MAX_EXPLOSIONS; i++) {
    if (explosions[i].t == 0) { explosions[i] = {(int16_t)x,(int16_t)y,12}; break; }
  }
}

// -------------------------------------------------------
// SPACE INVADERS — UPDATE
// -------------------------------------------------------
void updateSI(uint16_t dt) {
  alienMoveTimer += dt;
  int aliveCount = 0;
  for (int i = 0; i < MAX_ALIENS; i++) if (aliens[i].alive) aliveCount++;
  if (aliveCount == 0) { initSI(); return; }

  // Bug fix: se qualche alieno supera il fondo dell'area, reset immediato
  for (int i = 0; i < MAX_ALIENS; i++) {
    if (!aliens[i].alive) continue;
    if (alienScreenY(aliens[i].r) + ALIEN_H >= AREA_Y + AREA_H - 4) {
      initSI(); return;
    }
  }

  uint16_t speed = 300 - min((uint16_t)(aliveCount*10),(uint16_t)250);

  if (alienMoveTimer >= speed) {
    alienMoveTimer = 0;
    alienFrame = 1 - alienFrame;

    if (alienDropPending) {
      for (int i = 0; i < MAX_ALIENS; i++) aliens[i].r++;
      alienDir = -alienDir; alienDropPending = false;
      // Controllo immediato dopo il drop: se fuori area, reset
      for (int i = 0; i < MAX_ALIENS; i++) {
        if (!aliens[i].alive) continue;
        if (alienScreenY(aliens[i].r) + ALIEN_H >= AREA_Y + AREA_H - 4) {
          initSI(); return;
        }
      }
    } else {
      alienOffsetX += alienDir * 6;
      int minX=9999, maxX=-9999;
      for (int i = 0; i < MAX_ALIENS; i++) {
        if (!aliens[i].alive) continue;
        int sx = alienScreenX(aliens[i].c);
        if (sx < minX) minX = sx;
        if (sx+ALIEN_W > maxX) maxX = sx+ALIEN_W;
      }
      if (maxX >= AREA_X+AREA_W-4 || minX <= AREA_X+4) alienDropPending = true;
    }

    // Sparo alieno
    if (random(100) < 18) {
      int idx = random(MAX_ALIENS), tries = 0;
      while (!aliens[idx].alive && tries++ < 20) idx = random(MAX_ALIENS);
      if (aliens[idx].alive) {
        for (int i = 0; i < MAX_ALIEN_BULLETS; i++) {
          if (!alienBullets[i].active) {
            alienBullets[i] = {
              (int16_t)(alienScreenX(aliens[idx].c)+ALIEN_W/2),
              (int16_t)(alienScreenY(aliens[idx].r)+ALIEN_H), true};
            break;
          }
        }
      }
    }
  }

  // Proiettile navicella
  if (playerBullet.active) {
    playerBullet.y -= 5;
    if (playerBullet.y < AREA_Y) { playerBullet.active = false; }
    else {
      for (int i = 0; i < MAX_ALIENS; i++) {
        if (!aliens[i].alive) continue;
        int ax=alienScreenX(aliens[i].c), ay=alienScreenY(aliens[i].r);
        if (playerBullet.x>=ax && playerBullet.x<=ax+ALIEN_W &&
            playerBullet.y>=ay && playerBullet.y<=ay+ALIEN_H) {
          aliens[i].alive = false;
          addExplosion(ax+ALIEN_W/2, ay+ALIEN_H/2);
          siScore += 10; playerBullet.active = false; break;
        }
      }
    }
  }

  // Proiettili alieni
  int16_t sx2 = AREA_X+shipX-(11*SI_SCALE/2), sy2 = AREA_Y+AREA_H-12;
  for (int i = 0; i < MAX_ALIEN_BULLETS; i++) {
    if (!alienBullets[i].active) continue;
    alienBullets[i].y += 3;
    if (alienBullets[i].y > AREA_Y+AREA_H) { alienBullets[i].active=false; continue; }
    if (alienBullets[i].x>=sx2 && alienBullets[i].x<=sx2+22 &&
        alienBullets[i].y>=sy2 && alienBullets[i].y<=sy2+8) {
      siLives--;
      addExplosion(AREA_X+shipX, sy2+4);
      alienBullets[i].active = false;
      if (siLives==0) { siLives=3; siScore=0; initSI(); return; }
    }
  }

  // Esplosioni
  for (int i = 0; i < MAX_EXPLOSIONS; i++) if (explosions[i].t>0) explosions[i].t--;

  // AI navicella
  int targetRow=-1, targetCol=0;
  for (int i = 0; i < MAX_ALIENS; i++) {
    if (!aliens[i].alive) continue;
    if (aliens[i].r > targetRow) { targetRow=aliens[i].r; targetCol=aliens[i].c; }
  }
  int tx = alienScreenX(targetCol)+ALIEN_W/2-AREA_X;
  if (abs(shipX-tx)>3) shipX += (tx>shipX)?2:-2;
  shipX = constrain(shipX, 14, AREA_W-14);

  if (!playerBullet.active && random(100)<4)
    playerBullet = {(int16_t)(AREA_X+shipX),(int16_t)(AREA_Y+AREA_H-16),true};
}

// -------------------------------------------------------
// SPACE INVADERS — DRAW
// -------------------------------------------------------
void drawSI() {
  gfx->fillRect(AREA_X, AREA_Y, AREA_W, AREA_H, 0x0001);
  gfx->drawRect(AREA_X, AREA_Y, AREA_W, AREA_H, COL_DKGREEN);

  char buf[24];
  gfx->setTextSize(1);
  gfx->setTextColor(COL_AMBER);
  sprintf(buf,"SCORE:%04d",siScore);
  gfx->setCursor(AREA_X+4, AREA_Y+2); gfx->print(buf);
  gfx->setTextColor(COL_RED);
  sprintf(buf,"LIVES:%d",siLives);
  gfx->setCursor(AREA_X+AREA_W-60, AREA_Y+2); gfx->print(buf);

  // Scudi
  for (int s=0;s<NUM_SHIELDS;s++)
    for (int r=0;r<4;r++)
      for (int c=0;c<6;c++)
        if (shields[s].pixels[r][c])
          gfx->fillRect(AREA_X+shields[s].x+c*SI_SCALE,
                        AREA_Y+shields[s].y+r*SI_SCALE,
                        SI_SCALE,SI_SCALE,COL_GREEN);

  // Alieni
  for (int i=0;i<MAX_ALIENS;i++) {
    if (!aliens[i].alive) continue;
    uint16_t col = (aliens[i].r==0)?COL_CYAN:COL_GREEN;
    if (alienFrame==0) drawSprite11x8(ALIEN_A,alienScreenX(aliens[i].c),alienScreenY(aliens[i].r),col);
    else               drawSprite11x8(ALIEN_B,alienScreenX(aliens[i].c),alienScreenY(aliens[i].r),col);
  }

  // Navicella
  drawSprite11x4(SHIP, AREA_X+shipX-(11*SI_SCALE/2), AREA_Y+AREA_H-12, COL_CYAN);

  // Proiettile navicella
  if (playerBullet.active)
    gfx->fillRect(playerBullet.x-1, playerBullet.y, 2, 6, COL_WHITE);

  // Proiettili alieni
  for (int i=0;i<MAX_ALIEN_BULLETS;i++)
    if (alienBullets[i].active)
      gfx->fillRect(alienBullets[i].x-1, alienBullets[i].y, 2, 6, COL_RED);

  // Esplosioni
  int dirs[8][2] = {{1,0},{-1,0},{0,1},{0,-1},{1,1},{-1,1},{1,-1},{-1,-1}};
  for (int i=0;i<MAX_EXPLOSIONS;i++) {
    if (explosions[i].t==0) continue;
    int r=(12-explosions[i].t)*2;
    for (int d=0;d<8;d++)
      gfx->drawLine(explosions[i].x, explosions[i].y,
                    explosions[i].x+dirs[d][0]*r,
                    explosions[i].y+dirs[d][1]*r, COL_YELLOW);
  }

  // Linea terra
  gfx->drawFastHLine(AREA_X+2, AREA_Y+AREA_H-3, AREA_W-4, COL_GREEN);
}

// -------------------------------------------------------
// LOGO STELLARIS
// -------------------------------------------------------
void drawLogo() {
  int cx=425, cy=62, r=13;
  uint16_t col = COL_AMBER;

  gfx->drawCircle(cx, cy, r, col);
  gfx->drawCircle(cx, cy, (int)(r*0.70f), col);

  float angles[4] = {0, M_PI/2, M_PI, M_PI*3/2};
  for (int i=0;i<4;i++) {
    float a = angles[i];
    gfx->drawLine(cx+(int)(cosf(a)*(r-1)), cy+(int)(sinf(a)*(r-1)),
                  cx+(int)(cosf(a)*(r+3)), cy+(int)(sinf(a)*(r+3)), col);
  }

  int cr=r+5, cL=5;
  int corners[4][2]={{-1,-1},{1,-1},{1,1},{-1,1}};
  for (int i=0;i<4;i++) {
    int bx=cx+corners[i][0]*cr, by=cy+corners[i][1]*cr;
    gfx->drawLine(bx,by, bx-corners[i][0]*cL, by, col);
    gfx->drawLine(bx,by, bx, by-corners[i][1]*cL, col);
  }

  // "Stellaris" a sinistra del mirino, allineato verticalmente al centro
  gfx->setTextSize(1);
  gfx->setTextColor(col);
  gfx->setCursor(cx - r - 5 - 54, cy - 4); // 54px = larghezza appross. "Stellaris" size1
  gfx->print("Stellaris");
}

// -------------------------------------------------------
// PANNELLO PRINCIPALE
// -------------------------------------------------------
void drawBorder() {
  gfx->drawRect(2,2,SCR_W-4,SCR_H-4,COL_AMBER);
  gfx->drawRect(4,4,SCR_W-8,SCR_H-8,COL_DKGREEN);
}

void drawTitle() {
  gfx->setTextSize(2); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(10,12); gfx->print("[ DOS SYSTEM MONITOR ]");
  gfx->drawFastHLine(8,36,SCR_W-16,COL_DKGREEN);
}

void drawClock() {
  char buf[16];
  gfx->setTextSize(3); gfx->setTextColor(COL_GREEN);
  gfx->setCursor(10,42);
  sprintf(buf,"%02d:%02d:%02d",data.hh,data.mm,data.ss);
  gfx->print(buf);
  gfx->setTextSize(1); gfx->setTextColor(COL_GRAY);
  gfx->setCursor(10,75);
  sprintf(buf,"%02d/%02d/%04d",data.dd,data.mo,data.yy);
  gfx->print(buf);
}

void drawRam() {
  const uint16_t totalRam=640;
  uint16_t usedRam=totalRam-data.ram_kb;
  if(usedRam>totalRam) usedRam=totalRam;
  int barX=80,barY=94,barW=388,barH=18;
  gfx->setTextSize(1); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(10,98); gfx->print("RAM:");
  gfx->fillRect(barX,barY,barW,barH,0x0841);
  int fillW=(int)((float)usedRam/totalRam*barW);
  uint16_t barColor=COL_GREEN;
  if(usedRam>totalRam*0.75f) barColor=COL_YELLOW;
  if(usedRam>totalRam*0.90f) barColor=COL_RED;
  gfx->fillRect(barX,barY,fillW,barH,barColor);
  gfx->drawRect(barX,barY,barW,barH,COL_GRAY);
  char buf[32];
  sprintf(buf,"%dKB liberi / %dKB",data.ram_kb,totalRam);
  gfx->setTextColor(0x0000); gfx->setCursor(barX+4,barY+5); gfx->print(buf);
}

void drawDosVersion() {
  char buf[20];
  gfx->setTextSize(2); gfx->setTextColor(COL_CYAN);
  gfx->setCursor(10,122);
  sprintf(buf,"MS-DOS %d.%d",data.dos_major,data.dos_minor);
  gfx->print(buf);
}

void drawDrive() {
  gfx->setTextSize(1); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(36,152); gfx->print("DRIVE ACTIVITY");
  int lx=18,ly=162,lr=8;
  if(data.drive_active) {
    gfx->fillCircle(lx,ly,lr+3,0x4000);
    gfx->fillCircle(lx,ly,lr,COL_RED);
  } else {
    gfx->fillCircle(lx,ly,lr,0x2000);
    gfx->drawCircle(lx,ly,lr,COL_GRAY);
  }
}

void drawExtras() {
  gfx->drawFastVLine(260,90,95,COL_DKGREEN);
  const char* labels[]={"PWR","CLK","MEM","I/O"};
  uint16_t colors[]={COL_GREEN,COL_AMBER,COL_CYAN,COL_GREEN};
  for(int i=0;i<4;i++){
    int lx=280+i*52, ly=130;
    gfx->fillCircle(lx,ly,6,colors[i]);
    gfx->setTextSize(1); gfx->setTextColor(COL_GRAY);
    gfx->setCursor(lx-9,ly+10); gfx->print(labels[i]);
  }
  gfx->drawRect(268,157,200,22,COL_DKGREEN);
  gfx->setTextSize(1); gfx->setTextColor(COL_GREEN);
  gfx->setCursor(276,163); gfx->print("SYSTEM OK");
}

void drawStatus() {
  gfx->drawFastHLine(8,294,SCR_W-16,COL_DKGREEN);
  gfx->setTextSize(1);
  if(connected){
    gfx->setTextColor(COL_GREEN);
    gfx->setCursor(10,300); gfx->print("* CONNECTED  COM1 @ 9600");
  } else {
    gfx->setTextColor(COL_RED);
    gfx->setCursor(10,300); gfx->print("o WAITING FOR DOS...  ");
    gfx->setTextColor(COL_GRAY);
    gfx->print("Run VUMONITOR.EXE on the PC");
  }
  char buf[24];
  unsigned long sec=millis()/1000;
  sprintf(buf,"UP:%02lu:%02lu:%02lu",sec/3600,(sec%3600)/60,sec%60);
  gfx->setTextColor(COL_GRAY); gfx->setCursor(360,300); gfx->print(buf);
}

// -------------------------------------------------------
// PARSING PACCHETTO (esteso v3)
// -------------------------------------------------------
bool parsePacket(String pkt) {
  if(!pkt.startsWith("$")) return false;
  pkt=pkt.substring(1);
  String f[15]; int fc=0,idx=0;
  for(int i=0;i<=(int)pkt.length()&&fc<15;i++){
    if(i==(int)pkt.length()||pkt[i]==';'){f[fc++]=pkt.substring(idx,i);idx=i+1;}
  }
  if(fc<5) return false;
  if(f[0].startsWith("RAM:")) data.ram_kb=f[0].substring(4).toInt();
  data.hh=f[1].substring(0,2).toInt(); data.mm=f[1].substring(3,5).toInt();
  data.ss=f[1].substring(6,8).toInt(); data.dd=f[2].substring(0,2).toInt();
  data.mo=f[2].substring(3,5).toInt(); data.yy=f[2].substring(6,10).toInt();
  if(f[3].startsWith("DOS:")){data.dos_major=f[3].substring(4,5).toInt();data.dos_minor=f[3].substring(6).toInt();}
  if(f[4].startsWith("DRV:")) data.drive_active=f[4].substring(4).toInt();
  for(int i=5;i<fc;i++){
    if(f[i].startsWith("CPU:"))  data.cpu_type=f[i].substring(4).toInt();
    else if(f[i].startsWith("BIOS:")){ String t=f[i].substring(5); t.toCharArray(data.bios_info,16); }
    else if(f[i].startsWith("DSK:")) { String t=f[i].substring(4); t.toCharArray(data.drives,64); }
    else if(f[i].startsWith("VOL:")) { String t=f[i].substring(4); t.toCharArray(data.vol_label,16); }
    else if(f[i].startsWith("KBD:")){
      data.kbd_caps=f[i][4]-'0'; data.kbd_num=f[i][5]-'0'; data.kbd_scroll=f[i][6]-'0';
    }
    else if(f[i].startsWith("MOUSE:")){
      String ms=f[i].substring(6); data.mouse_present=ms[0]-'0';
      if(data.mouse_present){
        int c1=ms.indexOf(','),c2=ms.indexOf(',',c1+1),c3=ms.indexOf(',',c2+1);
        data.mouse_x=ms.substring(c1+1,c2).toInt();
        data.mouse_y=ms.substring(c2+1,c3).toInt();
        data.mouse_btn=ms.substring(c3+1).toInt();
      }
    }
    else if(f[i].startsWith("COM:"))   data.com_baud=f[i].substring(4).toInt();
    else if(f[i].startsWith("FILES:")) data.open_files=f[i].substring(6).toInt();
  }
  data.valid=true; return true;
}

// -------------------------------------------------------
// SNAKE (Pagina 2)
// -------------------------------------------------------
#define SN_AX 8
#define SN_AY 190
#define SN_AW 464
#define SN_AH 96
#define SN_C  8
#define SN_COLS (SN_AW/SN_C)
#define SN_ROWS (SN_AH/SN_C)
#define SN_MAX 200
struct SnSeg{int8_t x,y;};
SnSeg sn_body[SN_MAX];
int sn_len=5; int8_t sn_dx=1,sn_dy=0,sn_fx,sn_fy;
uint16_t sn_score=0,sn_timer=0; bool sn_dead=false;

void sn_food(){bool ok=false;while(!ok){sn_fx=random(SN_COLS);sn_fy=random(SN_ROWS);ok=true;for(int i=0;i<sn_len;i++)if(sn_body[i].x==sn_fx&&sn_body[i].y==sn_fy){ok=false;break;}}}
void sn_init(){sn_len=5;sn_dx=1;sn_dy=0;sn_score=0;sn_dead=false;sn_timer=0;for(int i=0;i<sn_len;i++)sn_body[i]={(int8_t)(10-i),(int8_t)5};sn_food();}
void sn_update(uint16_t dt){
  sn_timer+=dt;
  uint16_t spd=max(80,200-sn_score*2);
  if(sn_timer<spd) return;
  sn_timer=0;
  if(sn_dead){sn_init();return;}

  int8_t hx=sn_body[0].x, hy=sn_body[0].y;

  // AI solo se NON siamo sulla pagina Snake giocabile (pagina indice 1)
  if(currentPage!=1){
    int8_t wx=(sn_fx>hx)?1:(sn_fx<hx)?-1:0;
    int8_t wy=(sn_fy>hy)?1:(sn_fy<hy)?-1:0;
    int8_t ndx=sn_dx, ndy=sn_dy;
    if(wx!=0&&!(wx==-sn_dx&&sn_dy==0)){ndx=wx;ndy=0;}
    else if(wy!=0&&!(wy==-sn_dy&&sn_dx==0)){ndx=0;ndy=wy;}
    // Evita collisioni
    int8_t tnx=hx+ndx, tny=hy+ndy;
    bool bad=tnx<0||tnx>=SN_COLS||tny<0||tny>=SN_ROWS;
    for(int i=0;i<sn_len&&!bad;i++)
      if(sn_body[i].x==tnx&&sn_body[i].y==tny) bad=true;
    if(bad){
      int8_t alts[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
      for(int a=0;a<4;a++){
        if(alts[a][0]==-sn_dx&&alts[a][1]==0) continue;
        if(alts[a][1]==-sn_dy&&alts[a][0]==0) continue;
        int8_t ax=hx+alts[a][0], ay=hy+alts[a][1];
        if(ax<0||ax>=SN_COLS||ay<0||ay>=SN_ROWS) continue;
        bool sb=false;
        for(int i=0;i<sn_len;i++)
          if(sn_body[i].x==ax&&sn_body[i].y==ay){sb=true;break;}
        if(!sb){ndx=alts[a][0];ndy=alts[a][1];break;}
      }
    }
    sn_dx=ndx; sn_dy=ndy;
  }

  // Muovi serpente nella direzione corrente
  int8_t nx=hx+sn_dx, ny=hy+sn_dy;
  if(nx<0||nx>=SN_COLS||ny<0||ny>=SN_ROWS){sn_dead=true;return;}
  for(int i=0;i<sn_len;i++)
    if(sn_body[i].x==nx&&sn_body[i].y==ny){sn_dead=true;return;}
  for(int i=sn_len-1;i>0;i--) sn_body[i]=sn_body[i-1];
  sn_body[0]={(int8_t)nx,(int8_t)ny};
  if(nx==sn_fx&&ny==sn_fy){
    if(sn_len<SN_MAX) sn_len++;
    sn_score+=10;
    sn_food();
  }
}
void sn_draw(){
  gfx->fillRect(SN_AX,SN_AY,SN_AW,SN_AH,COL_BG);gfx->drawRect(SN_AX,SN_AY,SN_AW,SN_AH,COL_DKGREEN);
  char buf[24];gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);
  sprintf(buf,"SNAKE:%04d",sn_score);gfx->setCursor(SN_AX+4,SN_AY+2);gfx->print(buf);
  gfx->fillRect(SN_AX+sn_fx*SN_C+1,SN_AY+sn_fy*SN_C+1,SN_C-2,SN_C-2,COL_RED);
  for(int i=0;i<sn_len;i++){uint16_t col=(i==0)?COL_WHITE:(i%2==0?COL_GREEN:COL_DKGREEN);gfx->fillRect(SN_AX+sn_body[i].x*SN_C+1,SN_AY+sn_body[i].y*SN_C+1,SN_C-2,SN_C-2,col);}
  if(sn_dead){gfx->setTextColor(COL_RED);gfx->setCursor(SN_AX+SN_AW/2-30,SN_AY+SN_AH/2-5);gfx->print("GAME OVER");}
}

// -------------------------------------------------------
// PONG (Pagina 3)
// -------------------------------------------------------
#define PG_AX 8
#define PG_AY 190
#define PG_AW 464
#define PG_AH 96
#define PG_PW 4
#define PG_PH 20
#define PG_BW 5
struct PongSt{float bx,by,bdx,bdy;int p1y,p2y;uint8_t sc1,sc2;uint16_t timer;};
PongSt pg;
void pg_init(){pg.bx=PG_AW/2;pg.by=PG_AH/2;pg.bdx=2;pg.bdy=1.4f;pg.p1y=PG_AH/2;pg.p2y=PG_AH/2;}
void pg_update(uint16_t dt){
  pg.timer+=dt;if(pg.timer<16)return;pg.timer=0;
  pg.bx+=pg.bdx;pg.by+=pg.bdy;
  if(pg.by<=2){pg.by=2;pg.bdy=fabsf(pg.bdy);}if(pg.by>=PG_AH-PG_BW-2){pg.by=PG_AH-PG_BW-2;pg.bdy=-fabsf(pg.bdy);}
  if(pg.p1y<pg.by&&currentPage!=2)pg.p1y=min(pg.p1y+3,(int)PG_AH-PG_PH/2-2);
  if(pg.p1y>pg.by&&currentPage!=2)pg.p1y=max(pg.p1y-3,PG_PH/2+2);
  if(pg.p2y<pg.by)pg.p2y=min(pg.p2y+3,(int)PG_AH-PG_PH/2-2);if(pg.p2y>pg.by)pg.p2y=max(pg.p2y-3,PG_PH/2+2);
  if(pg.bx<=8+PG_PW+1&&pg.by>=pg.p1y-PG_PH/2&&pg.by<=pg.p1y+PG_PH/2){pg.bx=8+PG_PW+1;pg.bdx=fabsf(pg.bdx)*1.05f;pg.bdy+=(pg.by-pg.p1y)*0.1f;}
  if(pg.bx>=PG_AW-8-PG_PW-PG_BW-1&&pg.by>=pg.p2y-PG_PH/2&&pg.by<=pg.p2y+PG_PH/2){pg.bx=PG_AW-8-PG_PW-PG_BW-1;pg.bdx=-fabsf(pg.bdx)*1.05f;pg.bdy+=(pg.by-pg.p2y)*0.1f;}
  pg.bdx=constrain(pg.bdx,-6.0f,6.0f);pg.bdy=constrain(pg.bdy,-5.0f,5.0f);
  if(pg.bx<2){pg.sc2++;pg_init();}if(pg.bx>PG_AW-2){pg.sc1++;pg_init();}
  if(pg.sc1>=9||pg.sc2>=9){pg.sc1=0;pg.sc2=0;pg_init();}
}
void pg_draw(){
  gfx->fillRect(PG_AX,PG_AY,PG_AW,PG_AH,COL_BG);gfx->drawRect(PG_AX,PG_AY,PG_AW,PG_AH,COL_DKGREEN);
  char buf[16];gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);
  sprintf(buf,"PONG  %d : %d",pg.sc1,pg.sc2);gfx->setCursor(PG_AX+PG_AW/2-30,PG_AY+2);gfx->print(buf);
  for(int y=PG_AY+10;y<PG_AY+PG_AH-10;y+=8)gfx->fillRect(PG_AX+PG_AW/2-1,y,2,4,COL_GRAY);
  gfx->fillRect(PG_AX+6,PG_AY+pg.p1y-PG_PH/2,PG_PW,PG_PH,COL_GREEN);
  gfx->fillRect(PG_AX+PG_AW-6-PG_PW,PG_AY+pg.p2y-PG_PH/2,PG_PW,PG_PH,COL_CYAN);
  gfx->fillRect(PG_AX+(int)pg.bx,PG_AY+(int)pg.by,PG_BW,PG_BW,COL_WHITE);
}

// -------------------------------------------------------
// VU METER SIMULATO (Pagina 4) — senza hardware mic
// -------------------------------------------------------
float vuL[16]={0}, vuR[16]={0}, vuPL[16]={0}, vuPR[16]={0};

void vu_update(unsigned long t){
  for(int b=0;b<16;b++){
    float base=sinf(t/1000.0f*(0.3f+b*0.05f))*0.5f+0.5f;
    float tL=constrain(base*(0.6f+b*0.02f),0,1);
    float tR=constrain(base*(0.55f+b*0.03f),0,1);
    vuL[b]=vuL[b]*(tL>vuL[b]?0.4f:0.93f)+tL*(tL>vuL[b]?0.6f:0.07f);
    vuR[b]=vuR[b]*(tR>vuR[b]?0.4f:0.93f)+tR*(tR>vuR[b]?0.6f:0.07f);
    if(vuL[b]>vuPL[b])vuPL[b]=vuL[b]; else vuPL[b]*=0.995f;
    if(vuR[b]>vuPR[b])vuPR[b]=vuR[b]; else vuPR[b]*=0.995f;
  }
}

void vu_drawChannel(float* lv, float* pk, const char* label, int topY, int maxH){
  int BAR_W=(SCR_W-24)/16-2;
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);
  gfx->setCursor(8,topY);gfx->print(label);
  for(int b=0;b<16;b++){
    int bx=20+b*(BAR_W+2);
    int fillH=(int)(lv[b]*maxH);
    int peakY=topY+maxH-(int)(pk[b]*maxH);
    gfx->fillRect(bx,topY,BAR_W,maxH,COL_BG);
    if(fillH>0){
      int gH=min(fillH,(int)(maxH*0.5f));
      int yH=min(fillH-gH,(int)(maxH*0.25f));
      int rH=fillH-gH-yH;
      int y=topY+maxH;
      y-=gH;  gfx->fillRect(bx,y,BAR_W,gH, COL_GREEN);
      y-=yH;  gfx->fillRect(bx,y,BAR_W,yH, COL_YELLOW);
      y-=rH;  gfx->fillRect(bx,y,BAR_W,rH, COL_RED);
    }
    if(pk[b]>0.02f) gfx->fillRect(bx,peakY,BAR_W,2,COL_WHITE);
    gfx->drawRect(bx,topY,BAR_W,maxH,COL_DKGREEN);
  }
}

void vu_draw(){
  int CH_H=110;
  vu_drawChannel(vuL,vuPL,"L",42,CH_H);
  gfx->drawFastHLine(8,42+CH_H+4,SCR_W-16,COL_DKGREEN);
  vu_drawChannel(vuR,vuPR,"R",42+CH_H+10,CH_H);
}

// -------------------------------------------------------
// PAGINA 5 — AUDIO PLAYER
// -------------------------------------------------------
// Touch zones pagina 5 (coordinate Y nel sistema touch invertito)
#define BTN_PREV_Y1  380  // bordo sinistro area centrale
#define BTN_PREV_Y2  480
#define BTN_PLAY_Y1  240
#define BTN_PLAY_Y2  380
#define BTN_NEXT_Y1  100
#define BTN_NEXT_Y2  240
#define BTN_VDOWN_Y1 0
#define BTN_VDOWN_Y2 100

void drawPage5(){
  char buf[48];

  // Titolo sezione
  gfx->setTextSize(1); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(10,42); gfx->print("AUDIO PLAYER");
  gfx->drawFastHLine(8,54,SCR_W-16,COL_DKGREEN);

  // Nome traccia corrente
  gfx->setTextSize(1); gfx->setTextColor(COL_GRAY);
  gfx->setCursor(10,60); gfx->print("NOW PLAYING:");
  if(currentTrack>=0 && trackCount>0){
    gfx->setTextSize(1); gfx->setTextColor(COL_WHITE);
    gfx->setCursor(10,72);
    // Tronca nome se troppo lungo
    String nm=String(playlist[currentTrack].name);
    if(nm.length()>40) nm=nm.substring(0,40)+"...";
    gfx->print(nm.c_str());
  } else {
    gfx->setTextColor(COL_GRAY); gfx->setCursor(10,72);
    gfx->print("Nessuna traccia");
  }

  // Stato
  gfx->setTextSize(1);
  if(isPlaying && !isPaused){
    gfx->setTextColor(COL_GREEN); gfx->setCursor(10,86); gfx->print(">> PLAYING");
  } else if(isPaused){
    gfx->setTextColor(COL_YELLOW); gfx->setCursor(10,86); gfx->print("|| PAUSED");
  } else {
    gfx->setTextColor(COL_GRAY); gfx->setCursor(10,86); gfx->print("[] STOPPED");
  }

  // Tempo trascorso
  if(isPlaying){
    unsigned long elapsed=(millis()-trackStartTime)/1000;
    sprintf(buf,"%02lu:%02lu",elapsed/60,elapsed%60);
    gfx->setTextColor(COL_CYAN); gfx->setCursor(200,86); gfx->print(buf);
  }

  // Barra volume
  gfx->setTextSize(1); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(10,100); gfx->print("VOL:");
  int volW=(int)(audioVolume*200);
  gfx->fillRect(45,100,200,8,COL_BG);
  gfx->fillRect(45,100,volW,8,COL_CYAN);
  gfx->drawRect(45,100,200,8,COL_GRAY);
  sprintf(buf,"%d%%",(int)(audioVolume*100));
  gfx->setTextColor(COL_WHITE); gfx->setCursor(250,100); gfx->print(buf);

  // Pulsanti grafici
  int btnY=118, btnH=30, btnW=80;
  // [PREV]
  gfx->drawRect(10,btnY,btnW,btnH,COL_GRAY);
  gfx->setTextColor(COL_WHITE); gfx->setTextSize(1);
  gfx->setCursor(22,btnY+11); gfx->print("<< PREV");
  // [PLAY/PAUSE]
  uint16_t playCol=isPlaying?COL_YELLOW:COL_GREEN;
  gfx->drawRect(100,btnY,btnW,btnH,playCol);
  gfx->setTextColor(playCol); gfx->setCursor(108,btnY+11);
  gfx->print(isPlaying&&!isPaused?"|| PAUSE":">> PLAY");
  // [STOP]
  gfx->drawRect(190,btnY,btnW,btnH,COL_RED);
  gfx->setTextColor(COL_RED); gfx->setCursor(208,btnY+11); gfx->print("[] STOP");
  // [NEXT]
  gfx->drawRect(280,btnY,btnW,btnH,COL_GRAY);
  gfx->setTextColor(COL_WHITE); gfx->setCursor(293,btnY+11); gfx->print("NEXT >>");
  // [V-] [V+]
  gfx->drawRect(370,btnY,40,btnH,COL_GRAY);
  gfx->setTextColor(COL_CYAN); gfx->setCursor(381,btnY+11); gfx->print("V-");
  gfx->drawRect(420,btnY,50,btnH,COL_GRAY);
  gfx->setTextColor(COL_CYAN); gfx->setCursor(431,btnY+11); gfx->print("V+");

  // Separatore playlist / VU meter
  gfx->drawFastHLine(8,155,SCR_W-16,COL_DKGREEN);
  gfx->drawFastVLine(285,155,130,COL_DKGREEN); // divide playlist e VU

  // --- PLAYLIST (sinistra) ---
  gfx->setTextSize(1); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(10,160); gfx->print("PLAYLIST:");
  sprintf(buf,"%d",trackCount);
  gfx->setTextColor(COL_GRAY); gfx->setCursor(260,160); gfx->print(buf);

  if(trackCount==0){
    gfx->setTextColor(COL_GRAY); gfx->setCursor(10,176);
    gfx->print("Nessun file in /AUDIO");
  } else {
    int startIdx=max(0,(int)currentTrack-2);
    int endIdx=min((int)trackCount,startIdx+6);
    for(int i=startIdx;i<endIdx;i++){
      int lineY=172+(i-startIdx)*18;
      if(i==currentTrack){
        gfx->fillRect(8,lineY-2,275,16,0x0841);
        gfx->setTextColor(COL_GREEN);
        gfx->setCursor(10,lineY); gfx->print(">");
      } else {
        gfx->setTextColor(COL_GRAY);
        gfx->setCursor(10,lineY); gfx->print(" ");
      }
      String nm=String(playlist[i].name);
      if(nm.length()>28) nm=nm.substring(0,28);
      gfx->setCursor(20,lineY); gfx->print(nm.c_str());
    }
  }

  // --- VU METER FORMA D'ONDA (destra) ---
  const int WX=292, WY=160, WW=175, WH=120;
  gfx->fillRect(WX,WY,WW,WH,COL_BG);
  gfx->drawRect(WX,WY,WW,WH,COL_DKGREEN);

  // Etichetta
  gfx->setTextSize(1); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(WX+4,WY+3); gfx->print("WAVE");

  if(isPlaying && !isPaused){
    // Linea centrale
    int midY=WY+WH/2;
    gfx->drawFastHLine(WX+1,midY,WW-2,0x1082);

    // Disegna forma d'onda
    int plotW=WW-4;
    int step=max(1,WAVE_SAMPLES/plotW);
    int prevX=WX+2, prevY=midY;

    for(int x=0;x<plotW;x++){
      int idx=(x*WAVE_SAMPLES)/plotW;
      idx=constrain(idx,0,WAVE_SAMPLES-1);
      int sampleY=midY - constrain(waveBuf[idx],-WH/2+4,WH/2-4);
      int curX=WX+2+x;

      // Colore gradiente: verde→giallo→rosso in base all'ampiezza
      int amp=abs(waveBuf[idx]);
      uint16_t wCol=amp>60?COL_RED:amp>30?COL_YELLOW:COL_GREEN;

      if(x>0) gfx->drawLine(prevX,prevY,curX,sampleY,wCol);
      prevX=curX; prevY=sampleY;
    }
  } else {
    // Linea piatta quando fermo
    int midY=WY+WH/2;
    gfx->drawFastHLine(WX+2,midY,WW-4,COL_DKGREEN);
    gfx->setTextSize(1); gfx->setTextColor(COL_GRAY);
    gfx->setCursor(WX+30,WY+WH/2-4);
    gfx->print(isPaused?"PAUSED":"STOPPED");
  }
}

// Touch handler pagina 5
void handleTouchPage5(uint16_t ty){
  int dispX = 480 - (int)ty;
  Serial.printf("Page5 touch: ty=%d dispX=%d\n", ty, dispX);

  // Bordo sinistro (ty grande) → torna alla pagina 4
  if(ty > 480 - TOUCH_ZONE){
    currentPage--;
    return;
  }

  // Pulsanti player
  if(dispX>=10 && dispX<=89){           // PREV
    audioPrev();
  } else if(dispX>=100 && dispX<=179){  // PLAY/PAUSE
    if(isPlaying && !isPaused){ isPaused=true; }
    else if(isPaused){ isPaused=false; }
    else if(trackCount>0){ audioPlay(max(0,(int)currentTrack)); }
  } else if(dispX>=190 && dispX<=269){  // STOP
    audioStop();
  } else if(dispX>=280 && dispX<=359){  // NEXT
    audioNext();
  } else if(dispX>=360 && dispX<=409){  // VOL-
    audioVolume=max(0.0f,audioVolume-0.1f);
    if(audioOut) audioOut->SetGain(audioVolume);
  } else {                              // VOL+ (tutto il resto a destra)
    audioVolume=min(1.0f,audioVolume+0.1f);
    if(audioOut) audioOut->SetGain(audioVolume);
  }
}

// -------------------------------------------------------
// ELEMENTI COMUNI
// -------------------------------------------------------
void drawTitle(const char* extra=""){
  gfx->setTextSize(2);gfx->setTextColor(COL_AMBER);
  gfx->setCursor(10,12);gfx->print("[ DOS SYSTEM MONITOR ]");
  if(extra[0]){gfx->setTextSize(1);gfx->setTextColor(COL_GRAY);gfx->setCursor(SCR_W-50,18);gfx->print(extra);}
  gfx->drawFastHLine(8,36,SCR_W-16,COL_DKGREEN);
}
void drawPageDots(){
  for(int i=0;i<NUM_PAGES;i++){
    int x=SCR_W/2-((NUM_PAGES-1)*8)+i*16,y=287;
    if(i==currentPage) gfx->fillCircle(x,y,4,COL_AMBER);
    else               gfx->drawCircle(x,y,4,COL_GRAY);
  }
}
void drawArrows(){
  gfx->setTextColor(COL_GRAY);gfx->setTextSize(2);
  if(currentPage>0){gfx->setCursor(6,150);gfx->print("<");}
  if(currentPage<NUM_PAGES-1){gfx->setCursor(SCR_W-18,150);gfx->print(">");}
}

// -------------------------------------------------------
// PAGINA 2 — CPU, BIOS, Drive, Volume
// -------------------------------------------------------
void drawPage2(){
  drawLogo();
  char buf[32];
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(10,42);gfx->print("PROCESSOR:");
  gfx->setTextSize(2);gfx->setTextColor(COL_CYAN);gfx->setCursor(10,54);
  sprintf(buf,"i%d",data.cpu_type);gfx->print(buf);
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(10,80);gfx->print("BIOS:");
  gfx->setTextColor(COL_GREEN);gfx->setCursor(55,80);gfx->print(data.bios_info);
  gfx->drawFastHLine(8,92,SCR_W-16,COL_DKGREEN);
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(10,98);gfx->print("DRIVES:");
  String dsks=String(data.drives);int ypos=110,start=0;
  while(start<(int)dsks.length()&&ypos<182){
    int comma=dsks.indexOf(',',start);
    String entry=(comma<0)?dsks.substring(start):dsks.substring(start,comma);
    if(entry.length()>2){
      char letter=entry[0];unsigned long fkb=entry.substring(2).toInt();
      gfx->setTextColor(COL_GRAY);gfx->setCursor(10,ypos);sprintf(buf,"%c:",letter);gfx->print(buf);
      int bX=30,bW=200,bH=8;gfx->fillRect(bX,ypos,bW,bH,COL_BG);
      int fw=constrain((int)((float)fkb/2097152.0f*bW),0,bW);
      gfx->fillRect(bX,ypos,fw,bH,COL_GREEN);gfx->drawRect(bX,ypos,bW,bH,COL_GRAY);
      sprintf(buf,"%luKB",fkb);gfx->setTextColor(COL_WHITE);gfx->setCursor(bX+bW+4,ypos);gfx->print(buf);
      ypos+=12;
    }
    if(comma<0)break;start=comma+1;
  }
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(10,170);gfx->print("VOLUME:");
  gfx->setTextColor(COL_YELLOW);gfx->setCursor(60,170);gfx->print(data.vol_label);
  sn_draw();
}

// -------------------------------------------------------
// PAGINA 3 — KBD, Mouse, COM, Files
// -------------------------------------------------------
void ledInd(int x,int y,const char* lb,bool on,uint16_t col){
  if(on){gfx->fillCircle(x,y,6,col);gfx->fillCircle(x,y,2,COL_WHITE);}
  else{gfx->fillCircle(x,y,6,0x2000);gfx->drawCircle(x,y,6,COL_GRAY);}
  gfx->setTextSize(1);gfx->setTextColor(on?col:COL_GRAY);gfx->setCursor(x-8,y+10);gfx->print(lb);
}
void drawPage3(){
  drawLogo();
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(10,42);gfx->print("KEYBOARD:");
  ledInd(30, 65,"CAP",data.kbd_caps,  COL_GREEN);
  ledInd(80, 65,"NUM",data.kbd_num,   COL_CYAN);
  ledInd(130,65,"SCR",data.kbd_scroll,COL_YELLOW);
  gfx->drawFastVLine(180,42,80,COL_DKGREEN);
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(190,42);gfx->print("MOUSE:");
  char buf[32];
  if(data.mouse_present){
    gfx->setTextColor(COL_GREEN);gfx->setCursor(190,54);gfx->print("DETECTED");
    sprintf(buf,"X:%-3d Y:%-3d",data.mouse_x,data.mouse_y);
    gfx->setTextColor(COL_WHITE);gfx->setCursor(190,66);gfx->print(buf);
    ledInd(200,85,"L",data.mouse_btn&1,COL_RED);
    ledInd(220,85,"R",data.mouse_btn&2,COL_RED);
  } else {gfx->setTextColor(COL_GRAY);gfx->setCursor(190,54);gfx->print("NOT FOUND");}
  gfx->drawFastHLine(8,105,SCR_W-16,COL_DKGREEN);
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(10,112);gfx->print("COM1 BAUD:");
  sprintf(buf,"%d",data.com_baud);gfx->setTextColor(COL_CYAN);gfx->setCursor(90,112);gfx->print(buf);
  gfx->setTextColor(COL_AMBER);gfx->setCursor(10,126);gfx->print("OPEN FILES:");
  sprintf(buf,"%d",data.open_files);gfx->setTextColor(COL_GREEN);gfx->setCursor(90,126);gfx->print(buf);
  int bX=10,bY=138,bW=240,bH=10;gfx->fillRect(bX,bY,bW,bH,COL_BG);
  int fw=constrain((int)((float)data.open_files/20.0f*bW),0,bW);
  gfx->fillRect(bX,bY,fw,bH,COL_GREEN);gfx->drawRect(bX,bY,bW,bH,COL_GRAY);
  gfx->setTextSize(1);gfx->setTextColor(COL_AMBER);gfx->setCursor(36,155);gfx->print("DRIVE ACTIVITY");
  if(data.drive_active){gfx->fillCircle(18,163,9,0x4000);gfx->fillCircle(18,163,7,COL_RED);}
  else{gfx->fillCircle(18,163,7,0x2000);gfx->drawCircle(18,163,7,COL_GRAY);}
  pg_draw();
}

// -------------------------------------------------------
// TOUCH
// -------------------------------------------------------
void handleTouch(){
  TouchPoint tp;
  if(!touch.read(tp)||!tp.touched) return;

  uint16_t ty = tp.y;
  uint16_t tx = tp.x;

  // Pagina 5: gestione completa separata
  if(currentPage==4){
    handleTouchPage5(ty);
    delay(100);
    unsigned long w5=millis();
    while(millis()-w5<600){ touch.read(tp); if(!tp.touched) break; delay(20); }
    delay(100);
    return;
  }

  // Area giochi: tx 190-300 corrisponde a Y display 190-286
  // Pagine 0/1/2 hanno giochi nella fascia bassa
  if(tx>=190 && tx<=300 && currentPage<=2){
    // Mappa ty → X display (ty piccolo=destra, ty grande=sinistra)
    int dispX = 480 - (int)ty; // 0=sinistra, 480=destra

    if(currentPage==0){
      // SPACE INVADERS
      // Sinistra → muovi navicella a sinistra
      // Destra → muovi navicella a destra
      // Centro → spara
      if(dispX < 150){
        shipX = max(14, shipX - 20);
      } else if(dispX > 330){
        shipX = min(AREA_W-14, shipX + 20);
      } else {
        if(!playerBullet.active)
          playerBullet={(int16_t)(AREA_X+shipX),(int16_t)(AREA_Y+AREA_H-16),true};
      }
      delay(80);
      return;
    }
    else if(currentPage==1){
      // SNAKE — cambia direzione in base alla zona toccata
      int centerX = 240;
      int centerY = 55;
      int relY = (int)tx - 190; // 0-110
      int dx = dispX - centerX;
      int dy = relY - centerY;

      int8_t ndx=sn_dx, ndy=sn_dy;
      if(abs(dx) >= abs(dy)){
        if(dx > 20  && sn_dy==0){ ndx= 1; ndy=0; } // destra
        else if(dx < -20 && sn_dy==0){ ndx=-1; ndy=0; } // sinistra
      } else {
        if(dy > 10  && sn_dx==0){ ndx=0; ndy= 1; } // giù
        else if(dy < -10 && sn_dx==0){ ndx=0; ndy=-1; } // su
      }
      // Verifica che la nuova direzione non porti subito in una cella occupata
      if(ndx!=sn_dx || ndy!=sn_dy){
        int8_t hx=sn_body[0].x, hy=sn_body[0].y;
        int8_t nx=hx+ndx, ny=hy+ndy;
        bool safe=true;
        if(nx<0||nx>=SN_COLS||ny<0||ny>=SN_ROWS) safe=false;
        for(int i=1;i<sn_len&&safe;i++)
          if(sn_body[i].x==nx&&sn_body[i].y==ny) safe=false;
        if(safe){ sn_dx=ndx; sn_dy=ndy; }
      }
      delay(100);
      unsigned long ws=millis();
      while(millis()-ws<300){ touch.read(tp); if(!tp.touched) break; delay(10); }
      return;
    }
    else if(currentPage==2){
      // PONG — il dito controlla direttamente il paddle sinistro
      // tx 190-300 → paddle Y 10-86
      int relY = (int)tx - 190; // 0-110
      pg.p1y = map(relY, 0, 110, PG_PH/2+2, PG_AH-PG_PH/2-2);
      // Nessun debounce: aggiornamento continuo per fluidità
      return;
    }
  }

  // Navigazione pagine sui bordi (solo fuori dall'area giochi)
  if(ty < TOUCH_ZONE && currentPage < NUM_PAGES-1){
    currentPage++;
  } else if(ty > 480 - TOUCH_ZONE && currentPage > 0){
    currentPage--;
  }

  delay(100);
  unsigned long wait = millis();
  while(millis()-wait < 600){
    touch.read(tp);
    if(!tp.touched) break;
    delay(20);
  }
  delay(100);
}

// -------------------------------------------------------
// MICROSD — splash screen
// -------------------------------------------------------
void showSplash(){
  if(!sdOk) return;
  File f=SD.open("/SPLASH.BMP");
  if(!f){ Serial.println("SPLASH.BMP non trovato"); return; }

  uint8_t hdr[54];
  if(f.read(hdr,54)!=54){ f.close(); return; }
  if(hdr[0]!='B'||hdr[1]!='M'){ f.close(); return; }

  uint32_t dataOffset=(uint32_t)hdr[10]|((uint32_t)hdr[11]<<8)|((uint32_t)hdr[12]<<16)|((uint32_t)hdr[13]<<24);
  int32_t  imgW=(int32_t)((uint32_t)hdr[18]|((uint32_t)hdr[19]<<8)|((uint32_t)hdr[20]<<16)|((uint32_t)hdr[21]<<24));
  int32_t  imgH=(int32_t)((uint32_t)hdr[22]|((uint32_t)hdr[23]<<8)|((uint32_t)hdr[24]<<16)|((uint32_t)hdr[25]<<24));
  uint16_t bpp=(uint16_t)hdr[28]|((uint16_t)hdr[29]<<8);
  Serial.printf("BMP: %dx%d %dbpp offset=%d\n",imgW,imgH,bpp,dataOffset);
  if(bpp!=24){ f.close(); return; }

  int stride=((imgW*3)+3)&~3;
  int drawW=min((int32_t)SCR_W,imgW);
  int drawH=min(abs(imgH),(int32_t)SCR_H);
  bool flipY=(imgH>0);

  uint8_t* buf=(uint8_t*)malloc(stride);
  if(!buf){ f.close(); return; }

  f.seek(dataOffset);
  gfx->fillScreen(COL_BG);

  for(int row=0;row<drawH;row++){
    if((int)f.read(buf,stride)<drawW*3) break;
    int destY=flipY?(drawH-1-row):row;
    for(int x=0;x<drawW;x++){
      uint8_t b=buf[x*3+0];
      uint8_t g=buf[x*3+1];
      uint8_t r=buf[x*3+2];
      gfx->drawPixel(x,destY,((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3));
    }
  }
  free(buf); f.close();
  display.flush();
  Serial.println("Splash OK");
}

// -------------------------------------------------------
// MICROSD — log CSV
// -------------------------------------------------------
void initLog(){
  if(!sdOk) return;
  if(SD.exists("/DATALOG.CSV")) return;
  File f=SD.open("/DATALOG.CSV",FILE_WRITE);
  if(!f) return;
  f.println("DATA,ORA,RAM_KB,DOS,DRIVE,CPU");
  f.close();
}

void writeLog(){
  if(!sdOk||!connected) return;
  File f=SD.open("/DATALOG.CSV",FILE_APPEND);
  if(!f) return;
  char line[128];
  snprintf(line,sizeof(line),"%04d/%02d/%02d,%02d:%02d:%02d,%d,%d.%d,%d,%d\n",
    data.yy,data.mo,data.dd,data.hh,data.mm,data.ss,
    data.ram_kb,data.dos_major,data.dos_minor,data.drive_active,data.cpu_type);
  f.print(line); f.close();
}

// -------------------------------------------------------
// JSON per WebSocket
// -------------------------------------------------------
String buildJson(){
  char buf[256];
  snprintf(buf,sizeof(buf),
    "{\"ram\":%d,\"hh\":%d,\"mm\":%d,\"ss\":%d,"
    "\"dd\":%d,\"mo\":%d,\"yy\":%d,"
    "\"dos_maj\":%d,\"dos_min\":%d,\"drv\":%d,"
    "\"cpu\":%d,\"bios\":\"%s\",\"dsk\":\"%s\","
    "\"vol\":\"%s\",\"caps\":%d,\"num\":%d,\"scroll\":%d,"
    "\"mouse\":%d,\"mx\":%d,\"my\":%d,\"mbtn\":%d,"
    "\"com\":%d,\"files\":%d,\"conn\":%d}",
    data.ram_kb,data.hh,data.mm,data.ss,
    data.dd,data.mo,data.yy,
    data.dos_major,data.dos_minor,data.drive_active,
    data.cpu_type,data.bios_info,data.drives,
    data.vol_label,data.kbd_caps,data.kbd_num,data.kbd_scroll,
    data.mouse_present,data.mouse_x,data.mouse_y,data.mouse_btn,
    data.com_baud,data.open_files,connected?1:0);
  return String(buf);
}

// -------------------------------------------------------
// SETUP
// -------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial2.begin(SERIAL_BAUD, SERIAL_8N1, RX_PIN, -1);

  display.begin();
  touch.begin();
  gfx = display.getCanvas();
  gfx->setRotation(3);

  memset(&data,0,sizeof(data));
  data.yy=2026; data.dd=1; data.mo=1;
  data.cpu_type=486;
  strcpy(data.bios_info,"?");
  strcpy(data.drives,"C:0");
  strcpy(data.vol_label,"?");

  randomSeed(analogRead(0));
  initSI();
  sn_init();
  pg_init();

  // MicroSD — HSPI sui pin dallo schematico, inizializzato PRIMA del display
  sdSPI.begin(SD_CLK, SD_MISO, SD_MOSI, SD_CS);
  if(SD.begin(SD_CS, sdSPI, 20000000)){
    sdOk=true;
    Serial.println("SD OK");
    initLog();
    loadPlaylist();
    showSplash();
    delay(2500);
    gfx->fillScreen(COL_BG);
    display.flush();
  } else {
    Serial.println("SD non trovata");
  }

  // Audio output I2S
  audioOut = new AudioOutputI2S();
  audioOut->SetPinout(I2S_BCLK, I2S_LRCLK, I2S_DOUT);
  audioOut->SetGain(audioVolume);
  Serial.println("Audio I2S OK");

  // Pannello iniziale
  gfx->fillScreen(COL_BG);
  drawBorder(); drawTitle("1/5"); drawLogo(); drawSI(); drawStatus();
  display.flush();

  // WiFi

  // WiFi — avviato DOPO il display, potenza minima per ridurre interferenze
  gfx->setTextSize(1); gfx->setTextColor(COL_AMBER);
  gfx->setCursor(10, SCR_H-14); gfx->print("Connessione WiFi...");
  display.flush();

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_2dBm); // potenza minima
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int att=0;
  while(WiFi.status()!=WL_CONNECTED && att<20){ delay(500); att++; }

  if(WiFi.status()==WL_CONNECTED){
    wifiOk=true;
    String ip="WiFi OK  IP:"+WiFi.localIP().toString();
    gfx->fillRect(0,SCR_H-20,SCR_W,20,COL_BG);
    gfx->setTextSize(1); gfx->setTextColor(COL_GREEN);
    gfx->setCursor(10,SCR_H-14); gfx->print(ip.c_str());
    display.flush(); delay(2000);
    httpServer.on("/",[](){
      if(sdOk && SD.exists("/INDEX.HTM")){
        File f=SD.open("/INDEX.HTM");
        httpServer.streamFile(f,"text/html");
        f.close();
      } else {
        httpServer.send(200,"text/html",
          "<html><body style='background:#111;color:#FFA800;font-family:monospace;padding:20px'>"
          "<h2>DOSPANEL v5.0</h2>"
          "<p>WebSocket: ws://[questo IP]:81</p>"
          "<p>SD non trovata o INDEX.HTM mancante</p>"
          "<p><a style='color:#00FC00' href='/datalog.csv'>Scarica DATALOG.CSV</a></p>"
          "<p><a style='color:#00FC00' href='/files'>Gestione file SD</a></p>"
          "</body></html>");
      }
    });

    // Pagina gestione file SD
    httpServer.on("/files",[](){
      if(!sdOk){ httpServer.send(503,"text/html","<h2>SD non disponibile</h2>"); return; }
      String html="<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<title>DOSPANEL - File SD</title>"
        "<style>body{background:#111;color:#FFA800;font-family:monospace;padding:20px}"
        "h2{color:#FFA800}h3{color:#07E0;margin-top:20px}a{color:#00FC00}"
        "table{border-collapse:collapse;width:100%}"
        "td,th{padding:6px 12px;border:1px solid #333;text-align:left}"
        "th{color:#FFA800}.btn{background:#1a1a1a;border:1px solid #444;"
        "color:#00FC00;padding:4px 10px;cursor:pointer;font-family:monospace}"
        ".btn:hover{border-color:#FFA800;color:#FFA800}"
        ".del{color:#F800}input[type=file]{color:#888}.prog{color:#07E0}"
        "</style></head><body>"
        "<h2>FILE SULLA MICROSD</h2>"
        "<a href='/' class='btn'>Torna al pannello</a><br><br>"
        "<h3>Root /</h3>"
        "<table><tr><th>File</th><th>Dim</th><th>Azione</th></tr>";
      File root=SD.open("/");
      File entry=root.openNextFile();
      while(entry){
        if(!entry.isDirectory()){
          String name=String(entry.name());
          size_t sz=entry.size();
          String szStr=sz>1024?String(sz/1024)+" KB":String(sz)+" B";
          html+="<tr><td>"+name+"</td><td class='prog'>"+szStr+"</td><td>"
               "<a href='/download?f="+name+"' class='btn'>Scarica</a> "
               "<a href='/delete?f=/"+name+"' class='btn del' "
               "onclick=\"return confirm('Eliminare "+name+"?')\">Elimina</a>"
               "</td></tr>";
        }
        entry.close(); entry=root.openNextFile();
      }
      root.close();
      html+="</table><br>"
            "<b>Carica in root (INDEX.HTM, SPLASH.BMP...):</b><br>"
            "<form method='POST' action='/upload?dir=/' enctype='multipart/form-data'>"
            "<input type='file' name='file'> "
            "<input type='submit' value='Carica' class='btn'></form>"
            "<h3>Cartella /AUDIO</h3>";
      if(!SD.exists("/AUDIO")){
        html+="<p style='color:#555'>Cartella non esistente - verra' creata al primo upload.</p>";
      } else {
        html+="<table><tr><th>File</th><th>Dim</th><th>Azione</th></tr>";
        File adir=SD.open("/AUDIO");
        File af=adir.openNextFile();
        bool has=false;
        while(af){
          if(!af.isDirectory()){
            has=true;
            String name=String(af.name());
            size_t sz=af.size();
            String szStr=sz>1024?String(sz/1024)+" KB":String(sz)+" B";
            html+="<tr><td>"+name+"</td><td class='prog'>"+szStr+"</td><td>"
                 "<a href='/download?f=/AUDIO/"+name+"' class='btn'>Scarica</a> "
                 "<a href='/delete?f=/AUDIO/"+name+"' class='btn del' "
                 "onclick=\"return confirm('Eliminare "+name+"?')\">Elimina</a>"
                 "</td></tr>";
          }
          af.close(); af=adir.openNextFile();
        }
        adir.close();
        if(!has) html+="<tr><td colspan='3' style='color:#555'>Nessun file</td></tr>";
        html+="</table>";
      }
      html+="<br><b>Carica file MP3/WAV in /AUDIO:</b><br>"
            "<form method='POST' action='/upload?dir=/AUDIO' enctype='multipart/form-data'>"
            "<input type='file' name='file' accept='.mp3,.wav,.MP3,.WAV'> "
            "<input type='submit' value='Carica' class='btn'></form>"
            "<p style='color:#555;font-size:11px'>La playlist si aggiorna automaticamente dopo il caricamento</p>"
            "</body></html>";
      httpServer.send(200,"text/html",html);
    });

    // Download file generico dalla SD
    httpServer.on("/download",[](){
      if(!sdOk){ httpServer.send(503,"text/plain","SD non disponibile"); return; }
      if(!httpServer.hasArg("f")){ httpServer.send(400,"text/plain","Parametro mancante"); return; }
      String fname = "/" + httpServer.arg("f");
      if(!SD.exists(fname)){ httpServer.send(404,"text/plain","File non trovato"); return; }
      File f = SD.open(fname);
      httpServer.sendHeader("Content-Disposition","attachment; filename=" + httpServer.arg("f"));
      httpServer.streamFile(f,"application/octet-stream");
      f.close();
    });

    // Elimina file dalla SD
    httpServer.on("/delete",[](){
      if(!sdOk){ httpServer.send(503,"text/plain","SD non disponibile"); return; }
      if(!httpServer.hasArg("f")){ httpServer.send(400,"text/plain","Parametro mancante"); return; }
      String fname=httpServer.arg("f"); // path già completo es: /AUDIO/song.mp3
      if(!fname.startsWith("/")) fname="/"+fname;
      if(SD.remove(fname)){
        // Se era un file audio, ricarica playlist
        if(fname.startsWith("/AUDIO")) { audioStop(); loadPlaylist(); }
        httpServer.sendHeader("Location","/files");
        httpServer.send(302,"text/plain","");
      } else {
        httpServer.send(500,"text/plain","Errore eliminazione: "+fname);
      }
    });

    // Upload file sulla SD
    httpServer.on("/upload", HTTP_POST,
      [](){
        String dir=httpServer.hasArg("dir")?httpServer.arg("dir"):"/";
        httpServer.send(200,"text/html",
          "<html><body style='background:#111;color:#00FC00;font-family:monospace;padding:20px'>"
          "<h2>OK - File caricato</h2>"
          "<a href='/files' style='color:#FFA800'>Torna ai file</a>"
          "</body></html>");
      },
      [](){
        HTTPUpload& upload = httpServer.upload();
        static File uploadFile;
        if(upload.status == UPLOAD_FILE_START){
          // Leggi cartella destinazione dal parametro dir
          String dir=httpServer.hasArg("dir")?httpServer.arg("dir"):"/";
          if(!dir.endsWith("/")) dir+="/";
          // Crea cartella se non esiste
          if(dir!="/" && !SD.exists(dir)) SD.mkdir(dir);
          String fname=dir+upload.filename;
          Serial.println("Upload: "+fname);
          if(SD.exists(fname)) SD.remove(fname);
          uploadFile=SD.open(fname,FILE_WRITE);
        } else if(upload.status == UPLOAD_FILE_WRITE){
          if(uploadFile) uploadFile.write(upload.buf,upload.currentSize);
        } else if(upload.status == UPLOAD_FILE_END){
          if(uploadFile){ uploadFile.close(); Serial.println("Upload OK"); }
          // Se abbiamo caricato un audio, ricarica la playlist
          if(httpServer.hasArg("dir") && httpServer.arg("dir")=="/AUDIO"){
            audioStop(); loadPlaylist();
          }
        }
      }
    );

    // Playlist JSON
    httpServer.on("/playlist",[](){
      if(!sdOk||trackCount==0){ httpServer.send(200,"application/json","[]"); return; }
      String json="[";
      for(int i=0;i<trackCount;i++){
        if(i>0) json+=",";
        json+="{\"idx\":"+String(i)+
              ",\"name\":\""+String(playlist[i].name)+
              "\",\"type\":\""+(playlist[i].isMP3?"MP3":"WAV")+
              "\",\"current\":"+(i==currentTrack?"true":"false")+"}";
      }
      json+="]";
      httpServer.send(200,"application/json",json);
    });

    // Stato player JSON
    httpServer.on("/playerstatus",[](){
      char buf[128];
      unsigned long elapsed=isPlaying?(millis()-trackStartTime)/1000:0;
      snprintf(buf,sizeof(buf),
        "{\"playing\":%s,\"paused\":%s,\"track\":%d,\"elapsed\":%lu,\"vol\":%d,\"total\":%d}",
        isPlaying?"true":"false",
        isPaused?"true":"false",
        currentTrack,elapsed,
        (int)(audioVolume*100),trackCount);
      httpServer.send(200,"application/json",String(buf));
    });

    // Comandi player
    httpServer.on("/player",[](){
      if(!httpServer.hasArg("cmd")){ httpServer.send(400,"text/plain","cmd mancante"); return; }
      String cmd=httpServer.arg("cmd");
      if(cmd=="play"){
        int idx=httpServer.hasArg("idx")?httpServer.arg("idx").toInt():currentTrack;
        audioPlay(idx<0?0:idx);
      } else if(cmd=="pause"){
        isPaused=!isPaused;
      } else if(cmd=="stop"){
        audioStop();
      } else if(cmd=="next"){
        audioNext();
      } else if(cmd=="prev"){
        audioPrev();
      } else if(cmd=="vol"){
        if(httpServer.hasArg("v")){
          audioVolume=constrain(httpServer.arg("v").toFloat()/100.0f,0.0f,1.0f);
          if(audioOut) audioOut->SetGain(audioVolume);
        }
      } else if(cmd=="reload"){
        audioStop(); loadPlaylist();
      }
      httpServer.send(200,"application/json","{\"ok\":true}");
    });

    httpServer.on("/datalog.csv",[](){
      if(!sdOk){ httpServer.send(404,"text/plain","SD non disponibile"); return; }
      File f=SD.open("/DATALOG.CSV");
      if(!f){ httpServer.send(404,"text/plain","File non trovato"); return; }
      httpServer.sendHeader("Content-Disposition","attachment; filename=DATALOG.CSV");
      httpServer.streamFile(f,"text/csv");
      f.close();
    });
    httpServer.begin();
    wsServer.begin();
    wsServer.onEvent([](uint8_t num,WStype_t type,uint8_t* pl,size_t len){
      if(type==WStype_CONNECTED){ String j=buildJson(); wsServer.sendTXT(num,j); }
    });
    Serial.println("WiFi OK: "+WiFi.localIP().toString());
  } else {
    gfx->fillRect(0,SCR_H-20,SCR_W,20,COL_BG);
    gfx->setTextSize(1); gfx->setTextColor(COL_GRAY);
    gfx->setCursor(10,SCR_H-14); gfx->print("WiFi non disponibile");
    display.flush(); delay(1000);
  }

  Serial.println("DOSPANEL v5.0 ready.");
}

// -------------------------------------------------------
// LOOP
// -------------------------------------------------------
void loop() {
  // WiFi
  if(wifiOk){ httpServer.handleClient(); wsServer.loop(); }

  // Touch
  handleTouch();

  // Leggi seriale
  while(Serial2.available()){
    char c=Serial2.read();
    if(c=='\n'){
      rxBuffer.trim();
      if(parsePacket(rxBuffer)){
        connected=true; lastReceived=millis();
        if(wifiOk && millis()-lastWsBroadcast>500){
          String j=buildJson(); wsServer.broadcastTXT(j);
          lastWsBroadcast=millis();
        }
      }
      rxBuffer="";
    } else {
      rxBuffer+=c;
      if(rxBuffer.length()>256) rxBuffer="";
    }
  }

  // Timeout connessione
  if(connected && (millis()-lastReceived>5000)){
    connected=false;
    if(wifiOk){ String j=buildJson(); wsServer.broadcastTXT(j); }
  }

  // Log CSV ogni LOG_INTERVAL_SEC secondi
  if(connected&&sdOk&&millis()-lastLogTime>LOG_INTERVAL_SEC*1000UL){
    writeLog(); lastLogTime=millis();
  }

  // Aggiorna audio player e forma d'onda
  audioUpdate();
  updateWaveform();

  // Aggiorna giochi
  unsigned long now=millis();
  uint16_t dt=(uint16_t)(now-lastSIUpdate);
  lastSIUpdate=now;
  updateSI(dt);
  sn_update(dt);
  pg_update(dt);
  vu_update(now);

  // Ridisegna
  gfx->fillScreen(COL_BG);
  const char* lbl[]={"1/5","2/5","3/5","4/5","5/5"};
  drawBorder();
  drawTitle(lbl[currentPage]);

  switch(currentPage){
    case 0:
      drawClock(); drawLogo(); drawRam();
      drawDosVersion(); drawDrive(); drawExtras(); drawSI();
      break;
    case 1:
      drawPage2(); sn_draw();
      break;
    case 2:
      drawPage3(); pg_draw();
      break;
    case 3:
      vu_draw();
      break;
    case 4:
      drawPage5();
      break;
  }

  drawPageDots();
  drawArrows();
  drawStatus();
  display.flush();
  delay(16);
}
