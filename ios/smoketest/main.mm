// OpenJam iPhone smoke test
//
// Checks, on a real iPhone sideloaded with a free Apple ID, what an iOS port
// of the game would need - before any of the port is worth doing:
//
//   1. The guest memory arena: ReXGlue (src/system/xmemory.cpp, MapViewsMac)
//      reserves one 4.5 GB range and maps nine views of one shared-memory
//      object into it, two pairs aliasing the same pages. That needs a 4.5 GB
//      address reservation, and a way to make shared memory: shm_open as
//      ReXGlue does, a file in the app's own tmp folder, or Mach memory
//      entries (vm_remap), each tried in turn.
//   2. How much memory iOS lets the app use, and committing 512 MB of it.
//   3. Recovering from a fault in a signal handler, which the runtime's
//      emulation of the GPU's registers is built on.
//   4. Metal, which the graphics would reach through MoltenVK.
//   5. A game controller.
//
// Results are shown on screen and written to Documents/report.txt, which the
// Files app shows under "On My iPhone".

#import <GameController/GameController.h>
#import <Metal/Metal.h>
#import <UIKit/UIKit.h>

#include <fcntl.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include <os/proc.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::string g_report;

void Line(const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  g_report += buf;
  g_report += "\n";
}

// --- 1. the guest memory arena ----------------------------------------------

// ReXGlue v0.10.0's map_info: guest range and its offset in the section.
struct View {
  uint64_t start, end, target;
};
constexpr View kViews[] = {
    {0x00000000, 0x3FFFFFFF, 0x0000000000000000ull},
    {0x40000000, 0x7EFFFFFF, 0x0000000040000000ull},
    {0x7F000000, 0x7FFFFFFF, 0x0000000100000000ull},
    {0x80000000, 0x8FFFFFFF, 0x0000000080000000ull},
    {0x90000000, 0x9FFFFFFF, 0x0000000080000000ull},
    {0xA0000000, 0xBFFFFFFF, 0x0000000100000000ull},
    {0xC0000000, 0xDFFFFFFF, 0x0000000100000000ull},
    {0xE0000000, 0xFFFFFFFF, 0x0000000100001000ull},
    {0x100000000, 0x11FFFFFFF, 0x0000000100000000ull},
};
constexpr uint64_t kSection = 0x120000000ull;  // 4.5 GB
constexpr uint64_t kTotal = 0x120000000ull;    // the reserved range

uint8_t* g_base = nullptr;

// Writes through guest-virtual A0001000 and reads through guest-physical
// 1000: the same page of the section if the views alias as they must.
bool AliasHolds(uint8_t* base) {
  *reinterpret_cast<volatile uint32_t*>(base + 0xA0001000ull) = 0x4A414D21u;
  return *reinterpret_cast<volatile uint32_t*>(base + 0x100000000ull + 0x1000) == 0x4A414D21u;
}

bool MapFromFd(int fd, const char* how) {
  void* reserved = mmap(nullptr, kTotal, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (reserved == MAP_FAILED) {
    Line("  %s: reserving 4.5 GB failed, errno %d (%s)", how, errno, strerror(errno));
    return false;
  }
  auto* base = static_cast<uint8_t*>(reserved);
  for (const View& v : kViews) {
    const size_t size = size_t(v.end - v.start + 1);
    const off_t offset = off_t(v.target & ~uint64_t(0xFFFF));
    void* got = mmap(base + v.start, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd,
                     offset);
    if (got == MAP_FAILED) {
      Line("  %s: view at %llX failed, errno %d (%s)", how, (unsigned long long)v.start, errno,
           strerror(errno));
      munmap(reserved, kTotal);
      return false;
    }
  }
  const bool alias = AliasHolds(base);
  Line("  %s: all 9 views mapped at %p, alias %s", how, base, alias ? "holds" : "BROKEN");
  if (alias) g_base = base;
  return alias;
}

bool TryShmOpen() {
  const char* name = "/openjam_arena";
  shm_unlink(name);
  int fd = shm_open(name, O_RDWR | O_CREAT, 0600);
  if (fd < 0) {
    Line("  shm_open: failed, errno %d (%s)", errno, strerror(errno));
    return false;
  }
  if (ftruncate(fd, off_t(kSection)) != 0) {
    Line("  shm_open: sizing to 4.5 GB failed, errno %d (%s)", errno, strerror(errno));
    close(fd);
    shm_unlink(name);
    return false;
  }
  const bool ok = MapFromFd(fd, "shm_open");
  close(fd);
  shm_unlink(name);
  return ok;
}

bool TryTmpFile() {
  std::string path = std::string(NSTemporaryDirectory().UTF8String) + "openjam_arena_XXXXXX";
  int fd = mkstemp(path.data());
  if (fd < 0) {
    Line("  tmp file: failed, errno %d (%s)", errno, strerror(errno));
    return false;
  }
  unlink(path.c_str());
  if (ftruncate(fd, off_t(kSection)) != 0) {
    Line("  tmp file: sizing to 4.5 GB failed, errno %d (%s)", errno, strerror(errno));
    close(fd);
    return false;
  }
  const bool ok = MapFromFd(fd, "tmp file");
  close(fd);
  return ok;
}

// Mach memory: allocate the section as anonymous memory, then remap its
// pieces into the reserved range, shared rather than copied.
bool TryMach() {
  vm_address_t section = 0;
  kern_return_t kr = vm_allocate(mach_task_self(), &section, kSection, VM_FLAGS_ANYWHERE);
  if (kr != KERN_SUCCESS) {
    Line("  mach: allocating 4.5 GB failed, %d (%s)", kr, mach_error_string(kr));
    return false;
  }
  vm_address_t base = 0;
  kr = vm_allocate(mach_task_self(), &base, kTotal, VM_FLAGS_ANYWHERE);
  if (kr != KERN_SUCCESS) {
    Line("  mach: reserving 4.5 GB failed, %d (%s)", kr, mach_error_string(kr));
    vm_deallocate(mach_task_self(), section, kSection);
    return false;
  }
  for (const View& v : kViews) {
    vm_address_t at = base + v.start;
    vm_prot_t cur = 0, max = 0;
    kr = vm_remap(mach_task_self(), &at, v.end - v.start + 1, 0,
                       VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                       section + (v.target & ~uint64_t(0xFFFF)), FALSE, &cur, &max,
                       VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
      Line("  mach: view at %llX failed, %d (%s)", (unsigned long long)v.start, kr,
           mach_error_string(kr));
      vm_deallocate(mach_task_self(), base, kTotal);
      vm_deallocate(mach_task_self(), section, kSection);
      return false;
    }
  }
  const bool alias = AliasHolds(reinterpret_cast<uint8_t*>(base));
  Line("  mach: all 9 views remapped at %p, alias %s", (void*)base, alias ? "holds" : "BROKEN");
  if (alias) g_base = reinterpret_cast<uint8_t*>(base);
  return alias;
}

bool TestArena() {
  Line("[arena] the guest memory arena (ReXGlue layout)");
  // The largest single range iOS gives this app at all, for context.
  uint64_t biggest = 0;
  for (uint64_t gb = 64; gb >= 1; gb /= 2) {
    void* p = mmap(nullptr, gb << 30, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) {
      munmap(p, gb << 30);
      biggest = gb;
      break;
    }
  }
  Line("  largest single reservation: %llu GB (of 64 tried)", (unsigned long long)biggest);
  const bool shm = TryShmOpen();
  const bool tmp = !shm && TryTmpFile();
  const bool mach = !shm && !tmp && TryMach();
  return shm || tmp || mach;
}

// --- 2. memory ---------------------------------------------------------------

bool TestMemory() {
  Line("[memory] how much the app may use");
  Line("  available now: %zu MB", os_proc_available_memory() >> 20);
  if (!g_base) {
    Line("  skipped: no arena");
    return false;
  }
  constexpr size_t kSize = 512ull << 20;
  uint8_t* at = g_base + 0x100000000ull;  // the guest-physical view
  const auto t0 = std::chrono::steady_clock::now();
  for (size_t off = 0; off < kSize; off += 16384) at[off] = uint8_t(off >> 14);
  const double ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  Line("  512 MB of guest memory touched in %.0f ms; %zu MB left", ms,
       os_proc_available_memory() >> 20);
  return true;
}

// --- 3. faults ----------------------------------------------------------------

uint8_t* g_trap = nullptr;
volatile sig_atomic_t g_trap_hits = 0;

void OnFault(int, siginfo_t* info, void*) {
  auto* addr = static_cast<uint8_t*>(info->si_addr);
  if (g_trap && addr >= g_trap && addr < g_trap + 16384) {
    mprotect(g_trap, 16384, PROT_READ | PROT_WRITE);
    ++g_trap_hits;
    return;
  }
  signal(SIGBUS, SIG_DFL);
  signal(SIGSEGV, SIG_DFL);
}

bool TestFault() {
  Line("[fault] recovering from an access violation");
  struct sigaction sa {};
  sa.sa_sigaction = OnFault;
  sa.sa_flags = SA_SIGINFO;
  struct sigaction old_bus {}, old_segv {};
  sigaction(SIGBUS, &sa, &old_bus);
  sigaction(SIGSEGV, &sa, &old_segv);
  g_trap = static_cast<uint8_t*>(
      mmap(nullptr, 16384, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0));
  *reinterpret_cast<volatile uint32_t*>(g_trap + 16) = 0x1234;
  const bool ok = g_trap_hits == 1 && *reinterpret_cast<volatile uint32_t*>(g_trap + 16) == 0x1234;
  sigaction(SIGBUS, &old_bus, nullptr);
  sigaction(SIGSEGV, &old_segv, nullptr);
  Line("  fault caught %d time(s), write survived: %s", int(g_trap_hits), ok ? "yes" : "no");
  return ok;
}

// --- 4. Metal ------------------------------------------------------------------

bool TestMetal() {
  Line("[metal] the GPU");
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (!device) {
    Line("  no Metal device");
    return false;
  }
  Line("  %s, Apple family 6: %s, max buffer %llu MB", device.name.UTF8String,
       [device supportsFamily:MTLGPUFamilyApple6] ? "yes" : "no",
       (unsigned long long)(device.maxBufferLength >> 20));
  return true;
}

std::string Controllers() {
  std::string out;
  for (GCController* c in GCController.controllers) {
    if (!out.empty()) out += ", ";
    out += c.vendorName ? c.vendorName.UTF8String : "controller";
  }
  return out.empty() ? "none connected (pair one in Settings > Bluetooth)" : out;
}

}  // namespace

@interface ViewController : UIViewController
@property(nonatomic, strong) UITextView* text;
@end

@implementation ViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.blackColor;
  self.text = [[UITextView alloc] initWithFrame:self.view.bounds];
  self.text.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  self.text.backgroundColor = UIColor.blackColor;
  self.text.textColor = UIColor.whiteColor;
  self.text.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
  self.text.editable = NO;
  [self.view addSubview:self.text];

  Line("OPENJAM IPHONE SMOKE TEST - screenshot this and send it back");
  Line("%s, iOS %s", UIDevice.currentDevice.model.UTF8String,
       UIDevice.currentDevice.systemVersion.UTF8String);
  Line("");
  Line("%s arena", TestArena() ? "[ OK ]" : "[FAIL]");
  Line("%s memory", TestMemory() ? "[ OK ]" : "[FAIL]");
  Line("%s fault", TestFault() ? "[ OK ]" : "[FAIL]");
  Line("%s metal", TestMetal() ? "[ OK ]" : "[FAIL]");
  Line("[info] controller: %s", Controllers().c_str());
  [self show];

  [NSNotificationCenter.defaultCenter addObserverForName:GCControllerDidConnectNotification
                                                  object:nil
                                                   queue:NSOperationQueue.mainQueue
                                              usingBlock:^(NSNotification*) {
                                                Line("[info] controller connected: %s",
                                                     Controllers().c_str());
                                                [self show];
                                              }];
}

- (void)show {
  self.text.text = [NSString stringWithUTF8String:g_report.c_str()];
  NSString* docs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES)
                       .firstObject;
  [self.text.text writeToFile:[docs stringByAppendingPathComponent:@"report.txt"]
                   atomically:YES
                     encoding:NSUTF8StringEncoding
                        error:nil];
}
@end

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation AppDelegate
- (BOOL)application:(UIApplication*)application
    didFinishLaunchingWithOptions:(NSDictionary*)options {
  self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
  self.window.rootViewController = [[ViewController alloc] init];
  [self.window makeKeyAndVisible];
  return YES;
}
@end

int main(int argc, char* argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass([AppDelegate class]));
  }
}
