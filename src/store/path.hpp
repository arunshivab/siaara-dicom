// SIAARA DICOM Toolkit - safe handling of paths that come from DICOM content (standards PARSE-51).
//
// A File ID from a DICOMDIR (PS3.10 8.2, Referenced File ID (0004,1500)) or any other value taken
// from a data set is untrusted. Every component must be a plain name: not empty, not "." or "..",
// no separators ('/', '\\'), no drive or stream syntax (':'), no control characters, none of the
// characters Windows forbids (<>"|?*), no trailing dot or space, and not a reserved Windows device
// name (CON, PRN, AUX, NUL, COM0-9, LPT0-9, CONIN$, CONOUT$, also with an extension). The joined
// path must stay under the root even after symbolic links are resolved.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/status.hpp"

namespace siaara::store {

bool path_component_ok(std::string_view c) noexcept;

// Splits a multi-valued CS File ID ("DIR\SUB\FILE") into components (padding removed).
std::vector<std::string> split_file_id(std::string_view value);

// Joins sanitised components under root. Fails with Errc::Rejected on any unsafe component or
// on more than `max_components` components.
Result<std::filesystem::path> safe_join(const std::filesystem::path& root,
                                        const std::vector<std::string>& components,
                                        std::size_t max_components = 32);

// True when `p` (after resolving symbolic links and "..", where it exists) lies inside `root`.
bool is_within(const std::filesystem::path& root, const std::filesystem::path& p);

// safe_join, then, if the file does not exist and `case_insensitive`, looks each component up
// case-insensitively (ASCII) in its folder: media written on one system and read on another
// often differ in case. With case_insensitive = false no extra lookup is done and the file system
// decides: on a case-insensitive one (NTFS, default APFS) a File ID differing from the stored name
// only in case still resolves, because it names the same file. Finally checks is_within.
// NotFound when no file matches.
Result<std::filesystem::path> resolve_file_id(const std::filesystem::path& root,
                                              const std::vector<std::string>& components,
                                              bool case_insensitive);

}  // namespace siaara::store
