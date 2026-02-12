#ifndef SEND_FILE_HANDLER_HPP
#define SEND_FILE_HANDLER_HPP

#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/iostreams/device/mapped_file.hpp>
#include <boost/iostreams/device/file_descriptor.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>
#include <filesystem>

#include "file_server_handler.hpp"
#include "../../http_response.hpp"
#include "../../http_headers.hpp"
#include "../../data/http_data.hpp"
#include "../mime_types.hpp"
#include "../../data/out_sendfile.hpp"

namespace thinger::http {

            using namespace std::filesystem;

            class sendfile_file_handler : public file_server_handler {

            public:
                sendfile_file_handler(const std::string &doc_root) :
                        file_server_handler(doc_root) {
                }

                virtual ~sendfile_file_handler() {
                }

            public:

                virtual bool handle_request(std::shared_ptr<request> request) {

                    auto full_path = get_fs_path(request);
                    if(full_path.empty()) return false;

                    auto http_request = request->get_http_request();

                    LOG_F(2, "sendfile handler. sending file: %s", full_path.c_str());

                    if (!exists(full_path) || !is_regular_file(full_path))
                    {
                        request->handle_error(http_response::not_found);
                        return false;
                    }

                    int fd(open(full_path.c_str(), O_RDONLY));

                    if (fd == -1) {
                        request->handle_error(http_response::not_found);
                        return false;
                    }

                    auto fs = file_size(full_path);


                    auto response = std::make_shared<http_response>(true);
                    response->status = http_response::ok;
                    response->set_keep_alive(http_request->keep_alive());
                    response->set_content_type(mime_types::extension_to_type(full_path.extension().string()));
                    response->set_content_length(fs);
                    response->set_last_frame(false);

                    request->handle_response(response);

                    auto data = std::make_shared<http_data>(std::make_shared<base::out_sendfile>(fd, fs));

                    request->handle_response(data);

                    return true;
                }
            };
}

#endif
