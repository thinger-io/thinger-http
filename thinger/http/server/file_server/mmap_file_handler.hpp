#ifndef MMAP_FILE_HANDLER_HPP
#define MMAP_FILE_HANDLER_HPP

#include <filesystem>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <unordered_map>

#include "file_server_handler.hpp"
#include "../../common/http_response.hpp"
#include "../mime_types.hpp"
#include "../../data/out_buffer.hpp"

namespace thinger::http {

    using namespace std::filesystem;

    class mmap_file_handler : public file_server_handler {

    public:
        mmap_file_handler(const std::string &doc_root) :
                file_server_handler(doc_root) {

        }

        virtual ~mmap_file_handler() {

        }

    private:

        std::unordered_map<std::string, boost::iostreams::mapped_file_source> mmapped_files_;
        boost::shared_mutex _access;

    public:

        virtual bool handle_request(std::shared_ptr<request> request) {

            auto full_path = get_fs_path(request);
            if(full_path.empty()) return false;

            auto http_request = request->get_http_request();

            std::string request_path = full_path.string();

            auto iterator = mmapped_files_.find(request_path);
            if (iterator == mmapped_files_.end()) {
                if (!exists(full_path) || !is_regular_file(full_path)) {
                    request->handle_error(http_response::not_found);
                    return false;
                }

                boost::upgrade_lock<boost::shared_mutex> upgrade_lock(_access);
                boost::upgrade_to_unique_lock<boost::shared_mutex> unique_lock(upgrade_lock);
                // ensure no other concurrent thread already loaded the file
                iterator = mmapped_files_.find(request_path);
                // load file
                if (iterator == mmapped_files_.end()) {
                    iterator = mmapped_files_.insert(std::make_pair(request_path, boost::iostreams::mapped_file_source(
                            full_path.c_str()))).first;
                }
            }

            const boost::iostreams::mapped_file_source &file = iterator->second;

            auto response = std::make_shared<http_response>(true);
            response->status = http_response::ok;
            response->set_keep_alive(http_request->keep_alive());
            response->set_content_type(mime_types::extension_to_type(full_path.extension().string()));
            response->set_content_length(file.size());

            auto data = std::make_shared<base::out_buffer>((uint8_t *) file.data(), file.size(), false);
            response->set_next_data(data);

            request->handle_response(response);
            return true;
        }
    };
}


#endif
