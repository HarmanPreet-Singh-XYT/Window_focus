#include "include/window_focus/window_focus_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <sys/utsname.h>
#include <cmath>

#ifdef HAVE_PULSEAUDIO
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#endif

#include <linux/input.h>
#include <linux/joystick.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <poll.h>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <algorithm>
#include <sstream>
#include <set>

#include "window_focus_plugin_private.h"

#define WINDOW_FOCUS_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), window_focus_plugin_get_type(), \
                               WindowFocusPlugin))

// Input source identification for debugging
enum class InputSource {
  Keyboard,
  Mouse,
  Joystick,
  HIDDevice,
  SystemAudio
};

static const char* input_source_name(InputSource source) {
  switch (source) {
    case InputSource::Keyboard: return "Keyboard";
    case InputSource::Mouse: return "Mouse";
    case InputSource::Joystick: return "Joystick";
    case InputSource::HIDDevice: return "HID Device";
    case InputSource::SystemAudio: return "System Audio";
  }
  return "Unknown";
}

struct _WindowFocusPlugin {
  GObject parent_instance;

  FlMethodChannel* channel;

  // Monitoring flags
  bool enableDebug;
  bool monitorKeyboard;
  bool monitorMouse;
  bool monitorControllers;
  bool monitorSystemAudio;
  bool monitorHIDDevices;

  // Thresholds
  int inactivityThreshold; // milliseconds
  float audioThreshold;

  // State tracking
  std::atomic<bool> userIsActive;
  std::atomic<bool> isShuttingDown;
  std::atomic<int> threadCount;

  std::chrono::steady_clock::time_point lastActivityTime;
  std::mutex activityMutex;
  std::mutex channelMutex;
  std::mutex shutdownMutex;
  std::condition_variable shutdownCv;

  // Input device tracking - joystick fds are /dev/input/js* devices
  std::vector<int> joystickFds;
  // Track which event device numbers correspond to joysticks (to avoid double-monitoring)
  std::set<int> joystickEventNumbers;
  // HID device fds are /dev/input/event* devices (non-keyboard, non-mouse, non-joystick)
  std::vector<int> hidDeviceFds;
  std::mutex deviceMutex;

  // Mouse tracking
  int lastMouseX;
  int lastMouseY;
  std::mutex mouseMutex;

  // System Audio
#ifdef HAVE_PULSEAUDIO
  pa_simple* systemAudioStream;
  std::mutex audioMutex;
#endif

  // Keyboard state tracking for edge detection
  char lastKeyState[32];
  std::mutex keyStateMutex;
};

G_DEFINE_TYPE(WindowFocusPlugin, window_focus_plugin, g_object_get_type())

// Forward declarations
static void update_last_activity_time(WindowFocusPlugin* self);
static void report_activity(WindowFocusPlugin* self, InputSource source);
static void safe_invoke_method(WindowFocusPlugin* self, const gchar* method_name, const gchar* message);
static void safe_invoke_method_with_map(WindowFocusPlugin* self, const gchar* method_name, FlValue* data);
static void start_monitoring_threads(WindowFocusPlugin* self);
static void stop_monitoring_threads(WindowFocusPlugin* self);
static void check_for_inactivity(WindowFocusPlugin* self);
static void start_focus_listener(WindowFocusPlugin* self);
static void monitor_joystick_devices(WindowFocusPlugin* self);
static void monitor_hid_devices_thread(WindowFocusPlugin* self);
static void initialize_joysticks(WindowFocusPlugin* self);
static void initialize_hid_devices(WindowFocusPlugin* self);
static void close_input_devices(WindowFocusPlugin* self);
static bool check_system_audio(WindowFocusPlugin* self);
static std::optional<std::vector<uint8_t>> take_screenshot(WindowFocusPlugin* self, bool activeWindowOnly);
static int find_joystick_event_number(int js_fd);

// Update last activity time
static void update_last_activity_time(WindowFocusPlugin* self) {
  std::lock_guard<std::mutex> lock(self->activityMutex);
  self->lastActivityTime = std::chrono::steady_clock::now();
}

// Report activity from a specific source — handles state transition + debug logging
static void report_activity(WindowFocusPlugin* self, InputSource source) {
  if (self->isShuttingDown) return;

  update_last_activity_time(self);

  if (!self->userIsActive) {
    self->userIsActive = true;

    if (self->enableDebug) {
      std::cout << "[WindowFocus] User became active (source: "
                << input_source_name(source) << ")" << std::endl;
    }

    safe_invoke_method(self, "onUserActive", "User is active");
  }
}

// Safe method invocation — must be called from main thread or use g_idle_add
// Since fl_method_channel_invoke_method should be called from the main thread,
// we use g_idle_add to marshal the call.

struct MethodInvocationData {
  WindowFocusPlugin* self;
  std::string method_name;
  FlValue* value; // ownership transferred
};

static gboolean invoke_method_on_main_thread(gpointer user_data) {
  auto* data = static_cast<MethodInvocationData*>(user_data);

  if (!data->self->isShuttingDown) {
    std::lock_guard<std::mutex> lock(data->self->channelMutex);
    if (data->self->channel && !data->self->isShuttingDown) {
      fl_method_channel_invoke_method(
          data->self->channel,
          data->method_name.c_str(),
          data->value,
          nullptr, nullptr, nullptr);
    }
  }

  if (data->value) {
    fl_value_unref(data->value);
  }
  delete data;
  return G_SOURCE_REMOVE;
}

static void safe_invoke_method(WindowFocusPlugin* self, const gchar* method_name, const gchar* message) {
  if (self->isShuttingDown) return;

  auto* data = new MethodInvocationData();
  data->self = self;
  data->method_name = method_name;
  data->value = fl_value_new_string(message);
  fl_value_ref(data->value);

  g_idle_add(invoke_method_on_main_thread, data);
}

static void safe_invoke_method_with_map(WindowFocusPlugin* self, const gchar* method_name, FlValue* map_data) {
  if (self->isShuttingDown) return;

  auto* data = new MethodInvocationData();
  data->self = self;
  data->method_name = method_name;
  data->value = map_data;
  fl_value_ref(data->value);

  g_idle_add(invoke_method_on_main_thread, data);
}

// Get focused window information — MUST be called with its own Display* or
// from the thread that owns the Display*
static FlValue* get_focused_window_info(Display* display) {
  FlValue* result = fl_value_new_map();

  if (!display) {
    fl_value_set_string_take(result, "title", fl_value_new_string(""));
    fl_value_set_string_take(result, "appName", fl_value_new_string(""));
    fl_value_set_string_take(result, "windowTitle", fl_value_new_string(""));
    return result;
  }

  Window focused_window;
  int revert_to;
  XGetInputFocus(display, &focused_window, &revert_to);

  if (focused_window == None || focused_window == PointerRoot) {
    fl_value_set_string_take(result, "title", fl_value_new_string(""));
    fl_value_set_string_take(result, "appName", fl_value_new_string(""));
    fl_value_set_string_take(result, "windowTitle", fl_value_new_string(""));
    return result;
  }

  // Helper lambda to get window title
  auto get_window_title = [&](Display* dpy, Window w) -> std::string {
    Atom net_wm_name = XInternAtom(dpy, "_NET_WM_NAME", False);
    Atom utf8_string = XInternAtom(dpy, "UTF8_STRING", False);

    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_after;
    unsigned char* prop = nullptr;

    if (XGetWindowProperty(dpy, w, net_wm_name, 0, 1024, False, utf8_string,
                           &actual_type, &actual_format, &nitems, &bytes_after,
                           &prop) == Success && prop) {
      std::string result(reinterpret_cast<char*>(prop));
      XFree(prop);
      return result;
    }

    char* window_name = nullptr;
    if (XFetchName(dpy, w, &window_name) && window_name) {
      std::string result(window_name);
      XFree(window_name);
      return result;
    }

    return "";
  };

  std::string title;
  std::string app_name;
  
  // ALWAYS walk up the window tree to find the top-level window
  Window root, parent;
  Window* children = nullptr;
  unsigned int nchildren;
  Window current = focused_window;
  
  // First, walk up to find the top-level window
  Window top_level = focused_window;
  for (int depth = 0; depth < 20; depth++) {
    if (!XQueryTree(display, current, &root, &parent, &children, &nchildren)) {
      break;
    }
    if (children) XFree(children);
    
    if (parent == root || parent == None) {
      top_level = current;
      break;
    }
    current = parent;
  }
  
  // Now get properties from the top-level window
  title = get_window_title(display, top_level);
  
  XClassHint class_hint;
  if (XGetClassHint(display, top_level, &class_hint)) {
    if (class_hint.res_class) {
      app_name = class_hint.res_class;
      XFree(class_hint.res_class);
    }
    if (class_hint.res_name) {
      XFree(class_hint.res_name);
    }
  }
  
  // If still no title, try the originally focused window
  if (title.empty() && focused_window != top_level) {
    title = get_window_title(display, focused_window);
  }

  fl_value_set_string_take(result, "title", fl_value_new_string(title.c_str()));
  fl_value_set_string_take(result, "appName", fl_value_new_string(app_name.c_str()));
  fl_value_set_string_take(result, "windowTitle", fl_value_new_string(title.c_str()));

  return result;
}

// Find the event device number corresponding to a joystick fd
// Returns -1 if not found
static int find_joystick_event_number(int js_fd) {
  // Read the joystick name
  char js_name[256] = {0};
  if (ioctl(js_fd, JSIOCGNAME(sizeof(js_name)), js_name) < 0) {
    return -1;
  }

  // Scan /dev/input/event* to find matching device
  DIR* dir = opendir("/dev/input");
  if (!dir) return -1;

  int event_num = -1;
  struct dirent* entry;

  while ((entry = readdir(dir)) != nullptr) {
    if (strncmp(entry->d_name, "event", 5) != 0) continue;

    std::string device_path = std::string("/dev/input/") + entry->d_name;
    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    char ev_name[256] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(ev_name)), ev_name);
    close(fd);

    if (strcmp(js_name, ev_name) == 0) {
      // Extract event number
      event_num = atoi(entry->d_name + 5);
      break;
    }
  }

  closedir(dir);
  return event_num;
}

// Initialize joystick devices
static void initialize_joysticks(WindowFocusPlugin* self) {
  std::lock_guard<std::mutex> lock(self->deviceMutex);

  for (int fd : self->joystickFds) {
    close(fd);
  }
  self->joystickFds.clear();
  self->joystickEventNumbers.clear();

  if (self->enableDebug) {
    std::cout << "[WindowFocus] Scanning for joystick devices..." << std::endl;
  }

  for (int i = 0; i < 16; i++) {
    std::string device_path = "/dev/input/js" + std::to_string(i);
    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd >= 0) {
      char name[128] = {0};
      if (ioctl(fd, JSIOCGNAME(sizeof(name)), name) >= 0) {
        self->joystickFds.push_back(fd);

        // Find corresponding event device to avoid double-monitoring
        int event_num = find_joystick_event_number(fd);
        if (event_num >= 0) {
          self->joystickEventNumbers.insert(event_num);
        }

        if (self->enableDebug) {
          std::cout << "[WindowFocus] Found joystick " << i << ": " << name
                    << " at " << device_path;
          if (event_num >= 0) {
            std::cout << " (event" << event_num << ")";
          }
          std::cout << std::endl;
        }
      } else {
        close(fd);
      }
    }
  }

  if (self->enableDebug) {
    std::cout << "[WindowFocus] Total joysticks found: " << self->joystickFds.size() << std::endl;
  }
}

// Initialize HID devices (non-keyboard, non-mouse, non-joystick event devices)
static void initialize_hid_devices(WindowFocusPlugin* self) {
  std::lock_guard<std::mutex> lock(self->deviceMutex);

  for (int fd : self->hidDeviceFds) {
    close(fd);
  }
  self->hidDeviceFds.clear();

  if (self->enableDebug) {
    std::cout << "[WindowFocus] Scanning for HID devices..." << std::endl;
  }

  DIR* dir = opendir("/dev/input");
  if (!dir) {
    if (self->enableDebug) {
      std::cerr << "[WindowFocus] Failed to open /dev/input directory" << std::endl;
    }
    return;
  }

  struct dirent* entry;
  while ((entry = readdir(dir)) != nullptr) {
    if (strncmp(entry->d_name, "event", 5) != 0) continue;

    int event_num = atoi(entry->d_name + 5);

    // Skip event devices that correspond to joysticks we're already monitoring
    if (self->joystickEventNumbers.count(event_num) > 0) {
      if (self->enableDebug) {
        std::cout << "[WindowFocus] Skipping event" << event_num
                  << " (already monitored as joystick)" << std::endl;
      }
      continue;
    }

    std::string device_path = std::string("/dev/input/") + entry->d_name;
    int fd = open(device_path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;

    char name[256] = {0};
    ioctl(fd, EVIOCGNAME(sizeof(name)), name);
    std::string device_name(name);

    // Get device capabilities using the proper bitmask size
    unsigned long evbit[(EV_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};

    if (ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), evbit) < 0) {
      close(fd);
      continue;
    }

    // Proper bit testing macro
    #define TEST_BIT(bit, array) ((array[(bit) / (8 * sizeof(long))] >> ((bit) % (8 * sizeof(long)))) & 1)

    bool has_key = TEST_BIT(EV_KEY, evbit);
    bool has_rel = TEST_BIT(EV_REL, evbit);
    bool has_abs = TEST_BIT(EV_ABS, evbit);

    // More sophisticated device classification
    std::string name_lower = device_name;
    std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);

    // Check for keyboard indicators
    bool is_keyboard = false;
    if (has_key && !has_rel && !has_abs) {
      // Has keys but no positional input — likely keyboard
      // Verify by checking if it has letter keys
      unsigned long keybit[(KEY_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
      if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
        // Check for common letter keys (KEY_Q = 16, KEY_A = 30, KEY_Z = 44)
        if (TEST_BIT(KEY_Q, keybit) && TEST_BIT(KEY_A, keybit) && TEST_BIT(KEY_Z, keybit)) {
          is_keyboard = true;
        }
      }
    }
    if (name_lower.find("keyboard") != std::string::npos) {
      is_keyboard = true;
    }

    // Check for mouse indicators
    bool is_mouse = false;
    if (has_rel) {
      unsigned long relbit[(REL_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
      if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbit)), relbit) >= 0) {
        // Mouse has REL_X and REL_Y
        if (TEST_BIT(REL_X, relbit) && TEST_BIT(REL_Y, relbit)) {
          is_mouse = true;
        }
      }
    }
    if (name_lower.find("mouse") != std::string::npos ||
        name_lower.find("trackpad") != std::string::npos ||
        name_lower.find("touchpad") != std::string::npos ||
        name_lower.find("trackpoint") != std::string::npos) {
      is_mouse = true;
    }

    // Check for gamepad/joystick (has absolute axes like ABS_X, ABS_Y and buttons)
    bool is_gamepad = false;
    if (has_abs && has_key) {
      unsigned long absbit[(ABS_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
      unsigned long keybit[(KEY_MAX + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
      if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbit)), absbit) >= 0 &&
          ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit) >= 0) {
        bool has_abs_xy = TEST_BIT(ABS_X, absbit) && TEST_BIT(ABS_Y, absbit);
        // BTN_GAMEPAD = BTN_SOUTH = 0x130, BTN_JOYSTICK = 0x120
        bool has_gamepad_buttons = TEST_BIT(BTN_SOUTH, keybit) || TEST_BIT(BTN_JOYSTICK, keybit);
        if (has_abs_xy && has_gamepad_buttons) {
          is_gamepad = true;
        }
      }
    }
    if (name_lower.find("gamepad") != std::string::npos ||
        name_lower.find("joystick") != std::string::npos ||
        name_lower.find("controller") != std::string::npos) {
      is_gamepad = true;
    }

    #undef TEST_BIT

    if (self->enableDebug) {
      std::cout << "[WindowFocus] Device: " << device_path
                << " (" << device_name << ")"
                << " KEY=" << has_key
                << " REL=" << has_rel
                << " ABS=" << has_abs
                << " [keyboard=" << is_keyboard
                << " mouse=" << is_mouse
                << " gamepad=" << is_gamepad << "]" << std::endl;
    }

    // Skip keyboards and mice — we monitor them via X11
    // Skip gamepads — we monitor them via /dev/input/js*
    if (is_keyboard || is_mouse || is_gamepad) {
      if (self->enableDebug) {
        const char* reason = is_keyboard ? "keyboard" : (is_mouse ? "mouse" : "gamepad");
        std::cout << "[WindowFocus] Skipping " << device_path
                  << " (" << reason << ")" << std::endl;
      }
      close(fd);
    } else {
      self->hidDeviceFds.push_back(fd);
      if (self->enableDebug) {
        std::cout << "[WindowFocus] Monitoring HID device: " << device_path
                  << " (" << device_name << ")" << std::endl;
      }
    }
  }
  closedir(dir);

  if (self->enableDebug) {
    std::cout << "[WindowFocus] Total HID devices monitored: " << self->hidDeviceFds.size() << std::endl;
  }
}

// Close all input devices
static void close_input_devices(WindowFocusPlugin* self) {
  std::lock_guard<std::mutex> lock(self->deviceMutex);

  for (int fd : self->joystickFds) {
    close(fd);
  }
  self->joystickFds.clear();
  self->joystickEventNumbers.clear();

  for (int fd : self->hidDeviceFds) {
    close(fd);
  }
  self->hidDeviceFds.clear();
}

// Dedicated joystick monitoring thread using poll() for efficient I/O
static void monitor_joystick_devices(WindowFocusPlugin* self) {
  self->threadCount++;

  std::thread([self]() {
    if (self->enableDebug) {
      std::cout << "[WindowFocus] Joystick monitoring thread started" << std::endl;
    }

    auto lastDeviceReinit = std::chrono::steady_clock::now();
    const auto deviceReinitInterval = std::chrono::seconds(30);

    while (!self->isShuttingDown) {
      // Build pollfd array from current joystick fds
      std::vector<struct pollfd> pfds;
      {
        std::lock_guard<std::mutex> lock(self->deviceMutex);
        for (int fd : self->joystickFds) {
          struct pollfd pfd;
          pfd.fd = fd;
          pfd.events = POLLIN;
          pfd.revents = 0;
          pfds.push_back(pfd);
        }
      }

      if (pfds.empty()) {
        // No joysticks, sleep and retry
        std::unique_lock<std::mutex> lock(self->shutdownMutex);
        if (self->shutdownCv.wait_for(lock, std::chrono::seconds(5),
            [self] { return self->isShuttingDown.load(); })) {
          break;
        }

        // Periodically try to find new joysticks
        auto now = std::chrono::steady_clock::now();
        if (now - lastDeviceReinit > deviceReinitInterval) {
          lastDeviceReinit = now;
          if (self->monitorControllers && !self->isShuttingDown) {
            initialize_joysticks(self);
          }
        }
        continue;
      }

      // poll with 200ms timeout
      int ret = poll(pfds.data(), pfds.size(), 200);

      if (self->isShuttingDown) break;

      if (ret > 0) {
        std::lock_guard<std::mutex> lock(self->deviceMutex);
        bool inputDetected = false;

        for (size_t i = 0; i < pfds.size() && i < self->joystickFds.size(); i++) {
          if (!(pfds[i].revents & POLLIN)) continue;

          int fd = self->joystickFds[i];
          struct js_event event;

          while (true) {
            ssize_t bytes = read(fd, &event, sizeof(event));
            if (bytes != sizeof(event)) {
              if (bytes == -1 && errno != EAGAIN && self->enableDebug) {
                std::cerr << "[WindowFocus] Error reading joystick " << i
                          << ": " << strerror(errno) << std::endl;
              }
              break;
            }

            int event_type = event.type & ~JS_EVENT_INIT;

            // Skip init events — they report initial state, not user input
            if (event.type & JS_EVENT_INIT) {
              continue;
            }

            if (event_type == JS_EVENT_BUTTON) {
              if (self->enableDebug) {
                std::cout << "[WindowFocus] Joystick " << i << " button "
                          << (int)event.number << " = " << event.value << std::endl;
              }
              inputDetected = true;
            } else if (event_type == JS_EVENT_AXIS) {
              if (abs(event.value) > 3000) {
                if (self->enableDebug) {
                  std::cout << "[WindowFocus] Joystick " << i << " axis "
                            << (int)event.number << " = " << event.value << std::endl;
                }
                inputDetected = true;
              }
            }
          }
        }

        if (inputDetected) {
          report_activity(self, InputSource::Joystick);
        }
      } else if (ret < 0 && errno != EINTR) {
        if (self->enableDebug) {
          std::cerr << "[WindowFocus] poll() error on joysticks: " << strerror(errno) << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
      }

      // Periodically reinitialize
      auto now = std::chrono::steady_clock::now();
      if (now - lastDeviceReinit > deviceReinitInterval) {
        lastDeviceReinit = now;
        if (self->monitorControllers && !self->isShuttingDown) {
          initialize_joysticks(self);
        }
      }
    }

    if (self->enableDebug) {
      std::cout << "[WindowFocus] Joystick monitoring thread stopped" << std::endl;
    }
    self->threadCount--;
  }).detach();
}

// Dedicated HID device monitoring thread
static void monitor_hid_devices_thread(WindowFocusPlugin* self) {
  self->threadCount++;

  std::thread([self]() {
    if (self->enableDebug) {
      std::cout << "[WindowFocus] HID device monitoring thread started" << std::endl;
    }

    auto lastDeviceReinit = std::chrono::steady_clock::now();
    const auto deviceReinitInterval = std::chrono::seconds(30);

    while (!self->isShuttingDown) {
      std::vector<struct pollfd> pfds;
      {
        std::lock_guard<std::mutex> lock(self->deviceMutex);
        for (int fd : self->hidDeviceFds) {
          struct pollfd pfd;
          pfd.fd = fd;
          pfd.events = POLLIN;
          pfd.revents = 0;
          pfds.push_back(pfd);
        }
      }

      if (pfds.empty()) {
        std::unique_lock<std::mutex> lock(self->shutdownMutex);
        if (self->shutdownCv.wait_for(lock, std::chrono::seconds(5),
            [self] { return self->isShuttingDown.load(); })) {
          break;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastDeviceReinit > deviceReinitInterval) {
          lastDeviceReinit = now;
          if (self->monitorHIDDevices && !self->isShuttingDown) {
            initialize_hid_devices(self);
          }
        }
        continue;
      }

      int ret = poll(pfds.data(), pfds.size(), 200);

      if (self->isShuttingDown) break;

      if (ret > 0) {
        std::lock_guard<std::mutex> lock(self->deviceMutex);
        bool inputDetected = false;

        for (size_t i = 0; i < pfds.size() && i < self->hidDeviceFds.size(); i++) {
          if (!(pfds[i].revents & POLLIN)) continue;

          int fd = self->hidDeviceFds[i];
          struct input_event event;

          while (true) {
            ssize_t bytes = read(fd, &event, sizeof(event));
            if (bytes != sizeof(event)) {
              if (bytes == -1 && errno != EAGAIN && self->enableDebug) {
                std::cerr << "[WindowFocus] Error reading HID device " << i
                          << ": " << strerror(errno) << std::endl;
              }
              break;
            }

            if (event.type != EV_SYN && event.type != EV_MSC) {
              if (self->enableDebug) {
                std::cout << "[WindowFocus] HID device " << i
                          << " event: type=" << event.type
                          << " code=" << event.code
                          << " value=" << event.value << std::endl;
              }
              inputDetected = true;
            }
          }
        }

        if (inputDetected) {
          report_activity(self, InputSource::HIDDevice);
        }
      }

      auto now = std::chrono::steady_clock::now();
      if (now - lastDeviceReinit > deviceReinitInterval) {
        lastDeviceReinit = now;
        if (self->monitorHIDDevices && !self->isShuttingDown) {
          initialize_hid_devices(self);
        }
      }
    }

    if (self->enableDebug) {
      std::cout << "[WindowFocus] HID device monitoring thread stopped" << std::endl;
    }
    self->threadCount--;
  }).detach();
}

// Replace check_system_audio entirely with this version:
static bool check_system_audio(WindowFocusPlugin* self) {
#ifdef HAVE_PULSEAUDIO
  if (!self->monitorSystemAudio || self->isShuttingDown) {
    return false;
  }

  // Don't hold the mutex during the potentially long pa_simple_new
  // Instead, check and create outside, then swap in
  pa_simple* stream = nullptr;

  {
    std::lock_guard<std::mutex> lock(self->audioMutex);
    stream = self->systemAudioStream;
  }

  if (!stream) {
    if (self->enableDebug) {
      std::cout << "[WindowFocus] Initializing system audio stream..." << std::endl;
    }

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_FLOAT32LE;
    ss.channels = 2;
    ss.rate = 44100;

    // CRITICAL: Small fragment size for low-latency, non-blocking-ish reads
    // 256 frames * 2 channels * 4 bytes = 2048 bytes = ~5.8ms of audio
    pa_buffer_attr attr;
    attr.maxlength = (uint32_t)-1;
    attr.tlength = (uint32_t)-1;
    attr.prebuf = (uint32_t)-1;
    attr.minreq = (uint32_t)-1;
    attr.fragsize = sizeof(float) * 2 * 256;

    int error = 0;
    pa_simple* new_stream = nullptr;

    // Step 1: Discover the actual default sink name
    // On PipeWire, @DEFAULT_SINK@ might not resolve correctly for .monitor
    std::string default_sink;
    FILE* pipe = popen("pactl get-default-sink 2>/dev/null", "r");
    if (pipe) {
      char buf[512] = {0};
      if (fgets(buf, sizeof(buf), pipe)) {
        default_sink = buf;
        // Trim whitespace/newline
        while (!default_sink.empty() &&
               (default_sink.back() == '\n' ||
                default_sink.back() == '\r' ||
                default_sink.back() == ' ')) {
          default_sink.pop_back();
        }
      }
      pclose(pipe);
    }

    if (self->enableDebug) {
      std::cout << "[WindowFocus] Detected default sink: '"
                << default_sink << "'" << std::endl;
    }

    // Build list of sources to try, most specific first
    std::vector<std::string> sources_to_try;

    if (!default_sink.empty()) {
      sources_to_try.push_back(default_sink + ".monitor");
    }

    // Also discover all available monitor sources
        // Use awk with the field variable passed safely
    FILE* src_pipe = popen(
        "pactl list sources short 2>/dev/null | grep monitor | awk '{print $NF}' | head -20",
        "r");
    if (src_pipe) {
      char buf[512];
      while (fgets(buf, sizeof(buf), src_pipe)) {
        std::string src(buf);
        while (!src.empty() &&
               (src.back() == '\n' || src.back() == '\r' || src.back() == ' ')) {
          src.pop_back();
        }
        if (!src.empty()) {
          // Avoid duplicates
          bool already_listed = false;
          for (const auto& existing : sources_to_try) {
            if (existing == src) { already_listed = true; break; }
          }
          if (!already_listed) {
            sources_to_try.push_back(src);
          }
        }
      }
      pclose(src_pipe);
    }

    // Fallback entries
    sources_to_try.push_back("@DEFAULT_SINK@.monitor");
    sources_to_try.push_back("@DEFAULT_MONITOR@");

    if (self->enableDebug) {
      std::cout << "[WindowFocus] Will try " << sources_to_try.size()
                << " audio sources:" << std::endl;
      for (const auto& s : sources_to_try) {
        std::cout << "[WindowFocus]   - " << s << std::endl;
      }
    }

    for (const auto& source : sources_to_try) {
      if (self->isShuttingDown) return false;

      if (self->enableDebug) {
        std::cout << "[WindowFocus] Trying: " << source << std::endl;
      }

      new_stream = pa_simple_new(
          nullptr,
          "WindowFocusMonitor",
          PA_STREAM_RECORD,
          source.c_str(),
          "System Audio Monitor",
          &ss,
          nullptr,
          &attr,
          &error);

      if (new_stream) {
        if (self->enableDebug) {
          std::cout << "[WindowFocus] ✓ Connected to: " << source << std::endl;
        }
        break;
      }

      if (self->enableDebug) {
        std::cerr << "[WindowFocus] ✗ Failed '" << source
                  << "': " << pa_strerror(error) << std::endl;
      }
    }

    if (!new_stream) {
      std::cerr << "[WindowFocus] FAILED: Could not connect to any audio monitor source!"
                << std::endl;
      std::cerr << "[WindowFocus] Run: pactl list sources short" << std::endl;
      return false;
    }

    {
      std::lock_guard<std::mutex> lock(self->audioMutex);
      self->systemAudioStream = new_stream;
      stream = new_stream;
    }

    if (self->enableDebug) {
      std::cout << "[WindowFocus] Audio threshold: " << self->audioThreshold << std::endl;
      std::cout << "[WindowFocus] Audio monitoring active!" << std::endl;
    }
  }

  // Read a small chunk — this blocks for ~5.8ms which is acceptable
  const size_t num_frames = 256;
  const size_t num_samples = num_frames * 2;  // stereo
  float buffer[num_samples];
  int error;

  if (pa_simple_read(stream, buffer, sizeof(buffer), &error) < 0) {
    std::cerr << "[WindowFocus] Audio read error: " << pa_strerror(error) << std::endl;

    std::lock_guard<std::mutex> lock(self->audioMutex);
    if (self->systemAudioStream) {
      pa_simple_free(self->systemAudioStream);
      self->systemAudioStream = nullptr;
    }
    return false;
  }

  // Calculate peak AND RMS
  float peak = 0.0f;
  float sum_sq = 0.0f;

  for (size_t i = 0; i < num_samples; i++) {
    float v = std::abs(buffer[i]);
    if (v > peak) peak = v;
    sum_sq += buffer[i] * buffer[i];
  }

  float rms = std::sqrt(sum_sq / (float)num_samples);

  // ALWAYS log periodically in debug mode so we know the thread is alive
  if (self->enableDebug) {
    static auto last_audio_log = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_audio_log).count();

    // Log every 1 second OR when activity detected
    if (ms >= 1000 || peak > self->audioThreshold) {
      last_audio_log = now;
      std::cout << "[WindowFocus] 🔊 Audio peak=" << peak
                << " rms=" << rms
                << " threshold=" << self->audioThreshold
                << (peak > self->audioThreshold ? " *** DETECTED ***" : " (silent)")
                << std::endl;
    }
  }

  return peak > self->audioThreshold;

#else
  static bool warned = false;
  if (self->monitorSystemAudio && !warned) {
    std::cerr << "[WindowFocus] PulseAudio not compiled in! "
              << "Rebuild with -DHAVE_PULSEAUDIO" << std::endl;
    warned = true;
  }
  return false;
#endif
}


// Replace monitor_system_audio_thread entirely:
static void monitor_system_audio_thread(WindowFocusPlugin* self) {
  self->threadCount++;

  std::thread([self]() {
    if (self->enableDebug) {
      std::cout << "[WindowFocus] System audio thread STARTED (thread ID: "
                << std::this_thread::get_id() << ")" << std::endl;
    }

    // Small delay to let the rest of initialization finish
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    int consecutive_detections = 0;
    int read_count = 0;

    while (!self->isShuttingDown) {
      if (!self->monitorSystemAudio) {
        std::unique_lock<std::mutex> lock(self->shutdownMutex);
        self->shutdownCv.wait_for(lock, std::chrono::milliseconds(500),
            [self] { return self->isShuttingDown.load(); });
        continue;
      }

      bool detected = check_system_audio(self);
      read_count++;

      if (self->isShuttingDown) break;

      if (detected) {
        consecutive_detections++;
        // Require 2+ consecutive detections to avoid false positives
        if (consecutive_detections >= 2) {
          report_activity(self, InputSource::SystemAudio);
        }
      } else {
        consecutive_detections = 0;
      }

      // Log thread health periodically
      if (self->enableDebug && read_count % 500 == 0) {
        std::cout << "[WindowFocus] Audio thread alive - "
                  << read_count << " reads completed" << std::endl;
      }

      // pa_simple_read already blocks for ~5.8ms per read,
      // so no additional sleep needed for responsive detection.
      // But check shutdown between reads:
      if (self->isShuttingDown) break;
    }

    // Cleanup
#ifdef HAVE_PULSEAUDIO
    {
      std::lock_guard<std::mutex> lock(self->audioMutex);
      if (self->systemAudioStream) {
        pa_simple_free(self->systemAudioStream);
        self->systemAudioStream = nullptr;
      }
    }
#endif

    if (self->enableDebug) {
      std::cout << "[WindowFocus] System audio thread STOPPED ("
                << read_count << " total reads)" << std::endl;
    }
    self->threadCount--;
  }).detach();
}

// Check for inactivity
static void check_for_inactivity(WindowFocusPlugin* self) {
  self->threadCount++;

  std::thread([self]() {
    if (self->enableDebug) {
      std::cout << "[WindowFocus] Inactivity monitoring thread started" << std::endl;
    }

    while (!self->isShuttingDown) {
      {
        std::unique_lock<std::mutex> lock(self->shutdownMutex);
        if (self->shutdownCv.wait_for(lock, std::chrono::seconds(1),
            [self] { return self->isShuttingDown.load(); })) {
          break;
        }
      }

      if (self->isShuttingDown) break;

      auto now = std::chrono::steady_clock::now();
      int64_t duration;

      {
        std::lock_guard<std::mutex> lock(self->activityMutex);
        duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - self->lastActivityTime).count();
      }

      if (duration > self->inactivityThreshold && self->userIsActive) {
        self->userIsActive = false;
        if (self->enableDebug) {
          std::cout << "[WindowFocus] User is inactive. Duration: " << duration
                    << "ms, Threshold: " << self->inactivityThreshold << "ms" << std::endl;
        }
        safe_invoke_method(self, "onUserInactivity", "User is inactive");
      }
    }

    if (self->enableDebug) {
      std::cout << "[WindowFocus] Inactivity monitoring thread stopped" << std::endl;
    }
    self->threadCount--;
  }).detach();
}

// Focus listener — uses its OWN Display connection for thread safety
static void start_focus_listener(WindowFocusPlugin* self) {
  self->threadCount++;

  std::thread([self]() {
    if (self->enableDebug) {
      std::cout << "[WindowFocus] Focus listener thread started" << std::endl;
    }

    Display* threadDisplay = XOpenDisplay(nullptr);
    if (!threadDisplay) {
      std::cerr << "[WindowFocus] Focus listener: failed to open X11 display" << std::endl;
      self->threadCount--;
      return;
    }

    Window last_focused = None;
    std::string last_app_name;
    std::string last_title;

    // Send initial focus state
    Window focused_window;
    int revert_to;
    XGetInputFocus(threadDisplay, &focused_window, &revert_to);
    
    if (focused_window != None && focused_window != PointerRoot) {
      FlValue* window_info = get_focused_window_info(threadDisplay);
      safe_invoke_method_with_map(self, "onFocusChange", window_info);
      
      // Store initial state
      FlValue* app_val = fl_value_lookup_string(window_info, "appName");
      FlValue* title_val = fl_value_lookup_string(window_info, "title");
      if (app_val) last_app_name = fl_value_get_string(app_val);
      if (title_val) last_title = fl_value_get_string(title_val);
      last_focused = focused_window;
      
      if (self->enableDebug) {
        std::cout << "[WindowFocus] Initial focus: " << last_app_name 
                  << " - " << last_title << std::endl;
      }
      
      fl_value_unref(window_info);
    }

    while (!self->isShuttingDown) {
      try {
        XGetInputFocus(threadDisplay, &focused_window, &revert_to);

        if (focused_window != None && focused_window != PointerRoot) {
          // Get current window info
          FlValue* window_info = get_focused_window_info(threadDisplay);
          
          FlValue* app_val = fl_value_lookup_string(window_info, "appName");
          FlValue* title_val = fl_value_lookup_string(window_info, "title");
          
          std::string current_app = app_val ? fl_value_get_string(app_val) : "";
          std::string current_title = title_val ? fl_value_get_string(title_val) : "";
          
          // Detect change: window ID changed OR app/title changed
          bool changed = (focused_window != last_focused) || 
                        (current_app != last_app_name) ||
                        (current_title != last_title);
          
          if (changed) {
            last_focused = focused_window;
            last_app_name = current_app;
            last_title = current_title;

            if (self->enableDebug) {
              std::cout << "[WindowFocus] Focus changed to: " << current_app 
                        << " - " << current_title 
                        << " (window ID: " << focused_window << ")" << std::endl;
            }

            safe_invoke_method_with_map(self, "onFocusChange", window_info);
          } else {
            fl_value_unref(window_info);
          }
        }
      } catch (...) {
        if (self->enableDebug) {
          std::cerr << "[WindowFocus] Exception in focus listener" << std::endl;
        }
      }

      {
        std::unique_lock<std::mutex> lock(self->shutdownMutex);
        if (self->shutdownCv.wait_for(lock, std::chrono::milliseconds(250),
            [self] { return self->isShuttingDown.load(); })) {
          break;
        }
      }
    }

    XCloseDisplay(threadDisplay);

    if (self->enableDebug) {
      std::cout << "[WindowFocus] Focus listener thread stopped" << std::endl;
    }
    self->threadCount--;
  }).detach();
}

// X11 keyboard/mouse monitoring thread — uses its OWN Display connection
static void monitor_x11_events(WindowFocusPlugin* self) {
  self->threadCount++;

  std::thread([self]() {
    if (self->enableDebug) {
      std::cout << "[WindowFocus] X11 input monitor thread started" << std::endl;
    }

    Display* threadDisplay = XOpenDisplay(nullptr);
    if (!threadDisplay) {
      std::cerr << "[WindowFocus] X11 input monitor: failed to open X11 display" << std::endl;
      self->threadCount--;
      return;
    }

    // Initialize keyboard state tracking
    char prevKeys[32] = {0};
    XQueryKeymap(threadDisplay, prevKeys);

    int prevMouseX = 0, prevMouseY = 0;
    {
      Window root_ret, child;
      int root_x, root_y, win_x, win_y;
      unsigned int mask;
      if (XQueryPointer(threadDisplay, DefaultRootWindow(threadDisplay),
                        &root_ret, &child, &root_x, &root_y, &win_x, &win_y, &mask)) {
        prevMouseX = root_x;
        prevMouseY = root_y;
      }
    }

    while (!self->isShuttingDown) {
      bool activityDetected = false;
      InputSource activitySource = InputSource::Keyboard;

      // Check keyboard
      if (self->monitorKeyboard) {
        char keys[32];
        XQueryKeymap(threadDisplay, keys);

        // Detect ANY state change (press OR release)
        bool keyStateChanged = false;
        for (int i = 0; i < 32; i++) {
          if (keys[i] != prevKeys[i]) {
            keyStateChanged = true;
            
            if (self->enableDebug) {
              // Show which byte changed for debugging
              std::cout << "[WindowFocus] Keyboard state changed at byte " << i 
                        << ": 0x" << std::hex << (int)(unsigned char)prevKeys[i] 
                        << " -> 0x" << (int)(unsigned char)keys[i] 
                        << std::dec << std::endl;
            }
            break;
          }
        }

        if (keyStateChanged) {
          activityDetected = true;
          activitySource = InputSource::Keyboard;

          if (self->enableDebug) {
            std::cout << "[WindowFocus] Keyboard input detected" << std::endl;
          }
        }

        memcpy(prevKeys, keys, sizeof(prevKeys));
      }

      // Check mouse
      if (self->monitorMouse) {
        Window root_ret, child;
        int root_x, root_y, win_x, win_y;
        unsigned int mask;

        if (XQueryPointer(threadDisplay, DefaultRootWindow(threadDisplay),
                          &root_ret, &child, &root_x, &root_y, &win_x, &win_y, &mask)) {

          // Check movement
          if (root_x != prevMouseX || root_y != prevMouseY) {
            prevMouseX = root_x;
            prevMouseY = root_y;
            activityDetected = true;
            activitySource = InputSource::Mouse;

            if (self->enableDebug) {
              std::cout << "[WindowFocus] Mouse movement detected: "
                        << root_x << "," << root_y << std::endl;
            }
          }

          // Check button press
          if (mask & (Button1Mask | Button2Mask | Button3Mask | Button4Mask | Button5Mask)) {
            activityDetected = true;
            activitySource = InputSource::Mouse;

            if (self->enableDebug) {
              std::cout << "[WindowFocus] Mouse button detected, mask=0x"
                        << std::hex << mask << std::dec << std::endl;
            }
          }
        }
      }

      if (activityDetected) {
        report_activity(self, activitySource);
      }

      // Sleep 50ms for responsive detection
      {
        std::unique_lock<std::mutex> lock(self->shutdownMutex);
        if (self->shutdownCv.wait_for(lock, std::chrono::milliseconds(50),
            [self] { return self->isShuttingDown.load(); })) {
          break;
        }
      }
    }

    XCloseDisplay(threadDisplay);

    if (self->enableDebug) {
      std::cout << "[WindowFocus] X11 input monitor thread stopped" << std::endl;
    }
    self->threadCount--;
  }).detach();
}

// Take screenshot — called from main thread, uses its own display connection
static std::optional<std::vector<uint8_t>> take_screenshot(WindowFocusPlugin* self, bool activeWindowOnly) {
  // Open a dedicated display connection for the screenshot
  Display* screenshotDisplay = XOpenDisplay(nullptr);
  if (!screenshotDisplay) {
    if (self->enableDebug) {
      std::cerr << "[WindowFocus] Screenshot: failed to open X11 display" << std::endl;
    }
    return std::nullopt;
  }

  Window window;

  if (activeWindowOnly) {
    int revert_to;
    XGetInputFocus(screenshotDisplay, &window, &revert_to);
    if (window == None || window == PointerRoot) {
      window = DefaultRootWindow(screenshotDisplay);
    }
  } else {
    window = DefaultRootWindow(screenshotDisplay);
  }

  XWindowAttributes attrs;
  if (!XGetWindowAttributes(screenshotDisplay, window, &attrs)) {
    XCloseDisplay(screenshotDisplay);
    return std::nullopt;
  }

  int width = attrs.width;
  int height = attrs.height;

  if (width <= 0 || height <= 0) {
    XCloseDisplay(screenshotDisplay);
    return std::nullopt;
  }

  // Cap screenshot size to prevent excessive memory usage
  if (width > 7680 || height > 4320) {
    if (self->enableDebug) {
      std::cerr << "[WindowFocus] Screenshot: dimensions too large ("
                << width << "x" << height << ")" << std::endl;
    }
    XCloseDisplay(screenshotDisplay);
    return std::nullopt;
  }

  XImage* image = XGetImage(screenshotDisplay, window, 0, 0, width, height, AllPlanes, ZPixmap);
  if (!image) {
    XCloseDisplay(screenshotDisplay);
    return std::nullopt;
  }

  GdkPixbuf* pixbuf = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, width, height);
  if (!pixbuf) {
    XDestroyImage(image);
    XCloseDisplay(screenshotDisplay);
    return std::nullopt;
  }

  guchar* pixels = gdk_pixbuf_get_pixels(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(pixbuf);

  for (int y = 0; y < height; y++) {
    for (int x = 0; x < width; x++) {
      unsigned long pixel = XGetPixel(image, x, y);

      guchar r = (pixel >> 16) & 0xFF;
      guchar g = (pixel >> 8) & 0xFF;
      guchar b = pixel & 0xFF;

      guchar* p = pixels + y * rowstride + x * 3;
      p[0] = r;
      p[1] = g;
      p[2] = b;
    }
  }

  XDestroyImage(image);
  XCloseDisplay(screenshotDisplay);

  gchar* buffer;
  gsize buffer_size;
  GError* error = nullptr;

  if (!gdk_pixbuf_save_to_buffer(pixbuf, &buffer, &buffer_size, "png", &error, NULL)) {
    if (error) {
      if (self->enableDebug) {
        std::cerr << "[WindowFocus] Failed to save screenshot: " << error->message << std::endl;
      }
      g_error_free(error);
    }
    g_object_unref(pixbuf);
    return std::nullopt;
  }

  g_object_unref(pixbuf);

  std::vector<uint8_t> result(buffer, buffer + buffer_size);
  g_free(buffer);

  return result;
}

// Start all monitoring threads
static void start_monitoring_threads(WindowFocusPlugin* self) {
  // Initialize input devices
  if (self->monitorControllers) {
    initialize_joysticks(self);
  }

  if (self->monitorHIDDevices) {
    initialize_hid_devices(self);
  }

  // Start dedicated threads for each subsystem
  monitor_x11_events(self);       // Keyboard + Mouse via X11
  start_focus_listener(self);     // Window focus changes
  check_for_inactivity(self);     // Inactivity detection

  if (self->monitorControllers) {
    monitor_joystick_devices(self); // Joystick monitoring
  }

  if (self->monitorHIDDevices) {
    monitor_hid_devices_thread(self); // HID device monitoring
  }

  if (self->monitorSystemAudio) {
    monitor_system_audio_thread(self);     // System audio monitoring
  }
}

// Stop all monitoring threads
static void stop_monitoring_threads(WindowFocusPlugin* self) {
  self->isShuttingDown = true;

  {
    std::lock_guard<std::mutex> lock(self->shutdownMutex);
    self->shutdownCv.notify_all();
  }

  auto waitStart = std::chrono::steady_clock::now();
  while (self->threadCount > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto elapsed = std::chrono::steady_clock::now() - waitStart;
    if (std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 3000) {
      if (self->enableDebug) {
        std::cerr << "[WindowFocus] Timeout waiting for threads. Remaining: "
                  << self->threadCount.load() << std::endl;
      }
      break;
    }
  }

  close_input_devices(self);

#ifdef HAVE_PULSEAUDIO
  {
    std::lock_guard<std::mutex> lock(self->audioMutex);
    if (self->systemAudioStream) {
      pa_simple_free(self->systemAudioStream);
      self->systemAudioStream = nullptr;
    }
  }
#endif
}

// Handle method calls
static void window_focus_plugin_handle_method_call(
    WindowFocusPlugin* self,
    FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;
  const gchar* method = fl_method_call_get_name(method_call);
  FlValue* args = fl_method_call_get_args(method_call);

  if (strcmp(method, "setDebugMode") == 0) {
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* debug_val = fl_value_lookup_string(args, "debug");
      if (debug_val && fl_value_get_type(debug_val) == FL_VALUE_TYPE_BOOL) {
        self->enableDebug = fl_value_get_bool(debug_val);
        std::cout << "[WindowFocus] Debug mode set to " << (self->enableDebug ? "true" : "false") << std::endl;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected a bool for 'debug'", nullptr));
      }
    }
  } else if (strcmp(method, "setKeyboardMonitoring") == 0) {
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* enabled_val = fl_value_lookup_string(args, "enabled");
      if (enabled_val && fl_value_get_type(enabled_val) == FL_VALUE_TYPE_BOOL) {
        self->monitorKeyboard = fl_value_get_bool(enabled_val);
        std::cout << "[WindowFocus] Keyboard monitoring set to " << (self->monitorKeyboard ? "true" : "false") << std::endl;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected a bool for 'enabled'", nullptr));
      }
    }
  } else if (strcmp(method, "setMouseMonitoring") == 0) {
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* enabled_val = fl_value_lookup_string(args, "enabled");
      if (enabled_val && fl_value_get_type(enabled_val) == FL_VALUE_TYPE_BOOL) {
        self->monitorMouse = fl_value_get_bool(enabled_val);
        std::cout << "[WindowFocus] Mouse monitoring set to " << (self->monitorMouse ? "true" : "false") << std::endl;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected a bool for 'enabled'", nullptr));
      }
    }
  } else if (strcmp(method, "setControllerMonitoring") == 0) {
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* enabled_val = fl_value_lookup_string(args, "enabled");
      if (enabled_val && fl_value_get_type(enabled_val) == FL_VALUE_TYPE_BOOL) {
        bool newValue = fl_value_get_bool(enabled_val);
        if (newValue && !self->monitorControllers) {
          initialize_joysticks(self);
          monitor_joystick_devices(self);
        } else if (!newValue && self->monitorControllers) {
          std::lock_guard<std::mutex> lock(self->deviceMutex);
          for (int fd : self->joystickFds) close(fd);
          self->joystickFds.clear();
          self->joystickEventNumbers.clear();
        }
        self->monitorControllers = newValue;
        std::cout << "[WindowFocus] Controller monitoring set to " << (self->monitorControllers ? "true" : "false") << std::endl;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected a bool for 'enabled'", nullptr));
      }
    }
  } else if (strcmp(method, "setAudioMonitoring") == 0) {
    // Note: Flutter side calls this "setAudioMonitoring" but we're monitoring system audio output
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* enabled_val = fl_value_lookup_string(args, "enabled");
      if (enabled_val && fl_value_get_type(enabled_val) == FL_VALUE_TYPE_BOOL) {
        bool newValue = fl_value_get_bool(enabled_val);
        if (newValue && !self->monitorSystemAudio) {
          self->monitorSystemAudio = true;
          monitor_system_audio_thread(self);
        } else {
          self->monitorSystemAudio = newValue;
        }
        std::cout << "[WindowFocus] System audio monitoring set to " << (self->monitorSystemAudio ? "true" : "false") << std::endl;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected a bool for 'enabled'", nullptr));
      }
    }
  } else if (strcmp(method, "setAudioThreshold") == 0) {
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* threshold_val = fl_value_lookup_string(args, "threshold");
      if (threshold_val && fl_value_get_type(threshold_val) == FL_VALUE_TYPE_FLOAT) {
        self->audioThreshold = fl_value_get_float(threshold_val);
        std::cout << "[WindowFocus] Audio threshold set to " << self->audioThreshold << std::endl;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected a double for 'threshold'", nullptr));
      }
    }
  } else if (strcmp(method, "setHIDMonitoring") == 0) {
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* enabled_val = fl_value_lookup_string(args, "enabled");
      if (enabled_val && fl_value_get_type(enabled_val) == FL_VALUE_TYPE_BOOL) {
        bool newValue = fl_value_get_bool(enabled_val);
        if (newValue && !self->monitorHIDDevices) {
          initialize_hid_devices(self);
          monitor_hid_devices_thread(self);
        } else if (!newValue && self->monitorHIDDevices) {
          std::lock_guard<std::mutex> lock(self->deviceMutex);
          for (int fd : self->hidDeviceFds) close(fd);
          self->hidDeviceFds.clear();
        }
        self->monitorHIDDevices = newValue;
        std::cout << "[WindowFocus] HID device monitoring set to " << (self->monitorHIDDevices ? "true" : "false") << std::endl;
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected a bool for 'enabled'", nullptr));
      }
    }
  } else if (strcmp(method, "setInactivityTimeOut") == 0) {
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* timeout_val = fl_value_lookup_string(args, "inactivityTimeOut");
      if (timeout_val && fl_value_get_type(timeout_val) == FL_VALUE_TYPE_INT) {
        self->inactivityThreshold = fl_value_get_int(timeout_val);
        std::cout << "[WindowFocus] Inactivity threshold set to " << self->inactivityThreshold << std::endl;
        g_autoptr(FlValue) result = fl_value_new_int(self->inactivityThreshold);
        response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
      } else {
        response = FL_METHOD_RESPONSE(fl_method_error_response_new("Invalid argument", "Expected an integer for 'inactivityTimeOut'", nullptr));
      }
    }
  } else if (strcmp(method, "getIdleThreshold") == 0) {
    g_autoptr(FlValue) result = fl_value_new_int(self->inactivityThreshold);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else if (strcmp(method, "takeScreenshot") == 0) {
    bool activeWindowOnly = false;
    if (fl_value_get_type(args) == FL_VALUE_TYPE_MAP) {
      FlValue* active_val = fl_value_lookup_string(args, "activeWindowOnly");
      if (active_val && fl_value_get_type(active_val) == FL_VALUE_TYPE_BOOL) {
        activeWindowOnly = fl_value_get_bool(active_val);
      }
    }

    auto screenshot = take_screenshot(self, activeWindowOnly);
    if (screenshot.has_value()) {
      g_autoptr(FlValue) result = fl_value_new_uint8_list(screenshot->data(), screenshot->size());
      response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
    } else {
      response = FL_METHOD_RESPONSE(fl_method_error_response_new("SCREENSHOT_ERROR", "Failed to take screenshot", nullptr));
    }
  } else if (strcmp(method, "checkScreenRecordingPermission") == 0) {
    g_autoptr(FlValue) result = fl_value_new_bool(TRUE);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(result));
  } else if (strcmp(method, "requestScreenRecordingPermission") == 0) {
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (strcmp(method, "getPlatformVersion") == 0) {
    response = get_platform_version();
  } else if (strcmp(method, "getMonitoringStatus") == 0) {
    // Diagnostic method to check what's actually being monitored
    FlValue* status = fl_value_new_map();
    fl_value_set_string_take(status, "keyboard", fl_value_new_bool(self->monitorKeyboard));
    fl_value_set_string_take(status, "mouse", fl_value_new_bool(self->monitorMouse));
    fl_value_set_string_take(status, "controllers", fl_value_new_bool(self->monitorControllers));
    fl_value_set_string_take(status, "systemAudio", fl_value_new_bool(self->monitorSystemAudio));
    fl_value_set_string_take(status, "hidDevices", fl_value_new_bool(self->monitorHIDDevices));
    fl_value_set_string_take(status, "userIsActive", fl_value_new_bool(self->userIsActive.load()));
    fl_value_set_string_take(status, "threadCount", fl_value_new_int(self->threadCount.load()));
    fl_value_set_string_take(status, "inactivityThreshold", fl_value_new_int(self->inactivityThreshold));

    {
      std::lock_guard<std::mutex> lock(self->deviceMutex);
      fl_value_set_string_take(status, "joystickCount", fl_value_new_int(self->joystickFds.size()));
      fl_value_set_string_take(status, "hidDeviceCount", fl_value_new_int(self->hidDeviceFds.size()));
    }

    response = FL_METHOD_RESPONSE(fl_method_success_response_new(status));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }

  if (!response) {
    response = FL_METHOD_RESPONSE(fl_method_error_response_new(
        "INVALID_ARGS", "Invalid or missing arguments", nullptr));
  }

  fl_method_call_respond(method_call, response, nullptr);
}

FlMethodResponse* get_platform_version() {
  struct utsname uname_data = {};
  uname(&uname_data);
  g_autofree gchar *version = g_strdup_printf("Linux %s", uname_data.version);
  g_autoptr(FlValue) result = fl_value_new_string(version);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

static void window_focus_plugin_dispose(GObject* object) {
  WindowFocusPlugin* self = WINDOW_FOCUS_PLUGIN(object);

  if (self->enableDebug) {
    std::cout << "[WindowFocus] Plugin disposing..." << std::endl;
  }

  // Stop all monitoring
  stop_monitoring_threads(self);

  // Clear channel — no longer owned by us after dispose
  {
    std::lock_guard<std::mutex> lock(self->channelMutex);
    if (self->channel) {
      g_object_unref(self->channel);
      self->channel = nullptr;
    }
  }

  if (self->enableDebug) {
    std::cout << "[WindowFocus] Plugin disposed" << std::endl;
  }

  G_OBJECT_CLASS(window_focus_plugin_parent_class)->dispose(object);
}

static void window_focus_plugin_class_init(WindowFocusPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = window_focus_plugin_dispose;
}

static void window_focus_plugin_init(WindowFocusPlugin* self) {
  // Initialize default values
  self->enableDebug = false;
  self->monitorKeyboard = true;
  self->monitorMouse = true;
  self->monitorControllers = true;
  self->monitorSystemAudio = false;
  self->monitorHIDDevices = false;
  self->inactivityThreshold = 60000; // 60 seconds
  self->audioThreshold = 0.01f;

  self->userIsActive = true;
  self->isShuttingDown = false;
  self->threadCount = 0;

  self->lastActivityTime = std::chrono::steady_clock::now();

  self->lastMouseX = 0;
  self->lastMouseY = 0;

  memset(self->lastKeyState, 0, sizeof(self->lastKeyState));

#ifdef HAVE_PULSEAUDIO
  self->systemAudioStream = nullptr;
#endif
}

static void method_call_cb(FlMethodChannel* channel, FlMethodCall* method_call,
                          gpointer user_data) {
  WindowFocusPlugin* plugin = WINDOW_FOCUS_PLUGIN(user_data);
  window_focus_plugin_handle_method_call(plugin, method_call);
}

void window_focus_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  WindowFocusPlugin* plugin = WINDOW_FOCUS_PLUGIN(
      g_object_new(window_focus_plugin_get_type(), nullptr));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  plugin->channel = fl_method_channel_new(
      fl_plugin_registrar_get_messenger(registrar),
      "expert.kotelnikoff/window_focus",
      FL_METHOD_CODEC(codec));

  fl_method_channel_set_method_call_handler(
      plugin->channel, method_call_cb,
      g_object_ref(plugin), g_object_unref);

  // Start monitoring threads
  start_monitoring_threads(plugin);

  if (plugin->enableDebug) {
    std::cout << "[WindowFocus] Plugin registered and monitoring started" << std::endl;
  }

  g_object_unref(plugin);
}