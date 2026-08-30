#include "SDInterface.h"
#include "lang_var.h"
#include "mbedtls/base64.h"

namespace {
bool storagePath(String& path) {
  path.trim();
  if (path.length() == 0 || path.length() > 240) return false;
  if (!path.startsWith("/")) path = "/" + path;
  int start = 1;
  while (start <= path.length()) {
    int end = path.indexOf('/', start);
    if (end < 0) end = path.length();
    if (path.substring(start, end) == "..") return false;
    if (end >= path.length()) break;
    start = end + 1;
  }
  return true;
}

bool storageNumber(const String& text, size_t& value) {
  if (text.length() == 0) return false;
  for (size_t i = 0; i < text.length(); ++i) {
    if (!isDigit(text[i])) return false;
  }
  value = static_cast<size_t>(strtoull(text.c_str(), nullptr, 10));
  return true;
}

bool storageUint64(const String& text, uint64_t& value) {
  if (text.length() == 0) return false;
  for (size_t i = 0; i < text.length(); ++i) {
    if (!isDigit(text[i])) return false;
  }
  value = strtoull(text.c_str(), nullptr, 10);
  return true;
}

bool storageCrc32Value(const String& text, uint32_t& value) {
  if (text.length() != 8) return false;
  uint32_t parsed = 0;
  for (size_t i = 0; i < text.length(); ++i) {
    const char ch = text[i];
    uint32_t nibble;
    if (ch >= '0' && ch <= '9') nibble = ch - '0';
    else if (ch >= 'a' && ch <= 'f') nibble = ch - 'a' + 10;
    else if (ch >= 'A' && ch <= 'F') nibble = ch - 'A' + 10;
    else return false;
    parsed = (parsed << 4) | nibble;
  }
  value = parsed;
  return true;
}

uint32_t storageCrc32Update(uint32_t crc, const uint8_t* data, size_t length) {
  while (length--) {
    crc ^= *data++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xEDB88320UL & static_cast<uint32_t>(
        -static_cast<int32_t>(crc & 1U)
      ));
    }
  }
  return crc;
}

bool storageFileCrc32(const String& path, uint64_t& size, uint32_t& checksum) {
  File file = MARAUDER_STORAGE.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  uint8_t buffer[1024];
  uint64_t total = 0;
  uint32_t crc = 0xFFFFFFFFUL;
  while (file.available()) {
    const size_t received = file.read(buffer, sizeof(buffer));
    if (received == 0) break;
    crc = storageCrc32Update(crc, buffer, received);
    total += received;
  }
  const bool complete = total == file.size();
  file.close();
  if (!complete) return false;
  size = total;
  checksum = crc ^ 0xFFFFFFFFUL;
  return true;
}

void storageError(const String& message) { Serial.println("SD:ERR:" + message); }
} // namespace

// GCOVR_EXCL_START -- requires mounted SPIFFS and SD filesystems.
namespace {
  bool removeTree(fs::FS& fs, const String& path, bool keep_root = false) {
    if (!fs.exists(path))
      return true;

    File node = fs.open(path);
    if (!node)
      return false;

    if (!node.isDirectory()) {
      node.close();
      return fs.remove(path);
    }

    File child = node.openNextFile();
    while (child) {
      String child_path = child.path();
      child.close();
      if (!removeTree(fs, child_path)) {
        node.close();
        return false;
      }
      child = node.openNextFile();
    }

    node.close();
    return keep_root || fs.rmdir(path);
  }

  String joinPath(const String& base, const String& child) {
    return base == "/" ? "/" + child : base + "/" + child;
  }

  bool copyTree(
    fs::FS& source,
    const String& source_path,
    fs::FS* destination,
    const String& destination_path,
    size_t& files_copied,
    size_t& bytes_copied,
    uint8_t& error
  ) {
    File source_node = source.open(source_path);
    if (!source_node) {
      error = 3;
      return false;
    }

    if (!source_node.isDirectory()) {
      if (destination) {
        File destination_file = destination->open(destination_path, FILE_WRITE);
        if (!destination_file) {
          source_node.close();
          error = 3;
          return false;
        }

        uint8_t buffer[512];
        while (source_node.available()) {
          size_t bytes_read = source_node.read(buffer, sizeof(buffer));
          if (bytes_read == 0 || destination_file.write(buffer, bytes_read) != bytes_read) {
            source_node.close();
            destination_file.close();
            error = 3;
            return false;
          }
          bytes_copied += bytes_read;
        }
        destination_file.close();
      }
      else
        bytes_copied += source_node.size();
      source_node.close();
      files_copied++;
      return true;
    }

    if (destination && destination_path != "/" &&
        !destination->exists(destination_path) && !destination->mkdir(destination_path)) {
      source_node.close();
      error = 3;
      return false;
    }

    File child = source_node.openNextFile();
    while (child) {
      String child_source_path = child.path();
      String child_name = child_source_path;
      if (child_name.startsWith(source_path))
        child_name.remove(0, source_path.length());
      while (child_name.startsWith("/"))
        child_name.remove(0, 1);
      child.close();

      if (!copyTree(
        source,
        child_source_path,
        destination,
        joinPath(destination_path, child_name),
        files_copied,
        bytes_copied,
        error
      )) {
        source_node.close();
        return false;
      }
      child = source_node.openNextFile();
    }

    source_node.close();
    return true;
  }

}
// GCOVR_EXCL_STOP

#ifdef HAS_C5_SD
  SDInterface::SDInterface(SPIClass* spi, int cs)
    : _spi(spi), _cs(cs) {}
#endif

bool SDInterface::initSD() {
  #ifndef HAS_SD
    return false;
  #else
    if (this->supported) return true;
    String display_string = "";
    #ifdef HAS_VIRTUAL_SD
    /* Preserve a valid spool, but format a blank or incompatible reserved
       partition inside this same mount call. Splitting this into separate
       mount/format/remount operations can strand the wear-level layer. */
    if (!FFat.begin(true, "/android", 10, "android")) {
      Serial.println(F("Failed to mount Android virtual SD"));
      this->supported = false;
      return false;
    }
    this->supported = true;
    this->cardType = 1;
    this->cardSizeMB = FFat.totalBytes() / (1024 * 1024);
    #else
    #ifdef KIT
      pinMode(SD_DET, INPUT);
      if (digitalRead(SD_DET) != LOW) {
        this->supported = false;
        return false;
      }
    #endif

    pinMode(SD_CS, OUTPUT);

    delay(10);
    #if (defined(MARAUDER_M5STICKC)) || (defined(HAS_CYD_TOUCH)) || (defined(MARAUDER_CARDPUTER)) || (defined(MARAUDER_CARDPUTER_ADV))
      /* Set up SPI SD Card using external pin header
      StickCPlus Header - SPI SD Card Reader
                  3v3   -   3v3
                  GND   -   GND
                   G0   -   CLK
              G36/G25   -   MISO
                  G26   -   MOSI
                        -   CS (jumper to SD Card GND Pin)
      */
      #if defined(MARAUDER_M5STICKC)
        enum { SPI_SCK = 0, SPI_MISO = 36, SPI_MOSI = 26 };
      #elif defined(HAS_CYD_TOUCH) || defined(MARAUDER_CARDPUTER) || defined(MARAUDER_CARDPUTER_ADV) || defined(HAS_SEPARATE_SD)
        enum { SPI_SCK = SD_SCK, SPI_MISO = SD_MISO, SPI_MOSI = SD_MOSI };
      #else
        enum { SPI_SCK = 0, SPI_MISO = 36, SPI_MOSI = 26 };
      #endif
      #if !defined(MARAUDER_CARDPUTER) && !defined(MARAUDER_CARDPUTER_ADV)
        this->spiExt = new SPIClass();
      #else
        this->spiExt = new SPIClass(FSPI);
      #endif
      Serial.println(F("Using external SPI configuration..."));
      this->spiExt->begin(SPI_SCK, SPI_MISO, SPI_MOSI, SD_CS);
      if (!SD.begin(SD_CS, *(this->spiExt))) {
    #elif defined(HAS_C5_SD)
      if (!SD.begin(SD_CS, *_spi)) {
    #else
      if (!SD.begin(SD_CS)) {
    #endif
      Serial.println(F("Failed to mount SD Card"));
      this->supported = false;
      return false;
    }
    else {
      this->supported = true;
      this->cardType = SD.cardType();
      this->cardSizeMB = SD.cardSize() / (1024 * 1024);
    }
    #endif

    if (this->supported) {
      const int NUM_DIGITS = this->cardSizeMB > 0 ? log10(this->cardSizeMB) + 1 : 1;
      char sz[NUM_DIGITS + 1];
      sz[NUM_DIGITS] = 0;
      uint64_t size = this->cardSizeMB;
      for (size_t i = NUM_DIGITS; i--; size /= 10) sz[i] = '0' + (size % 10);
      this->card_sz = sz;
    }

    if (!MARAUDER_STORAGE.exists("/SCRIPTS")) MARAUDER_STORAGE.mkdir("/SCRIPTS");
    this->sd_files = new LinkedList<String>();
    return true;
  #endif
}

File SDInterface::getFile(String path) {
  if (this->supported) {
    File file = MARAUDER_STORAGE.open(path, FILE_READ);

    //if (file)
    return file;
  }
}

bool SDInterface::removeFile(String file_path) {
  if (MARAUDER_STORAGE.remove(file_path))
    return true;
  else
    return false;
}

// GCOVR_EXCL_START -- requires mounted SPIFFS and SD filesystems.
bool SDInterface::migrateSPIFFS(uint8_t operation, size_t& files_copied, size_t& bytes_copied, uint8_t& error) {
  files_copied = bytes_copied = error = 0;

  if (!this->supported) {
    error = 1;
    return false;
  }

  const String backup_path = "/spiffs";
  File backup = MARAUDER_STORAGE.open(backup_path);
  bool valid_backup = backup && backup.isDirectory();
  backup.close();

  if (operation == 1) {
    if (!valid_backup) {
      error = 2;
      return false;
    }
    return copyTree(MARAUDER_STORAGE, backup_path, nullptr, "", files_copied, bytes_copied, error);
  }

  if (operation == 2) {
    if (!valid_backup) {
      error = 2;
      return false;
    }
    const String rollback_path = "/spiffs.restore-rollback";
    if (!removeTree(MARAUDER_STORAGE, rollback_path)) {
      error = 3;
      return false;
    }
    size_t rollback_files = 0, rollback_bytes = 0;
    uint8_t rollback_error = 0;
    if (!copyTree(SPIFFS, "/", &MARAUDER_STORAGE, rollback_path, rollback_files, rollback_bytes, rollback_error)) {
      removeTree(MARAUDER_STORAGE, rollback_path);
      error = 3;
      return false;
    }
    bool cleared = removeTree(SPIFFS, "/", true);
    if (cleared && copyTree(MARAUDER_STORAGE, backup_path, &SPIFFS, "/", files_copied, bytes_copied, error)) {
      removeTree(MARAUDER_STORAGE, rollback_path);
      return true;
    }
    removeTree(SPIFFS, "/", true);
    size_t recovered_files = 0, recovered_bytes = 0;
    uint8_t recovery_error = 0;
    copyTree(MARAUDER_STORAGE, rollback_path, &SPIFFS, "/", recovered_files, recovered_bytes, recovery_error);
    removeTree(MARAUDER_STORAGE, rollback_path);
    error = 3;
    return false;
  }

  const String staging_path = "/spiffs.tmp";
  const String previous_path = "/spiffs.previous";

  if (!removeTree(MARAUDER_STORAGE, staging_path) || !removeTree(MARAUDER_STORAGE, previous_path)) {
    error = 3;
    return false;
  }

  if (!copyTree(SPIFFS, "/", &MARAUDER_STORAGE, staging_path, files_copied, bytes_copied, error)) {
    removeTree(MARAUDER_STORAGE, staging_path);
    return false;
  }

  if (MARAUDER_STORAGE.exists(backup_path) && !MARAUDER_STORAGE.rename(backup_path, previous_path)) {
    removeTree(MARAUDER_STORAGE, staging_path);
    error = 3;
    return false;
  }

  if (!MARAUDER_STORAGE.rename(staging_path, backup_path)) {
    if (MARAUDER_STORAGE.exists(previous_path))
      MARAUDER_STORAGE.rename(previous_path, backup_path);
    error = 3;
    return false;
  }

  removeTree(MARAUDER_STORAGE, previous_path);
  return true;
}
// GCOVR_EXCL_STOP

void SDInterface::listDirToLinkedList(LinkedList<String>* file_names, String str_dir, String ext) {
  if (this->supported) {
    File dir = MARAUDER_STORAGE.open(str_dir);
    while (true)
    {
      File entry = dir.openNextFile();
      if (!entry)
      {
        break;
      }

      if (entry.isDirectory())
        continue;

      String file_name = entry.name();
      if (ext != "") {
        if (file_name.endsWith(ext)) {
          file_names->add(file_name);
        }
      }
      else
        file_names->add(file_name);
    }
  }
}

void SDInterface::listDir(String str_dir){
  if (this->supported) {
    File dir = MARAUDER_STORAGE.open(str_dir);
    while (true)
    {
      File entry = dir.openNextFile();
      if (! entry)
      {
        break;
      }
      //for (uint8_t i = 0; i < numTabs; i++)
      //{
      //  Serial.print('\t');
      //}
      Serial.print(entry.name());
      Serial.print("\t");
      Serial.println(entry.size());
      entry.close();
    }
  }
}

bool SDInterface::handleStorageCommand(LinkedList<String>& args) {
  // A boot-time failure must not permanently disable Android-backed storage.
  // Retry the one-call mount whenever the phone requests storage access.
  if (!this->supported && !this->initSD()) {
    storageError("not_mounted");
    return false;
  }
  if (args.size() < 2) {
    storageError("usage");
    return false;
  }

  const String operation = args.get(1);
  #ifdef HAS_VIRTUAL_SD
  if (operation == "host" && args.size() == 4) {
    uint64_t total = 0;
    uint64_t free = 0;
    if (!storageUint64(args.get(2), total) || !storageUint64(args.get(3), free) ||
        total == 0 || free > total) {
      storageError("invalid_host_capacity");
      return false;
    }
    this->androidHostTotalBytes = total;
    this->androidHostFreeBytes = free;
    this->androidHostCapacityValid = true;
    // General firmware status and the on-device info screen must describe the
    // virtual SD presented to the user, not just its small flash-backed spool.
    this->cardSizeMB = total / (1024 * 1024);
    this->card_sz = String(this->cardSizeMB);
    Serial.println("SD:HOST:total=" + String(total));
    Serial.println("SD:HOST:free=" + String(free));
    Serial.println(F("SD:OK:host-capacity"));
    return true;
  }
  #endif
  if (operation == "status") {
    Serial.println(F("SD:STATUS:mounted=true"));
    #ifdef HAS_VIRTUAL_SD
      Serial.println(F("SD:STATUS:type=virtual"));
      Serial.println(
        String(F("SD:STATUS:backing=")) +
        (this->androidHostCapacityValid ? F("android") : F("spool"))
      );
      Serial.println(
        "SD:STATUS:total=" + String(
          this->androidHostCapacityValid ? this->androidHostTotalBytes : FFat.totalBytes()
        )
      );
      Serial.println(
        "SD:STATUS:free=" + String(
          this->androidHostCapacityValid ? this->androidHostFreeBytes : FFat.freeBytes()
        )
      );
      Serial.println("SD:STATUS:spool_total=" + String(FFat.totalBytes()));
      Serial.println("SD:STATUS:spool_free=" + String(FFat.freeBytes()));
    #else
      Serial.println(F("SD:STATUS:type=physical"));
      Serial.println("SD:STATUS:total=" + String(SD.totalBytes()));
      Serial.println("SD:STATUS:free=" + String(SD.totalBytes() - SD.usedBytes()));
    #endif
    Serial.println(F("SD:OK"));
    return true;
  }

  if (operation == "list") {
    String path = args.size() >= 3 ? args.get(2) : "/";
    if (!storagePath(path)) {
      storageError("invalid_path");
      return false;
    }
    File directory = MARAUDER_STORAGE.open(path);
    if (!directory || !directory.isDirectory()) {
      if (directory) directory.close();
      storageError("cannot_open:" + path);
      return false;
    }
    Serial.println("SD:LIST:" + path);
    size_t count = 0;
    while (true) {
      File entry = directory.openNextFile();
      if (!entry) break;
      String name = entry.name();
      name = name.substring(name.lastIndexOf('/') + 1);
      if (entry.isDirectory()) {
        Serial.println("SD:DIR:[" + String(count) + "] " + name);
      } else {
        Serial.println(
          "SD:FILE:[" + String(count) + "] " + name + " " + String(entry.size()) + " " +
          String(static_cast<uint32_t>(entry.getLastWrite()))
        );
      }
      entry.close();
      ++count;
    }
    directory.close();
    if (count == 0) Serial.println(F("SD:EMPTY"));
    Serial.println("SD:OK:listed " + String(count) + " entries");
    return true;
  }

  if (operation == "size" && args.size() >= 3) {
    String path = args.get(2);
    if (!storagePath(path)) {
      storageError("invalid_path");
      return false;
    }
    File file = MARAUDER_STORAGE.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      storageError("not_found:" + path);
      return false;
    }
    Serial.println("SD:SIZE:" + String(file.size()));
    file.close();
    Serial.println(F("SD:OK"));
    return true;
  }

  if (operation == "read" && args.size() >= 5) {
    String path = args.get(2);
    size_t offset = 0;
    size_t wanted = 0;
    if (!storagePath(path) || !storageNumber(args.get(3), offset) ||
        !storageNumber(args.get(4), wanted)) {
      storageError("invalid_read_request");
      return false;
    }
    File file = MARAUDER_STORAGE.open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      storageError("cannot_open:" + path);
      return false;
    }
    const size_t fileSize = file.size();
    if (offset > fileSize) offset = fileSize;
    const size_t length = min(wanted, fileSize - offset);
    if (!file.seek(offset)) {
      file.close();
      storageError("seek_failed");
      return false;
    }

    Serial.println("SD:READ:BEGIN:" + path);
    Serial.println("SD:READ:SIZE:" + String(fileSize));
    Serial.println("SD:READ:OFFSET:" + String(offset));
    Serial.println("SD:READ:LENGTH:" + String(length));
    Serial.println(F("SD:READ:ENCODING:base64"));
    constexpr size_t chunkSize = 384;
    uint8_t input[chunkSize];
    unsigned char encoded[((chunkSize + 2) / 3) * 4 + 1];
    size_t total = 0;
    while (total < length) {
      const size_t requested = min(chunkSize, length - total);
      const size_t received = file.read(input, requested);
      if (received == 0) break;
      size_t encodedLength = 0;
      if (mbedtls_base64_encode(
            encoded, sizeof(encoded), &encodedLength, input, received
          ) != 0) {
        file.close();
        storageError("base64_encode_failed");
        return false;
      }
      encoded[encodedLength] = '\0';
      Serial.println("SD:READ:DATA:" + String(reinterpret_cast<char*>(encoded)));
      total += received;
    }
    file.close();
    Serial.println("SD:READ:END:bytes=" + String(total));
    if (total != length) {
      storageError("file_read_failed");
      return false;
    }
    Serial.println(F("SD:OK"));
    return true;
  }

  if ((operation == "write" || operation == "append") && args.size() >= 4) {
    String path = args.get(2);
    if (!storagePath(path) || path == "/") {
      storageError("invalid_path");
      return false;
    }
    if (buffer_obj.isActiveFile(path)) {
      storageError("active_file:" + path);
      return false;
    }
    const String encoded = args.get(3);
    const size_t capacity = (encoded.length() * 3) / 4 + 4;
    uint8_t* decoded = static_cast<uint8_t*>(malloc(capacity));
    if (!decoded) {
      storageError("oom");
      return false;
    }
    size_t decodedLength = 0;
    if (mbedtls_base64_decode(
          decoded, capacity, &decodedLength,
          reinterpret_cast<const unsigned char*>(encoded.c_str()), encoded.length()
        ) != 0) {
      free(decoded);
      storageError("base64_decode_failed");
      return false;
    }
    File file = MARAUDER_STORAGE.open(
      path, operation == "append" ? FILE_APPEND : FILE_WRITE, true
    );
    if (!file) {
      free(decoded);
      storageError((operation == "append" ? "cannot_open:" : "cannot_create:") + path);
      return false;
    }
    const size_t written = file.write(decoded, decodedLength);
    file.close();
    free(decoded);
    Serial.println(
      String(operation == "append" ? "SD:APPEND:bytes=" : "SD:WRITE:bytes=") + written
    );
    if (written != decodedLength) {
      storageError("short_write:" + path);
      return false;
    }
    Serial.println(
      String(operation == "append" ? "SD:OK:appended:" : "SD:OK:created:") + path
    );
    return true;
  }

  if (operation == "crc32" && args.size() >= 3) {
    String path = args.get(2);
    if (!storagePath(path) || path == "/") {
      storageError("invalid_path");
      return false;
    }
    if (buffer_obj.isActiveFile(path)) {
      storageError("active_file:" + path);
      return false;
    }
    uint64_t size = 0;
    uint32_t checksum = 0;
    if (!storageFileCrc32(path, size, checksum)) {
      storageError("cannot_checksum:" + path);
      return false;
    }
    char crcText[9];
    snprintf(crcText, sizeof(crcText), "%08lX", static_cast<unsigned long>(checksum));
    Serial.println("SD:CRC32:" + String(crcText));
    char sizeText[24];
    snprintf(sizeText, sizeof(sizeText), "%llu", static_cast<unsigned long long>(size));
    Serial.println("SD:CRC32:SIZE:" + String(sizeText));
    Serial.println(F("SD:OK"));
    return true;
  }

  if (operation == "ack" && args.size() == 5) {
    String path = args.get(2);
    uint64_t expectedSize = 0;
    uint32_t expectedChecksum = 0;
    if (!storagePath(path) || path == "/" ||
        !storageUint64(args.get(3), expectedSize) ||
        !storageCrc32Value(args.get(4), expectedChecksum)) {
      storageError("invalid_ack");
      return false;
    }
    if (buffer_obj.isActiveFile(path)) {
      storageError("active_file:" + path);
      return false;
    }
    uint64_t actualSize = 0;
    uint32_t actualChecksum = 0;
    if (!storageFileCrc32(path, actualSize, actualChecksum)) {
      storageError("cannot_checksum:" + path);
      return false;
    }
    if (actualSize != expectedSize || actualChecksum != expectedChecksum) {
      storageError("ack_mismatch");
      return false;
    }
    if (!MARAUDER_STORAGE.remove(path)) {
      storageError("release_failed:" + path);
      return false;
    }
    char crcText[9];
    snprintf(crcText, sizeof(crcText), "%08lX", static_cast<unsigned long>(actualChecksum));
    Serial.println("SD:ACK:path=" + path);
    char sizeText[24];
    snprintf(sizeText, sizeof(sizeText), "%llu", static_cast<unsigned long long>(actualSize));
    Serial.println("SD:ACK:size=" + String(sizeText));
    Serial.println("SD:ACK:crc32=" + String(crcText));
    Serial.println(F("SD:OK"));
    return true;
  }

  if (operation == "mkdir" && args.size() >= 3) {
    String path = args.get(2);
    if (!storagePath(path) || path == "/") {
      storageError("invalid_path");
      return false;
    }
    if (MARAUDER_STORAGE.exists(path) || MARAUDER_STORAGE.mkdir(path)) {
      Serial.println("SD:OK:mkdir:" + path);
      return true;
    }
    storageError("mkdir_failed:" + path);
    return false;
  }

  if (operation == "rm" && args.size() >= 3) {
    String path = args.get(2);
    if (!storagePath(path) || path == "/") {
      storageError("invalid_path");
      return false;
    }
    File target = MARAUDER_STORAGE.open(path);
    if (!target) {
      storageError("not_found:" + path);
      return false;
    }
    const bool directory = target.isDirectory();
    target.close();
    const bool removed = directory ? MARAUDER_STORAGE.rmdir(path) : MARAUDER_STORAGE.remove(path);
    if (!removed) {
      storageError("remove_failed:" + path);
      return false;
    }
    Serial.println("SD:OK:removed:" + path);
    return true;
  }

  storageError("unsupported");
  return false;
}

void SDInterface::runUpdate(String file_name) {
  if (file_name == "")
    file_name = "/update.bin";

  #ifdef HAS_SCREEN
    display_obj.tft.setTextWrap(false);
    display_obj.tft.setFreeFont(NULL);
    display_obj.tft.setCursor(0, TFT_HEIGHT / 3);
    display_obj.tft.setTextSize(1);
    display_obj.tft.setTextColor(TFT_WHITE);
  
    display_obj.tft.println("Opening " + file_name + "...");
  #endif

  File updateBin = MARAUDER_STORAGE.open(file_name);

  if (updateBin) {
    if(updateBin.isDirectory()){
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F(text_table2[0]));
      #endif
      Serial.print(F("Error, could not find \""));
      Serial.print(file_name);
      Serial.println(F("\""));
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      updateBin.close();
      return;
    }

    size_t updateSize = updateBin.size();

    if (updateSize > 0) {
      #ifdef HAS_SCREEN
        display_obj.tft.println(F(text_table2[1]));
      #endif
      Serial.println(F("Starting update over SD. Please wait..."));
      this->performUpdate(updateBin, updateSize);
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_RED);
        display_obj.tft.println(F(text_table2[2]));
      #endif
      Serial.println(F("Error, file is empty"));
      #ifdef HAS_SCREEN
        display_obj.tft.setTextColor(TFT_WHITE);
      #endif
      return;
    }

    updateBin.close();
    
      // whe finished remove the binary from sd card to indicate end of the process
    #ifdef HAS_SCREEN
      display_obj.tft.println(F(text_table2[3]));
    #endif
    const esp_partition_t *running = esp_ota_get_running_partition();

    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);

    esp_err_t result = esp_ota_set_boot_partition(next);
     
    ESP.restart();
  }
  else {
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_RED);
      display_obj.tft.println(F(text_table2[4]));
    #endif
    Serial.println(F("Could not load update.bin from sd root"));
    #ifdef HAS_SCREEN
      display_obj.tft.setTextColor(TFT_WHITE);
    #endif
  }
}

void SDInterface::performUpdate(Stream &updateSource, size_t updateSize) {
  if (Update.begin(updateSize)) {   
    #ifdef HAS_SCREEN
      display_obj.tft.println(text_table2[5] + String(updateSize));
      display_obj.tft.println(F(text_table2[6]));
    #endif
    size_t written = Update.writeStream(updateSource);
    if (written == updateSize) {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[7] + String(written) + text_table2[10]);
      #endif
      Serial.print(F("Written : "));
      Serial.print(written);
      Serial.println(F(" successfully"));
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[8] + String(written) + "/" + String(updateSize) + text_table2[9]);
      #endif
      Serial.print(F("Written only : "));
      Serial.print(written);
      Serial.print(F("/"));
      Serial.print(updateSize);
      Serial.println(F(". Retry?"));
    }
    if (Update.end()) {
      if (Update.isFinished()) {

      }
      else {
        #ifdef HAS_SCREEN
          display_obj.tft.setTextColor(TFT_RED);
          display_obj.tft.println(text_table2[12]);
        #endif
        Serial.println(F("Update not finished? Something went wrong!"));
        #ifdef HAS_SCREEN
          display_obj.tft.setTextColor(TFT_WHITE);
        #endif
      }
    }
    else {
      #ifdef HAS_SCREEN
        display_obj.tft.println(text_table2[13] + String(Update.getError()));
      #endif
      Serial.print(F("Error Occurred. Error #: "));
      Serial.println(Update.getError());
    }

  }
  else
  {
    #ifdef HAS_SCREEN
      display_obj.tft.println(text_table2[14]);
    #endif
    Serial.println(F("Not enough space to begin OTA"));
  }
}
