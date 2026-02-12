#ifndef FILE_SERVER_HANDLER_HPP
#define FILE_SERVER_HANDLER_HPP

#include "../request_handler.hpp"
#include <filesystem>
#include <functional>
#include <memory>

namespace thinger::http{

    class file_server_handler : public request_handler {


    public:
        file_server_handler(const std::string &www_root, std::string default_index="index.html");

        virtual ~file_server_handler() {

        }

    public:

        void on_not_found(std::function<void(std::shared_ptr<request> request)> on_not_found);
        void on_root(std::function<void(std::shared_ptr<request> request)> on_root);

        std::filesystem::path get_fs_path(std::shared_ptr<request> request) const;

    protected:
        std::filesystem::path root_;
        std::string default_index_;
        std::function<void(std::shared_ptr<request> request)> on_not_found_;
        std::function<void(std::shared_ptr<request> request)> on_root_;

    };

}

#endif
