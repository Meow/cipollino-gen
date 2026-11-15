// The few operating-system services the host code needs, for POSIX and Windows.
#pragma once

#include <cstddef>
#include <string>

namespace plat {

// Create a directory only its owner can use (POSIX mode 0700; on Windows a new directory
// takes its access list from its parent). With `exist_ok`, an existing one is not an error.
bool make_private_dir(const std::string &path, bool exist_ok, std::string &err);

// Create `path`, which must not exist yet, write `len` bytes and force them to disk.
// POSIX mode 0600.
bool write_new_file(const std::string &path, const void *data, size_t len, std::string &err);

// Whether files can be created in the directory.
bool dir_writable(const std::string &path, std::string &err);

// True if stderr is a terminal that understands "\r" and ANSI erase-line; on Windows this
// also switches the console into that mode.
bool stderr_is_terminal();

}  // namespace plat
