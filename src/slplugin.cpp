// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// libShiftLight.so: the "Shift Light" menu in My Vehicle (plugin/ShiftLight.xml) and the starter of the HUD shift light
// (libshiftlight.so in SL_MODE=service, a process of its own). A native HMI plugin app, loaded by HmiMain3D at every
// iDrive start (ShiftLight.manifest, autoStart).
//
// The plugin skeleton follows st1ng's libCarPlayMenu.so (nbtevo-carplay-advanced src/cpm.cpp), which runs on this car:
// a bmw::PluginApp built with the framework's constructor, given a copy of its vtable with onInit replaced, handlers
// registered with addHandler, checkboxes and radio buttons set with updateModel(id, bool). The car data follows NBT-AAD's
// iDrive app (bmw::PluginApp::cdsAddPropertyChangedEventHandler for driving.mode / driving.mode35up / driving.gear,
// and driving.shiftIndicator ('{"shiftIndicator":n}', libhmiremoting.so: the gear display, for manual mode),
// registered from a bmw::Runnable posted to the app thread). Those two calls are looked up at run time (dlsym), so a
// framework without them still loads the menu; only the drive-mode and gear readout is then missing.
//
// Required Notice: Copyright 2026 nbtevo-carplay-advanced contributors
// Required Notice: Copyright (c) 2026 nanaisu - NBT-AAD (https://github.com/Sq00ky/Supra-iDrive6-Android-Auto)
//
// What it does:
//   * the menu switches are files in SLP_DIR (slplugin_ids.h); the worker reads them every second
//   * "Shift light" on: starts the worker (spawn /bin/cat with LD_PRELOAD=shiftlight-worker.so), and a watcher thread
//     starts it again when it ends (20 s after its start at the soonest); off: SIGTERM (the worker clears the HUD; it
//     also stops by itself when the file "on" is gone). Each iDrive start adds a line to SLP_DIR/events.log, where the
//     worker notes its rare events.
//   * car data: the raw values go to SLP_CAR for the worker, and every change to the log (which value is which mode is
//     found in the car and written to SLP_DIR/car-map)
//   * boot guard: three iDrive starts in a row that end within kStableS of loading the plugin write SLP_DIR/disabled, and
//     from then on create() returns nothing (the menu is gone; remove the file and restart iDrive to enable it again)
#include "slplugin_ids.h"

extern "C" {
void *malloc(__SIZE_TYPE__);
void free(void *);
void *memcpy(void *, const void *, __SIZE_TYPE__);
void *memset(void *, int, __SIZE_TYPE__);
int open(const char *, int, ...);
long read(int, void *, unsigned long);
long write(int, const void *, unsigned long);
int close(int);
int unlink(const char *);
int kill(int, int);
int snprintf(char *, unsigned long, const char *, ...);
// QNX's own spawn() with an explicit fd map and signal settings, as ID6's plugins start their workers (diagnostic_worker.h;
// spawnl from inside HmiMain3D returned -1 on this car, 2026-11-19, likely because it hands the child every open descriptor).
int spawn(const char *, int, const int *, const void *, char *const *, char *const *);
int waitpid(int, int *, int);
int sigfillset(void *);
int sigemptyset(void *);
int *__get_errno_ptr(void);
int usleep(unsigned);
int pthread_create(int *, const void *, void *(*)(void *), void *);
void *dlopen(const char *, int);
void *dlsym(void *, const char *);
struct sl_timespec {
  long sec, nsec;
};
int clock_gettime(int, struct sl_timespec *);
}
#define O_RDONLY 0
#define O_WRONLY 1
#define O_APPEND 8
#define O_CREAT 0x100
#define O_TRUNC 0x200
#define O_RDWR 2
#define WNOHANG 64         // as compiled into ID6's libShiftDeck.so (waitpid(child, 0, 64))
#define SPAWN_SIGS 6       // SPAWN_SETSIGMASK | SPAWN_SETSIGDEF, ditto
#define RTLD_NOW 2
#define SIGTERM 15

inline void *operator new(__SIZE_TYPE__, void *p) {
  return p;
}  // placement new
// clang calls these EABI helpers for aligned memsets/copies; QNX libc.so.3 does not export them (st1ng's note: the HMI
// then refuses the plugin, "Unresolved symbols"). Local to the plugin.
extern "C" __attribute__((visibility("hidden"))) void __aeabi_memclr8(void *p, __SIZE_TYPE__ n) {
  memset(p, 0, n);
}
extern "C" __attribute__((visibility("hidden"))) void __aeabi_memclr4(void *p, __SIZE_TYPE__ n) {
  memset(p, 0, n);
}
extern "C" __attribute__((visibility("hidden"))) void __aeabi_memclr(void *p, __SIZE_TYPE__ n) {
  memset(p, 0, n);
}
extern "C" __attribute__((visibility("hidden"))) void __aeabi_memcpy8(void *d, const void *s, __SIZE_TYPE__ n) {
  memcpy(d, s, n);
}
extern "C" __attribute__((visibility("hidden"))) void __aeabi_memcpy4(void *d, const void *s, __SIZE_TYPE__ n) {
  memcpy(d, s, n);
}
extern "C" __attribute__((visibility("hidden"))) void __aeabi_memcpy(void *d, const void *s, __SIZE_TYPE__ n) {
  memcpy(d, s, n);
}

namespace bmw {
class IDispatcher;
class Bundle;
class Parcel;
class Runnable;
template <typename T>
class BasicString {
public:
  BasicString(const T *s);  // _ZN3bmw11BasicStringIcEC1EPKc
  ~BasicString();           // _ZN3bmw11BasicStringIcED1Ev
  T *m_data;                // the only member
};
enum RHMIInterfaceVersion {
  RHMI_INTERFACE_VERSION_1 = 1
};
template <typename R, typename A1, typename A2, typename A3, typename A4>
struct Delegate {
  void *object;
  R (*stub)(void *, A1);
};
typedef Delegate<bool, const Parcel &, void, void, void> ActionDelegate;
typedef Delegate<bool, const BasicString<unsigned short> &, void, void, void> CdsDelegate;
class PluginApp {
public:
  PluginApp(IDispatcher &disp, BasicString<char> name, unsigned major, unsigned minor, RHMIInterfaceVersion rhmi,
            const Bundle &params, const Bundle &config);
  void addHandler(unsigned actionId, const ActionDelegate &handler);
  void updateModel(unsigned modelId, bool value);
  bool onInit();  // the base implementation
  unsigned char m_storage[0x200];
};
}  // namespace bmw

namespace {

typedef unsigned u32;

// ---- log (RAM) and files ---------------------------------------------------------------------------------------------
u32 g_logBytes;

long monoS() {
  sl_timespec t;
  clock_gettime(2, &t);
  return t.sec;
}

void logLine(const char *s) {
  char b[300];
  sl_timespec t;
  clock_gettime(2, &t);
  int n = snprintf(b, sizeof b, "%6ld.%ld ShiftLight: %s\n", t.sec, t.nsec / 100000000, s);
  if (n < 0)
    return;
  if (n > (int)sizeof b - 1)
    n = sizeof b - 1;
  int flags = O_WRONLY | O_CREAT | O_APPEND;
  if (g_logBytes > 65536) {
    flags = O_WRONLY | O_CREAT | O_TRUNC;  // a small log in RAM: start again
    g_logBytes = 0;
  }
  int fd = open(SLP_PLOG, flags, 0666);
  if (fd < 0)
    return;
  long w = write(fd, b, (unsigned long)n);
  (void)w;
  close(fd);
  g_logBytes += (u32)n;
}

void dirFile(char *out, unsigned cap, const char *name) {
  snprintf(out, cap, "%s/%s", SLP_DIR, name);
}

bool pathExists(const char *p) {
  int fd = open(p, O_RDONLY);
  if (fd < 0)
    return false;
  close(fd);
  return true;
}

bool flagGet(const char *name) {
  char p[160];
  dirFile(p, sizeof p, name);
  return pathExists(p);
}

void writeText(const char *p, const char *s, int n) {
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return;
  long w = write(fd, s, (unsigned long)n);
  (void)w;
  close(fd);
}

void flagSet(const char *name, bool on) {
  char p[160];
  dirFile(p, sizeof p, name);
  if (on)
    writeText(p, "1\n", 2);
  else
    unlink(p);
}

int readIntPath(const char *p, int def) {
  char b[24];
  int fd = open(p, O_RDONLY);
  if (fd < 0)
    return def;
  long n = read(fd, b, sizeof b - 1);
  close(fd);
  if (n <= 0)
    return def;
  int v = 0, k = 0, neg = 0;
  if (b[0] == '-') {
    neg = 1;
    k = 1;
  }
  if (k >= n || b[k] < '0' || b[k] > '9')
    return def;
  for (; k < n && b[k] >= '0' && b[k] <= '9' && v < 100000000; k++)
    v = v * 10 + (b[k] - '0');
  return neg ? -v : v;
}

int readInt(const char *name, int def) {
  char p[160];
  dirFile(p, sizeof p, name);
  return readIntPath(p, def);
}

void writeInt(const char *name, int v) {
  char p[160], b[24];
  dirFile(p, sizeof p, name);
  int n = snprintf(b, sizeof b, "%d\n", v);
  writeText(p, b, n);
}

// ---- the switches ----------------------------------------------------------------------------------------------------
bmw::PluginApp *g_app;
bool g_on, g_sport, g_manual;
int g_rpmIdx = (SLP_RPM_DEFAULT - SLP_RPM_FIRST) / SLP_RPM_STEP;

int rpmOf(int k) {
  return SLP_RPM_FIRST + k * SLP_RPM_STEP;
}

void loadSettings() {
  g_on = flagGet(SLP_F_ON);
  g_sport = flagGet(SLP_F_SPORT);
  g_manual = flagGet(SLP_F_MANUAL);
  int rpm = readInt(SLP_F_RPM, SLP_RPM_DEFAULT), best = 0;
  for (int k = 1; k < SLP_RPM_N; k++)  // the nearest choice
    if ((rpm - rpmOf(k) < 0 ? rpmOf(k) - rpm : rpm - rpmOf(k)) < (rpm - rpmOf(best) < 0 ? rpmOf(best) - rpm : rpm - rpmOf(best)))
      best = k;
  g_rpmIdx = best;
  if (rpm != rpmOf(best))
    writeInt(SLP_F_RPM, rpmOf(best));  // a value the menu no longer offers (6,750, 7,000, a test value): the nearest
  char b[120];
  snprintf(b, sizeof b, "settings: shift light %s, shift point %d, sport only %s, manual only %s", g_on ? "on" : "off",
           rpmOf(g_rpmIdx), g_sport ? "on" : "off", g_manual ? "on" : "off");
  logLine(b);
}

// ---- the worker --------------------------------------------------------------------------------------------------------
volatile long g_lastStart = -1000;

int workerPid() {
  int pid = readIntPath(SLP_PID, 0);
  return pid > 0 && kill(pid, 0) == 0 ? pid : 0;
}

// struct inheritance as libShiftDeck.so fills it: 88 bytes, flags at 0, sigmask at 8, sigdefault at 16 (sigset_t 8 bytes)
struct Inherit {
  u32 flags;
  int pgroup;
  u32 sigmask[2];
  u32 sigdefault[2];
  unsigned char rest[88 - 24];
};
volatile int g_child;  // the last spawned worker, reaped by the watcher
typedef char kInheritIs88Bytes[sizeof(Inherit) == 88 ? 1 : -1];  // libShiftDeck.so's memset(&flags, 0, 88)

char g_envLd[] = "LD_LIBRARY_PATH=/opt/hmi/lib:/opt/hmi/otf:/boot/lib:/lib:/usr/lib:/opt/sys/lib:/opt/car/lib";
char g_envPath[] = "PATH=/bin:/usr/bin:/sbin:/usr/sbin:/opt/sys/bin";
char g_envIp[] = "DSI_IP_ADDRESS=160.48.199.254";
char g_envAps[] = "DSI_APS_PARTITION_NAME=interaction";
char g_envMode[] = "SL_MODE=service";
char g_envDir[] = "SL_DIR=" SLP_DIR;
char g_envPre[] = "LD_PRELOAD=" SLP_DIR "/" SLP_F_WORKER;
char g_argCat[] = "cat";

void workerStart(const char *why) {
  char b[200];
  int pid = workerPid();
  if (pid) {
    snprintf(b, sizeof b, "worker already running (pid %d), %s", pid, why);
    logLine(b);
    return;
  }
  g_lastStart = monoS();
  int nul = open("/dev/null", O_RDWR), out = open(SLP_WLOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
  if (nul < 0) {
    logLine("worker not started: /dev/null did not open");
    if (out >= 0)
      close(out);
    return;
  }
  int fds[3] = { nul, out >= 0 ? out : nul, out >= 0 ? out : nul };  // stdin, stdout, stderr: nothing else of the HMI's
  Inherit in;
  memset(&in, 0, sizeof in);
  in.flags = SPAWN_SIGS;
  sigfillset(in.sigdefault);  // every signal to its default
  sigemptyset(in.sigmask);    // none blocked (SIGTERM must reach it)
  char *argv[] = { g_argCat, 0 };
  char *envp[] = { g_envLd, g_envPath, g_envIp, g_envAps, g_envMode, g_envDir, g_envPre, 0 };
  pid = spawn("/bin/cat", 3, fds, &in, argv, envp);
  int err = pid < 0 ? *__get_errno_ptr() : 0;
  close(nul);
  if (out >= 0)
    close(out);
  if (pid > 0)
    g_child = pid;
  if (pid > 0)
    snprintf(b, sizeof b, "worker started (%s): pid %d", why, pid);
  else
    snprintf(b, sizeof b, "worker NOT started (%s): spawn -1, errno %d", why, err);
  logLine(b);
}

void workerStop(const char *why) {
  char b[120];
  int pid = workerPid();
  if (pid)
    kill(pid, SIGTERM);
  snprintf(b, sizeof b, "worker stop (%s): %s", why, pid ? "SIGTERM sent" : "not running");
  logLine(b);
}

// ---- menu handlers ---------------------------------------------------------------------------------------------------
bool onOn(void *, const bmw::Parcel &) {
  g_on = !g_on;
  flagSet(SLP_F_ON, g_on);
  g_app->updateModel(SLP_MODEL_ON, g_on);
  logLine(g_on ? "menu: shift light on" : "menu: shift light off");
  if (g_on) {
    unlink(SLP_ASIDE);
    workerStart("menu");
  } else {
    workerStop("menu");
  }
  return true;
}

bool onSport(void *, const bmw::Parcel &) {
  g_sport = !g_sport;
  flagSet(SLP_F_SPORT, g_sport);
  g_app->updateModel(SLP_MODEL_SPORT, g_sport);
  logLine(g_sport ? "menu: only in SPORT and SPORT+ on" : "menu: only in SPORT and SPORT+ off");
  return true;
}

// no menu row on this car (no gearbox mode in its car data); kept for a car that has one
__attribute__((unused)) bool onManual(void *, const bmw::Parcel &) {
  g_manual = !g_manual;
  flagSet(SLP_F_MANUAL, g_manual);
  g_app->updateModel(SLP_MODEL_MANUAL, g_manual);
  logLine(g_manual ? "menu: only in manual mode on" : "menu: only in manual mode off");
  return true;
}

void showRpm() {
  for (int j = 0; j < SLP_RPM_N; j++)  // the HMI does not switch a plugin's radio buttons itself
    g_app->updateModel(SLP_MODEL_RPM0 + j, j == g_rpmIdx);
}

template <int K>
bool onRpm(void *, const bmw::Parcel &) {
  g_rpmIdx = K;
  writeInt(SLP_F_RPM, rpmOf(K));
  showRpm();
  char b[80];
  snprintf(b, sizeof b, "menu: shift point %d", rpmOf(K));
  logLine(b);
  return true;
}

// ---- car data (driving.mode, driving.mode35up, driving.gear) ---------------------------------------------------------
typedef void (*PostFn)(bmw::PluginApp *, void *runnable, unsigned delayMs);
typedef void (*CdsFn)(bmw::PluginApp *, bmw::BasicString<char> &, unsigned, const bmw::CdsDelegate &);
PostFn g_post;
CdsFn g_cds;
int g_car[6] = { -1, -1, -1, -1, -1, -1 };  // raw values of kCarProps
const char *const kCarProps[6] = { "driving.mode", "driving.mode35up", "driving.gear", "driving.shiftIndicator",
                                   "navigation.guidanceStatus", "vehicle.statePWF" };

void *frameworkSym(const char *name) {
  static const char *const libs[2] = { "libAppFramework.A.so", "libCoreFramework.A.so" };
  for (int i = 0; i < 2; i++) {
    void *h = dlopen(libs[i], RTLD_NOW);  // already loaded by the HMI: the same instance
    void *s = h ? dlsym(h, name) : 0;
    if (s)
      return s;
  }
  return 0;
}

void writeCar() {
  char b[128];
  int n = snprintf(b, sizeof b, "mode %d\nmode35up %d\ngear %d\nshift %d\nnav %d\npwf %d\n", g_car[0], g_car[1], g_car[2],
                   g_car[3], g_car[4], g_car[5]);
  writeText(SLP_CAR, b, n);
}

// the first integer after "key" (or anywhere, key 0) in the narrowed text; def when none
int intIn(const char *t, const char *key, int def) {
  const char *p = t;
  if (key) {
    int kl = 0;
    while (key[kl])
      kl++;
    for (; *p; p++) {
      int k = 0;
      while (k < kl && p[k] == key[k])
        k++;
      if (k == kl) {
        p += kl;
        break;
      }
    }
    if (!*p)
      return def;
  }
  for (; *p && !(*p >= '0' && *p <= '9') && *p != '-'; p++)
    ;
  if (!*p)
    return def;
  int neg = *p == '-', v = 0;
  if (neg)
    p++;
  if (!(*p >= '0' && *p <= '9'))
    return def;
  for (; *p >= '0' && *p <= '9' && v < 100000; p++)
    v = v * 10 + (*p - '0');
  return neg ? -v : v;
}

bool onCar(int i, const bmw::BasicString<unsigned short> &value) {
  char t[160];
  int n = 0;
  const unsigned short *p = value.m_data;
  while (p && p[n] && n < (int)sizeof t - 1) {
    t[n] = p[n] < 0x80 ? (char)p[n] : '?';
    n++;
  }
  t[n] = 0;
  static const char *const keys[6] = { "\"mode\"", "\"mode\"", 0, "\"shiftIndicator\"", "\"guidanceStatus\"", "\"statePWF\"" };
  int v = intIn(t, keys[i], -1);
  if (v != g_car[i]) {
    g_car[i] = v;
    writeCar();
    char b[240];
    snprintf(b, sizeof b, "car: %s = %d ('%s')", kCarProps[i], v, t);
    logLine(b);
  }
  return true;
}
bool onMode(void *, const bmw::BasicString<unsigned short> &v) {
  return onCar(0, v);
}
bool onMode35(void *, const bmw::BasicString<unsigned short> &v) {
  return onCar(1, v);
}
bool onGear(void *, const bmw::BasicString<unsigned short> &v) {
  return onCar(2, v);
}
bool onShift(void *, const bmw::BasicString<unsigned short> &v) {
  return onCar(3, v);
}
bool onNav(void *, const bmw::BasicString<unsigned short> &v) {
  return onCar(4, v);
}
bool onPwf(void *, const bmw::BasicString<unsigned short> &v) {
  return onCar(5, v);
}

struct CarRunnable {
  void **vptr;  // bmw::Runnable: the vtable pointer only
};
void *g_runVtbl[5];
CarRunnable g_carRunnable;

void runnableNoop(void *) {}
void runnableRun(void *) {  // on the app thread, kCdsDelayMs after onInit
  typedef bool (*Stub)(void *, const bmw::BasicString<unsigned short> &);
  static const Stub stubs[6] = { onMode, onMode35, onGear, onShift, onNav, onPwf };
  for (int i = 0; i < 6; i++) {
    bmw::BasicString<char> prop(kCarProps[i]);
    bmw::CdsDelegate d;
    d.object = g_app;
    d.stub = stubs[i];
    g_cds(g_app, prop, 0, d);
  }
  logLine("car data: driving.mode, .mode35up, .gear, .shiftIndicator, navigation.guidanceStatus, vehicle.statePWF registered");
}

void subscribeCar() {
  writeCar();  // all unknown until the first values
  g_post = reinterpret_cast<PostFn>(frameworkSym("_ZN3bmw9PluginApp4postEPNS_8RunnableEj"));
  g_cds = reinterpret_cast<CdsFn>(
      frameworkSym("_ZN3bmw9PluginApp33cdsAddPropertyChangedEventHandlerERNS_11BasicStringIcEEjRKNS_8DelegateIbRKNS1_ItEEvvvEE"));
  if (!g_post || !g_cds) {
    logLine(!g_post ? "car data: no PluginApp::post in the framework: no drive mode or gear"
                    : "car data: no PluginApp::cdsAddPropertyChangedEventHandler in the framework: no drive mode or gear");
    return;
  }
  void **base = reinterpret_cast<void **>(frameworkSym("_ZTVN3bmw8RunnableE"));
  if (base)
    memcpy(g_runVtbl, base, sizeof g_runVtbl);  // offset-to-top and typeinfo of bmw::Runnable
  g_runVtbl[2] = reinterpret_cast<void *>(&runnableNoop);  // destructors
  g_runVtbl[3] = reinterpret_cast<void *>(&runnableNoop);
  g_runVtbl[4] = reinterpret_cast<void *>(&runnableRun);
  g_carRunnable.vptr = g_runVtbl + 2;
  g_post(g_app, &g_carRunnable, 1500);
  logLine(base ? "car data: subscription posted" : "car data: subscription posted (Runnable vtable built here)");
}

// ---- boot guard and the watcher thread -------------------------------------------------------------------------------
const long kStableS = 20;  // an iDrive start that lasts this long counts as good
const long kFirstStartS = 12;
const long kRetryS = 20;  // a worker that ended is started again after this long (not after it stepped aside)

void *watcher(void *) {
  long t0 = monoS();
  bool stable = false;
  for (;;) {
    usleep(500000);
    long t = monoS() - t0;
    if (!stable && t >= kStableS) {
      char p[160];
      dirFile(p, sizeof p, SLP_F_PENDING);
      unlink(p);
      writeInt(SLP_F_STRIKES, 0);
      stable = true;
      logLine("boot guard: up for 20 s, this start counts as good");
    }
    if (g_child > 0 && waitpid(g_child, 0, WNOHANG) == g_child) {
      char b[80];
      snprintf(b, sizeof b, "worker %d ended", g_child);
      logLine(b);
      g_child = 0;
    }
    if (t >= kFirstStartS && g_on && monoS() - g_lastStart >= kRetryS && !workerPid())
      workerStart(g_lastStart < 0 ? "iDrive start" : "it had ended");
  }
  return 0;
}

// own vtable: a copy of bmw::PluginApp's vtable with onInit replaced (st1ng: 0xac bytes = 2 header words + 41 slots)
enum {
  kSlotOnInit = 22,
  kVtblSlots = 41
};
void *g_vtbl[2 + kVtblSlots];

void addAction(bmw::PluginApp *self, unsigned id, bool (*stub)(void *, const bmw::Parcel &)) {
  bmw::ActionDelegate d;
  d.object = self;
  d.stub = stub;
  self->addHandler(id, d);
}

bool appOnInit(bmw::PluginApp *self) {
  typedef bool (*Stub)(void *, const bmw::Parcel &);
  static const Stub rpm[SLP_RPM_N] = { onRpm<0>, onRpm<1>, onRpm<2>, onRpm<3>, onRpm<4>, onRpm<5>, onRpm<6> };
  addAction(self, SLP_ACT_ON, onOn);
  addAction(self, SLP_ACT_SPORT, onSport);
  for (int k = 0; k < SLP_RPM_N; k++)
    addAction(self, SLP_ACT_RPM0 + k, rpm[k]);
  bool ok = self->onInit();  // non-virtual call = the base implementation
  self->updateModel(SLP_MODEL_ON, g_on);
  self->updateModel(SLP_MODEL_SPORT, g_sport);
  showRpm();
  logLine(ok ? "onInit: handlers registered, base onInit ok" : "onInit: handlers registered, base onInit FAILED");
  subscribeCar();
  return ok;
}

}  // namespace

extern "C" void *ShiftLight_create(bmw::IDispatcher &disp, const char *name, const bmw::Bundle &params,
                                   const bmw::Bundle &config) {
  char b[160], p[160];
  if (g_app) {
    logLine("create: already created");
    return 0;
  }
  if (flagGet(SLP_F_DISABLED)) {
    logLine("disabled (" SLP_DIR "/" SLP_F_DISABLED "): no menu. Remove the file and restart iDrive to enable it.");
    return 0;
  }
  // boot guard: boot_pending still there = the last start did not last kStableS
  dirFile(p, sizeof p, SLP_F_PENDING);
  int strikes = pathExists(p) ? readInt(SLP_F_STRIKES, 0) + 1 : 0;
  if (strikes >= 3) {
    flagSet(SLP_F_DISABLED, true);
    unlink(p);
    writeInt(SLP_F_STRIKES, 0);
    logLine("boot guard: 3 iDrive starts in a row ended within 20 s: disabled (" SLP_DIR "/" SLP_F_DISABLED ")");
    return 0;
  }
  writeInt(SLP_F_STRIKES, strikes);
  writeText(p, "1\n", 2);
  snprintf(b, sizeof b, "create (%s), boot guard: %d short start(s) before this one", name ? name : "?", strikes);
  logLine(b);
  unlink(SLP_ASIDE);
  {  // the permanent event log (shared with the worker): one line per iDrive start
    int n = readInt(SLP_F_STARTS, 0) + 1;
    writeInt(SLP_F_STARTS, n);
    char e[160], ln[200];
    dirFile(e, sizeof e, SLP_F_EVENTS);
    int sz = 0, fd = open(e, O_RDONLY);
    if (fd >= 0) {
      char x[1024];
      long r;
      while ((r = read(fd, x, sizeof x)) > 0 && sz < 64000)
        sz += (int)r;
      close(fd);
    }
    int len = snprintf(ln, sizeof ln, "=== iDrive start %d (plugin, %ld s since boot) ===\n", n, monoS());
    fd = open(e, sz > 32768 ? O_WRONLY | O_CREAT | O_TRUNC : O_WRONLY | O_CREAT | O_APPEND, 0666);
    if (fd >= 0) {
      long w = write(fd, ln, (unsigned long)len);
      (void)w;
      close(fd);
    }
  }

  void *obj = malloc(0x200);
  if (!obj)
    return 0;
  memset(obj, 0, 0x200);
  new (obj) bmw::PluginApp(disp, bmw::BasicString<char>(name ? name : "ShiftLight"), 1, 0, bmw::RHMI_INTERFACE_VERSION_1,
                           params, config);
  void **base = *reinterpret_cast<void ***>(obj);  // bmw::PluginApp's vtable
  memcpy(g_vtbl, base - 2, sizeof(g_vtbl));        // [offset-to-top, typeinfo, 41 slots]
  g_vtbl[2 + kSlotOnInit] = reinterpret_cast<void *>(&appOnInit);
  *reinterpret_cast<void ***>(obj) = g_vtbl + 2;
  g_app = reinterpret_cast<bmw::PluginApp *>(obj);
  loadSettings();
  static bool watching;
  int tid;
  if (!watching && pthread_create(&tid, 0, watcher, 0) != 0)
    logLine("watcher thread not started: the worker starts only from the menu, and the boot guard counts this start");
  else
    watching = true;
  logLine("create ok");
  return obj;
}

extern "C" void ShiftLight_destroy(void *app) {
  logLine("destroy");
  if (!app)
    return;
  void **vt = *reinterpret_cast<void ***>(app);  // vtable[0] = complete destructor, without free
  reinterpret_cast<void (*)(void *)>(vt[0])(app);
  free(app);
  if (app == g_app)
    g_app = 0;
}

struct pai {
  unsigned version;
  const char *name;
  const char *vendor;
  unsigned rhmi;
  void *create;
  void *destroy;
};
__attribute__((used, visibility("default"))) struct pai pluginAppInfo = {
  1, "Shift Light", "hud-shiftlight", 1, (void *)ShiftLight_create, (void *)ShiftLight_destroy
};
