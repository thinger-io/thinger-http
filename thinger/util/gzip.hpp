#ifndef __GZIP_H__
#define __GZIP_H__

#include <sstream>
#include <boost/iostreams/filtering_streambuf.hpp>
#include <boost/iostreams/copy.hpp>
#include <boost/iostreams/filter/gzip.hpp>

#include <cryptopp/gzip.h>
#include <cryptopp/files.h>

class Gzip {
public:

	static void compress(const boost::filesystem::path& path, std::string& destination)
	{
		CryptoPP::FileSource ss(path.string().c_str(), true,
          new CryptoPP::Gzip(
                  new CryptoPP::StringSink(destination), 1)
		);
	}

	/*
	std::string decompress(const std::string& data)
	{
		std::string decompressed;
		CryptoPP::ZlibDecompressor unzipper(new CryptoPP::StringSink(decompressed));
		unzipper.Put( (unsigned char*) data.d./push ata(), data.size());
		unzipper.MessageEnd();
		return decompressed;
	}
	 */

	static bool get_compressed_file(const boost::filesystem::path& path, std::string& destination){

		namespace bio = boost::iostreams;

		std::ifstream inStream(path.string(), std::ios_base::in);
		std::stringstream compressed;

		bio::filtering_streambuf<bio::input> out;
		out.push(bio::gzip_compressor(bio::gzip_params(bio::gzip::best_speed)));
		out.push(inStream);
		bio::copy(out, compressed);

		destination = compressed.str();

		return true;
	}

};

#endif // __GZIP_H__