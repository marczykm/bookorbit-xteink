// Simulator, window side: shows the panel in an SDL window, turns keys into button presses and runs the firmware in a child
// process (started again on ESP.restart() and on waking from deep sleep, so each run starts from a clean state like the
// device; RTC_NOINIT_ATTR variables are carried over).
//
//   .pio/build/simulator/program [--x3 | --x4pro] [--sd DIR] [--slow] [--scale N] [--no-demo]
//
// Unless --no-demo is given it also runs sim/demo_server.py (a stand-in BookOrbit server on port 3000, login demo/demo),
// with its own SD card folder (sim/sdcard-demo) set up to use it. --no-demo uses sim/sdcard, e.g. for your own server.
//
// Keys: Esc/Backspace = Back, Enter = OK, arrows = Left/Right/Up/Down, P = Power (hold for a long press),
//       R = reset, S = screenshot, W = Wi-Fi out of reach, F = panel refresh times on/off, +/- = battery.
// X4 Pro (--x4pro): the mouse is the touchscreen (click = tap, drag = swipe), H the Home key (hold for a long press),
//       Up/Down the two side keys; the paper is tinted by the frontlight.
#include <SDL.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "sim_link.h"

extern char** environ;

namespace sim {
int runFirmware(int shmFd, int ptyFd);
}

namespace {
constexpr uint32_t kPaper = 0xFFF4F1EA, kInk = 0xFF1E1E1E;

sim::Shared* shm = nullptr;
int shmFd = -1, ptyFd = -1;
pid_t child = -1, demo = -1;
std::string self;
bool sleeping = false, crashed = false;

// A pseudo-terminal that stands in for the device's USB serial port, so tools/drive.py can drive the simulator.
std::string openPty() {
  const int fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (fd < 0 || grantpt(fd) || unlockpt(fd)) return "";
  const std::string name = ptsname(fd);
  const int slave = open(name.c_str(), O_RDWR | O_NOCTTY);  // kept open: raw mode stays set and output never errors
  if (slave >= 0) {
    termios t;
    tcgetattr(slave, &t);
    cfmakeraw(&t);
    tcsetattr(slave, TCSANOW, &t);
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  ptyFd = fd;
  return name;
}

void start(int reason) {
  shm->resetReason = reason;
  shm->buttons = 0;
  shm->touchTail = shm->touchHead;  // gestures meant for the previous run are dropped
  std::vector<std::string> args = {self, "--firmware", std::to_string(shmFd), std::to_string(ptyFd)};
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(&a[0]);
  argv.push_back(nullptr);
  if (posix_spawn(&child, self.c_str(), nullptr, nullptr, argv.data(), environ) != 0) {
    perror("posix_spawn");
    exit(1);
  }
  sleeping = crashed = false;
}

void stopChild() {
  if (child > 0) {
    kill(child, SIGKILL);
    waitpid(child, nullptr, 0);
    child = -1;
  }
}

// The demo server, and (on the demo SD card, when it has none yet) the settings that point the device at it.
void startDemo(bool seed) {
  const std::string script = SIM_DIR "/demo_server.py";
  const char* args[] = {"python3", script.c_str(), "--port", "3000", nullptr};
  if (posix_spawnp(&demo, "python3", nullptr, nullptr, (char**)args, environ) != 0) {
    demo = -1;
    fprintf(stderr, "[sim] could not start the demo server (python3 not found)\n");
    return;
  }
  const std::string cfg = std::string(SIM_DIR) + "/sdcard-demo/bookorbit.json";
  struct stat st;
  if (!seed || stat(cfg.c_str(), &st) == 0) return;
  mkdir(SIM_DIR "/sdcard-demo", 0755);
  if (FILE* f = fopen(cfg.c_str(), "w")) {
    fputs("{\"ssid\":\"Simulator Wi-Fi\",\"wifiPassword\":\"demo\",\"server\":\"http://127.0.0.1:3000\","
          "\"username\":\"demo\",\"password\":\"demo\"}\n", f);
    fclose(f);
    printf("[sim] wrote %s (demo server, demo/demo)\n", cfg.c_str());
  }
}

void stopDemo() {
  if (demo > 0) {
    kill(demo, SIGTERM);
    waitpid(demo, nullptr, 0);
    demo = -1;
  }
}

void title(SDL_Window* w) {
  std::string t = std::string("BookOrbit simulator - ") + (shm->x3 ? "X3" : shm->x4pro ? "X4 Pro" : "X4");
  t += "  battery " + std::to_string(shm->battery) + "%";
  if (shm->x4pro) {
    t += shm->lightLevel ? "  light " + std::to_string(shm->lightLevel) + "%, warm " + std::to_string(shm->lightWarm) + "%"
                         : std::string("  light off");
  }
  if (shm->wifiDown) t += "  [no Wi-Fi]";
  if (shm->slow) t += "  [slow panel]";
  if (sleeping) t += "  - asleep, P wakes";
  if (crashed) t += "  - CRASHED, R restarts";
  SDL_SetWindowTitle(w, t.c_str());
}

// The paper colour: on the X4 Pro a little darker with the light off, and lit cool white to warm amber by the frontlight.
uint32_t paperColor() {
  if (!shm->x4pro) return kPaper;
  const float level = shm->lightLevel / 100.0f, warm = shm->lightWarm / 100.0f;
  const float off[3] = {214, 210, 200};
  const float lit[3] = {244 + 11 * warm, 248 - 18 * warm, 255 - 70 * warm};
  uint32_t c = 0xFF000000;
  for (int i = 0; i < 3; i++) c |= (uint32_t)(off[i] + (lit[i] - off[i]) * level) << (16 - 8 * i);
  return c;
}

// The landscape panel buffer turned to portrait, the way gfx draws it (portrait x = panel height - 1 - row, y = column).
void render(SDL_Texture* tex, int pw, int ph) {
  void* pixels;
  int pitch;
  if (SDL_LockTexture(tex, nullptr, &pixels, &pitch)) return;
  const uint32_t paper = paperColor();
  const int fw = shm->fbWidth, fh = shm->fbHeight, stride = shm->fbStride;
  for (int y = 0; y < ph; y++) {
    uint32_t* row = (uint32_t*)((uint8_t*)pixels + y * pitch);
    for (int x = 0; x < pw; x++) {
      const int px = y, py = fh - 1 - x;
      const bool white = px < fw && py >= 0 && (shm->fb[py * stride + px / 8] & (0x80 >> (px & 7)));
      row[x] = white ? paper : kInk;
    }
  }
  SDL_UnlockTexture(tex);
}

void screenshot(int pw, int ph) {
  mkdir(SIM_DIR "/screenshots", 0755);
  SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, pw, ph, 32, SDL_PIXELFORMAT_ARGB8888);
  const int fw = shm->fbWidth, fh = shm->fbHeight, stride = shm->fbStride;
  for (int y = 0; y < ph; y++)
    for (int x = 0; x < pw; x++) {
      const int px = y, py = fh - 1 - x;
      const bool white = px < fw && (shm->fb[py * stride + px / 8] & (0x80 >> (px & 7)));
      ((uint32_t*)((uint8_t*)s->pixels + y * s->pitch))[x] = white ? 0xFFFFFFFF : 0xFF000000;
    }
  char stamp[32], name[1024];
  const time_t now = time(nullptr);
  strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
  snprintf(name, sizeof(name), "%s/screenshots/%s.bmp", SIM_DIR, stamp);
  SDL_SaveBMP(s, name);
  SDL_FreeSurface(s);
  printf("[sim] screenshot: %s\n", name);
}

// A gesture for the firmware, in the panel's native frame (portrait x runs against the panel's y, portrait y along its x).
void sendTouch(sim::TouchRecord::Kind kind, int x0 = 0, int y0 = 0, int x1 = 0, int y1 = 0) {
  const int pw = shm->fbHeight, ph = shm->fbWidth;
  sim::TouchRecord r{kind, (float)y0 / ph, 1.0f - (float)x0 / pw, (float)y1 / ph, 1.0f - (float)x1 / pw};
  if (shm->touchHead - shm->touchTail >= (uint32_t)sim::kTouchRing) return;  // the firmware is not reading: drop it
  shm->touch[shm->touchHead % sim::kTouchRing] = r;
  __atomic_add_fetch(&shm->touchHead, 1, __ATOMIC_RELEASE);
}

int buttonFor(SDL_Keycode k) {
  if (shm->x4pro) {  // only the two side keys and Power
    if (k == SDLK_UP) return 4;
    if (k == SDLK_DOWN) return 5;
    return k == SDLK_p ? 6 : -1;
  }
  switch (k) {
    case SDLK_ESCAPE: case SDLK_BACKSPACE: return 0;  // Back
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE: return 1;  // OK
    case SDLK_LEFT: return 2;
    case SDLK_RIGHT: return 3;
    case SDLK_UP: return 4;
    case SDLK_DOWN: return 5;
    case SDLK_p: return 6;  // Power
    default: return -1;
  }
}
}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "--firmware")) return sim::runFirmware(atoi(argv[2]), atoi(argv[3]));

  self = argv[0];
  bool x3 = false, x4pro = false, slow = false, withDemo = true;
  int scale = 1;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--x3")) x3 = true;
    else if (!strcmp(argv[i], "--x4pro")) x4pro = true;
    else if (!strcmp(argv[i], "--slow")) slow = true;
    else if (!strcmp(argv[i], "--no-demo")) withDemo = false;
    else if (!strcmp(argv[i], "--sd") && i + 1 < argc) setenv("SIM_SD", argv[++i], 1);
    else if (!strcmp(argv[i], "--scale") && i + 1 < argc) scale = std::max(1, atoi(argv[++i]));
    else {
      fprintf(stderr, "usage: %s [--x3 | --x4pro] [--sd DIR] [--slow] [--scale N] [--no-demo]\n", argv[0]);
      return 2;
    }
  }

  char path[] = "/tmp/bookorbit-sim-XXXXXX";
  shmFd = mkstemp(path);
  unlink(path);
  if (shmFd < 0 || ftruncate(shmFd, sizeof(sim::Shared))) return perror("shared memory"), 1;
  shm = (sim::Shared*)mmap(nullptr, sizeof(sim::Shared), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd, 0);
  shm->x3 = x3 && !x4pro;
  shm->x4pro = x4pro;
  shm->slow = slow;
  shm->battery = 87;
  shm->fbWidth = x3 ? 792 : 800;
  shm->fbHeight = x3 ? 528 : 480;
  shm->fbStride = (shm->fbWidth + 7) / 8;
  memset(shm->fb, 0xFF, sizeof(shm->fb));

  if (withDemo) {
    const bool seed = !getenv("SIM_SD");
    if (seed) setenv("SIM_SD", SIM_DIR "/sdcard-demo", 1);
    startDemo(seed);
  }
  const std::string pty = openPty();
  printf("[sim] %s, SD card: %s\n", shm->x3 ? "Xteink X3" : x4pro ? "Xteink X4 Pro" : "Xteink X4", getenv("SIM_SD") ? getenv("SIM_SD") : SIM_DIR "/sdcard");
  if (!pty.empty()) printf("[sim] serial port: %s  (e.g. python tools/drive.py %s OUTDIR ...)\n", pty.c_str(), pty.c_str());
  if (x4pro) printf("[sim] touch: click = tap, drag = swipe; H Home (hold: light), Up/Down side keys, P Power\n");
  else printf("[sim] keys: Esc/Backspace Back, Enter OK, arrows, P Power\n");
  printf("[sim] R reset, S screenshot, W Wi-Fi, F slow panel, +/- battery\n");
  fflush(stdout);

  if (SDL_Init(SDL_INIT_VIDEO)) return fprintf(stderr, "SDL: %s\n", SDL_GetError()), 1;
  const int pw = shm->fbHeight, ph = shm->fbWidth;  // portrait
  SDL_Window* win = SDL_CreateWindow("BookOrbit simulator", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, pw * scale, ph * scale,
                                     SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  SDL_RenderSetLogicalSize(ren, pw, ph);
  SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, pw, ph);

  start(1);  // ESP_RST_POWERON
  title(win);
  uint32_t shownSeq = ~0u;
  bool dirty = true;
  int downX = -1, downY = -1;        // where the mouse button went down (X4 Pro touch)
  uint32_t homeDownAt = 0;           // when H went down; 0 = not held
  bool homeLongSent = false;
  int shownLight = -1, shownWarm = -1;
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) {
        stopChild();
        stopDemo();
        return 0;
      }
      if ((e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) && !e.key.repeat) {
        const bool down = e.type == SDL_KEYDOWN;
        const int b = buttonFor(e.key.keysym.sym);
        if (b >= 0) {
          if (down) __atomic_or_fetch(&shm->buttons, 1u << b, __ATOMIC_RELEASE);
          else __atomic_and_fetch(&shm->buttons, ~(1u << b), __ATOMIC_RELEASE);
          if (down && b == 6 && sleeping) {
            start(8);  // ESP_RST_DEEPSLEEP
            shm->buttons = 1u << 6;  // still held, as when the device wakes
          }
        } else if (down) {
          switch (e.key.keysym.sym) {
            case SDLK_r: stopChild(); start(1); break;
            case SDLK_s: screenshot(pw, ph); break;
            case SDLK_w: shm->wifiDown = !shm->wifiDown; break;
            case SDLK_f: shm->slow = !shm->slow; break;
            case SDLK_EQUALS: case SDLK_PLUS: case SDLK_KP_PLUS: shm->battery = std::min(100, shm->battery + 10); break;
            case SDLK_MINUS: case SDLK_KP_MINUS: shm->battery = std::max(0, shm->battery - 10); break;
          }
          title(win);
        }
      }
      if (e.type == SDL_WINDOWEVENT) dirty = true;
      if (shm->x4pro && e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        downX = e.button.x;
        downY = e.button.y;
      }
      if (shm->x4pro && e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT && downX >= 0) {
        const int dx = e.button.x - downX, dy = e.button.y - downY;
        if (dx * dx + dy * dy > 30 * 30) sendTouch(sim::TouchRecord::Swipe, downX, downY, e.button.x, e.button.y);
        else sendTouch(sim::TouchRecord::Tap, downX, downY);
        downX = -1;
      }
      if (shm->x4pro && e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.sym == SDLK_h) {
        homeDownAt = SDL_GetTicks() | 1;
        homeLongSent = false;
      }
      if (shm->x4pro && e.type == SDL_KEYUP && e.key.keysym.sym == SDLK_h && homeDownAt) {
        if (!homeLongSent) sendTouch(sim::TouchRecord::HomeTap);
        homeDownAt = 0;
      }
    }
    if (homeDownAt && !homeLongSent && SDL_GetTicks() - homeDownAt >= 700) {  // the Home key's long press, while held
      sendTouch(sim::TouchRecord::HomeLong);
      homeLongSent = true;
    }
    if (shm->x4pro && (shm->lightLevel != shownLight || shm->lightWarm != shownWarm)) {
      shownLight = shm->lightLevel;
      shownWarm = shm->lightWarm;
      dirty = true;
      title(win);
    }

    int status;
    if (child > 0 && waitpid(child, &status, WNOHANG) == child) {
      child = -1;
      const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
      if (code == sim::kExitRestart) {
        start(3);  // ESP_RST_SW
      } else if (code == sim::kExitSleep) {
        sleeping = true;
      } else {
        crashed = true;
        if (WIFSIGNALED(status)) fprintf(stderr, "[sim] firmware crashed: %s\n", strsignal(WTERMSIG(status)));
        else fprintf(stderr, "[sim] firmware exited with %d\n", code);
      }
      title(win);
    }

    const uint32_t seq = __atomic_load_n(&shm->frameSeq, __ATOMIC_ACQUIRE);
    if (seq != shownSeq || dirty) {
      shownSeq = seq;
      dirty = false;
      render(tex, pw, ph);
      SDL_RenderClear(ren);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
      SDL_RenderPresent(ren);
    } else {
      SDL_Delay(10);
    }
  }
}
