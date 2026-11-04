#pragma once
// Simulator: the SD card is a folder on the host (sim/sdcard unless --sd says otherwise).
#include <fcntl.h>

#include <memory>
#include <vector>

#include "Arduino.h"

typedef int oflag_t;
#ifndef O_WRITE
#define O_WRITE O_WRONLY
#endif
#ifndef O_READ
#define O_READ O_RDONLY
#endif

class FsFile : public Stream {
 public:
  FsFile() = default;
  explicit operator bool() const { return fd_ && *fd_ >= 0; }
  bool isOpen() const { return static_cast<bool>(*this); }
  int read(void* buf, size_t n);
  int read() override;
  int peek() override;
  int available() override;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buf, size_t n) override;
  size_t write(const void* buf, size_t n) { return write((const uint8_t*)buf, n); }
  using Print::write;
  bool seekSet(uint64_t pos);
  bool seek(uint64_t pos) { return seekSet(pos); }
  uint64_t curPosition() const;
  uint64_t position() const { return curPosition(); }
  uint64_t fileSize() const;
  uint64_t size() const { return fileSize(); }
  bool close();
  void flush() override {}
  bool sync() { return true; }

 private:
  friend class SDCardManager;
  std::shared_ptr<int> fd_;  // shared like a copied SdFat handle; closed with the last copy
};

class SDCardManager {
 public:
  bool begin();
  bool ready() const { return true; }
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  String readFile(const char* path);
  bool writeFile(const char* path, const String& content);
  bool ensureDirectoryExists(const char* path);
  FsFile open(const char* path, oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rmdir(const char* path);
  bool rename(const char* path, const char* newPath);
  bool replaceFile(const char* tmpPath, const char* path) { return (!exists(path) || remove(path)) && rename(tmpPath, path); }
  bool removeDir(const char* path);

  static SDCardManager& getInstance() { return instance; }
  static const char* root();  // the host folder

 private:
  static SDCardManager instance;
};

#define SdMan SDCardManager::getInstance()
