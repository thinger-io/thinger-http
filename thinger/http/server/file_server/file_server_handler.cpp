#include "file_server_handler.hpp"
#include "../request.hpp"

namespace thinger::http{

    file_server_handler::file_server_handler(const std::string& www_root, std::string default_index) :
        root_{www_root},
        default_index_{std::move(default_index)}
    {

    }

    std::filesystem::path file_server_handler::get_fs_path(std::shared_ptr<request> request) const{
        const std::string& resource = request->get_http_request()->get_resource();

        if(on_root_ && resource=="/"){
            on_root_(request);
            return {};
        }

        // compute full path with default index (i.e. index.html)
        std::filesystem::path full_path{root_ / resource};
        if(!default_index_.empty() && resource[resource.size() - 1] == '/'){
            full_path /= default_index_;
        }

        // file does not exist ? call listener
        if (on_not_found_ && !exists(full_path) || !is_regular_file(full_path)) {
            on_not_found_(request);
            return {};
        }

        return full_path;
    }

    void file_server_handler::on_root(std::function<void(std::shared_ptr<request>)> on_root){
        on_root_ = on_root;
    }

    void file_server_handler::on_not_found(std::function<void(std::shared_ptr<request>)> on_not_found){
        on_not_found_ = on_not_found;
    }

}

