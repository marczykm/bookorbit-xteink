#pragma once
// Minimal BookOrbit REST client (HTTP or HTTPS).
#include <Arduino.h>

struct Book {
  long id = 0;
  long fileId = 0;  // the EPUB file, 0 when the book has none
  char title[96] = "";
  char author[64] = "";
};

// An author, series or smart scope in a browse list.
struct Entity {
  long id = 0;
  char name[64] = "";
  int count = 0;  // books in it
};

// What a book list is about: all books, or the books of one author / series / smart scope.
enum class Scope { All = 0, Author = 1, Series = 2, Smart = 3 };

class BookOrbit {
 public:
  // `insecure` skips certificate checks (for self-signed servers).
  void configure(const String& server, bool insecure = false);
  // Called with true when a transfer starts and false when it ends (nestable), so the app can free RAM for the Wi-Fi stack.
  static void setActivityHook(void (*hook)(bool active));
  bool login(const String& user, const String& password, String& error);
  // Fills up to `max` books of one page; returns the count, or -1 on error. `total` is the library size.
  int listBooks(int page, int pageSize, Book* out, int max, int& total, String& error, Scope scope = Scope::All, long scopeId = 0);
  // Fills one page of authors, series or smart scopes (`kind` must be Author, Series or Smart); returns the count or -1.
  int listEntities(Scope kind, int page, int pageSize, Entity* out, int max, int& total, String& error);
  // Reports reading progress (0-100) for a book file.
  bool postProgress(long fileId, float percentage, String& error);
  // Uploads one reading session (a JSON object). `rejected` is set when the server refuses it for good (a 4xx other than
  // 401/408/429), so the caller can drop it instead of retrying forever.
  bool postSession(long fileId, const String& body, bool& rejected, String& error);
  // Reads the saved reading progress (0-100) of a book file.
  bool getProgress(long fileId, float& percentage, String& error);
  // The books being read now (the "continue reading" shelf, most recently read first): fills up to `max` books and their
  // progress 0-100, and returns the count (0 when nothing is in progress) or -1 on error.
  int fetchContinueReading(Book* books, float* progress, int max, String& error);
  // Fetches the dashboard widgets named in `types` and returns them as the server sent them, reduced to {"type":data} pairs.
  bool fetchWidgets(const char* const* types, int count, String& json, String& error);
  // Saves a book's cover thumbnail (whatever image format the server sends) to `path`.
  bool fetchThumbnail(long bookId, const String& path, String& error);
  // Where download() stores the book on the SD card.
  static String pathFor(const Book& book);
  // Saves the book's EPUB under /BookOrbit/ and returns its SD path in `path`.
  bool download(const Book& book, String& path, String& error, void (*progress)(size_t done, size_t total));

 private:
  int listBooksOnce(int page, int pageSize, Book* out, int max, int& total, String& error, Scope scope, long scopeId);
  int listEntitiesOnce(Scope kind, int page, int pageSize, Entity* out, int max, int& total, String& error);
  int fetchContinueReadingOnce(Book* books, float* progress, int max, String& error);
  bool fetchThumbnailOnce(long bookId, const String& path, String& error);
  bool fetchWidgetsOnce(const char* const* types, int count, String& json, String& error);
  bool postSessionOnce(long fileId, const String& body, bool& rejected, String& error);
  bool postProgressOnce(long fileId, float percentage, String& error);
  bool getProgressOnce(long fileId, float& percentage, String& error);
  bool downloadOnce(const Book& book, String& path, String& error, void (*progress)(size_t done, size_t total));
  bool refresh();
  String server_, token_, refreshToken_;
};
