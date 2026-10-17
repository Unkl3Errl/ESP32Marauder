#!/usr/bin/env python3
"""Run production Remote ID GPX publication with fault-injected host storage."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'esp32_marauder/WiFiScanRemoteId.h').read_text()
start = source[source.index('void WiFiScan::startRemoteIdGpx()'):source.index('void WiFiScan::logRemoteIdGpxPositions(')]
finish = source[source.index('void WiFiScan::finishRemoteIdGpx()'):source.index('\n#endif', source.index('void WiFiScan::finishRemoteIdGpx()'))]

harness = r'''
#include <cassert>
#include <cstring>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <cstdio>

class String : public std::string {
 public:
  using std::string::string;
  String(const std::string& value) : std::string(value) {}
  String(unsigned value) : std::string(std::to_string(value)) {}
  String substring(size_t first, size_t last) const { return substr(first, last - first); }
};
String operator+(const char* a, const String& b) { return String(std::string(a) + b); }
String operator+(const String& a, const char* b) { return String(static_cast<const std::string&>(a) + b); }
String operator+(const String& a, const String& b) { return String(static_cast<const std::string&>(a) + static_cast<const std::string&>(b)); }

constexpr int FILE_WRITE = 1;
constexpr int FILE_APPEND = 2;
constexpr int FILE_READ = 3;
constexpr size_t REMOTE_ID_CAPACITY = 4;
static std::map<std::string, std::string> files;
static bool fail_writes;
static bool fail_reads;
static bool fail_rename;
struct Handle { std::string path; size_t cursor; bool error = false; };
class File {
  std::shared_ptr<Handle> handle;
 public:
  File() = default;
  File(const std::string& path, size_t cursor) : handle(std::make_shared<Handle>(Handle{path, cursor, false})) {}
  explicit operator bool() const { return bool(handle); }
  size_t write(const uint8_t* data, size_t count) {
    if (!handle) return 0;
    if (fail_writes) { handle->error = true; return 0; }
    files[handle->path].append(reinterpret_cast<const char*>(data), count);
    handle->cursor += count;
    return count;
  }
  size_t print(const std::string& value) { return write(reinterpret_cast<const uint8_t*>(value.data()), value.size()); }
  size_t available() const { return handle ? files[handle->path].size() - handle->cursor : 0; }
  size_t read(uint8_t* data, size_t count) {
    if (!handle || fail_reads) return 0;
    if (count > available()) count = available();
    memcpy(data, files[handle->path].data() + handle->cursor, count);
    handle->cursor += count;
    return count;
  }
  void flush() {}
  void close() { handle.reset(); }
  int getWriteError() const { return handle && handle->error; }
};
struct Storage {
  bool exists(const std::string& path) const { return files.count(path); }
  File open(const std::string& path, int mode) {
    if (mode == FILE_READ && !exists(path)) return File();
    if (mode == FILE_WRITE) files[path] = "";
    return File(path, mode == FILE_APPEND ? files[path].size() : 0);
  }
  bool rename(const std::string& from, const std::string& to) {
    if (fail_rename || !exists(from) || exists(to)) return false;
    files[to] = files[from]; files.erase(from); return true;
  }
  bool remove(const std::string& path) { return files.erase(path); }
} MARAUDER_STORAGE;
struct Settings { template <typename T> T loadSetting(const char*) { return true; } } settings_obj;
struct Sd { bool supported = true; } sd_obj;
struct Console { template <typename T> void println(const T&) {} } Serial;
#define F(value) value
String remoteIdXmlEscape(const String& value) { return value; }
struct RemoteIdGpxTrack { bool used; unsigned pointCount; char name[64]; };
class WiFiScan {
 public:
  bool remote_id_gpx_active = false;
  bool remote_id_gpx_write_failed = false;
  String remote_id_gpx_file_name;
  uint16_t remote_id_gpx_session_index = 0;
  RemoteIdGpxTrack remote_id_gpx_tracks[REMOTE_ID_CAPACITY] = {};
  void startRemoteIdGpx();
  void finishRemoteIdGpx();
};
'''

cases = r'''
static WiFiScan prepare() {
  files.clear(); fail_writes = false; fail_reads = false; fail_rename = false;
  WiFiScan scan;
  scan.startRemoteIdGpx();
  assert(scan.remote_id_gpx_active);
  assert(files.count("/remoteid_0.gpx.part") == 1);
  assert(files.count("/remoteid_0.gpx") == 0);
  scan.remote_id_gpx_tracks[0].used = true;
  scan.remote_id_gpx_tracks[0].pointCount = 1;
  strcpy(scan.remote_id_gpx_tracks[0].name, "test-route");
  files["/remoteid_0_0.tmp"] = "      <trkpt lat=\"1\" lon=\"2\"></trkpt>\n";
  return scan;
}
static void retained(const WiFiScan& scan) {
  assert(!scan.remote_id_gpx_active);
  assert(files.count("/remoteid_0.gpx") == 0);
  assert(files.count("/remoteid_0.gpx.part") == 1);
}
int main() {
  auto scan = prepare();
  scan.finishRemoteIdGpx();
  assert(!scan.remote_id_gpx_active);
  assert(files.count("/remoteid_0.gpx.part") == 0);
  assert(files.count("/remoteid_0_0.tmp") == 0);
  const auto& route = files.at("/remoteid_0.gpx");
  assert(route.find("<gpx ") != std::string::npos);
  assert(route.find("<trkpt ") != std::string::npos);
  assert(route.substr(route.size() - 7) == "</gpx>\n");

  scan = prepare(); fail_writes = true;
  scan.finishRemoteIdGpx(); retained(scan);
  assert(files.count("/remoteid_0_0.tmp") == 1);

  scan = prepare(); fail_reads = true;
  scan.finishRemoteIdGpx(); retained(scan);
  assert(files.count("/remoteid_0_0.tmp") == 1);

  scan = prepare(); fail_rename = true;
  scan.finishRemoteIdGpx(); retained(scan);
  assert(files.count("/remoteid_0_0.tmp") == 1);

  scan = prepare(); files.erase("/remoteid_0_0.tmp");
  scan.finishRemoteIdGpx(); retained(scan);

  scan = prepare(); scan.remote_id_gpx_write_failed = true;
  scan.finishRemoteIdGpx(); retained(scan);
  assert(files.count("/remoteid_0_0.tmp") == 1);

  scan = prepare();
  WiFiScan second;
  second.startRemoteIdGpx();
  assert(second.remote_id_gpx_file_name == "/remoteid_1.gpx.part");
  assert(files.count("/remoteid_0.gpx.part") == 1);

  files.clear(); fail_writes = true;
  WiFiScan failedStart;
  failedStart.startRemoteIdGpx();
  assert(!failedStart.remote_id_gpx_active);
  assert(files.count("/remoteid_0.gpx") == 0);
  puts("Remote ID GPX: 8 publication and storage-failure cases passed");
}
'''
with tempfile.TemporaryDirectory(prefix='marauder-remoteid-storage-') as temporary:
    work = Path(temporary)
    test = work / 'test.cpp'
    binary = work / 'test'
    test.write_text(harness + start + finish + cases)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
