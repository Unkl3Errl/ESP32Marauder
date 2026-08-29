#include "Buffer.h"
#include "PcapHeader.h"
#include "lang_var.h"
#ifdef HAS_VIRTUAL_SD
#include "FFat.h"
#endif

namespace {
constexpr size_t ANDROID_SPOOL_SEGMENT_BYTES = 128 * 1024;
}

Buffer::Buffer(){
  bufA = (uint8_t*)malloc(BUF_SIZE);
  bufB = (uint8_t*)malloc(BUF_SIZE);
}

bool Buffer::createFile(const char* name, bool is_pcap, bool is_gpx){
  if (!fs) {
    Serial.println(F("Output file error: storage is not mounted"));
    return false;
  }

  int i=0;
  if (is_pcap) {
    do{
      fileName = "/"+String(name)+"_"+(String)i+".pcap";
      i++;
    } while(fs->exists(fileName));
  }
  else if ((!is_pcap) && (!is_gpx)) {
    do{
      fileName = "/"+String(name)+"_"+(String)i+".log";
      i++;
    } while(fs->exists(fileName));
  }
  else {
    do{
      fileName = "/"+String(name)+"_"+(String)i+".gpx";
      i++;
    } while(fs->exists(fileName));
  }

  file = fs->open(fileName, FILE_WRITE);
  if (!file) {
    Serial.println("Output file error: could not create " + fileName);
    return false;
  }
  file.close();

  if (!fs->exists(fileName)) {
    Serial.println("Output file error: " + fileName + " was not created");
    return false;
  }

  Serial.println("Output file: " + fileName);
  return true;
}

bool Buffer::open(bool is_pcap){
  if (!bufA || !bufB) {
    Serial.println(F("Output file error: capture buffers are unavailable"));
    writing = false;
    return false;
  }

  bufSizeA = 0;
  bufSizeB = 0;
  useA = true;
  closePending = false;
  writing = true;

  if (is_pcap) {
    uint8_t header[marauder::kPcapGlobalHeaderSize];
    marauder::makePcapGlobalHeader(SNAP_LEN, header);
    write(header, sizeof(header));
  }

  return true;
}

String Buffer::getFileName() {
  return this->fileName;
}

bool Buffer::isActiveFile(const String& path) const {
  return (writing || closePending) && fs != NULL && path == fileName;
}

void Buffer::writePcapHeader(File& target) {
  uint8_t header[marauder::kPcapGlobalHeaderSize];
  marauder::makePcapGlobalHeader(SNAP_LEN, header);
  target.write(header, sizeof(header));
}

void Buffer::rotateFile() {
  if (!createFile(fileBaseName.c_str(), fileIsPcap, fileIsGpx)) return;
  if (!fileIsPcap) return;
  File target = fs->open(fileName, FILE_APPEND);
  if (target) {
    writePcapHeader(target);
    target.close();
  }
}

bool Buffer::openFile(const char* file_name, fs::FS* fs, bool serial, bool is_pcap, bool is_gpx) {
  if (!close()) {
    Serial.println(F("Output file error: previous output is still waiting to be flushed"));
    return false;
  }

  bool save_pcap = settings_obj.loadSetting<bool>("SavePCAP");
  if (is_pcap && !save_pcap && !serial) {
    this->fs = NULL;
    this->serial = false;
    writing = false;
    Serial.println(F("PCAP output disabled by SavePCAP setting"));
    return true;
  }

  // SavePCAP controls only PCAP files. Explicit logs and GPX tracks must not
  // silently disappear when packet capture saving is disabled. The -serial
  // option is also an explicit output request and remains available.
  this->fs = (is_pcap && !save_pcap) ? NULL : fs;
  this->serial = serial;
  this->fileBaseName = file_name;
  this->fileIsPcap = is_pcap;
  this->fileIsGpx = is_gpx;
  if (this->fs) {
    if (!createFile(file_name, is_pcap, is_gpx)) {
      this->fs = NULL;
      this->serial = false;
      writing = false;
      return false;
    }
  }
  if (this->fs || this->serial) {
    return open(is_pcap);
  } else {
    writing = false;
    Serial.println(F("Output file error: no writable storage or serial output target"));
    return false;
  }
}

bool Buffer::pcapOpen(const char* file_name, fs::FS* fs, bool serial) {
  return openFile(file_name, fs, serial, true);
}

bool Buffer::logOpen(const char* file_name, fs::FS* fs, bool serial) {
  return openFile(file_name, fs, serial, false);
}

bool Buffer::gpxOpen(const char* file_name, fs::FS* fs, bool serial) {
  return openFile(file_name, fs, serial, false, true);
}

void Buffer::add(const uint8_t* buf, uint32_t len, bool is_pcap){
  // buffer is full -> drop packet
  if((useA && bufSizeA + len >= BUF_SIZE && bufSizeB > 0) || (!useA && bufSizeB + len >= BUF_SIZE && bufSizeA > 0)){
    //Serial.print(";"); 
    return;
  }
  
  if(useA && bufSizeA + len + 16 >= BUF_SIZE && bufSizeB == 0){
    useA = false;
    //Serial.println("\nswitched to buffer B");
  }
  else if(!useA && bufSizeB + len + 16 >= BUF_SIZE && bufSizeA == 0){
    useA = true;
    //Serial.println("\nswitched to buffer A");
  }

  uint32_t microSeconds = micros(); // e.g. 45200400 => 45s 200ms 400us
  uint32_t seconds = (microSeconds/1000)/1000; // e.g. 45200400/1000/1000 = 45200 / 1000 = 45s

  microSeconds -= seconds*1000*1000; // e.g. 45200400 - 45*1000*1000 = 45200400 - 45000000 = 400us (because we only need the offset)
  
  if (is_pcap) {
    write(seconds); // ts_sec
    write(microSeconds); // ts_usec
    write(len); // incl_len
    write(len); // orig_len
  }
  
  write(buf, len); // packet payload
}

void Buffer::append(wifi_promiscuous_pkt_t *packet, int len) {
  if (writing && packet && len > 0) {
    add(packet->payload, len, true);
  }
}

void Buffer::append(String log) {
  if (writing && log.length() > 0) {
    add((const uint8_t*)log.c_str(), log.length(), false);
  }
}

void Buffer::write(int32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint32_t n){
  uint8_t buf[4];
  buf[0] = n;
  buf[1] = n >> 8;
  buf[2] = n >> 16;
  buf[3] = n >> 24;
  write(buf,4);
}

void Buffer::write(uint16_t n){
  uint8_t buf[2];
  buf[0] = n;
  buf[1] = n >> 8;
  write(buf,2);
}

void Buffer::write(const uint8_t* buf, uint32_t len){
  if(!writing) return;
  while(saving) delay(10);
  
  if(useA){
    memcpy(&bufA[bufSizeA], buf, len);
    bufSizeA += len;
  }else{
    memcpy(&bufB[bufSizeB], buf, len);
    bufSizeB += len;
  }
}

bool Buffer::saveFs(){
  const size_t pending = bufSizeA + bufSizeB;
  #ifdef HAS_VIRTUAL_SD
  constexpr size_t SPOOL_WRITE_RESERVE_BYTES = 4096;
  if (FFat.freeBytes() < pending + SPOOL_WRITE_RESERVE_BYTES) {
    Serial.println(F("Android spool backpressure; retaining capture data in RAM"));
    return false;
  }
  #endif
  size_t currentSize = 0;
  File current = fs->open(fileName, FILE_READ);
  if (current) {
    currentSize = current.size();
    current.close();
  }
  if (!fileIsGpx && currentSize > 0 &&
      currentSize + pending > ANDROID_SPOOL_SEGMENT_BYTES) {
    rotateFile();
  } else if (!fs->exists(fileName)) {
    if (!createFile(fileBaseName.c_str(), fileIsPcap, fileIsGpx)) return false;
    if (fileIsPcap) {
      File target = fs->open(fileName, FILE_APPEND);
      if (target) {
        writePcapHeader(target);
        target.close();
      }
    }
  }
  file = fs->open(fileName, FILE_APPEND);
  if (!file) {
    Serial.println(text02+fileName+"'");
    return false;
  }

  size_t written = 0;
  if(useA){
    if(bufSizeB > 0){
      written += file.write(bufB, bufSizeB);
    }
    if(bufSizeA > 0){
      written += file.write(bufA, bufSizeA);
    }
  } else {
    if(bufSizeA > 0){
      written += file.write(bufA, bufSizeA);
    }
    if(bufSizeB > 0){
      written += file.write(bufB, bufSizeB);
    }
  }

  file.close();
  if (written != pending) {
    Serial.println(F("Android spool short write; retaining capture data in RAM"));
    return false;
  }
  return true;
}

bool Buffer::saveSerial() {
  // Saves to main console UART, user-facing app will ignore these markers
  // Uses / and ] in markers as they are illegal characters for SSIDs
  const char* mark_begin = "[BUF/BEGIN]";
  const size_t mark_begin_len = strlen(mark_begin);
  const char* mark_close = "[BUF/CLOSE]";
  const size_t mark_close_len = strlen(mark_close);

  // Additional buffer and memcpy's so that a single Serial.write() is called
  // This is necessary so that other console output isn't mixed into buffer stream
  uint8_t* buf = (uint8_t*)malloc(mark_begin_len + bufSizeA + bufSizeB + mark_close_len);
  if (!buf) return false;
  uint8_t* it = buf;
  memcpy(it, mark_begin, mark_begin_len);
  it += mark_begin_len;

  if(useA){
    if(bufSizeB > 0){
      memcpy(it, bufB, bufSizeB);
      it += bufSizeB;
    }
    if(bufSizeA > 0){
      memcpy(it, bufA, bufSizeA);
      it += bufSizeA;
    }
  } else {
    if(bufSizeA > 0){
      memcpy(it, bufA, bufSizeA);
      it += bufSizeA;
    }
    if(bufSizeB > 0){
      memcpy(it, bufB, bufSizeB);
      it += bufSizeB;
    }
  }

  memcpy(it, mark_close, mark_close_len);
  it += mark_close_len;
  Serial.write(buf, it - buf);
  free(buf);
  return true;
}

bool Buffer::save() {
  saving = true;

  if((bufSizeA + bufSizeB) == 0){
    saving = false;
    if (closePending) {
      closePending = false;
      fs = NULL;
      serial = false;
    }
    return true;
  }

  bool saved = this->fs == NULL;
  if(this->fs) saved = saveFs();
  if(this->serial) {
    const bool serialSaved = saveSerial();
    if (this->fs == NULL) saved = serialSaved;
  }

  // Never claim the RAM batch was saved when its durable spool write failed.
  // The next periodic save retries it after Android releases a closed segment.
  if (!saved) {
    saving = false;
    return false;
  }

  bufSizeA = 0;
  bufSizeB = 0;

  saving = false;
  if (closePending) {
    closePending = false;
    fs = NULL;
    serial = false;
  }
  return true;
}

bool Buffer::close() {
  if (!writing && !closePending) return true;
  writing = false;
  closePending = true;
  return save();
}
