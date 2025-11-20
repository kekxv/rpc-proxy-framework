#pragma once

#include <string>
#include <vector> // for base64_decode return type if needed. For now std::string is fine.

// Declare base64_chars as extern in the header
extern const std::string base64_chars;

// Helper for base64_decode, declared in header, defined in cpp
inline bool is_base64(unsigned char c);

// Base64 encoding function
std::string base64_encode(unsigned char const* bytes_to_encode, unsigned int in_len);

// Base64 decoding function
std::string base64_decode(std::string const& encoded_string);
