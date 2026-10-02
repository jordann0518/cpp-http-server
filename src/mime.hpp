#pragma once

#include <string>

// Returns the MIME type for a file path based on its extension.
// Text types include a UTF-8 charset parameter. Unknown extensions
// fall back to application/octet-stream.
std::string mime_type_for(const std::string& path);
