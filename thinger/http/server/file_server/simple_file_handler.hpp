#ifndef SIMPLE_FILE_HANDLER_HPP
#define SIMPLE_FILE_HANDLER_HPP

#include <filesystem>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>
#include "file_server_handler.hpp"
#include "../../http_response.hpp"
#include "../mime_types.hpp"
#include "../../data/out_array.hpp"
#include "../../../util/compression.hpp"

#include <iostream>
#include <fstream>
#include <chrono>

#include "thinger/http/util/utf8.hpp"


namespace thinger::http {


    using namespace std::filesystem;


    class simple_file_handler : public file_server_handler {

    public:
        simple_file_handler(const std::string &doc_root) :
                file_server_handler(doc_root) {
        }

        virtual ~simple_file_handler() {
        }

    public:

    	static bool send_file(const std::filesystem::path& full_path, request& request, bool force_download=false){
            auto http_request = request.get_http_request();

        	if(!exists(full_path) || !is_regular_file(full_path)) {
                request.handle_error(http_response::status::not_found);
			    return false;
		    }

        	auto extension = full_path.extension().string();
        	if(!extension.empty()){
                extension = extension.substr(1);
        	}

		    static std::set<std::string> extensions = {"js", "css", "json", "html", "svg", "txt", "php", "txt", "xml", "xhtml", "csv", "arff"};
		    auto fs = file_size(full_path);
		    bool gzip = false;

		    // check if gzip a text file
		    if(fs>200 && extensions.find(extension)!=extensions.end()){
			    const std::string& accept_encoding = http_request->get_header("Accept-Encoding");
			    std::vector<std::string> dataLine;
			    boost::split(dataLine, accept_encoding, boost::is_any_of(","));
			    for(auto& encoding : dataLine){
				    boost::trim(encoding);
				    boost::algorithm::to_lower(encoding);
				    if(encoding=="gzip"){
					    gzip = true;
					    break;
				    }
			    }
		    }

		    // build response
		    auto response = std::make_shared<http_response>();
		    response->set_keep_alive(http_request->keep_alive());
		    response->set_status(http_response::status::ok);

            // set content-type based on extension and/or file
            if(!extension.empty()){
                response->set_content_type(mime_types::extension_to_type(extension));
            }else{
                if(file_is_utf8(full_path.string())){
                    response->set_content_type(mime_types::text_plain);
                }else{
                    response->set_content_type(mime_types::application_octect_stream);
                }
            }

		    // if compress data
		    if(gzip){
			    // Read file content
			    std::ifstream file(full_path.c_str(), std::ios::binary | std::ios::ate);
			    if (!file) {
				    request.handle_error(http_response::status::not_found);
				    return false;
			    }
			    
			    std::streamsize size = file.tellg();
			    file.seekg(0, std::ios::beg);
			    
			    std::string buffer(size, '\0');
			    if (!file.read(buffer.data(), size)) {
				    request.handle_error(http_response::status::internal_server_error);
				    return false;
			    }
			    file.close();
			    
			    // Compress using gzip
			    auto data = std::make_shared<data::out_string>();
			    try {
				    data->get_string() = util::gzip::compress(buffer);
				    auto fsCompressed = data->get_size();
				    response->add_header("Content-Encoding", "gzip");
				    response->set_content_length(fsCompressed);
				    response->set_next_data(data);
			    } catch (const std::exception& e) {
				    // If compression fails, fall back to uncompressed
				    gzip = false;
			    }
		    }
		    
		    if(!gzip){
			    std::ifstream is(full_path.c_str(), std::ios::in | std::ios::binary);
			    if (!is) {
                    request.handle_error(http_response::status::not_found);
				    return false;
			    }
			    response->set_content_length(fs);
			    auto data = std::make_shared<data::out_array>(fs);
			    is.read(data->get_array(), fs);
			    is.close();
			    response->set_next_data(data);
		    }

		    if(force_download){
				response->add_header("Content-Disposition", "attachment; filename=\"" + full_path.filename().string() + "\"");
		    }

		    request.handle_response(response);
		    return true;
        }

        virtual bool handle_request(std::shared_ptr<request> request) {
            auto full_path = get_fs_path(request);
            if(full_path.empty()) return false;

            return send_file(full_path, *request);
        }
    };

}

#endif
