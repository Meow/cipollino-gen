// The few operating-system services the host code needs, for POSIX and Windows.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace plat {

  // Create a directory only its owner can use (POSIX mode 0700; on Windows a new directory
  // takes its access list from its parent). With `exist_ok`, an existing one is not an error.
  bool make_private_dir(const std::string &path, bool exist_ok, std::string &err);

  // Create `path`, which must not exist yet, write `len` bytes and force them to disk.
  // POSIX mode 0600.
  bool write_new_file(const std::string &path, const void *data, size_t len, std::string &err);

  // Whether files can be created in the directory.
  bool dir_writable(const std::string &path, std::string &err);

  // Read a file of at most `cap` bytes into `buf` without leaving copies of it in library
  // buffers; `len` is its size. A longer file is an error.
  bool read_file(const std::string &path, void *buf, size_t cap, size_t &len, std::string &err);

  // Whether there is a file or directory of that name.
  bool exists(const std::string &path);

  // The names of the directories inside `path`, sorted.
  bool list_subdirs(const std::string &path, std::vector<std::string> &names, std::string &err);

  // Whether the file mode lets anyone but the owner in. Always false on Windows, which has
  // access lists instead.
  bool open_to_others(const std::string &path);

  // True if stderr is a terminal that understands "\r" and ANSI erase-line; on Windows this
  // also switches the console into that mode.
  bool stderr_is_terminal();

} // namespace plat
