#include "worker_client.hpp"
#include "workers.hpp"

namespace thinger::asio {

worker_client::worker_client(const std::string& service_name) 
    : service_name_(service_name) {
    // Register as a worker client - workers will auto-start if needed
    get_workers().register_client(this);

    start();
}

worker_client::~worker_client() {
    // Ensure service is stopped before destruction
    stop();
    
    // Unregister from workers - workers will auto-stop if no clients remain
    get_workers().unregister_client(this);
}

} // namespace thinger::asio