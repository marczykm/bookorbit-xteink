#pragma once
// Minimal BookOrbit REST client (HTTP or HTTPS).
#include <Arduino.h>

struct Book {
  long id = 0;
  long fileId = 0;  // the EPUB file, 0 when the book has none
  char title[96] = "";
  char author[64] = "";
};

class BookOrbit {
 public:
  // `insecure` skips certificate checks (for self-signed servers).
  void configure(const String& server, bool insecure = false);
  bool login(const String& user, const String& password, String& error);
  // Fills up to `max` books of one page; returns the count, or -1 on error. `total` is the library size.
  int listBooks(int page, int pageSize, Book* out, int max, int& total, String& error);
  // Reports reading progress (0-100) for a book file.
  bool postProgress(long fileId, float percentage, String& error);
  // Uploads one reading session (a JSON object). `rejected` is set when the server refuses it for good (a 4xx other than
  // 401/408/429), so the caller can drop it instead of retrying forever.
  bool postSession(long fileId, const String& body, bool& rejected, String& error);
  // Reads the saved reading progress (0-100) of a book file.
  bool getProgress(long fileId, float& percentage, String& error);
  // The book being read now (the first of the "continue reading" shelf), with its progress 0-100. `found` is false when the shelf is empty.
  bool fetchContinueReading(Book& book, float& progress, bool& found, String& error);
  // Fetches the dashboard widgets named in `types` and returns them as the server sent them, reduced to {"type":data} pairs.
  bool fetchWidgets(const char* const* types, int count, String& json, String& error);
  // Saves a book's cover thumbnail (whatever image format the server sends) to `path`.
  bool fetchThumbnail(long bookId, const String& path, String& error);
  // Where download() stores the book on the SD card.
  static String pathFor(const Book& book);
  // Saves the book's EPUB under /BookOrbit/ and returns its SD path in `path`.
  bool download(const Book& book, String& path, String& error, void (*progress)(size_t done, size_t total));

 private:
  int listBooksOnce(int page, int pageSize, Book* out, int max, int& total, String& error);
  bool fetchContinueReadingOnce(Book& book, float& progress, bool& found, String& error);
  bool fetchWidgetsOnce(const char* const* types, int count, String& json, String& error);
  bool fetchThumbnailOnce(long bookId, const String& path, String& error);
  bool postSessionOnce(long fileId, const String& body, bool& rejected, String& error);
  bool postProgressOnce(long fileId, float percentage, String& error);
  bool getProgressOnce(long fileId, float& percentage, String& error);
  bool downloadOnce(const Book& book, String& path, String& error, void (*progress)(size_t done, size_t total));
  bool refresh();
  String server_, token_, refreshToken_;
};
