#ifndef THINGER_UTIL_COMPRESSION_HPP
#define THINGER_UTIL_COMPRESSION_HPP

#include <string>
#include <sstream>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>
#include <boost/iostreams/filter/zlib.hpp>

namespace thinger::util {

class gzip {
public:
    // Compress string to gzip format
    static std::string compress(const std::string& data) {
        std::stringstream compressed;
        std::stringstream origin(data);
        
        boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
        in.push(boost::iostreams::gzip_compressor());
        in.push(origin);
        
        boost::iostreams::copy(in, compressed);
        return compressed.str();
    }
    
    // Decompress gzip data
    static std::string decompress(const std::string& data) {
        std::stringstream decompressed;
        std::stringstream compressed(data);
        
        boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
        in.push(boost::iostreams::gzip_decompressor());
        in.push(compressed);
        
        boost::iostreams::copy(in, decompressed);
        return decompressed.str();
    }
    
    // Check if data might be gzip compressed (by checking magic bytes)
    static bool is_gzip(const std::string& data) {
        return data.size() >= 2 && 
               static_cast<unsigned char>(data[0]) == 0x1f && 
               static_cast<unsigned char>(data[1]) == 0x8b;
    }
};

class deflate {
public:
    // Compress string using deflate
    static std::string compress(const std::string& data) {
        std::stringstream compressed;
        std::stringstream origin(data);
        
        boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
        in.push(boost::iostreams::zlib_compressor());
        in.push(origin);
        
        boost::iostreams::copy(in, compressed);
        return compressed.str();
    }
    
    // Decompress deflate data
    static std::string decompress(const std::string& data) {
        std::stringstream decompressed;
        std::stringstream compressed(data);
        
        boost::iostreams::filtering_streambuf<boost::iostreams::input> in;
        in.push(boost::iostreams::zlib_decompressor());
        in.push(compressed);
        
        boost::iostreams::copy(in, decompressed);
        return decompressed.str();
    }
};

} // namespace thinger::util

#endif // THINGER_UTIL_COMPRESSION_HPP